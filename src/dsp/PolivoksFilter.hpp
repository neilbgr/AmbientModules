#pragma once
#include <rack.hpp>
#include "Common.hpp"

using namespace rack;

// Clean-room 2-pole multimode (LP/BP) filter emulating the SOLAR 42F's
// "POLIVOKS FILTER" block (manual p.21: "Double 12dB filter... you will not
// lose low frequencies when increasing resonance"). Derived independently
// from the classic Hal Chamberlin state-variable filter topology (public
// textbook math, e.g. as described in Zavalishin's "The Art of VA Filter
// Design") plus the real Polivoks VCF's circuit description (state-variable
// core, but with a hard-limited/diode-clipped resonance feedback path
// rather than a purely linear feedback gain — see Mutable Instruments'
// Shruthi-1 "Polivoks filter board" DIY archive for the reference circuit).
// No code from any existing filter implementation (Surge XT's K35,
// kocmoc's SVF/DIOD, etc.) was used — only published math/circuit
// descriptions.
//
// A state-variable filter's lowpass output is already unity gain at DC
// regardless of resonance (unlike a Moog-style ladder, which needs explicit
// gain compensation as feedback increases) — that alone accounts for most
// of the "no bass loss" behavior. The Polivoks' distinctive gritty/chaotic
// character under heavy resonance (and its ability to self-oscillate with a
// bounded, non-blowing-up amplitude) comes from that feedback path being
// nonlinearly limited rather than a clean linear gain; that's modeled here
// with a soft clip (ambient::fastTanh, same Pade(3,2) approximation used
// elsewhere in this pack, see dsp/Common.hpp) applied only to the
// copy of the bandpass state fed back into the resonance term — the
// integrator itself stays clean, same as a real diode clipper only limits
// the current fed back into the loop, not the capacitor's own charge.
//
// Cutoff ceiling: the explicit (non-zero-delay) Chamberlin recursion
//   low += f*band; band += f*(in - low - d*band)
// has the small-signal transfer function
//   H_LP(z) = f^2 z / ((z-1)(z-1+f*d) + f^2 z)
// whose poles leave the unit circle through z = -1 once f^2 + 2*f*d >= 4,
// i.e. it's only stable while f < sqrt(d^2+4) - d. Well before that limit,
// the pole heading for z = -1 already builds a parasitic resonance at
// Nyquist: |H_LP(-1)| = f^2 / (4 - 2*f*d - f^2), which exceeds the
// passband's own unity DC gain once f > (sqrt(d^2+8) - d) / 2. The old
// fixed sampleRate/6 clamp (f <= 1) sat beyond both limits whenever
// damping d > 1 (RES < ~0.5): self-oscillation at/near Nyquist for
// RES < ~0.25, a large Nyquist peak above that. f is therefore capped at
// that second, stricter bound — the Nyquist gain of the low-pass never
// exceeds its DC gain, which also leaves the near-Nyquist pole well damped
// (radius 0.68 at RES = 0) rather than a hair inside instability. For d <= 1
// (RES >= ~0.5, where the resonant character lives) the bound is >= 1, so
// the original sampleRate/6 cap still governs and nothing changes there.
// fastTanh(x)/x <= 1, so at large amplitude the damping term only gets
// weaker than its small-signal value d, and both bounds above grow as d
// shrinks: the small-signal (linear) bound is the worst case.
//
// Self-oscillation: the real Polivoks screams on its own with RES at max
// (its resonance is famously hard to tame), and so does the SOLAR 42F's.
// The last ~5% of the RES travel take d below 0 (SELF_OSC_DAMPING at RES 1;
// the explicit recursion's pole radius is sqrt(1 - f*d), so it starts
// exactly at d = 0, whatever the cutoff). Negative damping alone would grow
// without bound: the soft-clipped feedback saturates, so it keeps pushing
// energy in and nothing takes it out. The LIMITING*(band - fastTanh(band))
// term is negligible for small signals (cubic, so the onset and the
// small-signal response don't move) and grows linearly with amplitude, so
// the oscillation settles where the two balance: fading in from nothing at
// the onset, about +-1 at RES 1 (+-5V out of LunarFilter). The same term
// keeps loud, high-Q signals bounded below the onset too: without it, the
// saturated damping left them almost undamped (+-10V noise at RES 0.95 rang
// up to ~2200V); now they compress gently (a +-5V saw at RES 0.9 peaks at
// 11.5V instead of 13.8V).
struct PolivoksFilter {
    enum Mode { MODE_LP, MODE_BP };

    // Classic Chamberlin SVF's explicit (non-zero-delay) integrators are
    // only accurate/stable while the cutoff stays well below Nyquist —
    // clamping to sampleRate/6 keeps `f` (below) inside the range every
    // reference for this topology recommends, independently of whatever
    // clamps the caller already applies to the FREQ knob/CV upstream. On its
    // own this isn't enough at low resonance — see maxF() below.
    static constexpr float MAX_CUTOFF_RATIO = 1.f / 6.f;

    // Damping at RES 1. Damping falls linearly from 2 at RES 0, so it
    // crosses 0, where self-oscillation starts, at RES = 2/2.1 (~95%).
    static constexpr float SELF_OSC_DAMPING = -0.1f;
    // Linear loss the feedback's soft clip lets through at large amplitude
    // (see the header comment).
    static constexpr float LIMITING = 0.5f;
    // From exact silence nothing would ever start it: a real circuit's own
    // noise does that. This tiny kick (-80 dB) stands in for it.
    static constexpr float SELF_OSC_SEED = 1e-4f;

    float low = 0.f;
    float band = 0.f;

    // Coefficients are only recomputed when their inputs change (cutoff
    // usually comes from a knob, often static): with a CV patched they're
    // recomputed every sample, but that path is cheap (polynomial sin, no
    // libm call), so there's no throttling and no zipper on fast FM.
    float cachedCutoffHz = -1.f;
    float cachedSampleRate = -1.f;
    float cachedResonance = -1.f;
    float f = 0.f;
    float damping = 2.f;
    float fMax = 1.f;

    // Largest f for damping d: low-pass Nyquist gain <= DC gain (see the
    // header comment above), never above the sampleRate/6 cap's f = 1.
    static float maxF(float d) {
        return std::fmin(0.5f * (std::sqrt(d * d + 8.f) - d), 1.f);
    }

    // sin(x) for 0 <= x <= pi/6 (the cutoff clamp's whole range): Taylor
    // series to x^7, truncation error <= x^9/9! = 8e-9 there, i.e. below
    // float resolution — same result as std::sin, without the libm call.
    static float sinSmall(float x) {
        float x2 = x * x;
        return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f)));
    }

    void updateCoefficients(float sampleRate, float cutoffHz, float resonance) {
        if (resonance != cachedResonance) {
            cachedResonance = resonance;
            // resonance 0..1 -> damping from 2 (no resonance) down to
            // SELF_OSC_DAMPING (self-oscillating) — smaller damping means
            // more feedback.
            damping = rescale(resonance, 0.f, 1.f, 2.f, SELF_OSC_DAMPING);
            fMax = maxF(damping);
        }
        cachedCutoffHz = cutoffHz;
        cachedSampleRate = sampleRate;
        float fc = clamp(cutoffHz, 5.f, sampleRate * MAX_CUTOFF_RATIO);
        f = std::fmin(2.f * sinSmall((float)M_PI * fc / sampleRate), fMax);
    }

    // cutoffHz: filter cutoff (already includes any CV/knob combination).
    // resonance: 0..1, 0 = no resonance, top ~5% = self-oscillation.
    float process(float sampleRate, float input, float cutoffHz, float resonance, Mode mode) {
        if (cutoffHz != cachedCutoffHz || resonance != cachedResonance || sampleRate != cachedSampleRate) {
            updateCoefficients(sampleRate, cutoffHz, resonance);
        }

        if (damping < 0.f && band == 0.f && low == 0.f) {
            band = SELF_OSC_SEED;
        }
        float feedback = ambient::fastTanh(band);
        low += f * band;
        float high = input - low - damping * feedback - LIMITING * (band - feedback);
        band += f * high;

        // A single NaN/inf (e.g. from a NaN input voltage) would otherwise
        // stay in the integrators forever and silence the filter for good:
        // one cheap check on the sum catches either state going non-finite.
        if (!std::isfinite(low + band)) {
            low = 0.f;
            band = 0.f;
        }

        return (mode == MODE_LP) ? low : band;
    }
};
