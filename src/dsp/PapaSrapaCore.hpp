#pragma once
#include <rack.hpp>

using namespace rack;

// Shared "decelerating RC-style ramp" square-wave shaping, used by both the
// modulator and the audio oscillator below. The real hardware's square
// oscillators don't hold a flat rail: each half cycle is a decelerating
// RC-style ramp that only reaches a fraction of full swing before the
// (relatively fast, but not instant) flip. Measured off a captured
// real-hardware period (wave/PapaSrapaSquare.wav): the flip itself takes
// ~3-4 samples at 48kHz (~70us) while the sustained ramp for the rest of the
// half-cycle is ~40x slower — one time constant can't fit both, hence the
// fast+slow blend below. RAMP_SLOW_TAU_PERIODS is in multiples of the
// oscillator's own period (not a fixed time), so the reached fraction per
// half-cycle — and the risk of smearing the fundamental at high audio pitch
// — stays constant across the whole pitch range. Values below are a
// starting point tuned by ear against that recording (idiomatic
// approximation, not a literal circuit emulation).
namespace PapaSrapaRamp {
    constexpr float FAST_TAU = 0.00007f;     // ~70us: fast flip
    constexpr float SLOW_TAU_PERIODS = 1.5f; // sustained ramp, in periods
    constexpr float MIX = 0.3f;              // weight of the fast component
    constexpr float MAKEUP_GAIN = 2.f;       // restores ~unity peak swing
    // fast and slow each individually curve smoothly, but their common target
    // still flips instantly, so the blended sum still has a sharp corner (a
    // discontinuous slope) right at each flip. A short extra lowpass stage
    // here stands in for the finite bandwidth any real output/buffer stage
    // would have, rounding that corner off — physically motivated, not just
    // decorative — without blunting the fast component's speed (its tau is
    // well under FAST_TAU, so it only softens the kink itself).
    constexpr float SMOOTH_TAU = 0.00002f; // ~20us

    // shape() below is templated to work both scalar (T=float, used by the
    // mono PapaSrapaModulator) and 4-wide SIMD (T=simd::float_4, used by
    // PapaSrapaCore batching 4 poly channels) — rack::simd only provides
    // fmax/exp/clamp for float_4, not float, so these three small overloaded
    // helpers dispatch to the right one for each T at compile time.
    static inline float rampMax(float a, float b) { return std::max(a, b); }
    static inline simd::float_4 rampMax(simd::float_4 a, simd::float_4 b) { return simd::fmax(a, b); }
    static inline float rampExp(float x) { return std::exp(x); }
    static inline simd::float_4 rampExp(simd::float_4 x) { return simd::exp(x); }
    static inline float rampClamp(float x, float a, float b) { return clamp(x, a, b); }
    static inline simd::float_4 rampClamp(simd::float_4 x, simd::float_4 a, simd::float_4 b) { return simd::clamp(x, a, b); }

    // fastCoef/smoothCoef are 1-exp(-sampleTime/TAU) for the two FIXED taus
    // above — precomputed by the caller since they only depend on
    // sampleTime, not freq. Only the slowTau term below genuinely varies per
    // call (it tracks freq, which moves under FM).
    template <typename T>
    inline T shape(T& fast, T& slow, T& smooth, T phase, T freq,
                    float sampleTime, float fastCoef, float smoothCoef) {
        T target = simd::ifelse(phase < 0.5f, T(1.f), T(-1.f));
        fast += (target - fast) * fastCoef;

        T period = 1.f / rampMax(freq, T(1e-6f));
        T slowTau = SLOW_TAU_PERIODS * period;
        slow += (target - slow) * (1.f - rampExp(-sampleTime / slowTau));

        T blended = rampClamp((MIX * fast + (1.f - MIX) * slow) * MAKEUP_GAIN, T(-1.f), T(1.f));
        smooth += (blended - smooth) * smoothCoef;
        return smooth;
    }
}

// The low-frequency square modulator (Solar 42F drone voices 3/6's cross-mod
// source). Always monophonic: the real hardware has one LFO per module, not
// one per voice, so a single shared instance drives every poly channel's
// FM/AM and the (mono) LFO_OUTPUT — see LunarPapaSrapa.cpp. Pure DSP, no
// Module/param/light dependency. Stays scalar (not SIMD) — there's only ever
// one instance regardless of polyphony, so there's no per-channel factor to
// amortize by vectorizing it.
struct PapaSrapaModulator {
    // Named constant instead of std::pow(2.f, 30.f) — 2^30 is exactly
    // representable in float, and a compile-time literal removes any
    // dependency on the compiler actually folding a runtime powf call.
    static constexpr float TWO_POW_30 = 1073741824.f;

    float phase = 0.f;
    float fast = 0.f, slow = 0.f, smooth = 0.f;
    float lfoOut = 0.f; // last square sample in [-1, 1], tapped by LFO_OUTPUT

    float cachedSampleTime = -1.f;
    float fastCoef = 0.f;
    float smoothCoef = 0.f;

    // rateOctaves: modulator frequency (octaves rel. 1 Hz, same convention as
    // LunarLFO::octavesToHz). dividerAmount: 0..1, divides the frequency
    // down (1..8) before it reaches the FM/AM stage — richer, slower beating
    // at higher settings. Returns the modulator square sample in [-1, 1].
    float process(float sampleTime, float rateOctaves, float dividerAmount) {
        if (sampleTime != cachedSampleTime) {
            cachedSampleTime = sampleTime;
            fastCoef = 1.f - std::exp(-sampleTime / PapaSrapaRamp::FAST_TAU);
            smoothCoef = 1.f - std::exp(-sampleTime / PapaSrapaRamp::SMOOTH_TAU);
        }

        float modFreq = dsp::approxExp2_taylor5(rateOctaves + 30.f) / TWO_POW_30;
        float divider = 1.f + dividerAmount * 7.f; // 1..8
        modFreq /= divider;

        phase += modFreq * sampleTime;
        phase -= std::floor(phase);
        lfoOut = PapaSrapaRamp::shape(fast, slow, smooth, phase, modFreq, sampleTime, fastCoef, smoothCoef);
        return lfoOut;
    }
};

// One "Papa Srapa" audio voice (Solar 42F drone voices 3/6): a square audio
// oscillator (roughly C0..E7) cross-modulated (FM and/or AM) by an
// externally supplied modulator signal (see PapaSrapaModulator — shared
// across all poly channels, not per-voice). The white-noise mix is likewise
// a single shared source (see LunarPapaSrapa.cpp) rather than one generator
// per voice — a single noise source is enough even with the main VCO
// polyphonic. Not a literal circuit emulation of the real coupled
// Trigger-Schmidt oscillators (undocumented, chaotic) — an idiomatic
// approximation in the spirit of DroneVoice/LunarVCOCore/TriSquareLFO, tuned
// by ear against the real hardware. Pure DSP, no Module/param/light
// dependency.
//
// Processes 4 polyphonic channels at once via rack::simd::float_4 (see
// LunarPapaSrapa.cpp, which batches poly channels in groups of 4 and calls
// one PapaSrapaCore instance per group). modMode/noiseOnly are non-poly
// params (same for every channel in a group), so their branches below are
// evaluated once per group of 4, not masked per lane. The only
// approximation this vectorization introduces beyond what the scalar code
// already had is simd::exp (a polynomial approximation, ~1e-6 relative
// error — same family as the approxExp2_taylor5 approximation already used
// below) in place of std::exp, inside PapaSrapaRamp::shape's slowTau term.
struct PapaSrapaCore {
    enum ModMode { MODE_OFF, MODE_FM, MODE_AM, MODE_FM_AM };

    // Named constant instead of std::pow(2.f, 30.f) — 2^30 is exactly
    // representable in float, and a compile-time literal removes any
    // dependency on the compiler actually folding a runtime powf call.
    static constexpr float TWO_POW_30 = 1073741824.f;

    simd::float_4 audioPhase = 0.f;
    simd::float_4 audioFast = 0.f, audioSlow = 0.f, audioSmooth = 0.f;

    // Cache for PapaSrapaRamp's two fixed-tau coefficients — recomputed only
    // when sampleTime changes, instead of every sample. Stay plain floats:
    // they only depend on sampleTime (module-wide), never on a per-channel
    // quantity.
    float cachedSampleTime = -1.f;
    float fastCoef = 0.f;
    float smoothCoef = 0.f;

    static float nextNoise(uint32_t& state) {
        // xorshift32: cheap, good enough for audio-rate white noise, no runtime pow/trig.
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float)(int32_t)state / 2147483648.f;
    }

    // pitchOctaves: audio oscillator base frequency (octaves rel. C4, same
    // idiom as LunarVCOCore/DroneVoice), per channel. modSquare: the shared
    // modulator's current output (see PapaSrapaModulator), used for FM/AM —
    // module-wide, same for every channel in this group. modDepth: 0..1,
    // FM/AM modulation depth — module-wide. modMode: which of FM/AM (or
    // both/neither) is active — module-wide. noiseSample: the shared
    // white-noise source's current sample (see LunarPapaSrapa.cpp), mixed
    // in at noiseAmount (0..1) — both module-wide. noiseOnly: bypasses the
    // square oscillator entirely, returning noiseSample directly at unity
    // gain for every lane — module-wide. The caller (LunarPapaSrapa.cpp)
    // already decides at the group level whether to call process() at all,
    // skipping it entirely when every lane in the group is silent — the
    // per-channel `computeAudio` early-return the scalar code used to have
    // is therefore the caller's job now, not this struct's.
    // Returns the mixed output in roughly [-1, 1], one lane per channel.
    simd::float_4 process(float sampleTime, float sampleRate, simd::float_4 pitchOctaves,
                           float modSquare, float modDepth, ModMode modMode,
                           float noiseSample, float noiseAmount, bool noiseOnly) {
        if (noiseOnly)
            return simd::float_4(noiseSample);

        if (sampleTime != cachedSampleTime) {
            cachedSampleTime = sampleTime;
            fastCoef = 1.f - std::exp(-sampleTime / PapaSrapaRamp::FAST_TAU);
            smoothCoef = 1.f - std::exp(-sampleTime / PapaSrapaRamp::SMOOTH_TAU);
        }

        bool fmOn = (modMode == MODE_FM || modMode == MODE_FM_AM);
        bool amOn = (modMode == MODE_AM || modMode == MODE_FM_AM);

        float fmOctaves = fmOn ? modSquare * modDepth * 2.f : 0.f; // up to +-2 octaves
        simd::float_4 audioFreq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitchOctaves + fmOctaves + 30.f) / TWO_POW_30;
        audioFreq = simd::clamp(audioFreq, 0.f, sampleRate / 2.f);

        audioPhase += audioFreq * sampleTime;
        audioPhase -= simd::floor(audioPhase);
        simd::float_4 audioSquare = PapaSrapaRamp::shape(audioFast, audioSlow, audioSmooth, audioPhase, audioFreq, sampleTime, fastCoef, smoothCoef);

        float amGain = amOn ? (0.5f + 0.5f * modDepth * modSquare) : 1.f;
        simd::float_4 out = audioSquare * amGain;
        out += noiseSample * noiseAmount;

        return out;
    }
};
