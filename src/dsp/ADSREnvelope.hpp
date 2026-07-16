#pragma once
#include <rack.hpp>

using namespace rack;

// Full ADSR envelope with Hold (forces output to 1, VCA always open) and
// Self-generation (free-running Attack/Release loop, ignores gate). Pure DSP,
// no Module/param/light dependency.
struct ADSREnvelope {
    static constexpr float MIN_TIME = 0.001f;
    static constexpr float MAX_TIME = 15.f;
    static constexpr float LOG2_RATIO = 13.872675f; // log2(MAX_TIME / MIN_TIME)

    enum Stage { STAGE_IDLE, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN, STAGE_RELEASE };

    float out = 0.f;
    Stage stage = STAGE_IDLE;
    bool prevGate = false;

    static float lambdaFromKnob(float knobValue) {
        float time = MIN_TIME * dsp::approxExp2_taylor5(LOG2_RATIO * knobValue + 30.f) / std::pow(2.f, 30.f);
        return 1.f / time;
    }

    // Returns the envelope value in [0, 1].
    float process(float sampleTime, bool gate, bool selfGenerate, bool hold,
                   float attackKnob, float decayKnob, float sustainLevel, float releaseKnob) {
        if (hold) {
            out = 1.f;
            return out;
        }

        bool risingEdge = gate && !prevGate;
        prevGate = gate;

        if (selfGenerate) {
            if (stage != STAGE_ATTACK && stage != STAGE_RELEASE) {
                stage = STAGE_ATTACK;
            }
        } else {
            if (risingEdge) {
                stage = STAGE_ATTACK;
            } else if (!gate && stage != STAGE_IDLE && stage != STAGE_RELEASE) {
                stage = STAGE_RELEASE;
            }
        }

        float target = 0.f;
        float lambda = 0.f;
        switch (stage) {
            case STAGE_ATTACK:  target = 1.f;          lambda = lambdaFromKnob(attackKnob);  break;
            case STAGE_DECAY:   target = sustainLevel; lambda = lambdaFromKnob(decayKnob);   break;
            case STAGE_SUSTAIN: target = sustainLevel; lambda = 0.f;                          break;
            case STAGE_RELEASE: target = 0.f;          lambda = lambdaFromKnob(releaseKnob); break;
            default: break;
        }

        float y = out + (target - out) * lambda * sampleTime;
        out = (out == y) ? target : y;

        if (stage == STAGE_ATTACK && out >= 1.f) {
            stage = selfGenerate ? STAGE_RELEASE : STAGE_DECAY;
        } else if (stage == STAGE_DECAY && out <= sustainLevel) {
            stage = STAGE_SUSTAIN;
        } else if (stage == STAGE_RELEASE && out <= 0.f) {
            stage = selfGenerate ? STAGE_ATTACK : STAGE_IDLE;
        }

        return out;
    }
};
