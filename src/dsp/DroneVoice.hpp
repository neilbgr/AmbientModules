#pragma once
#include <rack.hpp>
#include <cstring>
#include "Common.hpp"
#include "PolyBlep.hpp"

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
    // Hardware tone: the real voice is much darker than a pure saw. Measured
    // on Elta Music's demo video, with one oscillator alone and the filter
    // fully open, at 41, 100, 634 and 661 Hz. The roll-off is the same in Hz
    // at every pitch. The best fit is two real poles (a 2-pole low-pass with
    // Q 0.35), about 1.3 dB rms from the measured harmonics, against 6-8 dB
    // for the plain saw.
    static constexpr float TONE_POLE1_HZ = 1700.f;
    static constexpr float TONE_POLE2_HZ = 10000.f;

    float phase[NUM_OSC] = {};
    float prevSaw[NUM_OSC] = {};
    // Last sample's phase step, for the LEDs (DroneLights).
    float lastDeltaPhase[NUM_OSC] = {};
    int fmTopology = 0; // 0 = average of active others, 1 = circular chain
    // true = hardware tone (the two poles above), false = the plain bright
    // saw of 2.5.1 and earlier.
    bool hardwareTone = true;
    float toneState1 = 0.f;
    float toneState2 = 0.f;
    float toneSampleRate = -1.f;
    float toneGain1 = 0.f;
    float toneGain2 = 0.f;

    // Gain of a TPT (trapezoidal, pre-warped) one-pole low-pass. The pole is
    // kept below 0.45 fs, so tan() stays finite at low sample rates.
    static float onePoleGain(float cutoffHz, float sampleRate) {
        float g = std::tan((float) M_PI * std::fmin(cutoffHz, 0.45f * sampleRate) / sampleRate);
        return g / (1.f + g);
    }

    static float onePole(float x, float& state, float gain) {
        float v = (x - state) * gain;
        float y = v + state;
        state = y + v;
        return y;
    }

    // Linear, so filtering the summed mix once is the same as filtering
    // each of the 5 oscillators.
    float tone(float mix, float sampleRate) {
        if (!hardwareTone) {
            // Track the input, so switching the tone on later starts
            // from the current level instead of a stale state (no click).
            toneState1 = mix;
            toneState2 = mix;
            return mix;
        }
        if (sampleRate != toneSampleRate) {
            toneSampleRate = sampleRate;
            toneGain1 = onePoleGain(TONE_POLE1_HZ, sampleRate);
            toneGain2 = onePoleGain(TONE_POLE2_HZ, sampleRate);
        }
        return onePole(onePole(mix, toneState1, toneGain1), toneState2, toneGain2);
    }

    // pitchParams: per-oscillator base pitch (octaves rel. C4).
    // active/mod: per-oscillator mute + "shared CV modulates this one" flags.
    // cv: shared pitch CV (already scaled by the attenuverter).
    // volt: VOLT knob+CV value, already clamped to 0..1 (0% = unchanged, 100% = full stroke).
    // Returns the summed sawtooth mix, after the tone (NOT yet divided by NUM_OSC or scaled to volts).
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

            float freq = dsp::FREQ_C4 * ambient::exp2Octaves(pitch);
            // Capped at maxOscillatorFreq() (20 kHz, or 0.45 fs below
            // 44.4 kHz): CV and FM can't push it up to Nyquist.
            freq = clamp(freq, 0.f, ambient::maxOscillatorFreq(sampleRate));

            // Phase keeps running even when inactive, so re-enabling an
            // oscillator doesn't cause an audible phase jump.
            float deltaPhase = freq * sampleTime;
            phase[i] += deltaPhase;
            phase[i] -= std::trunc(phase[i]);
            lastDeltaPhase[i] = deltaPhase;

            float sawValue = 2.f * (phase[i] - std::round(phase[i]));
            // The cross-FM source stays the naive saw: it only drives pitch,
            // never reaches the output directly, so keeping it unchanged
            // leaves the VOLT FM character exactly as tuned.
            newPrevSaw[i] = sawValue;

            if (active[i]) {
                // This saw's single hard edge (+1 -> -1) sits at phase == 0.5
                // (std::round flips there), not at the wrap — PolyBLEP-correct
                // it (band-limiting, see PolyBlep.hpp) so pitch sweeps don't
                // alias. Subtracted: it's a downward jump of 2.
                float edgeDistance = phase[i] + 0.5f;
                edgeDistance -= std::trunc(edgeDistance);
                mix += sawValue - polyBlep(edgeDistance, deltaPhase);
            }
        }
        for (int i = 0; i < NUM_OSC; i++) {
            prevSaw[i] = newPrevSaw[i];
        }

        return tone(mix, sampleRate);
    }
};

// The 5 oscillator LEDs of one DroneVoice, like the hardware's OSC STATUS
// LEDs: each follows its saw's ramp (0 right after the falling edge, 1 just
// before it: saw = 2 ramp - 1). A slow oscillator blinks with its ramp, an
// audio-rate one glows steadily. Display only: it never touches the
// voice, so the LED setting can't change the sound. Called every few
// samples (Lunar50Drone: 16), on channel 0.
struct DroneLights {
    static const int NUM_OSC = DroneVoice::NUM_OSC;
    // Smoothing (1/s, a symmetric one-pole, like an eye; Rack's
    // setBrightnessSmooth() lights up instantly, a peak follower that
    // wouldn't average).
    static constexpr float LAMBDA = 60.f;
    // Gain from x1 at SLOW_HZ and below (the LED shows the whole ramp) to
    // x2 at FAST_HZ and above (the steady 0.5 glow becomes full
    // brightness), log-interpolated between.
    static constexpr float SLOW_HZ = 2.f;
    static constexpr float FAST_HZ = 20.f;
    static constexpr float SPAN_OCTAVES = 3.321928f; // log2(FAST_HZ / SLOW_HZ)

    // The LEDs' own phases: copied from the voice while it runs, and run
    // on their own while it's skipped (envelope closed), cross-FM left out.
    float phase[NUM_OSC] = {};
    float level[NUM_OSC] = {};

    // Mitchell's log2 (exponent + linear mantissa, within 0.09 octave):
    // plenty for an LED gain; std::log2 here measured ~8 ns/sample, as
    // much as a fifth of the whole voice.
    static float cheapLog2(float x) {
        int32_t bits;
        std::memcpy(&bits, &x, sizeof(bits));
        return (float) bits * (1.f / 8388608.f) - 127.f;
    }

    // Integral of the ramp frac(x) from 0 to x.
    static float rampIntegral(float x) {
        float whole = std::floor(x);
        float r = x - whole;
        return 0.5f * (whole + r * r);
    }

    // The ramp averaged exactly over the phase travelled up to rampEnd, so
    // there's no aliasing whatever the pitch: an audio-rate one is 0.5.
    static float rampMean(float rampEnd, float travelled) {
        if (travelled < 1e-6f) {
            return rampEnd;
        }
        return (rampIntegral(rampEnd) - rampIntegral(rampEnd - travelled)) / travelled;
    }

    // samples: since the last call. voiceRan: the voice's process() ran
    // on its last sample, so its phases are current. Writes the 5
    // brightnesses (0..1) to brightness[].
    void update(const DroneVoice& voice, bool voiceRan, int samples, float sampleTime,
                const float pitchParams[NUM_OSC], const bool active[NUM_OSC], const bool mod[NUM_OSC],
                float cv, float volt, float brightness[NUM_OSC]) {
        float elapsed = samples * sampleTime;
        float coef = std::fmin(LAMBDA * elapsed, 1.f);
        float detuneOctaves = -DroneVoice::DETUNE_MAX_OCTAVES * clamp(volt * 2.f, 0.f, 1.f);
        for (int i = 0; i < NUM_OSC; i++) {
            float travelled;
            if (voiceRan) {
                phase[i] = voice.phase[i];
                travelled = voice.lastDeltaPhase[i] * samples;
            } else {
                float pitch = pitchParams[i] + (mod[i] ? cv : 0.f) + detuneOctaves;
                float freq = dsp::FREQ_C4 * ambient::exp2Octaves(pitch);
                freq = clamp(freq, 0.f, ambient::maxOscillatorFreq(1.f / sampleTime));
                travelled = freq * elapsed;
                phase[i] += travelled;
                phase[i] -= std::trunc(phase[i]);
            }
            float rampEnd = phase[i] + 0.5f;
            rampEnd -= std::trunc(rampEnd);
            level[i] = active[i] ? level[i] + (rampMean(rampEnd, travelled) - level[i]) * coef : 0.f;

            float freq = travelled / elapsed;
            float fast = clamp(cheapLog2(std::fmax(freq, 1e-3f) / SLOW_HZ) / SPAN_OCTAVES, 0.f, 1.f);
            brightness[i] = std::fmin((1.f + fast) * level[i], 1.f);
        }
    }
};
