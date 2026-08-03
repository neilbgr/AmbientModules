#pragma once
#include <rack.hpp>

using namespace rack;

// One "Papa Srapa" noise voice (Solar 42F drone voices 3/6): a low-frequency
// square modulator cross-modulating (FM and/or AM) a square audio oscillator
// (roughly C0..E7), plus an independent white-noise generator always mixed
// into the output. Not a literal circuit emulation of the real coupled
// Trigger-Schmidt oscillators (undocumented, chaotic) — an idiomatic
// approximation in the spirit of DroneVoice/LunarVCOCore/TriSquareLFO, tuned
// by ear against the real hardware. Pure DSP, no Module/param/light dependency.
struct PapaSrapaCore {
    enum ModMode { MODE_OFF, MODE_FM, MODE_AM, MODE_FM_AM };

    float modPhase = 0.f;
    float audioPhase = 0.f;
    float modFast = 0.f, modSlow = 0.f, modSmooth = 0.f;
    float audioFast = 0.f, audioSlow = 0.f, audioSmooth = 0.f;
    uint32_t noiseState = 0x1234567u; // xorshift32 state, must stay non-zero
    float noise = 0.f; // last white-noise sample in [-1, 1], tapped by the S&H
    float lfoOut = 0.f; // last modulator square sample in [-1, 1], tapped by LFO_OUTPUT

    // The real hardware's square oscillators don't hold a flat rail: each half
    // cycle is a decelerating RC-style ramp that only reaches a fraction of
    // full swing before the (relatively fast, but not instant) flip. Measured
    // off a captured real-hardware period (wave/PapaSrapaSquare.wav): the flip
    // itself takes ~3-4 samples at 48kHz (~70us) while the sustained ramp for
    // the rest of the half-cycle is ~40x slower — one time constant can't fit
    // both, hence the fast+slow blend below. RAMP_SLOW_TAU_PERIODS is in
    // multiples of the oscillator's own period (not a fixed time), so the
    // reached fraction per half-cycle — and the risk of smearing the
    // fundamental at high audio pitch — stays constant across the whole
    // pitch range. Values below are a starting point tuned by ear against
    // that recording, matching this file's existing "idiomatic approximation"
    // philosophy (see struct comment above).
    static constexpr float RAMP_FAST_TAU = 0.00007f;     // ~70us: fast flip
    static constexpr float RAMP_SLOW_TAU_PERIODS = 1.5f; // sustained ramp, in periods
    static constexpr float RAMP_MIX = 0.3f;              // weight of the fast component
    static constexpr float RAMP_MAKEUP_GAIN = 2.f;       // restores ~unity peak swing
    // fast and slow each individually curve smoothly, but their common target
    // still flips instantly, so the blended sum still has a sharp corner (a
    // discontinuous slope) right at each flip. A short extra lowpass stage
    // here stands in for the finite bandwidth any real output/buffer stage
    // would have, rounding that corner off — physically motivated, not just
    // decorative — without blunting the fast component's speed (its tau is
    // well under RAMP_FAST_TAU, so it only softens the kink itself).
    static constexpr float RAMP_SMOOTH_TAU = 0.00002f; // ~20us

    static float shapeRampedSquare(float& fast, float& slow, float& smooth, float phase, float freq, float sampleTime) {
        float target = (phase < 0.5f) ? 1.f : -1.f;
        fast += (target - fast) * (1.f - std::exp(-sampleTime / RAMP_FAST_TAU));

        float period = 1.f / std::max(freq, 1e-6f);
        float slowTau = RAMP_SLOW_TAU_PERIODS * period;
        slow += (target - slow) * (1.f - std::exp(-sampleTime / slowTau));

        float blended = clamp((RAMP_MIX * fast + (1.f - RAMP_MIX) * slow) * RAMP_MAKEUP_GAIN, -1.f, 1.f);
        smooth += (blended - smooth) * (1.f - std::exp(-sampleTime / RAMP_SMOOTH_TAU));
        return smooth;
    }

    static float nextNoise(uint32_t& state) {
        // xorshift32: cheap, good enough for audio-rate white noise, no runtime pow/trig.
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float)(int32_t)state / 2147483648.f;
    }

    // rateOctaves: LF modulator frequency (octaves, same convention as
    // LunarLFO::octavesToHz). dividerAmount: 0..1, divides the modulator's
    // frequency down (1..8) before it reaches the FM/AM stage — richer,
    // slower beating at higher settings.
    // pitchOctaves: audio oscillator base frequency (octaves rel. C4, same
    // idiom as LunarVCOCore/DroneVoice).
    // modDepth: 0..1, FM/AM modulation depth. modMode: which of FM/AM (or
    // both/neither) is active. noiseAmount: 0..1, independent white-noise mix.
    // noiseOnly: bypasses the square oscillator and modulator entirely,
    // returning the raw noise sample at unity gain — a deterministic stand-in
    // for the hardware's fiddly "pitch to zero, mod and divider to max"
    // clean-noise knob combination.
    // Returns the mixed output in roughly [-1, 1].
    float process(float sampleTime, float sampleRate,
                   float rateOctaves, float dividerAmount,
                   float pitchOctaves, float modDepth, ModMode modMode,
                   float noiseAmount, bool noiseOnly) {
        float modFreq = dsp::approxExp2_taylor5(rateOctaves + 30.f) / std::pow(2.f, 30.f);
        float divider = 1.f + dividerAmount * 7.f; // 1..8
        modFreq /= divider;

        modPhase += modFreq * sampleTime;
        modPhase -= std::floor(modPhase);
        float modSquare = shapeRampedSquare(modFast, modSlow, modSmooth, modPhase, modFreq, sampleTime);
        lfoOut = modSquare;

        noise = nextNoise(noiseState);
        if (noiseOnly)
            return noise;

        bool fmOn = (modMode == MODE_FM || modMode == MODE_FM_AM);
        bool amOn = (modMode == MODE_AM || modMode == MODE_FM_AM);

        float fmOctaves = fmOn ? modSquare * modDepth * 2.f : 0.f; // up to +-2 octaves
        float audioFreq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitchOctaves + fmOctaves + 30.f) / std::pow(2.f, 30.f);
        audioFreq = clamp(audioFreq, 0.f, sampleRate / 2.f);

        audioPhase += audioFreq * sampleTime;
        audioPhase -= std::floor(audioPhase);
        float audioSquare = shapeRampedSquare(audioFast, audioSlow, audioSmooth, audioPhase, audioFreq, sampleTime);

        float amGain = amOn ? (0.5f + 0.5f * modDepth * modSquare) : 1.f;
        float out = audioSquare * amGain;
        out += noise * noiseAmount;

        return out;
    }
};
