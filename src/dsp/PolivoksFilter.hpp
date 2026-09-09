#pragma once
#include <rack.hpp>

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
// with a soft clip (fastTanh, same Pade(3,2) approximation already used
// elsewhere in this pack, e.g. LunarVCOCore::fastTanh) applied only to the
// copy of the bandpass state fed back into the resonance term — the
// integrator itself stays clean, same as a real diode clipper only limits
// the current fed back into the loop, not the capacitor's own charge.
struct PolivoksFilter {
    enum Mode { MODE_LP, MODE_BP };

    // Classic Chamberlin SVF's explicit (non-zero-delay) integrators are
    // only accurate/stable while the cutoff stays well below Nyquist —
    // clamping to sampleRate/6 keeps `f` (below) inside the range every
    // reference for this topology recommends, independently of whatever
    // clamps the caller already applies to the FREQ knob/CV upstream.
    static constexpr float MAX_CUTOFF_RATIO = 1.f / 6.f;

    float low = 0.f;
    float band = 0.f;

    static float fastTanh(float x) {
        x = clamp(x, -3.f, 3.f);
        return x * (27.f + x * x) / (27.f + 9.f * x * x);
    }

    // cutoffHz: filter cutoff (already includes any CV/knob combination).
    // resonance: 0..1, 0 = no resonance, 1 = near self-oscillation.
    float process(float sampleRate, float input, float cutoffHz, float resonance, Mode mode) {
        float fc = clamp(cutoffHz, 5.f, sampleRate * MAX_CUTOFF_RATIO);
        float f = 2.f * std::sin((float)M_PI * fc / sampleRate);
        // resonance 0..1 -> damping from 2 (no resonance) down to 0.02
        // (near self-oscillation) — smaller damping means more feedback.
        float damping = rescale(resonance, 0.f, 1.f, 2.f, 0.02f);

        float feedback = fastTanh(band);
        low += f * band;
        float high = input - low - damping * feedback;
        band += f * high;

        return (mode == MODE_LP) ? low : band;
    }
};
