#pragma once
#include <rack.hpp>
#include "PolyBlep.hpp"

using namespace rack;

// One "AS3340-style" VCO voice: 6 discrete waveforms (4 fixed + 2 morphing),
// tune/octave/sub, lin or exp secondary FM input, hard sync. Pure DSP, no
// Module/param/light dependency.
//
// Processes 4 polyphonic channels at once via rack::simd::float_4 (see
// LunarVCO.cpp, which batches poly channels in groups of 4 and calls one
// LunarVCOCore instance per group). waveform/octaveOn/subOscOn are non-poly
// params (same value for every channel in a group), so the waveform
// switch() below is evaluated once per group of 4 — not per lane — and
// needs no masking; only per-channel signals (pitch/FM CV, shape, sync,
// and the oscillator's own running state) are actually float_4. The only
// approximations this vectorization introduces beyond what the scalar code
// already had are simd::sin/simd::exp (polynomial approximations, ~1e-6
// relative error — same family as the approxExp2_taylor5 approximation
// already used below) in place of std::sin/std::exp.
struct LunarVCOCore {
    enum Waveform {
        WAVE_SINE, WAVE_TRIANGLE, WAVE_INV_SAW, WAVE_SQUARE,
        WAVE_SAW_MORPH,       // shape = blend saw..inverted saw
        WAVE_SINE_SAW_MORPH,  // shape = blend sine..saw (phase-offset, see SINE_SAW_PHASE_OFFSET)
        NUM_WAVEFORMS
    };

    static constexpr float SINE_SAW_PHASE_OFFSET = 0.33333f; // a third of a period; tuned by ear against hardware
    // Named constant instead of std::pow(2.f, 30.f) — 2^30 is exactly
    // representable in float, and a compile-time literal removes any
    // dependency on the compiler actually folding a runtime powf call.
    static constexpr float TWO_POW_30 = 1073741824.f;
    // Named constant instead of `2.f * M_PI` — M_PI is a double, and mixing
    // it directly with simd::float_4 arithmetic below would rely on an
    // implicit double->float->float_4 conversion; spelling it out as a
    // float constant avoids that entirely.
    static constexpr float TWO_PI = 6.283185307f;

    // A real chip VCO (this one styled after the AS3340) has finite output
    // bandwidth: edges/corners are rounded, but — unlike PapaSrapaCore's
    // sloppier relaxation oscillator — it still reaches full swing quickly,
    // so a single short lowpass (tau relative to the current period, so the
    // amount of rounding stays proportionally the same across the whole
    // pitch range) is enough for that character. This does NOT band-limit
    // the hard edges on its own at high pitch (tau shrinks right along with
    // the period) — PolyBLEP below is still what keeps square/inv-saw from
    // aliasing into harsh top-end harmonics on high notes; this pass is
    // layered on top of the BLEP-corrected signal, not instead of it. No
    // hardware capture to calibrate against here — starting point, tune by
    // ear.
    static constexpr float EDGE_SMOOTH_TAU_PERIODS = 0.01f; // ~1% of period

    static simd::float_4 smoothEdge(simd::float_4& state, simd::float_4 value, simd::float_4 freq, float sampleTime) {
        simd::float_4 period = 1.f / simd::fmax(freq, simd::float_4(1e-6f));
        simd::float_4 tau = EDGE_SMOOTH_TAU_PERIODS * period;
        state += (value - state) * (1.f - simd::exp(-sampleTime / tau));
        return state;
    }

    // Cheap Pade(3,2) rational approximation of tanh (no simd::tanh in the
    // Rack SDK, and real std::tanh would mean 4 scalar calls + repacking per
    // SIMD group, per sample) — unity gain near 0 so normal-amplitude signal
    // passes through unchanged, smoothly saturating toward +-1 beyond that.
    static simd::float_4 fastTanh(simd::float_4 x) {
        x = simd::clamp(x, -3.f, 3.f);
        return x * (27.f + x * x) / (27.f + 9.f * x * x);
    }

    simd::float_4 phase = 0.f;
    simd::float_4 subPhase = 0.f;
    simd::float_4 cornerSmooth = 0.f;
    simd::float_4 subSmooth = 0.f;
    dsp::TSchmittTrigger<simd::float_4> syncTrigger;

    // pitchOctaves: tune + 1V/oct input, already summed by the caller (per channel).
    // expFmOctaves: exponential secondary-CV contribution (added to pitch, per channel), 0 if in lin mode.
    // linFmHz: linear secondary-CV contribution (added directly in Hz, per channel), 0 if in exp mode.
    // waveform: 0..5 (see enum above), same for all 4 channels in this group.
    // shape: 0..1 (pulse width or morph blend, see above), per channel.
    // octaveOn/subOscOn: same for all 4 channels in this group.
    // syncInput: raw voltage of the hard sync jack, per channel.
    // Returns the oscillator's dry output in roughly [-1, 1] (sub included), one lane per channel.
    simd::float_4 process(float sampleTime, float sampleRate, simd::float_4 pitchOctaves,
                           simd::float_4 expFmOctaves, simd::float_4 linFmHz, int waveform, simd::float_4 shape,
                           bool octaveOn, bool subOscOn, simd::float_4 syncInput) {

        simd::float_4 subFreq = 0.f;
        simd::float_4 freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitchOctaves + expFmOctaves + 30.f) / TWO_POW_30;
        freq += linFmHz;
        if (subOscOn) {
            subFreq = freq * 0.5f; // one octave below
        }
        if (octaveOn) {
            freq *= 8.f; // +3 octaves
        }
        freq = simd::clamp(freq, 0.f, sampleRate / 2.f);

        simd::float_4 triggered = syncTrigger.process(syncInput, simd::float_4(-0.25f), simd::float_4(0.25f));
        phase = simd::ifelse(triggered, simd::float_4::zero(), phase);

        simd::float_4 dt = freq * sampleTime;
        phase += dt;
        phase -= simd::floor(phase);

        simd::float_4 saw = 2.f * phase - 1.f;
        simd::float_4 out;
        switch (waveform) {
            case WAVE_SINE:
                out = simd::sin(TWO_PI * phase);
                break;
            case WAVE_TRIANGLE:
                out = simd::ifelse(phase < 0.5f, 4.f * phase - 1.f, 3.f - 4.f * phase);
                break;
            case WAVE_INV_SAW:
                // Ramp resets (falls back to +1) at phase == 0 — PolyBLEP-
                // correct that edge (band-limiting, matters most at high
                // pitch); the edge smoothing pass below adds the extra
                // rounded-corner character on top.
                out = -saw + polyBlep(phase, dt);
                break;
            case WAVE_SQUARE: {
                // shape = pulse width here; two hard edges per cycle (rise at
                // phase == 0, fall at phase == duty) — PolyBLEP-correct both
                // (band-limiting); the edge smoothing pass below adds the
                // extra rounded-corner character on top. Clamp away from
                // 0/1: at the extremes the duty cycle degenerates to
                // permanent silence (or DC), i.e. no pulse at all.
                simd::float_4 duty = simd::clamp(shape, 0.02f, 0.98f);
                out = simd::ifelse(phase < duty, simd::float_4(1.f), simd::float_4(-1.f));
                out += polyBlep(phase, dt);
                out -= polyBlep(simd::fmod(phase - duty + 1.f, simd::float_4(1.f)), dt);
                break;
            }
            case WAVE_SAW_MORPH: {
                // shape 0 = rising saw, 0.5 = symmetric triangle, 1 = falling
                // ramp (inverted saw): move the single peak's position across
                // the cycle instead of crossfading saw with -saw (which
                // cancels to a flat zero line at shape=0.5) — peak-to-peak
                // stays exactly 2 for every shape value.
                simd::float_4 bp = simd::clamp(1.f - shape, 0.001f, 0.999f);
                out = simd::ifelse(phase < bp,
                                    -1.f + 2.f * phase / bp,
                                    1.f - 2.f * (phase - bp) / (1.f - bp));
                break;
            }
            case WAVE_SINE_SAW_MORPH: {
                // Real hardware crossfades a sine core and a sawtooth core
                // that run out of phase with each other, not in-phase — an
                // in-phase linear mix of saw with its own inverse (same trick
                // WAVE_SAW_MORPH avoids above) would partially cancel toward
                // a flat line around shape=0.5; the phase offset is what
                // makes a genuinely linear blend viable here.
                simd::float_4 sawPhase = phase + SINE_SAW_PHASE_OFFSET;
                sawPhase -= simd::floor(sawPhase);
                simd::float_4 sine = simd::sin(TWO_PI * phase);
                simd::float_4 sawOffset = 2.f * sawPhase - 1.f;
                out = (1.f - shape) * sine + shape * sawOffset;
                break;
            }
            default:
                out = 0.f;
        }

        if (waveform != WAVE_SINE) {
            out = smoothEdge(cornerSmooth, out, freq, sampleTime);
        } else {
            cornerSmooth = out; // stay primed so switching waveform mid-play doesn't glitch
        }

        if (subOscOn) {
            simd::float_4 subDt = subFreq * sampleTime;
            subPhase += subDt;
            subPhase -= simd::floor(subPhase);
            simd::float_4 sub = simd::ifelse(subPhase < 0.5f, simd::float_4(1.f), simd::float_4(-1.f));
            sub += polyBlep(subPhase, subDt);
            sub -= polyBlep(simd::fmod(subPhase - 0.5f + 1.f, simd::float_4(1.f)), subDt);
            sub = smoothEdge(subSmooth, sub, subFreq, sampleTime);
            out += sub;
            out *= 0.5f; // keep overall level in check with the sub mixed in
        }

        // PolyBLEP correction near Square/Inv Saw edges (and the sub-osc sum
        // above) can transiently push out past +-1 — soft-clip rather than
        // let those spikes hit the module's x5V output stage undamped.
        return fastTanh(out);
    }
};
