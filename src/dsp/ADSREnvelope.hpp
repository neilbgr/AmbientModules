#pragma once
#include <rack.hpp>
#include "Common.hpp"

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
    static constexpr float MIN_TIME = ambient::ENV_MIN_TIME;
    static constexpr float MAX_TIME = ambient::ENV_MAX_TIME;
    // Overshoot the real target so the exponential curve actually crosses it
    // in bounded time, instead of approaching it forever (same trick as
    // Fundamental's ADSR, which uses 1.01f for its attack target).
    static constexpr float TARGET_OVERSHOOT = 0.01f;

    enum Stage { STAGE_IDLE, STAGE_ATTACK, STAGE_DECAY, STAGE_SUSTAIN, STAGE_RELEASE };

    float out = 0.f;
    Stage stage = STAGE_IDLE;
    bool prevGate = false;

    // knobValue in [0,1] -> lambda (1/time). Attack/decay/release knobs are
    // non-poly (same for every channel), so callers should compute the 3
    // lambdas once per process() call and pass them to process() below
    // rather than recomputing per channel.
    // Same 1ms..15s taper as AREnvelope, see dsp/Common.hpp.
    static float lambdaFromKnob(float knobValue) {
        return ambient::envLambdaFromKnob(knobValue);
    }

    // Returns the envelope value in [0, 1].
    float process(float sampleTime, bool gate, bool selfGenerate,
                   float attackLambda, float decayLambda, float sustainLevel, float releaseLambda) {
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
            case STAGE_ATTACK:  target = 1.f + TARGET_OVERSHOOT; lambda = attackLambda;  break;
            case STAGE_DECAY:   target = sustainLevel;            lambda = decayLambda;   break;
            // Sustain keeps tracking the knob at the decay rate (was lambda
            // 0, which made the snap below jump straight to the new level):
            // moving Sustain while held now glides there instead of
            // stepping (an audible click on the enveloped VCA output).
            case STAGE_SUSTAIN: target = sustainLevel;            lambda = decayLambda;   break;
            case STAGE_RELEASE: target = -TARGET_OVERSHOOT;       lambda = releaseLambda; break;
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
