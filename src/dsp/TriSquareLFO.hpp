#pragma once
#include <rack.hpp>

using namespace rack;

// Unipolar triangle/square LFO (output always in [0, 1]), matching the
// SOLAR 42F's LFO A/B design intent (positive voltage swing only). Pure DSP,
// no Module/param/light dependency — one instance per LFO.
struct TriSquareLFO {
    float phase = 0.f;

    // freqHz: LFO frequency in Hz.
    // waveAmount: 0 = pure triangle, 1 = pure square, 0.5 = equal blend.
    // Returns the LFO value in [0, 1].
    float process(float sampleTime, float freqHz, float waveAmount) {
        phase += freqHz * sampleTime;
        phase -= std::floor(phase);

        float square = (phase < 0.5f) ? 1.f : 0.f;
        float triangle = 1.f - std::fabs(2.f * phase - 1.f);

        return waveAmount * square + (1.f - waveAmount) * triangle;
    }
};
