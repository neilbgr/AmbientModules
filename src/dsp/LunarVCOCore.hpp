#pragma once
#include <rack.hpp>
#include "PolyBlep.hpp"
#include "Common.hpp"
#include <dsp/minblep.hpp>

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
    // the period) — the minBLEP/BLAMP corrections in process() are still
    // what keep the edges from aliasing on high notes; this pass is layered
    // on top of the band-limited signal, not instead of it. No hardware
    // capture to calibrate against here — starting point, tune by ear.
    static constexpr float EDGE_SMOOTH_TAU_PERIODS = 0.01f; // ~1% of period

    // One-pole coefficient of that lowpass for this sample. freq may be
    // negative (through-zero linear FM, see process()) — only its magnitude
    // sets the period.
    static simd::float_4 edgeSmoothCoef(simd::float_4 freq, float sampleTime) {
        simd::float_4 period = 1.f / simd::fmax(simd::fabs(freq), simd::float_4(1e-6f));
        simd::float_4 tau = EDGE_SMOOTH_TAU_PERIODS * period;
        return 1.f - simd::exp(-sampleTime / tau);
    }

    // The sub, though, is a divide-by-two flip-flop on the hardware: its
    // edges are sharp whatever the pitch, rounded only by a fixed output
    // bandwidth. Measured on Elta's demo video (sub alone, nine notes from 46
    // to 184 Hz): the same mild roll-off for every note, relative to 0.9 kHz
    // about -1.6 dB at 4.5 kHz, -4.9 dB at 9 kHz and -8.5 dB at 13.7 kHz on
    // the cleanest four, i.e. a one-pole lowpass at about 6 kHz (-1.8 /
    // -5.0 / -7.8 dB). The main, measured the same way at 198 Hz, matches
    // EDGE_SMOOTH_TAU_PERIODS. See process() for how the sub's edges get
    // their own rounding.
    static constexpr float SUB_EDGE_BANDWIDTH_HZ = 6000.f;

    // Square's duty cycle and Saw morph's peak position for a shape value,
    // shared by naiveWave() and the edge scan in process(), which must agree
    // exactly on where the edges are. Square: the full 0..1, as on the
    // hardware, where the pulse vanishes at either end of the PW knob (the
    // Elta demo turns it fully clockwise to leave only the sub). It narrows
    // continuously to nothing: its two band-limited edges merge and cancel,
    // so there's no click on the way. 2.5.0 stopped at 2..98 % to keep the
    // pulse from vanishing.
    static simd::float_4 pulseDuty(simd::float_4 shape) {
        return simd::clamp(shape, 0.f, 1.f);
    }

    // What a vanished pulse would leave is DC: a constant +-1 (about 3.8 V
    // after saturation, thumping through the envelope VCA at every note,
    // and offsetting the sub inside fastTanh). So over the last
    // PULSE_DC_FADE of the duty range at either end, the pulse's average
    // (2 duty - 1) is progressively taken off, all of it at 0 / 1: the main
    // is then exactly 0 and the sub plays alone, centred. Between the fades
    // the square is untouched. A function of shape only (no edge), so it
    // needs no band-limiting of its own.
    static constexpr float PULSE_DC_FADE = 0.02f;
    static simd::float_4 pulseDcRemoval(simd::float_4 duty) {
        simd::float_4 e = simd::clamp(simd::fmax(duty - (1.f - PULSE_DC_FADE), PULSE_DC_FADE - duty) / PULSE_DC_FADE, 0.f, 1.f);
        return e * e * (3.f - 2.f * e) * (2.f * duty - 1.f);
    }
    static simd::float_4 morphPeak(simd::float_4 shape) {
        return simd::clamp(1.f - shape, 0.001f, 0.999f);
    }
    // Main phase where Sine to saw's offset saw drops (its own phase wraps).
    static constexpr float SINE_SAW_EDGE = 1.f - SINE_SAW_PHASE_OFFSET;

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
                // shape = pulse width here, see pulseDuty().
                simd::float_4 duty = pulseDuty(shape);
                return simd::ifelse(p < duty, simd::float_4(1.f), simd::float_4(-1.f)) - pulseDcRemoval(duty);
            }
            case WAVE_SAW_MORPH: {
                // shape 0 = rising saw, 0.5 = symmetric triangle, 1 = falling
                // ramp (inverted saw): move the single peak's position across
                // the cycle instead of crossfading saw with -saw (which
                // cancels to a flat zero line at shape=0.5) — peak-to-peak
                // stays exactly 2 for every shape value.
                simd::float_4 bp = morphPeak(shape);
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
                // The saw wraps at SINE_SAW_EDGE, compared directly (not
                // through p + offset rounding up to 1), like the edge scan.
                simd::float_4 sawPhase = simd::ifelse(p >= SINE_SAW_EDGE, p - SINE_SAW_EDGE, p + SINE_SAW_PHASE_OFFSET);
                simd::float_4 sine = simd::sin(TWO_PI * p);
                simd::float_4 sawOffset = 2.f * sawPhase - 1.f;
                return (1.f - shape) * sine + shape * sawOffset;
            }
            default:
                return 0.f;
        }
    }

    // PolyBLAMP residuals (4-point, linear phase) for the main waveform's
    // slope corners, in the unsaturated domain: they're added to the main
    // value before fastTanh, which is exact for these continuous waveforms
    // (Triangle and Saw morph have no hard edge, so shaping after the
    // correction bends nothing back into aliasing). A minBLAMP isn't an
    // option: integrating the minimum-phase step leaves its group delay as a
    // residual that never decays, so it can't be truncated. The other
    // waveforms have no corners; their hard edges go through the minBLEP.
    static simd::float_4 cornerCorrections(int waveform, simd::float_4 p, simd::float_4 dt, simd::float_4 shape) {
        switch (waveform) {
            case WAVE_TRIANGLE:
                // Two corners per cycle, slope -4 -> +4 at phase 0 and
                // +4 -> -4 at phase 0.5: slope change +-8 per unit phase.
                return 8.f * dt * (polyBlamp4Corner(p, simd::float_4(0.f), dt) - polyBlamp4Corner(p, simd::float_4(0.5f), dt));
            case WAVE_SAW_MORPH: {
                // Two corners per cycle: slope -2/(1-bp) -> 2/bp at phase 0,
                // and back at phase bp. Near shape 0/1, where one segment
                // gets only a few samples long, process() fades this
                // rendering out for a jump, see MORPH_JUMP_FULL_SAMPLES.
                simd::float_4 bp = morphPeak(shape);
                simd::float_4 slopeChange = 2.f / (bp * (1.f - bp));
                return slopeChange * dt * (polyBlamp4Corner(p, simd::float_4(0.f), dt) - polyBlamp4Corner(p, bp, dt));
            }
            default:
                return 0.f;
        }
    }

    // --- Saw morph near its saw / inverted-saw extremes -----------------
    // Close to shape 0 (or 1), one of the two ramps gets so steep that it
    // lasts only a couple of samples or less: to the ear it's a hard edge,
    // but the corner BLAMPs above treat it as two huge, almost cancelling
    // slope changes. What's left is the 4-point kernel's error, and fastTanh
    // then bends that near-edge (corrected before saturation) back into
    // aliasing. So when the short segment lasts under
    // MORPH_JUMP_FULL_SAMPLES, it's rendered as a plain jump at its middle
    // instead, band-limited by the minBLEP with the jump measured after
    // saturation, exactly like the inverted saw's edge; the two remaining
    // (gentle) corners keep the BLAMP. Between FULL and FADE samples, the two
    // renderings are crossfaded (on the saturated, band-limited signals, so
    // the blend itself stays band-limited), so sweeping pitch or shape never
    // switches timbre abruptly. Above FADE, Saw morph is exactly as before.
    // (A staircase of several smaller jumps spread over the segment, closer
    // to the ramp it replaces, measured worse: fastTanh's slope differs from
    // one step level to the next, which the BLAMP residual under the steps
    // turns into small un-band-limited errors.)
    static constexpr float MORPH_JUMP_FULL_SAMPLES = 2.f;
    static constexpr float MORPH_JUMP_FADE_SAMPLES = 4.f;

    // Weight of the jump rendering (0..1, per lane), see above. The short
    // segment is the falling one when bp >= 0.5 (shape <= 0.5), the rising
    // one otherwise. Faded out as the short segment approaches half a period
    // (only reachable within a few samples per period, i.e. right under the
    // frequency cap), so the side switch at shape 0.5 is never heard.
    static simd::float_4 morphJumpWeight(simd::float_4 bp, simd::float_4 absDt) {
        simd::float_4 shortLen = simd::fmin(bp, 1.f - bp);
        simd::float_4 samples = shortLen / simd::fmax(absDt, simd::float_4(1e-9f));
        simd::float_4 u = simd::clamp((MORPH_JUMP_FADE_SAMPLES - samples) / (MORPH_JUMP_FADE_SAMPLES - MORPH_JUMP_FULL_SAMPLES), 0.f, 1.f);
        simd::float_4 nearHalf = simd::clamp((0.4f - shortLen) * 10.f, 0.f, 1.f);
        return u * u * (3.f - 2.f * u) * nearHalf;
    }

    // Phase of the jump (middle of the short segment), and the level just
    // before it (just after it: the opposite).
    static simd::float_4 morphJumpEdge(simd::float_4 bp) {
        return simd::ifelse(bp >= 0.5f, 0.5f * (1.f + bp), 0.5f * bp);
    }
    static simd::float_4 morphJumpFrom(simd::float_4 bp) {
        return simd::ifelse(bp >= 0.5f, simd::float_4(1.f), simd::float_4(-1.f));
    }

    // Jump rendering of Saw morph (naive, right-continuous): the long ramp as
    // usual, the short one flat on either side of the jump. Which side the
    // phase is on is decided against morphJumpEdge() itself, the same edge
    // process() scans for crossings.
    static simd::float_4 morphJumpWave(simd::float_4 p, simd::float_4 bp) {
        simd::float_4 from = morphJumpFrom(bp);
        simd::float_4 flat = simd::ifelse(p >= morphJumpEdge(bp), -from, from);
        simd::float_4 fall = simd::ifelse(p < bp, -1.f + 2.f * p / bp, flat);
        simd::float_4 rise = simd::ifelse(p < bp, flat, 1.f - 2.f * (p - bp) / (1.f - bp));
        return simd::ifelse(bp >= 0.5f, fall, rise);
    }

    // The jump rendering's two remaining corners, between the long ramp
    // (slope +-2 / (1 - shortLen)) and the flat parts: same places and sign
    // as the ramp rendering's, see cornerCorrections().
    static simd::float_4 morphJumpCorners(simd::float_4 p, simd::float_4 dt, simd::float_4 bp) {
        simd::float_4 slopeChange = 2.f / (1.f - simd::fmin(bp, 1.f - bp));
        return slopeChange * dt * (polyBlamp4Corner(p, simd::float_4(0.f), dt) - polyBlamp4Corner(p, bp, dt));
    }

    // The sub is a divider of the main, as on the hardware (a flip-flop
    // toggled by the main's cycle reset): subCount counts the main's cycles
    // modulo the division (2, or 16 with Oct +3, since Oct +3 moves the main
    // only), and the sub is high for the first half of the count. So the
    // sub's edges always fall exactly on the main's cycle start, and the
    // main + sub mix (saturated together) sounds the same however and
    // whenever the sub is switched on; a free-running sub phase made it
    // depend on where the main happened to be at that moment.
    static simd::float_4 subLevel(simd::float_4 count, float division) {
        return simd::ifelse(count < 0.5f * division, simd::float_4(1.f), simd::float_4(-1.f));
    }
    static simd::float_4 wrapCount(simd::float_4 count, float division) {
        return count - division * simd::floor(count / division);
    }

    // Crossings of a moving edge by the phase within one segment of this
    // sample: the phase goes from pStart by pDelta while the edge goes from
    // eStart by eDelta, over the segment's time [t0, t0 + tLen] (fractions of
    // the sample interval). Works on r = phase - edge, so a moving edge (pulse
    // width under modulation) is located exactly, whichever of the two moves.
    // The naive waveforms are right-continuous (at the edge itself they
    // already have their right-hand value), so a phase landing exactly on
    // the edge has crossed it when moving up, but not yet when moving down:
    // upwards, start excluded and end included; downwards, start included
    // and end excluded. Each crossing is then counted exactly once, in the
    // sample where the naive value actually changes. Assumes at most one
    // crossing per segment (|phase step| <= 0.45, see maxOscillatorFreq).
    // Returns the crossing lanes' mask; tau = time of the crossing, dir = +1
    // when the phase passes the edge upwards (from its left side to its right
    // side), -1 downwards.
    static simd::float_4 crossing(simd::float_4 pStart, simd::float_4 pDelta, simd::float_4 eStart, simd::float_4 eDelta,
                                  simd::float_4 t0, simd::float_4 tLen, simd::float_4* tau, simd::float_4* dir) {
        simd::float_4 r = pStart - eStart;
        simd::float_4 dr = pDelta - eDelta;
        simd::float_4 up = dr > 0.f;
        simd::float_4 c = simd::ifelse(up, simd::floor(r) + 1.f, simd::floor(r));
        simd::float_4 crossed = simd::ifelse(up, c <= r + dr, c > r + dr) & (dr != 0.f) & (tLen > 0.f);
        *tau = t0 + tLen * (c - r) / simd::ifelse(dr != 0.f, dr, simd::float_4(1.f));
        *dir = simd::ifelse(up, simd::float_4(1.f), simd::float_4(-1.f));
        return crossed;
    }

    // Wraps a phase into [0, 1). A phase just below 0 (running backwards
    // under through-zero FM) rounds to exactly 1 after p - floor(p); kept
    // below 1, or the next sample's edge scan would see the phase-0 edge
    // again (it already counted it on the way down) and insert it twice.
    static simd::float_4 wrapPhase(simd::float_4 p) {
        p -= simd::floor(p);
        return simd::fmin(p, simd::float_4(0.99999994f));
    }

    // For an edge the naive wave switches at by comparing the phase with it
    // (phase >= edge), on lanes running the whole sample (no sync): whether
    // it was crossed, decided from that comparison itself, last sample vs
    // this one, instead of from crossing()'s arithmetic. When the phase lands
    // within rounding of the edge, the two can disagree, leaving a step with
    // no band-limiting (or a jump with no step); crossing() still gives the
    // timing. The phase can't run a whole cycle in one sample, so the edge
    // was crossed when the comparison changes without a wrap, or stays the
    // same across one (passed just before the wrap, or just after); the
    // comparison resets at the wrap, where these waves are continuous.
    // Updates crossed/dir/tau on the lanes in `whole`.
    static void naiveCrossing(simd::float_4 whole, simd::float_4 prevPhase, simd::float_4 phase, simd::float_4 ePrev,
                              simd::float_4 eNow, simd::float_4 dt, simd::float_4* crossed, simd::float_4* tau, simd::float_4* dir) {
        simd::float_4 forward = dt >= 0.f;
        simd::float_4 wrapped = simd::ifelse(forward, phase < prevPhase, phase > prevPhase);
        simd::float_4 isPast = phase >= eNow;
        simd::float_4 changed = (prevPhase >= ePrev) ^ isPast;
        simd::float_4 one = 1.f;
        simd::float_4 naiveDir = simd::ifelse(simd::ifelse(wrapped, forward, isPast), one, -one);
        *crossed = simd::ifelse(whole, simd::ifelse(wrapped, ~changed, changed), *crossed);
        *dir = simd::ifelse(whole, naiveDir, *dir);
        *tau = simd::ifelse(whole, simd::clamp(*tau, 0.f, 1.f), *tau);
    }

    simd::float_4 phase = 0.f;
    simd::float_4 subCount = 0.f; // see subLevel()
    float subDivision = 2.f;
    simd::float_4 prevDuty = 0.5f;
    simd::float_4 prevMorphBp = 0.5f; // Saw morph's peak position, previous sample
    bool dutyPrimed = false;
    simd::float_4 cornerSmooth = 0.f;
    // The sub's edges on their own, see SUB_EDGE_BANDWIDTH_HZ: the running
    // sum of their (saturated) jumps, band-limited by subBlep, and that
    // stream through the main's lowpass and through the sub's.
    dsp::MinBlepGenerator<16, 16, simd::float_4> subBlep;
    simd::float_4 subSteps = 0.f;
    simd::float_4 subStepsMainSmooth = 0.f;
    simd::float_4 subStepsSubSmooth = 0.f;
    dsp::TSchmittTrigger<simd::float_4> syncTrigger;
    simd::float_4 prevSyncInput = 0.f;
    // Band-limits every hard edge of the (saturated, main + sub) output:
    // minimum-phase BLEP from the Rack SDK, 16 zero crossings x 16 oversampling,
    // same as Fundamental VCO. Causal, so an edge needs no look-ahead, and hard
    // sync works without delaying the output.
    dsp::MinBlepGenerator<16, 16, simd::float_4> minBlep;
    // The minimum-phase step lags the ideal one: the centroid of its impulse
    // response sits blepDelay samples after the edge (2.968 for this 16/16
    // table). Only the edges go through it, so a ramp and its own reset
    // would come out of step: a sawtooth would gain an offset of slope x
    // blepDelay, growing with frequency (up to ~1, i.e. 5 V, near 15 kHz).
    // Instead, the continuous part of the signal (naive signal minus its
    // jumps) is delayed by the same amount: the output is that continuous
    // part blepDelay samples ago, plus the jumps' own band-limited steps,
    // i.e. the naive signal BLEP_DELAY = 3 samples ago, plus every jump
    // inserted since then (they already happened, so that older naive
    // value doesn't have them yet), plus the minBLEP residual, moved
    // forward by the remaining fraction (3 - blepDelay = 0.032 sample) by
    // linear interpolation of the continuous part. Exact for any waveform,
    // sub, sync or PWM, for a 2.97-sample (62 us at 48 kHz) output latency.
    static constexpr int BLEP_DELAY = 3;
    float blepDelay = 0.f;
    float delayFraction = 0.f; // BLEP_DELAY - blepDelay
    simd::float_4 naiveHistory[BLEP_DELAY] = {};
    simd::float_4 jumpHistory[BLEP_DELAY] = {};
    int historyPos = 0;
    simd::float_4 jumpsThisSample = 0.f;

    LunarVCOCore() {
        // Step response sampled every 1/16 sample over 2 x 16 samples (the
        // table's last point is exactly 1): centroid = integral of
        // (1 - step), trapezoidal rule.
        const int n = 2 * 16 * 16;
        float sum = 0.f;
        for (int i = 0; i < n; i++) {
            sum += 0.5f * ((1.f - minBlep.impulse[i]) + (1.f - minBlep.impulse[i + 1]));
        }
        blepDelay = sum / 16.f;
        delayFraction = BLEP_DELAY - blepDelay;
    }

    // Inserts, for each lane in mask, a jump of the saturated output at time
    // tau (0..1 into this sample): from fastTanh(xBefore) to fastTanh(xAfter),
    // scaled by weight (Saw morph's crossfade between its two renderings).
    void insertJump(simd::float_4 mask, simd::float_4 tau, simd::float_4 xBefore, simd::float_4 xAfter,
                    simd::float_4 weight = 1.f) {
        if (!simd::movemask(mask)) {
            return;
        }
        insertSaturatedJump(mask, tau, weight * (ambient::fastTanh(xAfter) - ambient::fastTanh(xBefore)));
    }

    // Same, with the jump of the saturated output already worked out (Saw
    // morph's two renderings blended into one jump: the minBLEP is linear).
    void insertSaturatedJump(simd::float_4 mask, simd::float_4 tau, simd::float_4 jump) {
        int bits = simd::movemask(mask);
        if (!bits) {
            return;
        }
        jumpsThisSample += mask & jump;
        for (int i = 0; i < 4; i++) {
            if (bits & (1 << i)) {
                simd::float_4 lane = simd::movemaskInverse<simd::float_4>(1 << i);
                // A crossing right at the previous sample (tau = 0, possible
                // downwards, see crossing()) goes just after it: the minBLEP
                // only takes -1 < p <= 0.
                minBlep.insertDiscontinuity(std::max(tau[i], 1e-6f) - 1.f, lane & jump);
            }
        }
    }

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

        simd::float_4 freq = dsp::FREQ_C4 * ambient::exp2Octaves(pitchOctaves + expFmOctaves);
        freq += linFmHz;
        // Oct +3 moves the main only: the sub (one octave below the main
        // without it) then divides the main by 16. Switching keeps the sub's
        // current level (count 0..1 <-> 0..15, high half to high half).
        float division = octaveOn ? 16.f : 2.f;
        if (octaveOn) {
            freq *= 8.f; // +3 octaves
        }
        if (division != subDivision) {
            subCount = simd::floor(subCount * division / subDivision);
            subDivision = division;
        }
        // Through-zero linear FM: a negative instantaneous frequency (deep
        // linear FM pulling below 0 Hz) runs the phase backwards instead of
        // being clipped at 0 Hz, which used to flatten the modulation and
        // add harmonics of its own — even to the sine. The sub, a divider of
        // the main, follows it (its count runs backwards too). Capped at
        // maxOscillatorFreq() either way, so CV can't push it up to Nyquist.
        float maxFreq = ambient::maxOscillatorFreq(sampleRate);
        freq = simd::clamp(freq, -maxFreq, maxFreq);
        simd::float_4 dt = freq * sampleTime;
        // BLAMP windows are as wide as the phase step, whichever way the
        // phase runs (the band-limiting kernel is symmetric in time, so
        // running the waveform backwards mirrors signal and correction
        // together).
        simd::float_4 absDt = simd::fabs(dt);

        simd::float_4 duty = pulseDuty(shape); // Square only
        // Saw morph: weight of its jump rendering (see
        // MORPH_JUMP_FULL_SAMPLES), 0 on every lane for any other waveform.
        simd::float_4 morphBp = morphPeak(shape);
        if (!dutyPrimed) {
            prevDuty = duty;
            prevMorphBp = morphBp;
            dutyPrimed = true;
        }
        simd::float_4 jumpW = 0.f;
        bool anyJump = false;
        if (waveform == WAVE_SAW_MORPH) {
            jumpW = morphJumpWeight(morphBp, absDt);
            anyJump = simd::movemask(jumpW > 0.f) != 0;
        }

        // Hard sync: the sync edge rarely lands exactly on a sample, so
        // locate it by linear interpolation of the sync input across the
        // trigger threshold (syncFrac = 0..1, fraction of this sample
        // interval elapsed when it crossed) and restart the phase from that
        // point instead of from 0. Lanes without sync get syncFrac = 1: one
        // segment covering the whole sample.
        simd::float_4 triggered = syncTrigger.process(syncInput, simd::float_4(-0.25f), simd::float_4(0.25f));
        bool anySync = simd::movemask(triggered) != 0;
        simd::float_4 syncFrac = 1.f;
        if (anySync) {
            syncFrac = simd::ifelse(triggered,
                simd::clamp((0.25f - prevSyncInput) / simd::fmax(syncInput - prevSyncInput, simd::float_4(1e-6f)), 0.f, 1.f),
                simd::float_4(1.f));
        }
        prevSyncInput = syncInput;

        simd::float_4 prevPhase = phase;
        simd::float_4 dDuty = duty - prevDuty;

        // New phases. A synced lane restarts at 0 at syncFrac and runs for
        // the rest of the sample.
        phase = wrapPhase(simd::ifelse(triggered, (1.f - syncFrac) * dt, prevPhase + dt));

        // The main's cycle resets this sample, which step the sub's count
        // (see subLevel()), in time order: the phase-0 crossing before the
        // sync (+1 forwards, -1 backwards), the sync restart itself (+1: it
        // resets the main's cycle like a wrap does), and the phase-0
        // crossing after it. Counted even with the sub off, so switching it
        // on always finds it in step with the main. mainBefore: the main's
        // phase just before each reset, for the sub's jump at that instant
        // (see the sub edges below).
        simd::float_4 subCountStart = subCount;
        simd::float_4 resetTau[3];
        simd::float_4 resetStep[3];
        simd::float_4 resetMainBefore[3];
        {
            simd::float_4 tau;
            simd::float_4 dir;
            simd::float_4 almostOne = 0.99999994f;
            simd::float_4 zero = 0.f;
            simd::float_4 wrap = crossing(prevPhase, dt * syncFrac, zero, zero, zero, syncFrac, &tau, &dir);
            resetTau[0] = tau;
            resetStep[0] = wrap & dir;
            resetMainBefore[0] = simd::ifelse(dir > 0.f, almostOne, zero);
            simd::float_4 cutPhase = prevPhase + syncFrac * dt;
            resetTau[1] = syncFrac;
            resetStep[1] = triggered & simd::float_4(1.f);
            resetMainBefore[1] = cutPhase - simd::floor(cutPhase);
            resetStep[2] = 0.f;
            resetTau[2] = 1.f;
            resetMainBefore[2] = zero;
            if (anySync) {
                simd::float_4 restLen = simd::ifelse(triggered, 1.f - syncFrac, zero);
                wrap = crossing(zero, dt * restLen, zero, zero, syncFrac, restLen, &tau, &dir);
                resetTau[2] = tau;
                resetStep[2] = wrap & dir;
                resetMainBefore[2] = simd::ifelse(dir > 0.f, almostOne, zero);
            }
        }
        subCount = wrapCount(subCountStart + resetStep[0] + resetStep[1] + resetStep[2], division);

        // Naive output in the saturated domain: the whole timbre (fastTanh
        // soft saturation of main, or of the main/sub average) is shaped
        // BEFORE band-limiting, so the hard edges below are corrected with
        // jumps measured on the saturated signal. Shaping after the
        // correction, as before, bent the band-limited edges back into
        // aliasing.
        simd::float_4 subValue = subOscOn ? subLevel(subCount, division) : simd::float_4(0.f);
        simd::float_4 mainValue = naiveWave(waveform, phase, shape) + cornerCorrections(waveform, phase, absDt, shape);
        simd::float_4 x = subOscOn ? 0.5f * (mainValue + subValue) : mainValue;
        simd::float_4 out = ambient::fastTanh(x);
        if (anyJump) {
            simd::float_4 jumpValue = morphJumpWave(phase, morphBp) + morphJumpCorners(phase, absDt, morphBp);
            simd::float_4 xJump = subOscOn ? 0.5f * (jumpValue + subValue) : jumpValue;
            out += jumpW * (ambient::fastTanh(xJump) - out);
        }

        // --- Hard edges -> minBLEP -------------------------------------
        // The sub's value at time tau into this sample: after every main
        // cycle reset up to tau, included. A main edge at the very instant
        // of a reset (Square's rise, Inverted saw's jump, the sync restart)
        // thus sees the sub after its own edge there, while the sub's jump
        // is measured on the main before its edge (see the sub edges below):
        // the two then add up to exactly the combined jump of the
        // saturated mix.
        auto subAt = [&](simd::float_4 tau) {
            if (!subOscOn) {
                return simd::float_4(0.f);
            }
            simd::float_4 count = subCountStart;
            for (int i = 0; i < 3; i++) {
                count += (resetTau[i] <= tau) & resetStep[i];
            }
            return subLevel(wrapCount(count, division), division);
        };
        auto combine = [&](simd::float_4 m, simd::float_4 sub) {
            return subOscOn ? 0.5f * (m + sub) : m;
        };

        // Main edges, over up to two segments: [0, syncFrac] from the
        // previous phase, then (synced lanes only) [syncFrac, 1] from 0.
        for (int seg = 0; seg < 2; seg++) {
            simd::float_4 t0 = (seg == 0) ? simd::float_4(0.f) : syncFrac;
            simd::float_4 tLen = (seg == 0) ? syncFrac : simd::ifelse(triggered, 1.f - syncFrac, simd::float_4(0.f));
            if (seg == 1 && !anySync) {
                break;
            }
            simd::float_4 pStart = (seg == 0) ? prevPhase : simd::float_4(0.f);
            simd::float_4 pDelta = dt * tLen;
            simd::float_4 tau;
            simd::float_4 dir;
            switch (waveform) {
                case WAVE_SQUARE: {
                    // Rise at phase 0 (-1 -> +1), fall at the moving duty
                    // (+1 -> -1).
                    simd::float_4 riseTau;
                    simd::float_4 riseDir;
                    simd::float_4 rise = crossing(pStart, pDelta, simd::float_4(0.f), simd::float_4(0.f), t0, tLen, &riseTau, &riseDir);
                    simd::float_4 dutyStart = prevDuty + dDuty * t0;
                    simd::float_4 fallTau;
                    simd::float_4 fallDir;
                    simd::float_4 fall = crossing(pStart, pDelta, dutyStart, dDuty * tLen, t0, tLen, &fallTau, &fallDir);

                    // The crossings above model the duty as moving linearly
                    // through the sample. When it jumps instead (noise into
                    // Shape CV), that model can count a fall edge the naive
                    // square never makes, or miss one. So on lanes running the
                    // whole sample (no sync), the inserted jumps must add up
                    // to the naive square's actual level change; where they
                    // don't, the sample gets a single jump between its two
                    // actual levels, mid-sample, instead.
                    // The levels either side of each edge carry the pulse's
                    // DC removal (see pulseDcRemoval()), as in naiveWave().
                    simd::float_4 dc = pulseDcRemoval(duty);
                    simd::float_4 consistent = simd::float_4::mask();
                    if (seg == 0) {
                        simd::float_4 levelPrev = simd::ifelse(prevPhase < prevDuty, simd::float_4(1.f), simd::float_4(-1.f));
                        simd::float_4 levelNow = simd::ifelse(phase < duty, simd::float_4(1.f), simd::float_4(-1.f));
                        simd::float_4 change = (rise & (2.f * riseDir)) - (fall & (2.f * fallDir));
                        consistent = triggered | (change == levelNow - levelPrev);
                        simd::float_4 fix = ~consistent & (levelNow != levelPrev);
                        simd::float_4 half = 0.5f;
                        simd::float_4 s = subAt(half);
                        insertJump(fix, half, combine(levelPrev - dc, s), combine(levelNow - dc, s));
                    }
                    simd::float_4 s = subAt(riseTau);
                    insertJump(rise & consistent, riseTau, combine(-riseDir - dc, s), combine(riseDir - dc, s));
                    s = subAt(fallTau);
                    insertJump(fall & consistent, fallTau, combine(fallDir - dc, s), combine(-fallDir - dc, s));
                    break;
                }
                case WAVE_INV_SAW: {
                    // Ramp falls from +1 to -1 over the cycle and jumps back
                    // up (-1 -> +1) at phase 0.
                    simd::float_4 edge = crossing(pStart, pDelta, simd::float_4(0.f), simd::float_4(0.f), t0, tLen, &tau, &dir);
                    simd::float_4 s = subAt(tau);
                    insertJump(edge, tau, combine(-dir, s), combine(dir, s));
                    break;
                }
                case WAVE_SINE_SAW_MORPH: {
                    // The offset saw drops (+1 -> -1, weighted by shape) where
                    // its own phase wraps, at main phase 1 - offset; the sine
                    // part is continuous there.
                    simd::float_4 e = SINE_SAW_EDGE;
                    simd::float_4 edge = crossing(pStart, pDelta, e, simd::float_4(0.f), t0, tLen, &tau, &dir);
                    if (seg == 0) {
                        naiveCrossing(~triggered, prevPhase, phase, e, e, dt, &edge, &tau, &dir);
                    }
                    simd::float_4 sine = (1.f - shape) * simd::sin(TWO_PI * e);
                    simd::float_4 s = subAt(tau);
                    insertJump(edge, tau, combine(sine + dir * shape, s), combine(sine - dir * shape, s));
                    break;
                }
                case WAVE_SAW_MORPH: {
                    // Jump rendering only (see MORPH_JUMP_FULL_SAMPLES): its
                    // jump, moving with the peak position, weighted by jumpW.
                    if (!anyJump) {
                        break;
                    }
                    simd::float_4 ePrev = morphJumpEdge(prevMorphBp);
                    simd::float_4 eNow = morphJumpEdge(morphBp);
                    simd::float_4 eDelta = eNow - ePrev;
                    simd::float_4 edge = crossing(pStart, pDelta, ePrev + eDelta * t0, eDelta * tLen, t0, tLen, &tau, &dir);
                    if (seg == 0) {
                        naiveCrossing(~triggered, prevPhase, phase, ePrev, eNow, dt, &edge, &tau, &dir);
                    }
                    edge &= jumpW > 0.f;
                    if (!simd::movemask(edge)) {
                        break;
                    }
                    // The naive signal carries the corners' BLAMP residual on
                    // top of the jump: measure the jump with it, or the
                    // difference fastTanh makes to it is left as a small
                    // un-band-limited step.
                    simd::float_4 mp = pStart + (tau - t0) * dt;
                    simd::float_4 corner = morphJumpCorners(mp - simd::floor(mp), absDt, morphBp);
                    simd::float_4 from = dir * morphJumpFrom(morphBp);
                    simd::float_4 s = subAt(tau);
                    insertJump(edge, tau, combine(from + corner, s), combine(corner - from, s), jumpW);
                    break;
                }
                default:
                    break; // Sine, Triangle: continuous
            }
        }

        // The main value (corner residuals included, as in the naive output
        // above) at phase p, for the jumps measured on top of it: the usual
        // rendering, and Saw morph's jump rendering.
        auto rampMainAt = [&](simd::float_4 p) {
            return naiveWave(waveform, p, shape) + cornerCorrections(waveform, p, absDt, shape);
        };
        auto jumpMainAt = [&](simd::float_4 p) {
            return morphJumpWave(p, morphBp) + morphJumpCorners(p, absDt, morphBp);
        };
        // Jump of the saturated output from (main at pBefore, sub subBefore)
        // to (main at pAfter, sub subAfter), both renderings blended by jumpW.
        auto blendedJump = [&](simd::float_4 pBefore, simd::float_4 pAfter, simd::float_4 subBefore, simd::float_4 subAfter) {
            simd::float_4 jump = ambient::fastTanh(combine(rampMainAt(pAfter), subAfter)) - ambient::fastTanh(combine(rampMainAt(pBefore), subBefore));
            if (anyJump) {
                simd::float_4 alt = ambient::fastTanh(combine(jumpMainAt(pAfter), subAfter)) - ambient::fastTanh(combine(jumpMainAt(pBefore), subBefore));
                jump += jumpW * (alt - jump);
            }
            return jump;
        };

        // Sync reset: the main waveform jumps from where the cycle was cut to
        // its value at phase 0. Running backwards under through-zero FM, the
        // segment scan above then counts the edge at phase 0 itself right at
        // the restart (start included downwards, see crossing()).
        if (anySync) {
            simd::float_4 cutPhase = prevPhase + syncFrac * dt;
            cutPhase -= simd::floor(cutPhase);
            simd::float_4 s = subAt(syncFrac);
            simd::float_4 zero = 0.f;
            insertSaturatedJump(triggered, syncFrac, blendedJump(cutPhase, zero, s, s));
        }

        // Sub edges: wherever a main cycle reset changes the sub's level,
        // measured on the main just before that reset (see subAt()). Each
        // also goes into the sub's own edge stream (see SUB_EDGE_BANDWIDTH_HZ).
        if (subOscOn) {
            simd::float_4 count = subCountStart;
            for (int i = 0; i < 3; i++) {
                simd::float_4 before = subLevel(wrapCount(count, division), division);
                count += resetStep[i];
                simd::float_4 after = subLevel(wrapCount(count, division), division);
                simd::float_4 mask = (resetStep[i] != 0.f) & (before != after);
                if (!simd::movemask(mask)) {
                    continue;
                }
                simd::float_4 tau = resetTau[i];
                simd::float_4 jump = blendedJump(resetMainBefore[i], resetMainBefore[i], before, after);
                insertSaturatedJump(mask, tau, jump);
                subSteps += mask & jump;
                int bits = simd::movemask(mask);
                for (int l = 0; l < 4; l++) {
                    if (bits & (1 << l)) {
                        simd::float_4 lane = simd::movemaskInverse<simd::float_4>(1 << l);
                        subBlep.insertDiscontinuity(std::max(tau[l], 1e-6f) - 1.f, lane & jump);
                    }
                }
            }
        }
        prevDuty = duty;
        prevMorphBp = morphBp;

        // Delayed output, see BLEP_DELAY: naive signal from BLEP_DELAY
        // samples ago, plus the jumps inserted over the last BLEP_DELAY
        // sample intervals (this one included), plus the minBLEP residual.
        // historyPos holds sample n-3, the next slot n-2 (and the jumps of
        // the interval ending at n-2).
        int next = (historyPos + 1) % BLEP_DELAY;
        simd::float_4 delayed = naiveHistory[historyPos] + jumpsThisSample;
        for (int k = 1; k < BLEP_DELAY; k++) {
            delayed += jumpHistory[(historyPos + BLEP_DELAY - k) % BLEP_DELAY];
        }
        // The continuous part's step from n-3 to n-2, without that
        // interval's jumps, scaled by the fractional delay.
        delayed += delayFraction * (naiveHistory[next] - naiveHistory[historyPos] - jumpHistory[next]);
        naiveHistory[historyPos] = out;
        jumpHistory[historyPos] = jumpsThisSample;
        historyPos = (historyPos + 1) % BLEP_DELAY;
        jumpsThisSample = 0.f;
        out = delayed + minBlep.process();

        // Finite chip bandwidth (see EDGE_SMOOTH_TAU_PERIODS), on the
        // band-limited output, with the main's period. A pure sine without
        // sub isn't smoothed: it's fastTanh(sin), just delayed like
        // everything else (see BLEP_DELAY).
        simd::float_4 mainCoef = edgeSmoothCoef(freq, sampleTime);
        if (waveform != WAVE_SINE || subOscOn) {
            cornerSmooth += (out - cornerSmooth) * mainCoef;
            out = cornerSmooth;
        }
        else {
            cornerSmooth = out; // stay primed so switching waveform mid-play doesn't glitch
        }

        // The sub's edges get the sub's own rounding instead (see
        // SUB_EDGE_BANDWIDTH_HZ). The rounding is linear and comes after the
        // saturation, so it can be swapped for just those edges: add their
        // band-limited stream through the sub's lowpass, minus the same
        // stream through the main's, already in the output. Both lowpasses
        // have unity gain at DC, so that difference only ever holds the
        // edges' rounding and settles back to 0 between them; the stream's
        // own level (a running sum) never reaches the output. Run even
        // with the sub off, so a switch-off lets the last edges settle.
        simd::float_4 subCoef = 1.f - std::exp(-2.f * (float) M_PI * SUB_EDGE_BANDWIDTH_HZ * sampleTime);
        simd::float_4 subStream = subSteps + subBlep.process();
        subStepsMainSmooth += (subStream - subStepsMainSmooth) * mainCoef;
        subStepsSubSmooth += (subStream - subStepsSubSmooth) * subCoef;
        out += subStepsSubSmooth - subStepsMainSmooth;
        // Keep the running sum near 0 for precision: shifting it and both
        // lowpass states by the same amount changes nothing above.
        simd::float_4 shift = simd::ifelse(simd::fabs(subSteps) > 16.f, subSteps, simd::float_4(0.f));
        subSteps -= shift;
        subStepsMainSmooth -= shift;
        subStepsSubSmooth -= shift;
        return out;
    }
};
