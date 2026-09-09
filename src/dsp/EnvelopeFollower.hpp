#pragma once
#include <rack.hpp>
#include "AREnvelope.hpp"

using namespace rack;

// Envelope follower + gate detector, independent of any Module — reusable
// as-is in other AmbientModules modules. Reimplements the SOLAR 42F's
// "CONTACT MIC + ENVELOPE FOLLOWER" block (manual p.12): rectify an incoming
// audio signal, then apply the same asymmetric attack/release exponential
// slew as AREnvelope (reusing its lambdaFromKnob mapping directly rather
// than duplicating the math) to a continuous level instead of a binary
// gate. A Schmitt-trigger hysteresis on that level produces a persistent
// gate output.
struct EnvelopeFollower {
    // Same starting-point-tunable-by-ear approach as LunarVCOCore's edge
    // smoothing / PapaSrapaCore's amplitude falloff: no real hardware
    // capture to calibrate this specific block against, so these are a
    // sensible default hysteresis band, not a measured value.
    static constexpr float GATE_LOW = 0.02f;
    static constexpr float GATE_HIGH = 0.05f;

    float env = 0.f;
    dsp::TSchmittTrigger<float> gateTrigger;

    // Reuse AREnvelope's exponential-taper knob-to-lambda mapping (same
    // 1ms..15s range) so attack/release on this module behave consistently
    // with every other envelope in the pack.
    static float lambdaFromKnob(float knobValue) {
        return AREnvelope::lambdaFromKnob(knobValue);
    }

    // input: rectified/normalized audio level (this module rectifies
    // before calling process, so it can also drive the clipping check on
    // the pre-rectified sample if ever needed).
    // Returns the current envelope value in [0, 1].
    float process(float sampleTime, float input, float attackLambda, float releaseLambda) {
        float target = std::fabs(input);
        float lambda = (target > env) ? attackLambda : releaseLambda;
        env += (target - env) * lambda * sampleTime;
        env = clamp(env, 0.f, 1.f);
        return env;
    }

    // Persistent (not one-shot) gate state from a hysteresis band on the
    // envelope level — call after process() with its return value.
    bool processGate(float envValue) {
        gateTrigger.process(envValue, GATE_LOW, GATE_HIGH);
        return gateTrigger.isHigh();
    }
};
