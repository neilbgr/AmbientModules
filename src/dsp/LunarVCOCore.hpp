#pragma once
#include <rack.hpp>
#include "PolyBlep.hpp"
#include "Common.hpp"

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

    // freq may be negative (through-zero linear FM, see process()) — only
    // its magnitude sets the period.
    static simd::float_4 smoothEdge(simd::float_4& state, simd::float_4 value, simd::float_4 freq, float sampleTime) {
        simd::float_4 period = 1.f / simd::fmax(simd::fabs(freq), simd::float_4(1e-6f));
        simd::float_4 tau = EDGE_SMOOTH_TAU_PERIODS * period;
        state += (value - state) * (1.f - simd::exp(-sampleTime / tau));
        return state;
    }

    // Naive (un-corrected) waveform value at phase p. Split out from
    // process() so hard sync below can also evaluate the waveform at the
    // exact (fractional) phase it was cut at, not just at this sample's
    // phase.
    static simd::float_4 naiveWave(int waveform, simd::float_4 p, simd::float_4 shape) {
        switch (waveform) {
            case WAVE_SINE:
                return simd::sin(TWO_PI * p);
            case WAVE_TRIANGLE:
                return simd::ifelse(p < 0.5f, 4.f * p - 1.f, 3.f - 4.f * p);
            case WAVE_INV_SAW: {
                simd::float_4 saw = 2.f * p - 1.f;
                return -saw;
            }
            case WAVE_SQUARE: {
                // shape = pulse width here. Clamp away from 0/1: at the
                // extremes the duty cycle degenerates to permanent silence
                // (or DC), i.e. no pulse at all.
                simd::float_4 duty = simd::clamp(shape, 0.02f, 0.98f);
                return simd::ifelse(p < duty, simd::float_4(1.f), simd::float_4(-1.f));
            }
            case WAVE_SAW_MORPH: {
                // shape 0 = rising saw, 0.5 = symmetric triangle, 1 = falling
                // ramp (inverted saw): move the single peak's position across
                // the cycle instead of crossfading saw with -saw (which
                // cancels to a flat zero line at shape=0.5) — peak-to-peak
                // stays exactly 2 for every shape value.
                simd::float_4 bp = simd::clamp(1.f - shape, 0.001f, 0.999f);
                return simd::ifelse(p < bp,
                                    -1.f + 2.f * p / bp,
                                    1.f - 2.f * (p - bp) / (1.f - bp));
            }
            case WAVE_SINE_SAW_MORPH: {
                // Real hardware crossfades a sine core and a sawtooth core
                // that run out of phase with each other, not in-phase — an
                // in-phase linear mix of saw with its own inverse (same trick
                // WAVE_SAW_MORPH avoids above) would partially cancel toward
                // a flat line around shape=0.5; the phase offset is what
                // makes a genuinely linear blend viable here.
                simd::float_4 sawPhase = p + SINE_SAW_PHASE_OFFSET;
                sawPhase -= simd::floor(sawPhase);
                simd::float_4 sine = simd::sin(TWO_PI * p);
                simd::float_4 sawOffset = 2.f * sawPhase - 1.f;
                return (1.f - shape) * sine + shape * sawOffset;
            }
            default:
                return 0.f;
        }
    }

    // Adds the band-limiting residuals for every hard edge (PolyBLEP) and
    // corner (PolyBLAMP) of the waveform within one sample of phase p —
    // this is what keeps them from aliasing into harsh top-end harmonics on
    // high notes. Sine has neither, so it gets nothing.
    static simd::float_4 addCorrections(int waveform, simd::float_4 out, simd::float_4 p, simd::float_4 dt,
                                        simd::float_4 shape) {
        switch (waveform) {
            case WAVE_TRIANGLE:
                // Two corners per cycle, slope -4 -> +4 at phase 0 and
                // +4 -> -4 at phase 0.5: slope change +-8 per unit phase.
                out += 8.f * dt * (polyBlamp(p, dt) - polyBlamp(simd::fmod(p - 0.5f + 1.f, simd::float_4(1.f)), dt));
                break;
            case WAVE_INV_SAW:
                // Ramp resets (falls back to +1) at phase == 0.
                out += polyBlep(p, dt);
                break;
            case WAVE_SQUARE: {
                // Two hard edges per cycle: rise at phase == 0, fall at
                // phase == duty.
                simd::float_4 duty = simd::clamp(shape, 0.02f, 0.98f);
                out += polyBlep(p, dt);
                out -= polyBlep(simd::fmod(p - duty + 1.f, simd::float_4(1.f)), dt);
                break;
            }
            case WAVE_SAW_MORPH: {
                // Two corners per cycle: slope -2/(1-bp) -> 2/bp at phase 0,
                // and back at phase bp. Near shape 0/1 one segment gets
                // shorter than a sample and its two corners land within the
                // same BLAMP window — fine, see polyBlamp(): the two
                // (large, opposite) residuals then add up to the BLEP of the
                // near-vertical edge they enclose, so no separate BLEP
                // fallback is needed at the extremes.
                simd::float_4 bp = simd::clamp(1.f - shape, 0.001f, 0.999f);
                simd::float_4 slopeChange = 2.f / (bp * (1.f - bp));
                out += slopeChange * dt * (polyBlamp(p, dt) - polyBlamp(simd::fmod(p - bp + 1.f, simd::float_4(1.f)), dt));
                break;
            }
            case WAVE_SINE_SAW_MORPH: {
                // Only the saw component has an edge (falls back to -1 at
                // its own, offset phase wrap), weighted by its share of the
                // blend.
                simd::float_4 sawPhase = p + SINE_SAW_PHASE_OFFSET;
                sawPhase -= simd::floor(sawPhase);
                out -= shape * polyBlep(sawPhase, dt);
                break;
            }
            default:
                break;
        }
        return out;
    }

    simd::float_4 phase = 0.f;
    simd::float_4 subPhase = 0.f;
    simd::float_4 cornerSmooth = 0.f;
    simd::float_4 subSmooth = 0.f;
    dsp::TSchmittTrigger<simd::float_4> syncTrigger;
    simd::float_4 prevSyncInput = 0.f;
    simd::float_4 delayedOut = 0.f;

    // pitchOctaves: tune + 1V/oct input, already summed by the caller (per channel).
    // expFmOctaves: exponential secondary-CV contribution (added to pitch, per channel), 0 if in lin mode.
    // linFmHz: linear secondary-CV contribution (added directly in Hz, per channel), 0 if in exp mode.
    // waveform: 0..5 (see enum above), same for all 4 channels in this group.
    // shape: 0..1 (pulse width or morph blend, see above), per channel.
    // octaveOn/subOscOn: same for all 4 channels in this group.
    // syncInput: raw voltage of the hard sync jack, per channel.
    // syncConnected: whether the sync jack is patched at all (see the
    //   one-sample delay below).
    // Returns the oscillator's dry output in roughly [-1, 1] (sub included), one lane per channel.
    simd::float_4 process(float sampleTime, float sampleRate, simd::float_4 pitchOctaves,
                           simd::float_4 expFmOctaves, simd::float_4 linFmHz, int waveform, simd::float_4 shape,
                           bool octaveOn, bool subOscOn, simd::float_4 syncInput, bool syncConnected) {

        simd::float_4 subFreq = 0.f;
        simd::float_4 freq = dsp::FREQ_C4 * ambient::exp2Octaves(pitchOctaves + expFmOctaves);
        freq += linFmHz;
        if (subOscOn) {
            subFreq = freq * 0.5f; // one octave below
        }
        if (octaveOn) {
            freq *= 8.f; // +3 octaves
        }
        // Through-zero linear FM: a negative instantaneous frequency (deep
        // linear FM pulling below 0 Hz) runs the phase backwards instead of
        // being clipped at 0 Hz, which used to flatten the modulation and
        // add harmonics of its own — even to the sine. The sub follows the
        // same signed frequency.
        freq = simd::clamp(freq, -sampleRate / 2.f, sampleRate / 2.f);
        subFreq = simd::clamp(subFreq, -sampleRate / 2.f, sampleRate / 2.f);
        simd::float_4 dt = freq * sampleTime;
        // BLEP/BLAMP windows are as wide as the phase step, whichever way
        // the phase runs. The corrections themselves need no sign flip:
        // they only depend on where the edge/corner sits along the
        // waveform, and the band-limiting kernel is symmetric in time, so
        // running the same waveform backwards just mirrors both the naive
        // signal and its correction together.
        simd::float_4 absDt = simd::fabs(dt);

        // Hard sync, band-limited: the sync edge rarely lands exactly on a
        // sample, so locate it by linear interpolation of the sync input
        // across the trigger threshold (syncFrac = 0..1, fraction of this
        // sample interval elapsed when it crossed), restart the phase from
        // that point instead of from 0, and BLEP the jump the reset causes.
        // Only evaluated when some lane actually triggered this sample.
        simd::float_4 triggered = syncTrigger.process(syncInput, simd::float_4(-0.25f), simd::float_4(0.25f));
        bool anySync = simd::movemask(triggered) != 0;
        simd::float_4 syncFrac = 0.f;
        simd::float_4 syncJump = 0.f;
        simd::float_4 syncArrival = 1.f;
        if (anySync) {
            syncFrac = simd::clamp((0.25f - prevSyncInput) / simd::fmax(syncInput - prevSyncInput, simd::float_4(1e-6f)),
                                   0.f, 1.f);
            simd::float_4 cutPhase = phase + syncFrac * dt;
            cutPhase -= simd::floor(cutPhase);
            // Extra jump on top of the waveform's own phase-0 edge (if any,
            // already corrected by addCorrections() as if the cycle had
            // ended naturally): from where the cycle was cut, to where it
            // would have arrived (phase 1 — or phase 0 while running
            // backwards under through-zero FM).
            syncArrival = simd::ifelse(dt < 0.f, simd::float_4(0.f), simd::float_4(1.f));
            syncJump = simd::ifelse(triggered,
                naiveWave(waveform, syncArrival, shape) - naiveWave(waveform, cutPhase, shape), simd::float_4(0.f));
            phase = simd::ifelse(triggered, -syncFrac * dt, phase);
        }
        prevSyncInput = syncInput;

        phase += dt;
        phase -= simd::floor(phase);

        simd::float_4 out = naiveWave(waveform, phase, shape);
        out = addCorrections(waveform, out, phase, absDt, shape);
        if (anySync) {
            // Post-edge half of the sync BLEP (this sample is 1-syncFrac
            // samples past the edge).
            out -= 0.5f * syncJump * syncFrac * syncFrac;
        }

        // The pre-edge half of a sync BLEP belongs to the PREVIOUS sample,
        // which can't be known in advance (sync is an external signal) — so
        // while the sync jack is patched, the main oscillator runs one
        // sample late (~20us, inaudible) to leave room for it. Unpatched,
        // there is no delay and the output is exactly what it was before.
        if (syncConnected) {
            simd::float_4 delayed = delayedOut;
            if (anySync) {
                // The total jump (the cut, plus the waveform's own edges and
                // corners near phase 0 that it skipped to), pre-edge side:
                // syncFrac samples before the edge.
                simd::float_4 virtualPhase = syncArrival - syncFrac * dt;
                simd::float_4 before = 1.f - syncFrac;
                simd::float_4 pre = 0.5f * syncJump * before * before
                    + addCorrections(waveform, simd::float_4(0.f), virtualPhase, absDt, shape);
                delayed += simd::ifelse(triggered, pre, simd::float_4(0.f));
            }
            delayedOut = out;
            out = delayed;
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
            simd::float_4 subAbsDt = simd::fabs(subDt);
            sub += polyBlep(subPhase, subAbsDt);
            sub -= polyBlep(simd::fmod(subPhase - 0.5f + 1.f, simd::float_4(1.f)), subAbsDt);
            sub = smoothEdge(subSmooth, sub, subFreq, sampleTime);
            out += sub;
            out *= 0.5f; // keep overall level in check with the sub mixed in
        }

        // PolyBLEP correction near Square/Inv Saw edges (and the sub-osc sum
        // above) can transiently push out past +-1 — soft-clip rather than
        // let those spikes hit the module's x5V output stage undamped.
        // ambient::fastTanh: Pade(3,2), see dsp/Common.hpp (no simd::tanh in
        // the Rack SDK).
        return ambient::fastTanh(out);
    }
};
