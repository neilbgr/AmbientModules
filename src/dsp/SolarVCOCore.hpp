#pragma once
#include <rack.hpp>

using namespace rack;

// One "AS3340-style" VCO voice: 6 discrete waveforms (4 fixed + 2 morphing),
// tune/octave/sub, lin or exp secondary FM input, hard sync. Pure DSP, no
// Module/param/light dependency.
struct SolarVCOCore {
    enum Waveform {
        WAVE_SINE, WAVE_TRIANGLE, WAVE_INV_SAW, WAVE_SQUARE,
        WAVE_SAW_MORPH,       // shape = blend saw..inverted saw
        WAVE_SINE_TRI_MORPH,  // shape = blend sine..triangle
        NUM_WAVEFORMS
    };

    float phase = 0.f;
    float subPhase = 0.f;
    dsp::SchmittTrigger syncTrigger;

    // pitchOctaves: tune + octave switch + 1V/oct input, already summed by the caller.
    // expFmOctaves: exponential secondary-CV contribution (added to pitch), 0 if in lin mode.
    // linFmHz: linear secondary-CV contribution (added directly in Hz), 0 if in exp mode.
    // waveform: 0..5 (see enum above). shape: 0..1 (pulse width or morph blend, see above).
    // subOscOn: mixes an additional square wave one octave below into the output.
    // syncInput: raw voltage of the hard sync jack.
    // Returns the oscillator's dry output in roughly [-1, 1] (sub included).
    float process(float sampleTime, float sampleRate, float pitchOctaves,
                   float expFmOctaves, float linFmHz, int waveform, float shape,
                   bool subOscOn, float syncInput) {
        float freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitchOctaves + expFmOctaves + 30.f) / std::pow(2.f, 30.f);
        freq += linFmHz;
        freq = clamp(freq, 0.f, sampleRate / 2.f);

        if (syncTrigger.process(syncInput)) {
            phase = 0.f;
        }

        phase += freq * sampleTime;
        phase -= std::floor(phase);

        float saw = 2.f * phase - 1.f;
        float out;
        switch (waveform) {
            case WAVE_SINE:
                out = std::sin(2.f * M_PI * phase);
                break;
            case WAVE_TRIANGLE:
                out = (phase < 0.5f) ? (4.f * phase - 1.f) : (3.f - 4.f * phase);
                break;
            case WAVE_INV_SAW:
                out = -saw;
                break;
            case WAVE_SQUARE:
                out = (phase < shape) ? 1.f : -1.f; // shape = pulse width here
                break;
            case WAVE_SAW_MORPH:
                out = saw + shape * (-saw - saw); // lerp(saw, -saw, shape)
                break;
            case WAVE_SINE_TRI_MORPH: {
                float sine = std::sin(2.f * M_PI * phase);
                float tri = (phase < 0.5f) ? (4.f * phase - 1.f) : (3.f - 4.f * phase);
                out = sine + shape * (tri - sine);
                break;
            }
            default:
                out = 0.f;
        }

        if (subOscOn) {
            subPhase += 0.5f * freq * sampleTime; // one octave below
            subPhase -= std::floor(subPhase);
            out += (subPhase < 0.5f) ? 1.f : -1.f;
            out *= 0.5f; // keep overall level in check with the sub mixed in
        }

        return out;
    }
};
