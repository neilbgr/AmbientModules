// Host-side DSP unit tests: no Rack engine, no Module — only the pure-DSP
// headers under src/dsp plus Rack's own (header-only) math/simd/dsp
// helpers. Build + run with `make test` (see Makefile). Exit code is the
// number of failed checks.
#include <rack.hpp>
#include <cstdio>
#include <cmath>
#include <complex>

#include "dsp/Common.hpp"
#include "dsp/PolivoksFilter.hpp"
#include "dsp/ADSREnvelope.hpp"
#include "dsp/AREnvelope.hpp"
#include "dsp/PapaSrapaCore.hpp"

using namespace rack;

// rack.hpp's color constants call these at static-init time; the real
// definitions live in Rack's library (not linked here, nothing else needs it).
extern "C" NVGcolor nvgRGBA(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    NVGcolor c;
    c.r = r / 255.f;
    c.g = g / 255.f;
    c.b = b / 255.f;
    c.a = a / 255.f;
    return c;
}
extern "C" NVGcolor nvgRGB(unsigned char r, unsigned char g, unsigned char b) {
    return nvgRGBA(r, g, b, 255);
}

static int failures = 0;

static void check(bool ok, const char* name, const char* detailFmt = "", double a = 0.0, double b = 0.0) {
    if (!ok) {
        failures++;
    }
    std::printf("%s  %s", ok ? "PASS" : "FAIL", name);
    if (detailFmt[0]) {
        std::printf("  (");
        std::printf(detailFmt, a, b);
        std::printf(")");
    }
    std::printf("\n");
}

// Deterministic white noise in [-1, 1).
struct Noise {
    uint32_t s = 0x12345678u;
    float next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return (float)(int32_t)s / 2147483648.f;
    }
};

static const float SAMPLE_RATES[] = {44100.f, 48000.f, 96000.f};

// --- PolivoksFilter ---------------------------------------------------------

// Every RES/cutoff/mode/sample-rate combination must ring down once its
// input stops. Before the cutoff bound fix, RES < ~0.25 at the top of the
// cutoff range kept oscillating forever (tail peak ~0.05..1.08, flat).
// Pass = silent (< 1e-6) by 2.5 s after the input stops, or still clearly
// decaying (peak over [2, 2.5] s at most a tenth of the one over [1, 1.5] s)
// — the latter covers legitimately long rings, e.g. RES 1 at 50 Hz (Q 50,
// 0.32 s time constant), which a fixed threshold would flag.
static void testFilterStability() {
    // Up to just below the self-oscillation onset (see testFilterSelfOscillation).
    const float resonances[] = {0.f, 0.1f, 0.2f, 0.25f, 0.3f, 0.5f, 0.75f, 0.9f, 0.95f};
    // Last one: max FREQ knob (20 kHz) + max CV (+10 octaves), i.e. far
    // past every internal clamp.
    const float cutoffs[] = {50.f, 1000.f, 5000.f, 10000.f, 20000.f, 20000.f * 1024.f};
    bool allDecay = true;
    bool allFinite = true;
    float worstRatio = 0.f;
    for (float sr : SAMPLE_RATES) {
        for (float res : resonances) {
            for (float fc : cutoffs) {
                for (int mode = 0; mode < 2; mode++) {
                    PolivoksFilter filter;
                    Noise noise;
                    int nDrive = (int)(0.5f * sr);
                    int aStart = nDrive + (int)(1.f * sr), aEnd = nDrive + (int)(1.5f * sr);
                    int bStart = nDrive + (int)(2.f * sr), bEnd = nDrive + (int)(2.5f * sr);
                    float peakA = 0.f, peakB = 0.f;
                    for (int n = 0; n < bEnd; n++) {
                        float in = (n < nDrive) ? noise.next() : 0.f;
                        float y = filter.process(sr, in, fc, res, (PolivoksFilter::Mode)mode);
                        if (!std::isfinite(y)) {
                            allFinite = false;
                        }
                        if (n >= aStart && n < aEnd) {
                            peakA = std::fmax(peakA, std::fabs(y));
                        }
                        if (n >= bStart) {
                            peakB = std::fmax(peakB, std::fabs(y));
                        }
                    }
                    // RES 0.95 sits a hair below the onset (Q ~200): it still
                    // dies out, just slowly.
                    float maxRatio = (res >= 0.95f) ? 0.6f : 0.1f;
                    bool decays = (peakB < 1e-6f) || (peakB <= maxRatio * peakA);
                    if (peakB >= 1e-6f && res < 0.95f) {
                        worstRatio = std::fmax(worstRatio, peakB / peakA);
                    }
                    if (!decays) {
                        allDecay = false;
                        std::printf("      not decaying: sr %.0f RES %.2f cutoff %.0f %s peak %g -> %g\n",
                            sr, res, fc, mode ? "BP" : "LP", peakA, peakB);
                    }
                }
            }
        }
    }
    check(allFinite, "filter: output always finite (all sr/RES/cutoff/modes)");
    check(allDecay, "filter: rings down after input stops (all sr/RES/cutoff/modes)", "worst late/early tail ratio below RES 0.95: %g", worstRatio);
}

// Like the real Polivoks, the top of the RES travel self-oscillates: from
// complete silence (no input at all), the filter starts on its own, settles
// at a bounded, VCO-like level (~+-5V once LunarFilter scales it back up)
// and sings at the cutoff frequency, at every sample rate and cutoff,
// including far past every internal clamp.
static void testFilterSelfOscillation() {
    const float cutoffs[] = {50.f, 220.f, 1000.f, 5000.f, 20000.f * 1024.f};
    bool allStart = true, allFinite = true, allPitched = true;
    float minPeak = 1e9f, maxPeak = 0.f, worstPitch = 0.f;
    for (float sr : SAMPLE_RATES) {
        for (float fc : cutoffs) {
            for (int mode = 0; mode < 2; mode++) {
                PolivoksFilter filter;
                int n0 = (int)(2.f * sr), n1 = (int)(3.f * sr);
                float peak = 0.f, prev = 0.f;
                int crossings = 0;
                for (int n = 0; n < n1; n++) {
                    float y = filter.process(sr, 0.f, fc, 1.f, (PolivoksFilter::Mode)mode);
                    if (!std::isfinite(y)) {
                        allFinite = false;
                    }
                    if (n >= n0) {
                        peak = std::fmax(peak, std::fabs(y));
                        if (prev < 0.f && y >= 0.f) {
                            crossings++;
                        }
                    }
                    prev = y;
                }
                minPeak = std::fmin(minPeak, peak);
                maxPeak = std::fmax(maxPeak, peak);
                if (peak < 0.5f) {
                    allStart = false;
                    std::printf("      not oscillating: sr %.0f cutoff %.0f %s peak %g\n", sr, fc, mode ? "BP" : "LP", peak);
                }
                // Pitch: crossings over 1 s = frequency; cutoffs past the
                // sampleRate/6 clamp sing at that clamp.
                float expected = std::fmin(fc, sr / 6.f);
                float err = std::fabs(crossings / expected - 1.f);
                worstPitch = std::fmax(worstPitch, err);
                if (err > 0.03f) {
                    allPitched = false;
                    std::printf("      off pitch: sr %.0f cutoff %.0f %s %d Hz\n", sr, fc, mode ? "BP" : "LP", crossings);
                }
            }
        }
    }
    // Driven hard (+-10V noise), at every RES: bounded, within a few times
    // what RES 0 lets through (~2.9). Before the LIMITING term, the soft-clipped
    // feedback left loud high-Q signals almost undamped: 45 at RES 0.9, 442
    // at RES 0.95.
    const float drivenResonances[] = {0.f, 0.5f, 0.9f, 0.95f, 1.f};
    float drivenPeak = 0.f;
    for (float res : drivenResonances) {
        for (float sr : SAMPLE_RATES) {
            for (float fc : cutoffs) {
                for (int mode = 0; mode < 2; mode++) {
                    PolivoksFilter filter;
                    Noise noise;
                    for (int n = 0; n < (int)sr; n++) {
                        float y = filter.process(sr, 2.f * noise.next(), fc, res, (PolivoksFilter::Mode)mode);
                        if (!std::isfinite(y)) {
                            allFinite = false;
                        }
                        drivenPeak = std::fmax(drivenPeak, std::fabs(y));
                    }
                }
            }
        }
    }
    check(drivenPeak <= 8.f, "filter: driven hard, any RES stays bounded", "max peak %g", drivenPeak);
    check(allFinite, "filter: self-oscillation output always finite");
    check(allStart, "filter: RES 1 self-oscillates from silence", "min peak %g", minPeak);
    check(maxPeak <= 2.f, "filter: self-oscillation stays bounded", "max peak %g", maxPeak);
    check(allPitched, "filter: self-oscillation sings at the cutoff", "worst pitch error %.4f", worstPitch);
}

// Linear-model check of the cutoff bound itself: never past the stability
// limit, and low-pass Nyquist gain never above its unity DC gain.
static void testFilterBound() {
    double worstNyq = 0.0, worstStab = 0.0;
    for (int i = 0; i <= 1000; i++) {
        float res = i / 1000.f;
        PolivoksFilter filter;
        filter.updateCoefficients(48000.f, 1e9f, res);
        double f = filter.f, d = filter.damping;
        double nyq = f * f / (4.0 - 2.0 * f * d - f * f); // |H_LP(-1)|
        worstNyq = std::fmax(worstNyq, nyq);
        worstStab = std::fmax(worstStab, f / (std::sqrt(d * d + 4.0) - d));
    }
    check(worstStab < 0.9, "filter: f stays below the Chamberlin stability limit", "max f/fLimit %.4f", worstStab);
    check(worstNyq <= 1.0 + 1e-5, "filter: low-pass Nyquist gain <= DC gain at max cutoff", "max |H(-1)| %.6f", worstNyq);

    // High resonance never hits the new bound: f there is still exactly the
    // original sampleRate/6 cap (2*sin(pi/6) = 1).
    PolivoksFilter filter;
    filter.updateCoefficients(44100.f, 1e9f, 0.6f);
    check(std::fabs(filter.f - 1.f) < 1e-6f, "filter: RES >= 0.51 keeps the original sampleRate/6 ceiling", "f %.7f", filter.f);
}

// sinSmall() must match std::sin over the whole range the cutoff clamp
// allows (0..pi/6), so replacing the libm call changes nothing audible.
static void testFilterSin() {
    double worst = 0.0;
    for (int i = 1; i <= 100000; i++) {
        float x = (float)M_PI / 6.f * i / 100000.f;
        worst = std::fmax(worst, std::fabs((double)PolivoksFilter::sinSmall(x) / std::sin((double)x) - 1.0));
    }
    check(worst < 3e-7, "filter: polynomial sin == std::sin on [0, pi/6]", "max rel err %.3g", worst);
}

static void testFilterNan() {
    const float bad[] = {NAN, INFINITY, -INFINITY};
    bool ok = true;
    for (float b : bad) {
        PolivoksFilter filter;
        for (int n = 0; n < 1000; n++) {
            filter.process(48000.f, 0.5f, 1000.f, 0.3f, PolivoksFilter::MODE_LP);
        }
        filter.process(48000.f, b, 1000.f, 0.3f, PolivoksFilter::MODE_LP);
        float y = 0.f;
        for (int n = 0; n < 4800; n++) {
            y = filter.process(48000.f, 0.5f, 1000.f, 0.3f, PolivoksFilter::MODE_LP);
        }
        if (!(std::fabs(y - 0.5f) < 1e-3f)) {
            ok = false;
        }
    }
    check(ok, "filter: recovers from a single NaN/inf input sample");
}

// --- Envelopes --------------------------------------------------------------

static void testEnvelopes() {
    float st = 1.f / 48000.f;

    // AR: full attack then release reaches an exact 0 (AREnvelope clamps).
    {
        AREnvelope env;
        float a = AREnvelope::lambdaFromKnob(0.2f), r = AREnvelope::lambdaFromKnob(0.3f);
        for (int n = 0; n < 48000; n++) {
            env.process(st, true, a, r);
        }
        float peak = env.out, y = 1.f;
        for (int n = 0; n < 48000 * 2; n++) {
            y = env.process(st, false, a, r);
        }
        check(peak == 1.f && y == 0.f, "AR envelope: reaches exactly 1 then exactly 0", "peak %g, end %g", peak, y);
    }

    // ADSR: release reaches exactly 0 and goes idle, for every release time.
    {
        bool ok = true;
        float worst = 0.f;
        for (float relKnob : {0.f, 0.1f, 0.3f, 0.5f}) {
            ADSREnvelope env;
            float a = ADSREnvelope::lambdaFromKnob(0.1f), d = ADSREnvelope::lambdaFromKnob(0.2f);
            float r = ADSREnvelope::lambdaFromKnob(relKnob);
            for (int n = 0; n < 48000; n++) {
                env.process(st, true, false, a, d, 0.5f, r);
            }
            float y = 1.f;
            for (int n = 0; n < 48000 * 30; n++) {
                y = env.process(st, false, false, a, d, 0.5f, r);
                if (env.stage == ADSREnvelope::STAGE_IDLE) {
                    break;
                }
            }
            y = env.process(st, false, false, a, d, 0.5f, r);
            if (!(y == 0.f && env.stage == ADSREnvelope::STAGE_IDLE)) {
                ok = false;
                worst = y;
            }
        }
        check(ok, "ADSR envelope: release ends idle at exactly 0", "last value %g", worst);
    }

    // ADSR: changing Sustain while held glides (decay rate) instead of
    // jumping. Before the fix, the 0.8 -> 0.2 change below was a single
    // 0.6 step.
    {
        ADSREnvelope env;
        float a = ADSREnvelope::lambdaFromKnob(0.1f), d = ADSREnvelope::lambdaFromKnob(0.2f), r = ADSREnvelope::lambdaFromKnob(0.3f);
        float prev = 0.f, maxStep = 0.f, y = 0.f;
        for (int n = 0; n < 96000; n++) {
            float sustain = (n < 48000) ? 0.8f : 0.2f;
            y = env.process(st, true, false, a, d, sustain, r);
            if (n > 47000) {
                maxStep = std::fmax(maxStep, std::fabs(y - prev));
            }
            prev = y;
        }
        // Largest step a one-pole at the decay rate can take for a 0.6 jump.
        float bound = 0.6f * d * st * 1.0001f;
        check(maxStep <= bound, "ADSR envelope: sustain change glides at the decay rate", "max step %g (bound %g)", maxStep, bound);
        check(std::fabs(y - 0.2f) < 1e-6f, "ADSR envelope: settles on the new sustain level", "end %g", y);
    }
}

// --- Shared helpers ---------------------------------------------------------

static void testCommon() {
    // exp2Octaves must stay bit-identical to the expression it replaced in
    // 6 places (approxExp2_taylor5(x + 30) / 2^30).
    {
        int diff = 0;
        double worstRel = 0.0;
        for (int i = 0; i <= 400000; i++) {
            volatile float x = -20.f + 40.f * i / 400000.f;
            float a = ambient::exp2Octaves((float)x);
            float b = dsp::approxExp2_taylor5((float)x + 30.f) / 1073741824.f;
            if (a != b) {
                diff++;
            }
            worstRel = std::fmax(worstRel, std::fabs((double)a / std::pow(2.0, (double)x) - 1.0));
        }
        check(diff == 0, "exp2Octaves: bit-identical to the old inline expression", "%g mismatches", diff);
        check(worstRel < 6e-6, "exp2Octaves: within 6e-6 of 2^x", "max rel err %.3g", worstRel);
    }
    // Same for the envelope taper (old: MIN_TIME * exp2(...+30) / 2^30).
    {
        int diff = 0;
        for (int i = 0; i <= 10000; i++) {
            float k = i / 10000.f;
            float old = 1.f / (0.001f * dsp::approxExp2_taylor5(13.872675f * k + 30.f) / 1073741824.f);
            if (old != ambient::envLambdaFromKnob(k)) {
                diff++;
            }
        }
        check(diff == 0, "envLambdaFromKnob: bit-identical to the old per-envelope copies", "%g mismatches", diff);
    }
    // Drone's soft clip: accurateTanh within 1e-4 of std::tanh everywhere.
    {
        double worst = 0.0;
        for (int i = -1000000; i <= 1000000; i++) {
            float x = i / 100000.f;
            worst = std::fmax(worst, std::fabs(ambient::accurateTanh(x) - std::tanh((double)x)));
        }
        check(worst < 1e-4, "accurateTanh: |err| < 1e-4 vs std::tanh on [-10, 10]", "max abs err %.3g", worst);
    }
    // fastTanh scalar and SIMD overloads agree (same Pade(3,2) formula).
    {
        bool same = true;
        for (int i = -4000; i <= 4000; i++) {
            float x = i / 1000.f;
            if (ambient::fastTanh(x) != ambient::fastTanh(simd::float_4(x))[0]) {
                same = false;
            }
        }
        check(same, "fastTanh: scalar and SIMD overloads identical");
    }
    // Patch-JSON index clamp.
    check(ambient::clampIndex(-5, 4) == 0 && ambient::clampIndex(2, 4) == 2 && ambient::clampIndex(1000000000000LL, 4) == 3,
        "clampIndex: clamps to [0, count-1]");
}

// The scalar modulator caches shape()'s freq-dependent coefficients; it must
// produce exactly what recomputing them every sample would.
static void testPapaSrapaModulatorCache() {
    float st = 1.f / 48000.f;
    PapaSrapaModulator cached;
    float phase = 0.f, fast = 0.f, slow = 0.f, smooth = 0.f;
    float fastCoef = ambient::onePoleCoef(st, PapaSrapaRamp::FAST_TAU);
    float smoothCoef = ambient::onePoleCoef(st, PapaSrapaRamp::SMOOTH_TAU);
    int diff = 0;
    for (int n = 0; n < 200000; n++) {
        float rate = (n < 100000) ? 2.f : 2.f + 6.f * (n - 100000) / 100000.f;
        float divider = (n % 50000) / 50000.f;
        float a = cached.process(st, rate, divider);

        float freq = ambient::exp2Octaves(rate) / (1.f + divider * 7.f);
        phase += freq * st;
        phase -= std::floor(phase);
        float b = PapaSrapaRamp::shape(fast, slow, smooth, phase, freq,
            PapaSrapaRamp::dutyThresholdFor(freq), PapaSrapaRamp::slowCoefFor(freq, st), fastCoef, smoothCoef);
        if (a != b) {
            diff++;
        }
    }
    check(diff == 0, "PapaSrapaModulator: cached coefficients == per-sample recompute", "%g mismatches", diff);
}

int main() {
    testFilterStability();
    testFilterSelfOscillation();
    testFilterBound();
    testFilterSin();
    testFilterNan();
    testEnvelopes();
    testCommon();
    testPapaSrapaModulatorCache();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures;
}
