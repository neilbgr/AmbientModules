#pragma once
#include <rack.hpp>

using namespace rack;

// Attack/Release envelope generator, independent of any Module — reusable
// as-is in other AmbientModules modules.
struct AREnvelope {
    static constexpr float MIN_TIME = 0.001f; // 1 ms
    static constexpr float MAX_TIME = 15.f;   // 15 s, slow drone-style swells
    static constexpr float LOG2_RATIO = 13.872675f; // log2(MAX_TIME / MIN_TIME)

    float out = 0.f;
    float attackLambda = 1.f / MIN_TIME;
    float releaseLambda = 1.f / MIN_TIME;

    // Same +30/pow(2,30) trick as DroneVoice's pitch-to-freq: keeps the
    // exponent argument non-negative for approxExp2_taylor5, cancelled out
    // afterwards. std::pow(2.f, 30.f) has literal args so it's folded at
    // compile time — no runtime powf call, unlike a direct std::pow(ratio, knob).
    static float lambdaFromKnob(float knobValue) {
        // knobValue in [0,1] -> time in seconds, exponential taper MIN_TIME..MAX_TIME
        float time = MIN_TIME * dsp::approxExp2_taylor5(LOG2_RATIO * knobValue + 30.f) / std::pow(2.f, 30.f);
        return 1.f / time;
    }

    // Cheap enough to call every sample directly — no throttling needed.
    void updateCoefficients(float attackKnob, float releaseKnob) {
        attackLambda = lambdaFromKnob(attackKnob);
        releaseLambda = lambdaFromKnob(releaseKnob);
    }

    // Returns the current envelope value in [0, 1].
    float process(float sampleTime, bool gate) {
        float target = gate ? 1.f : 0.f;
        float lambda = (target > out) ? attackLambda : releaseLambda;
        float y = out + (target - out) * lambda * sampleTime;
        out = (out == y) ? target : y; // snap to avoid a floating-point stall
        return out;
    }
};
