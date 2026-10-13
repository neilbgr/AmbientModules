#pragma once
#include <rack.hpp>

using namespace rack;

// Small DSP helpers shared across the pack, previously copy-pasted into
// several headers/modules. Pure functions/constants, no state. Kept in their
// own namespace: Cardinal links every plugin into one binary, so generic
// free-function names at global scope could collide with another plugin's.
namespace ambient {

// Named constant instead of std::pow(2.f, 30.f) — 2^30 is exactly
// representable in float, and a compile-time literal removes any
// dependency on the compiler actually folding a runtime powf call.
constexpr float TWO_POW_30 = 1073741824.f;

// 2^octaves via approxExp2_taylor5 (<=6e-6 relative error).
//
// The +30 bias (cancelled by the division by 2^30) dates from an older
// Rack SDK whose approximation needed a non-negative argument; the current
// exp2Floor() already adds its own +127 bias, so the range is fine without
// it. It is still kept on purpose: removing it changes the result's last
// bits (measured: ~22% of inputs over -20..+20 octaves differ, up to 5.4e-6
// relative, ~0.01 cent) since x+30 rounds once more before the +127, and
// this refactor must stay bit-identical to the per-module copies it
// replaces.
template <typename T>
inline T exp2Octaves(T octaves) {
    return dsp::approxExp2_taylor5(octaves + 30.f) / TWO_POW_30;
}

// Cheap Pade(3,2) rational approximation of tanh — unity gain near 0 so
// normal-amplitude signal passes through unchanged, smoothly saturating to
// exactly +-1 at |x| >= 3. Max deviation from real tanh is 0.0235 (at
// |x| ~= 1.57): fine as a saturation *character* (VCO output stage, filter
// resonance limiter, distortion), not as a faithful tanh replacement.
// Scalar and SIMD overloads (rack::simd has no tanh of its own).
inline float fastTanh(float x) {
    x = clamp(x, -3.f, 3.f);
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}
inline simd::float_4 fastTanh(simd::float_4 x) {
    x = simd::clamp(x, -3.f, 3.f);
    return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

// Accurate drop-in for std::tanh where the exact curve matters (e.g. a
// pre-existing std::tanh whose sound must not change): Lambert's continued
// fraction truncated to a (7,6) rational, clamped at +-4.97 (where it
// reaches 1 - 6e-7). Max absolute error vs std::tanh: 9.6e-5 over the whole
// real line (-80 dB), versus 2.35e-2 for fastTanh above. ~8 mul + 1 div,
// several times cheaper than libm's tanhf.
inline float accurateTanh(float x) {
    x = clamp(x, -4.97f, 4.97f);
    float x2 = x * x;
    return x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)))
             / (135135.f + x2 * (62370.f + x2 * (3150.f + x2 * 28.f)));
}

// 1 - exp(-sampleTime / tau): per-sample coefficient of a one-pole
// lowpass/slew with time constant tau. Callers cache it (it only changes
// with sampleTime or tau), so plain std::exp is fine here.
inline float onePoleCoef(float sampleTime, float tau) {
    return 1.f - std::exp(-sampleTime / tau);
}

// Envelope knob taper shared by AREnvelope/ADSREnvelope/EnvelopeFollower:
// knobValue in [0,1] -> lambda (1/time), exponential taper
// ENV_MIN_TIME..ENV_MAX_TIME (1 ms .. 15 s).
constexpr float ENV_MIN_TIME = 0.001f;
constexpr float ENV_MAX_TIME = 15.f;
constexpr float ENV_LOG2_RATIO = 13.872675f; // log2(ENV_MAX_TIME / ENV_MIN_TIME)
inline float envLambdaFromKnob(float knobValue) {
    float time = ENV_MIN_TIME * exp2Octaves(ENV_LOG2_RATIO * knobValue);
    return 1.f / time;
}

// "CV Range" context-menu presets shared by LunarLFO and LunarSequencer
// (index = saved cvRangeIndex, so the order is part of the patch format).
constexpr int NUM_CV_RANGES = 4;
constexpr float CV_RANGES[NUM_CV_RANGES][2] = { {0.f, 5.f}, {0.f, 10.f}, {-5.f, 5.f}, {-10.f, 10.f} };

// Clamp an integer read from patch JSON into [0, count - 1], so a corrupt or
// hand-edited patch can never index past a table or select a mode that
// doesn't exist.
inline int clampIndex(json_int_t value, int count) {
    if (value < 0) {
        return 0;
    }
    if (value >= count) {
        return count - 1;
    }
    return (int)value;
}

} // namespace ambient
