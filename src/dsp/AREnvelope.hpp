#pragma once
#include <rack.hpp>

using namespace rack;

// Attack/Release envelope generator, independent of any Module — reusable
// as-is in other AmbientModules modules.
struct AREnvelope {
    static constexpr float MIN_TIME = 0.001f; // 1 ms
    static constexpr float MAX_TIME = 15.f;   // 15 s, slow drone-style swells
    static constexpr float LOG2_RATIO = 13.872675f; // log2(MAX_TIME / MIN_TIME)
    // Overshoot the real target so the exponential curve actually crosses it
    // in bounded time (proportional to the knob), instead of approaching it
    // forever (same trick as ADSREnvelope/Fundamental's ADSR).
    static constexpr float TARGET_OVERSHOOT = 0.01f;
    // Same +30/2^30 trick as DroneVoice's pitch-to-freq: keeps the exponent
    // argument non-negative for approxExp2_taylor5, cancelled out afterwards.
    // Named constant instead of std::pow(2.f, 30.f) — 2^30 is exactly
    // representable in float, and a compile-time literal removes any
    // dependency on the compiler actually folding a runtime powf call.
    static constexpr float TWO_POW_30 = 1073741824.f;

    float out = 0.f;

    // knobValue in [0,1] -> lambda (1/time) in exponential taper MIN_TIME..MAX_TIME.
    // Attack/release knobs are non-poly (same for every channel), so callers
    // should compute this once per process() call and pass the result to
    // process() below rather than recomputing it per channel.
    static float lambdaFromKnob(float knobValue) {
        float time = MIN_TIME * dsp::approxExp2_taylor5(LOG2_RATIO * knobValue + 30.f) / TWO_POW_30;
        return 1.f / time;
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
