#pragma once
#include <rack.hpp>

using namespace rack;

// One "Classic Solar 50" drone voice: 5 sawtooth oscillators with a shared
// VOLT control. Per the official doc: "transposes down all 5 voice
// generators at the same time. After half the stroke of the knob,
// generators start to modulate each other creating FM synthesis effect" —
// i.e. knob 0%..50% (volt 0..0.5) ramps all 5 oscillators from unchanged
// down to max detune; 50%..100% (volt 0.5..1) keeps that detune frozen at
// max while ramping in FM depth, so FM always applies on top of the
// already-detuned-down pitch, never on the original pitch. Pure DSP, no
// Module/param/light dependency — reusable as-is for multiple voice slots
// in a future multi-voice panel.
struct DroneVoice {
    static const int NUM_OSC = 5;
    static constexpr float DETUNE_MAX_OCTAVES = 2.f; // max detune, reached at volt = 0.5 (and held through 1)
    static constexpr float FM_DEPTH_OCTAVES = 1.f;   // max FM depth at volt = 1
    // Named constant instead of std::pow(2.f, 30.f) — 2^30 is exactly
    // representable in float, and a compile-time literal removes any
    // dependency on the compiler actually folding a runtime powf call.
    static constexpr float TWO_POW_30 = 1073741824.f;

    float phase[NUM_OSC] = {};
    float prevSaw[NUM_OSC] = {};
    int fmTopology = 0; // 0 = average of active others, 1 = circular chain

    // pitchParams: per-oscillator base pitch (octaves rel. C4).
    // active/mod: per-oscillator mute + "shared CV modulates this one" flags.
    // cv: shared pitch CV (already scaled by the attenuverter).
    // volt: VOLT knob+CV value, already clamped to 0..1 (0% = unchanged, 100% = full stroke).
    // Returns the raw summed sawtooth mix (NOT yet divided by NUM_OSC or scaled to volts).
    float process(float sampleTime, float sampleRate,
                   const float pitchParams[NUM_OSC],
                   const bool active[NUM_OSC], const bool mod[NUM_OSC],
                   float cv, float volt) {
        // 0%..50% (volt 0..0.5): ramp from unchanged (0) to max detune.
        // 50%..100% (volt 0.5..1): stay frozen at max detune (clamp holds it
        // at 1) while fmAmount ramps in below.
        float detuneOctaves = -DETUNE_MAX_OCTAVES * clamp(volt * 2.f, 0.f, 1.f);
        float fmAmount = clamp((volt - 0.5f) * 2.f, 0.f, 1.f);

        float mix = 0.f;
        float newPrevSaw[NUM_OSC];
        for (int i = 0; i < NUM_OSC; i++) {
            float fmSource = 0.f;
            if (fmAmount > 0.f) {
                if (fmTopology == 0) {
                    // Average of all other currently-active oscillators.
                    float sum = 0.f;
                    int count = 0;
                    for (int j = 0; j < NUM_OSC; j++) {
                        if (j != i && active[j]) {
                            sum += prevSaw[j];
                            count++;
                        }
                    }
                    if (count > 0) {
                        fmSource = sum / count;
                    }
                } else {
                    // Circular chain, skipping inactive oscillators.
                    int j = (i - 1 + NUM_OSC) % NUM_OSC;
                    int guard = 0;
                    while (!active[j] && j != i && guard < NUM_OSC) {
                        j = (j - 1 + NUM_OSC) % NUM_OSC;
                        guard++;
                    }
                    if (active[j] && j != i) {
                        fmSource = prevSaw[j];
                    }
                }
            }

            float pitch = pitchParams[i] + (mod[i] ? cv : 0.f)
                          + detuneOctaves + fmAmount * fmSource * FM_DEPTH_OCTAVES;

            float freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitch + 30.f) / TWO_POW_30;
            freq = clamp(freq, 0.f, sampleRate / 2.f);

            // Phase keeps running even when inactive, so re-enabling an
            // oscillator doesn't cause an audible phase jump.
            float deltaPhase = std::fmin(freq * sampleTime, 0.5f);
            phase[i] += deltaPhase;
            phase[i] -= std::trunc(phase[i]);

            float sawValue = 2.f * (phase[i] - std::round(phase[i])); // sawtooth naïf, pas d'anti-aliasing
            newPrevSaw[i] = sawValue;

            if (active[i]) {
                mix += sawValue;
            }
        }
        for (int i = 0; i < NUM_OSC; i++) {
            prevSaw[i] = newPrevSaw[i];
        }

        return mix;
    }
};
