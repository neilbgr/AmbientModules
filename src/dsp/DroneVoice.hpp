#pragma once
#include <rack.hpp>

using namespace rack;

// One "Classic Solar 50" drone voice: 5 sawtooth oscillators with a shared
// VOLT control (negative = detune all down together, positive = cross-
// modulate active oscillators). Pure DSP, no Module/param/light dependency —
// reusable as-is for multiple voice slots in a future multi-voice panel.
struct DroneVoice {
    static const int NUM_OSC = 5;
    static constexpr float DETUNE_MAX_OCTAVES = 2.f; // max detune at volt = -1
    static constexpr float FM_DEPTH_OCTAVES = 1.f;   // max FM depth at volt = +1

    float phase[NUM_OSC] = {};
    float prevSaw[NUM_OSC] = {};
    int fmTopology = 0; // 0 = average of active others, 1 = circular chain

    // pitchParams: per-oscillator base pitch (octaves rel. C4).
    // active/mod: per-oscillator mute + "shared CV modulates this one" flags.
    // cv: shared pitch CV (already scaled by the attenuverter).
    // volt: VOLT knob+CV value, already clamped to -1..1.
    // Returns the raw summed sawtooth mix (NOT yet divided by NUM_OSC or scaled to volts).
    float process(float sampleTime, float sampleRate,
                   const float pitchParams[NUM_OSC],
                   const bool active[NUM_OSC], const bool mod[NUM_OSC],
                   float cv, float volt) {
        float detuneOctaves = std::fmin(volt, 0.f) * DETUNE_MAX_OCTAVES;
        float fmAmount = std::fmax(volt, 0.f);

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

            float freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitch + 30.f) / std::pow(2.f, 30.f);
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
