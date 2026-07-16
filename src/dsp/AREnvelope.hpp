#pragma once
#include <rack.hpp>

using namespace rack;

// Attack/Release envelope generator, independent of any Module — reusable
// as-is in other AmbientModules modules.
struct AREnvelope {
    static constexpr float MIN_TIME = 0.001f; // 1 ms
    static constexpr float MAX_TIME = 15.f;   // 15 s, slow drone-style swells

    float out = 0.f;
    float attackLambda = 1.f / MIN_TIME;
    float releaseLambda = 1.f / MIN_TIME;
    dsp::ClockDivider coeffDivider;

    AREnvelope() {
        coeffDivider.setDivision(16);
    }

    static float lambdaFromKnob(float knobValue) {
        // knobValue in [0,1] -> time in seconds, exponential taper MIN_TIME..MAX_TIME
        float time = MIN_TIME * std::pow(MAX_TIME / MIN_TIME, knobValue);
        return 1.f / time;
    }

    // Call once per sample; only recomputes the lambdas (pow()) every
    // coeffDivider.getDivision() samples.
    void updateCoefficients(float attackKnob, float releaseKnob) {
        if (coeffDivider.process()) {
            attackLambda = lambdaFromKnob(attackKnob);
            releaseLambda = lambdaFromKnob(releaseKnob);
        }
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
