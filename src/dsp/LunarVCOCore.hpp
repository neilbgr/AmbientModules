#pragma once
#include <rack.hpp>
#include "PolyBlep.hpp"

using namespace rack;

// One "AS3340-style" VCO voice: 6 discrete waveforms (4 fixed + 2 morphing),
// tune/octave/sub, lin or exp secondary FM input, hard sync. Pure DSP, no
// Module/param/light dependency.
struct LunarVCOCore {
    enum Waveform {
        WAVE_SINE, WAVE_TRIANGLE, WAVE_INV_SAW, WAVE_SQUARE,
        WAVE_SAW_MORPH,       // shape = blend saw..inverted saw
        WAVE_SINE_SAW_MORPH,  // shape = blend sine..saw (phase-offset, see SINE_SAW_PHASE_OFFSET)
        NUM_WAVEFORMS
    };

    static constexpr float SINE_SAW_PHASE_OFFSET = 0.33333f; // a third of a period; tuned by ear against hardware

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

    static float smoothEdge(float& state, float value, float freq, float sampleTime) {
        float period = 1.f / std::max(freq, 1e-6f);
        float tau = EDGE_SMOOTH_TAU_PERIODS * period;
        state += (value - state) * (1.f - std::exp(-sampleTime / tau));
        return state;
    }

    float phase = 0.f;
    float subPhase = 0.f;
    float cornerSmooth = 0.f;
    float subSmooth = 0.f;
    dsp::SchmittTrigger syncTrigger;

    // pitchOctaves: tune + 1V/oct input, already summed by the caller.
    // expFmOctaves: exponential secondary-CV contribution (added to pitch), 0 if in lin mode.
    // linFmHz: linear secondary-CV contribution (added directly in Hz), 0 if in exp mode.
    // waveform: 0..5 (see enum above). shape: 0..1 (pulse width or morph blend, see above).
    // octaveOn: multiplies frequency by 8 (+3 octaves) if true.
    // subOscOn: mixes an additional square wave one octave below into the output.
    // syncInput: raw voltage of the hard sync jack.
    // Returns the oscillator's dry output in roughly [-1, 1] (sub included).
    float process(float sampleTime, float sampleRate, float pitchOctaves,
                   float expFmOctaves, float linFmHz, int waveform, float shape,
                   bool octaveOn, bool subOscOn, float syncInput) {
        
        float subFreq = 0.f;
        float freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitchOctaves + expFmOctaves + 30.f) / std::pow(2.f, 30.f);
        freq += linFmHz;
        if (subOscOn) {
            subFreq = freq * 0.5f; // one octave below
        }
        if (octaveOn) {            
            freq *= 8.f; // +3 octaves
        }
        freq = clamp(freq, 0.f, sampleRate / 2.f);

        if (syncTrigger.process(syncInput, -0.25f, 0.25f)) {
            phase = 0.f;
        }

        float dt = freq * sampleTime;
        phase += dt;
        phase -= std::floor(phase);

        float saw = 2.f * phase - 1.f;
        float out;
        switch (waveform) {
            case WAVE_SINE:
                out = std::sin(2.f * M_PI * phase);
                break;
            case WAVE_TRIANGLE:
                out = (phase < 0.5f) ? (4.f * phase - 1.f) : (3.f - 4.f * phase);
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
                float duty = clamp(shape, 0.02f, 0.98f);
                out = (phase < duty) ? 1.f : -1.f;
                out += polyBlep(phase, dt);
                out -= polyBlep(std::fmod(phase - duty + 1.f, 1.f), dt);
                break;
            }
            case WAVE_SAW_MORPH: {
                // shape 0 = rising saw, 0.5 = symmetric triangle, 1 = falling
                // ramp (inverted saw): move the single peak's position across
                // the cycle instead of crossfading saw with -saw (which
                // cancels to a flat zero line at shape=0.5) — peak-to-peak
                // stays exactly 2 for every shape value.
                float bp = clamp(1.f - shape, 0.001f, 0.999f);
                out = (phase < bp) ? (-1.f + 2.f * phase / bp)
                                    : (1.f - 2.f * (phase - bp) / (1.f - bp));
                break;
            }
            case WAVE_SINE_SAW_MORPH: {
                // Real hardware crossfades a sine core and a sawtooth core
                // that run out of phase with each other, not in-phase — an
                // in-phase linear mix of saw with its own inverse (same trick
                // WAVE_SAW_MORPH avoids above) would partially cancel toward
                // a flat line around shape=0.5; the phase offset is what
                // makes a genuinely linear blend viable here.
                float sawPhase = phase + SINE_SAW_PHASE_OFFSET;
                sawPhase -= std::floor(sawPhase);
                float sine = std::sin(2.f * M_PI * phase);
                float sawOffset = 2.f * sawPhase - 1.f;
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
            float subDt = subFreq * sampleTime;
            subPhase += subDt;
            subPhase -= std::floor(subPhase);
            float sub = (subPhase < 0.5f) ? 1.f : -1.f;
            sub += polyBlep(subPhase, subDt);
            sub -= polyBlep(std::fmod(subPhase - 0.5f + 1.f, 1.f), subDt);
            sub = smoothEdge(subSmooth, sub, subFreq, sampleTime);
            out += sub;
            out *= 0.5f; // keep overall level in check with the sub mixed in
        }

        return out;
    }
};
