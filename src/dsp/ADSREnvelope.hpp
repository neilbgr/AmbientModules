#pragma once
#include <rack.hpp>

using namespace rack;

// Full ADSR envelope with Self-generation (once gated, loops Attack/Release
// as a pseudo-LFO instead of Attack/Decay/Sustain — still needs gate to be
// audible, and stops after its current release once gate drops). The loop
// always runs its 0..1 timing at full scale internally, so its period stays
// exactly Attack time + Release time regardless of Sustain; Sustain instead
// scales the returned value, acting as the pseudo-LFO's depth without
// touching its period. Any Hold button is the caller's responsibility to
// fold into `gate` before calling (see LunarVCO.cpp) — this struct only
// ever sees one combined gate signal. Pure DSP, no Module/param/light
// dependency.
struct ADSREnvelope {
    static constexpr float MIN_TIME = 0.001f;
    static constexpr float MAX_TIME = 15.f;
    static constexpr float LOG2_RATIO = 13.872675f; // log2(MAX_TIME / MIN_TIME)
    // Overshoot the real target so the exponential curve actually crosses it
    // in bounded time, instead of approaching it forever (same trick as
    // Fundamental's ADSR, which uses 1.01f for its attack target).
    static constexpr float TARGET_OVERSHOOT = 0.01f;

    enum Stage { STAGE_IDLE, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN, STAGE_RELEASE };

    float out = 0.f;
    Stage stage = STAGE_IDLE;
    bool prevGate = false;

    static float lambdaFromKnob(float knobValue) {
        float time = MIN_TIME * dsp::approxExp2_taylor5(LOG2_RATIO * knobValue + 30.f) / std::pow(2.f, 30.f);
        return 1.f / time;
    }

    // Returns the envelope value in [0, 1].
    float process(float sampleTime, bool gate, bool selfGenerate,
                   float attackKnob, float decayKnob, float sustainLevel, float releaseKnob) {
        bool risingEdge = gate && !prevGate;
        prevGate = gate;

        if (risingEdge) {
            stage = STAGE_ATTACK;
        } else if (!gate && stage != STAGE_IDLE && stage != STAGE_RELEASE) {
            stage = STAGE_RELEASE;
        } else if (gate && selfGenerate && stage != STAGE_ATTACK && stage != STAGE_RELEASE) {
            // Self-generation was (re-)enabled while already gated (e.g. toggled
            // off then on again mid-hold): jump back into the loop immediately
            // instead of staying stuck wherever it settled (idle/decay/sustain).
            stage = STAGE_ATTACK;
        }

        float target = 0.f;
        float lambda = 0.f;
        switch (stage) {
            case STAGE_ATTACK:  target = 1.f + TARGET_OVERSHOOT; lambda = lambdaFromKnob(attackKnob);  break;
            case STAGE_DECAY:   target = sustainLevel;            lambda = lambdaFromKnob(decayKnob);   break;
            case STAGE_SUSTAIN: target = sustainLevel;            lambda = 0.f;                          break;
            case STAGE_RELEASE: target = -TARGET_OVERSHOOT;       lambda = lambdaFromKnob(releaseKnob); break;
            default: break;
        }

        float y = out + (target - out) * lambda * sampleTime;
        out = (out == y) ? target : y;

        if (stage == STAGE_ATTACK && out >= 1.f) {
            stage = selfGenerate ? STAGE_RELEASE : STAGE_DECAY;
        } else if (stage == STAGE_DECAY && out <= sustainLevel) {
            stage = STAGE_SUSTAIN;
        } else if (stage == STAGE_RELEASE && out <= 0.f) {
            // Retrigger while still gated (covers both the self-gen loop and
            // a release that completed right as self-gen got turned off);
            // only truly stop once gate is gone.
            stage = gate ? STAGE_ATTACK : STAGE_IDLE;
        }

        // Self-generation scales the internal 0..1 sweep by Sustain after
        // the fact — depth without touching the timing computed above.
        return selfGenerate ? out * sustainLevel : out;
    }
};
