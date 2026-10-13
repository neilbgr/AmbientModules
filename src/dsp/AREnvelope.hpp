#pragma once
#include <rack.hpp>
#include "Common.hpp"

using namespace rack;

// Attack/Release envelope generator, independent of any Module — reusable
// as-is in other AmbientModules modules.
struct AREnvelope {
    static constexpr float MIN_TIME = ambient::ENV_MIN_TIME; // 1 ms
    static constexpr float MAX_TIME = ambient::ENV_MAX_TIME; // 15 s, slow drone-style swells
    // Overshoot the real target so the exponential curve actually crosses it
    // in bounded time (proportional to the knob), instead of approaching it
    // forever (same trick as ADSREnvelope/Fundamental's ADSR).
    static constexpr float TARGET_OVERSHOOT = 0.01f;

    float out = 0.f;

    // knobValue in [0,1] -> lambda (1/time) in exponential taper MIN_TIME..MAX_TIME.
    // Attack/release knobs are non-poly (same for every channel), so callers
    // should compute this once per process() call and pass the result to
    // process() below rather than recomputing it per channel.
    // Shared with ADSREnvelope/EnvelopeFollower, see dsp/Common.hpp.
    static float lambdaFromKnob(float knobValue) {
        return ambient::envLambdaFromKnob(knobValue);
    }

    // Returns the current envelope value in [0, 1].
    float process(float sampleTime, bool gate, float attackLambda, float releaseLambda) {
        float target = gate ? (1.f + TARGET_OVERSHOOT) : -TARGET_OVERSHOOT;
        float lambda = (target > out) ? attackLambda : releaseLambda;
        float y = out + (target - out) * lambda * sampleTime;
        out = (out == y) ? target : y; // snap to avoid a floating-point stall
        // Clamping the state itself (not just the returned value) pins it at
        // an exact 0/1 once crossed, so a caller can test out==0.f cheaply to
        // detect real silence instead of chasing an asymptotic tail.
        out = clamp(out, 0.f, 1.f);
        return out;
    }
};
