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
#include "dsp/LunarVCOCore.hpp"
#include "dsp/DroneVoice.hpp"

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

// dsp::MinBlepGenerator's constructor calls dsp::minBlepImpulse(), which
// lives in libRack (not linked here). Same algorithm as Rack's
// src/dsp/minblep.cpp (windowed sinc -> real cepstrum -> minimum-phase
// reconstruction -> integrate -> normalize), with a plain double-precision
// DFT instead of pffft: same table up to float rounding.
namespace rack {
namespace dsp {
static void computeMinBlepImpulse(int z, int o, float* output);

// Every LunarVCOCore (i.e. every MinBlepGenerator) asks for the same table:
// compute it once per size.
void minBlepImpulse(int z, int o, float* output) {
    static int cachedZ = 0;
    static int cachedO = 0;
    static std::vector<float> cached;
    int n = 2 * z * o;
    if (z != cachedZ || o != cachedO) {
        cached.assign(n, 0.f);
        computeMinBlepImpulse(z, o, cached.data());
        cachedZ = z;
        cachedO = o;
    }
    std::copy(cached.begin(), cached.end(), output);
}

static void computeMinBlepImpulse(int z, int o, float* output) {
    int n = 2 * z * o;
    std::vector<double> x(n);
    for (int i = 0; i < n; i++) {
        double p = -z + 2.0 * z * i / (n - 1);
        double s = (p == 0.0) ? 1.0 : std::sin(M_PI * p) / (M_PI * p);
        // Blackman-Harris window, as dsp::blackmanHarrisWindow().
        double t = (double) i / (n - 1);
        double w = 0.35875 - 0.48829 * std::cos(2 * M_PI * t) + 0.14128 * std::cos(4 * M_PI * t) - 0.01168 * std::cos(6 * M_PI * t);
        x[i] = s * w;
    }
    auto dft = [n](const std::vector<std::complex<double>>& in, bool inverse) {
        std::vector<std::complex<double>> out(n);
        double sign = inverse ? 1.0 : -1.0;
        for (int k = 0; k < n; k++) {
            std::complex<double> acc = 0.0;
            for (int j = 0; j < n; j++) {
                acc += in[j] * std::polar(1.0, sign * 2 * M_PI * (double) k * j / n);
            }
            out[k] = inverse ? acc / (double) n : acc;
        }
        return out;
    };
    std::vector<std::complex<double>> c(n);
    for (int i = 0; i < n; i++) {
        c[i] = x[i];
    }
    c = dft(c, false);
    for (int i = 0; i < n; i++) {
        c[i] = std::max(-30.0, std::log(std::abs(c[i])));
    }
    c = dft(c, true);
    // Fold the real cepstrum for the minimum-phase reconstruction.
    for (int i = 1; i < n / 2; i++) {
        c[i] = 2.0 * c[i].real();
    }
    for (int i = (n + 1) / 2; i < n; i++) {
        c[i] = 0.0;
    }
    c[0] = c[0].real();
    c = dft(c, false);
    for (int i = 0; i < n; i++) {
        c[i] = std::exp(c[i]);
    }
    c = dft(c, true);
    double total = 0.0;
    for (int i = 0; i < n; i++) {
        total += c[i].real();
        x[i] = total;
    }
    for (int i = 0; i < n; i++) {
        output[i] = (float) (x[i] / x[n - 1]);
    }
}
} // namespace dsp
} // namespace rack

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
        float b = PapaSrapaRamp::shape(fast, slow, smooth, phase, freq, freq * st,
            PapaSrapaRamp::dutyThresholdFor(freq), PapaSrapaRamp::slowCoefFor(freq, st), fastCoef, smoothCoef);
        if (a != b) {
            diff++;
        }
    }
    check(diff == 0, "PapaSrapaModulator: cached coefficients == per-sample recompute", "%g mismatches", diff);
}

// --- LunarVCO aliasing ------------------------------------------------------

// Aliasing measurement: fs = 32768 Hz and N = 4096 make sampleTime and the
// phase step m/N exactly representable, so f0 = m * fs / N sits exactly on
// DFT bin m with no window and no leakage. m is odd (coprime with N), so
// folded-back harmonics land between the harmonic bins.
// Ratio = power in the non-harmonic bins / power in the harmonic ones, in dB
// (lower is better):
//  - base: spacing of the legitimate partials (f0's bin, or half of it with
//    the sub one octave below);
//  - guard: bins around each partial that also count as legitimate (PWM
//    sidebands; 1 otherwise, for the tiny residual frequency error of the
//    linFm path);
//  - lowBandDb: same ratio counting only the aliases that fold below fs / 4
//    (12 kHz at 48 kHz), where they're clearly audible. Harmonics just above
//    Nyquist only fold to just below it, and every BLEP leaves some there.
static const float ALIAS_FS = 32768.f;
static const int ALIAS_N = 4096;

static double aliasRatioDb(const std::vector<float>& x, int base, int guard, double* lowBandDb) {
    static std::vector<double> cosTable, sinTable;
    if (cosTable.empty()) {
        cosTable.resize(ALIAS_N);
        sinTable.resize(ALIAS_N);
        for (int i = 0; i < ALIAS_N; i++) {
            cosTable[i] = std::cos(2 * M_PI * i / ALIAS_N);
            sinTable[i] = std::sin(2 * M_PI * i / ALIAS_N);
        }
    }
    double harmonic = 0.0;
    double alias = 0.0;
    double aliasLow = 0.0;
    for (int k = 1; k < ALIAS_N / 2; k++) {
        double re = 0.0;
        double im = 0.0;
        for (int j = 0; j < ALIAS_N; j++) {
            int idx = (int) (((long long) k * j) % ALIAS_N);
            re += x[j] * cosTable[idx];
            im -= x[j] * sinTable[idx];
        }
        double power = re * re + im * im;
        int nearest = (int) std::lround((double) k / base) * base;
        // nearest == 0 covers the PWM LFO's own low-frequency component
        // (the square's average follows the pulse width).
        if (std::abs(k - nearest) <= guard) {
            harmonic += power;
        }
        else {
            alias += power;
            if (k < ALIAS_N / 4) {
                aliasLow += power;
            }
        }
    }
    *lowBandDb = 10.0 * std::log10(std::max(aliasLow, 1e-30) / std::max(harmonic, 1e-30));
    return 10.0 * std::log10(std::max(alias, 1e-30) / std::max(harmonic, 1e-30));
}

// Renders N samples of one LunarVCOCore lane at f0 = m * fs / N (after a
// warm-up), the pitch set through linFm so the frequency is exact. shapeFn
// gives the shape (pulse width / morph) per sample, for PWM.
template <typename ShapeFn>
static std::vector<float> renderVco(int waveform, int m, bool octaveOn, bool subOn, ShapeFn shapeFn) {
    LunarVCOCore vco;
    float sampleTime = 1.f / ALIAS_FS;
    float f0 = m * ALIAS_FS / ALIAS_N;
    float linHz = octaveOn ? f0 / 8.f : f0;
    std::vector<float> out(ALIAS_N);
    int warmup = 4 * ALIAS_N;
    for (int i = 0; i < warmup + ALIAS_N; i++) {
        simd::float_4 y = vco.process(sampleTime, ALIAS_FS, simd::float_4(-30.f), simd::float_4(0.f), simd::float_4(linHz),
                                      waveform, simd::float_4(shapeFn(i)), octaveOn, subOn, simd::float_4(0.f));
        if (i >= warmup) {
            out[i - warmup] = y[0];
        }
    }
    return out;
}

struct AliasCase {
    const char* name;
    int waveform;
    int m;
    bool octaveOn;
    bool subOn;
    float shape;
    bool pwm;
    float maxLowBandDb; // pass threshold on the audible-band ratio
};

static void testVcoAliasing() {
    // m = 327: f0 = 2616 Hz = 0.08 fs (3.8 kHz at 48 kHz), Oct +3 on;
    // m = 615: 0.15 fs. Square + sub: main at m = 654 without Oct +3, so the
    // sub sits exactly on bin 327. PWM: an LFO on bin 1 (one cycle per
    // analysis window, so the signal stays periodic), PW 0.4..0.6 (Saw morph:
    // shape 0.05..0.25), its sidebands kept as legitimate within 6 bins of
    // each partial (20 for Saw morph, see below). Saw morph + sub: main on
    // bin 328 without Oct +3 (0.08 fs), the sub on bin 164.
    // Thresholds: a few dB above what this version measures. 2.5.0 (2-point
    // PolyBLEP/BLAMP, fastTanh after them) measured, below fs/4: square
    // -35.5 / -25.5 / -31.5 / -34.2 (PWM) / -32.4 (+ sub), inverted saw and
    // sine to saw -34.3, saw morph -40.9 (shape 0.1), triangle -74.9, sine
    // -115.6 dB. Saw morph before its jump rendering (MORPH_JUMP_FULL_SAMPLES):
    // -43.4 / -44.0 / -47.4 / -55.0 at shapes 0 / 0.05 / 0.1 / 0.15.
    const AliasCase cases[] = {
        {"square PW 50%", LunarVCOCore::WAVE_SQUARE, 327, true, false, 0.5f, false, -60.f},
        {"square PW 50%, 0.15 fs", LunarVCOCore::WAVE_SQUARE, 615, true, false, 0.5f, false, -60.f},
        {"square PW 10%", LunarVCOCore::WAVE_SQUARE, 327, true, false, 0.1f, false, -60.f},
        {"square PWM (LFO)", LunarVCOCore::WAVE_SQUARE, 327, true, false, 0.5f, true, -60.f},
        {"square PW 99%", LunarVCOCore::WAVE_SQUARE, 327, true, false, 0.99f, false, -60.f},
        // Up to 100%: what's left below fs/4 is the DC removal's own LFO
        // harmonics, just above bin 20 (see the guard below), not aliasing.
        {"square PWM up to 100%", LunarVCOCore::WAVE_SQUARE, 327, true, false, 0.95f, true, -50.f},
        {"square + sub", LunarVCOCore::WAVE_SQUARE, 654, false, true, 0.5f, false, -60.f},
        {"inverted saw", LunarVCOCore::WAVE_INV_SAW, 327, true, false, 0.f, false, -60.f},
        {"saw morph (shape 0)", LunarVCOCore::WAVE_SAW_MORPH, 327, true, false, 0.f, false, -70.f},
        {"saw morph (shape 0.05)", LunarVCOCore::WAVE_SAW_MORPH, 327, true, false, 0.05f, false, -70.f},
        {"saw morph (shape 0.1)", LunarVCOCore::WAVE_SAW_MORPH, 327, true, false, 0.1f, false, -70.f},
        {"saw morph (shape 0.15)", LunarVCOCore::WAVE_SAW_MORPH, 327, true, false, 0.15f, false, -70.f},
        {"saw morph (shape 0.25)", LunarVCOCore::WAVE_SAW_MORPH, 327, true, false, 0.25f, false, -70.f},
        {"saw morph (shape 0.9)", LunarVCOCore::WAVE_SAW_MORPH, 327, true, false, 0.9f, false, -70.f},
        {"saw morph (LFO 0.05-0.25)", LunarVCOCore::WAVE_SAW_MORPH, 327, true, false, 0.15f, true, -65.f},
        {"saw morph (shape 0.1) + sub", LunarVCOCore::WAVE_SAW_MORPH, 328, false, true, 0.1f, false, -60.f},
        {"sine to saw (shape 1)", LunarVCOCore::WAVE_SINE_SAW_MORPH, 327, true, false, 1.f, false, -60.f},
        {"triangle", LunarVCOCore::WAVE_TRIANGLE, 327, true, false, 0.f, false, -85.f},
        {"sine", LunarVCOCore::WAVE_SINE, 327, true, false, 0.f, false, -110.f},
    };
    for (const AliasCase& c : cases) {
        float shape = c.shape;
        bool pwm = c.pwm;
        std::vector<float> x = renderVco(c.waveform, c.m, c.octaveOn, c.subOn, [=](int i) {
            return pwm ? shape + std::min(0.1f, 1.f - shape) * std::sin(2.f * (float) M_PI * i / ALIAS_N) : shape;
        });
        double low;
        // Moving Saw morph's peak phase-modulates every partial, the high ones
        // most: its sidebands spread wider than the pulse width's. A pulse
        // width swept up to 100% (0.9..1, just touching it) also sweeps the
        // pulse's DC removal (see pulseDcRemoval()): a legitimate
        // low-frequency component, with harmonics of the LFO above bin 6.
        int guard = !pwm ? 1 : (c.waveform == LunarVCOCore::WAVE_SAW_MORPH || c.shape > 0.9f ? 20 : 6);
        double full = aliasRatioDb(x, c.subOn ? c.m / 2 : c.m, guard, &low);
        std::printf("INFO  LunarVCO aliasing, %-24s %7.1f dB full band, %7.1f dB below fs/4\n", c.name, full, low);
        if (c.maxLowBandDb < 0.f) {
            check(low <= c.maxLowBandDb, "LunarVCO aliasing below fs/4 under threshold", "%.1f dB (threshold %.0f)", low, c.maxLowBandDb);
        }
    }
}

// Hard sync from another LunarVCO's OSC Out (a raw +-5 V phase ramp, whose
// rising crossing of the 0.25 V threshold sits mid-ramp, so it's located
// exactly): master on bin 101 (no Oct +3), slave Square at 2.37x. The synced
// output repeats every master cycle, so its legitimate partials are the
// multiples of bin 101.
static double syncAliasLowDb() {
    LunarVCOCore vco;
    float sampleTime = 1.f / ALIAS_FS;
    int m = 101;
    float masterStep = (float) m / ALIAS_N;
    float masterPhase = 0.f;
    float slaveHz = 2.37f * m * ALIAS_FS / ALIAS_N;
    std::vector<float> x(ALIAS_N);
    int warmup = 4 * ALIAS_N;
    for (int i = 0; i < warmup + ALIAS_N; i++) {
        masterPhase += masterStep;
        masterPhase -= std::floor(masterPhase);
        simd::float_4 y = vco.process(sampleTime, ALIAS_FS, simd::float_4(-30.f), simd::float_4(0.f), simd::float_4(slaveHz),
                                      LunarVCOCore::WAVE_SQUARE, simd::float_4(0.5f), false, false,
                                      simd::float_4(masterPhase * 10.f - 5.f));
        if (i >= warmup) {
            x[i - warmup] = y[0];
        }
    }
    double low;
    aliasRatioDb(x, m, 1, &low);
    return low;
}

// Amplitude of the component at f (Hz, fs = 48 kHz) in x, Hann-windowed.
static double toneAmplitude(const std::vector<float>& x, double f) {
    double re = 0.0, im = 0.0, wsum = 0.0;
    int n = (int) x.size();
    for (int i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * std::cos(2 * M_PI * i / (n - 1));
        double ph = 2 * M_PI * f * i / 48000.0;
        wsum += w;
        re += w * x[i] * std::cos(ph);
        im -= w * x[i] * std::sin(ph);
    }
    return 2 * std::sqrt(re * re + im * im) / wsum;
}

// The sub's edge rounding is a fixed ~6 kHz bandwidth (SUB_EDGE_BANDWIDTH_HZ),
// whatever the note and Oct +3: its odd harmonics near 9 kHz, against an
// ideal square and relative to those near 0.9 kHz, about -5 dB everywhere
// (hardware: -4.5..-5.1 dB). Square at PW 100%, so the sub plays alone.
static void testVcoSubRounding() {
    double lo = 1e9, hi = -1e9;
    for (float subHz : {46.f, 184.f}) {
        for (int oct = 0; oct < 2; oct++) {
            LunarVCOCore vco;
            float fs = 48000.f;
            float pitch = std::log2(2.f * subHz / dsp::FREQ_C4);
            std::vector<float> x;
            for (int i = 0; i < 72000; i++) {
                float y = vco.process(1.f / fs, fs, simd::float_4(pitch), simd::float_4(0.f), simd::float_4(0.f),
                                      LunarVCOCore::WAVE_SQUARE, simd::float_4(1.f), oct != 0, true, simd::float_4(0.f))[0];
                if (i >= 24000) {
                    x.push_back(y);
                }
            }
            double a1 = toneAmplitude(x, subHz);
            auto vsSquare = [&](double target) {
                int h = (int) std::lround(target / subHz) | 1;
                return 20 * std::log10(toneAmplitude(x, h * subHz) / a1) + 20 * std::log10((double) h);
            };
            double drop = vsSquare(9000) - vsSquare(900);
            lo = std::min(lo, drop);
            hi = std::max(hi, drop);
        }
    }
    check(lo > -6.0 && hi < -3.5 && hi - lo < 0.5, "LunarVCO: sub edge rounding fixed (same at 46/184 Hz, Oct +3 or not)",
          "9 kHz vs 0.9 kHz: %.1f .. %.1f dB", lo, hi);
}

// The sub divides the main (see LunarVCOCore::subLevel()): switched on at
// any moment, the mix comes out the same (a free-running sub phase made it
// depend on that moment), and its count only ever steps where the main's
// phase wraps (sync, through-zero FM and Oct +3 included).
static void testVcoSubLock() {
    float fs = 48000.f;
    LunarVCOCore always;
    LunarVCOCore later;
    float worst = 0.f;
    for (int i = 0; i < 6000; i++) {
        simd::float_4 a = always.process(1.f / fs, fs, simd::float_4(0.37f), simd::float_4(0.f), simd::float_4(0.f),
                                         LunarVCOCore::WAVE_INV_SAW, simd::float_4(0.f), false, true, simd::float_4(0.f));
        simd::float_4 b = later.process(1.f / fs, fs, simd::float_4(0.37f), simd::float_4(0.f), simd::float_4(0.f),
                                        LunarVCOCore::WAVE_INV_SAW, simd::float_4(0.f), false, i >= 1234, simd::float_4(0.f));
        if (i >= 3000) {
            worst = std::max(worst, std::fabs(a[0] - b[0]));
        }
    }
    check(worst < 1e-4f, "LunarVCO: sub switched on late = sub on from the start (locked to the main)", "max difference %.2e", worst);

    LunarVCOCore vco;
    Noise noise;
    bool stepsOnWraps = true;
    float masterPhase = 0.f;
    for (int i = 0; i < 20000; i++) {
        float prevPhase = vco.phase[0];
        float prevCount = vco.subCount[0];
        float prevMaster = masterPhase;
        masterPhase += 0.013f;
        masterPhase -= std::floor(masterPhase);
        // The sync input (masterPhase * 10 - 5) rises through 0.25 V.
        bool synced = prevMaster < 0.525f && masterPhase >= 0.525f;
        bool octave = (i / 3000) % 2 == 1;
        // Deep through-zero linear FM, hard sync, Oct +3 switching.
        float linHz = 700.f * std::sin(0.002f * i) + 300.f * noise.next();
        vco.process(1.f / fs, fs, simd::float_4(-1.f), simd::float_4(0.f), simd::float_4(linHz), LunarVCOCore::WAVE_SQUARE,
                    simd::float_4(0.5f), octave, true, simd::float_4(masterPhase * 10.f - 5.f));
        bool wrapped = std::fabs(vco.phase[0] - prevPhase) > 0.5f || synced;
        bool octaveSwitch = (i % 3000) == 0;
        if (vco.subCount[0] != prevCount && !wrapped && !octaveSwitch) {
            stepsOnWraps = false;
        }
    }
    check(stepsOnWraps, "LunarVCO: the sub only steps on the main's cycle resets (FM, sync, Oct +3)");
}

static void testVcoRobustness() {
    // Pulse width at either end: the pulse vanishes (as on the hardware) and
    // leaves no DC. Without sub: silence. With sub: the sub alone, centred
    // (its average over whole cycles ~0, its two levels symmetric).
    for (float shape : {0.f, 1.f}) {
        for (int sub = 0; sub < 2; sub++) {
            LunarVCOCore vco;
            float fs = 48000.f;
            float peak = 0.f;
            double sum = 0.0;
            int n = 0;
            for (int i = 0; i < 48000; i++) {
                simd::float_4 y = vco.process(1.f / fs, fs, simd::float_4(-30.f), simd::float_4(0.f), simd::float_4(400.f),
                                              LunarVCOCore::WAVE_SQUARE, simd::float_4(shape), false, sub != 0, simd::float_4(0.f));
                if (i >= 24000) {
                    peak = std::max(peak, std::fabs(y[0]));
                    sum += y[0];
                    n++;
                }
            }
            double mean = sum / n; // 24000 samples = 100 sub cycles at 200 Hz
            bool ok = sub ? (std::fabs(mean) < 1e-3 && peak > 0.4f && peak < 0.6f) : (peak < 1e-4f);
            check(ok, sub ? (shape > 0.5f ? "LunarVCO: square at PW 100% + sub: the sub alone, centred"
                                          : "LunarVCO: square at PW 0% + sub: the sub alone, centred")
                          : (shape > 0.5f ? "LunarVCO: square at PW 100%: silent, no DC" : "LunarVCO: square at PW 0%: silent, no DC"),
                  "peak %.4f, mean %.5f", peak, mean);
        }
    }
    // Through-zero FM: a phase stepping from just above 0 to just below it
    // used to wrap to exactly 1.0f, and the next sample counted the phase-0
    // edge a second time (an inverted saw spike of about 2). Sum of the
    // jumps inserted: the single +1 -> -1 drop, saturated.
    {
        LunarVCOCore vco;
        float fs = 48000.f;
        vco.phase = 1e-8f;
        float jumps = 0.f;
        bool wrapOk = true;
        for (int i = 0; i < 8; i++) {
            vco.process(1.f / fs, fs, simd::float_4(-30.f), simd::float_4(0.f), simd::float_4(-2e-8f * fs),
                        LunarVCOCore::WAVE_INV_SAW, simd::float_4(0.f), false, false, simd::float_4(0.f));
            jumps += vco.jumpHistory[(vco.historyPos + LunarVCOCore::BLEP_DELAY - 1) % LunarVCOCore::BLEP_DELAY][0];
            wrapOk = wrapOk && vco.phase[0] < 1.f;
        }
        float expected = ambient::fastTanh(-1.f) - ambient::fastTanh(1.f);
        check(wrapOk && std::fabs(jumps - expected) < 1e-4f, "LunarVCO: backward phase wrap just below 0 inserts its edge once",
              "jumps %.3f (expected %.3f)", jumps, expected);
    }
    std::printf("INFO  LunarVCO aliasing, %-24s %7.1f dB below fs/4\n", "square hard-synced", syncAliasLowDb());
    check(syncAliasLowDb() <= -55.0, "LunarVCO: hard sync band-limited (aliasing below fs/4 under -55 dB)");

    // Frequency cap.
    check(ambient::maxOscillatorFreq(48000.f) == 20000.f && ambient::maxOscillatorFreq(96000.f) == 20000.f
          && std::fabs(ambient::maxOscillatorFreq(44100.f) - 19845.f) < 1e-2f
          && std::fabs(ambient::maxOscillatorFreq(22050.f) - 9922.5f) < 1e-2f,
          "maxOscillatorFreq: 20 kHz at 48/96 kHz, 0.45 fs at 44.1 kHz (19845 Hz) and 22.05 kHz");
    {
        float fs = 48000.f;
        LunarVCOCore vco;
        vco.process(1.f / fs, fs, simd::float_4(-30.f), simd::float_4(0.f), simd::float_4(30000.f),
                    LunarVCOCore::WAVE_SQUARE, simd::float_4(0.5f), true, true, simd::float_4(0.f));
        float limit = ambient::maxOscillatorFreq(fs) / fs;
        bool mainCapped = std::fabs(vco.phase[0] - limit) < 1e-5f;
        LunarVCOCore down;
        down.process(1.f / fs, fs, simd::float_4(-30.f), simd::float_4(0.f), simd::float_4(-30000.f),
                     LunarVCOCore::WAVE_SQUARE, simd::float_4(0.5f), false, false, simd::float_4(0.f));
        bool negativeCapped = std::fabs(down.phase[0] - (1.f - limit)) < 1e-5f;
        check(mainCapped && negativeCapped, "LunarVCO: 30 kHz (and -30 kHz through-zero) capped at 20 kHz");

        DroneVoice drone;
        float pitches[DroneVoice::NUM_OSC] = {8.f, 8.f, 8.f, 8.f, 8.f}; // ~67 kHz requested
        bool active[DroneVoice::NUM_OSC] = {true, false, false, false, false};
        bool mod[DroneVoice::NUM_OSC] = {};
        drone.process(1.f / fs, fs, pitches, active, mod, 0.f, 0.f);
        check(std::fabs(drone.phase[0] - limit) < 1e-5f, "DroneVoice: capped at 20 kHz");

        PapaSrapaCore papa;
        papa.process(1.f / fs, fs, simd::float_4(8.f), simd::float_4(0.f), simd::float_4(0.f), simd::float_4(0.f),
                     PapaSrapaCore::MODE_OFF, simd::float_4(0.f), 0.f, false);
        check(std::fabs(papa.audioPhase[0] - limit) < 1e-5f, "PapaSrapaCore: capped at 20 kHz");
    }

    // A pure sine without sub is untouched by this rework: fastTanh(sin), as
    // in 2.5.0, only blepDelay (2.968) samples later -- linear
    // interpolation between the samples 3 and 2 back.
    {
        LunarVCOCore vco;
        float fs = 48000.f;
        bool same = true;
        std::vector<float> expected;
        for (int i = 0; i < 20000; i++) {
            simd::float_4 y = vco.process(1.f / fs, fs, simd::float_4(0.5f), simd::float_4(0.f), simd::float_4(0.f),
                                          LunarVCOCore::WAVE_SINE, simd::float_4(0.f), true, false, simd::float_4(0.f));
            expected.push_back(ambient::fastTanh(simd::sin(LunarVCOCore::TWO_PI * vco.phase))[0]);
            if (i >= LunarVCOCore::BLEP_DELAY) {
                float a = expected[i - LunarVCOCore::BLEP_DELAY];
                float b = expected[i - LunarVCOCore::BLEP_DELAY + 1];
                if (std::fabs(y[0] - (a + vco.delayFraction * (b - a))) > 1e-6f) {
                    same = false;
                }
            }
        }
        check(same, "LunarVCO: Sine without sub = fastTanh(sin), 2.968 samples later");
        LunarVCOCore fresh;
        check(fresh.delayFraction >= 0.f && fresh.delayFraction < 0.1f, "LunarVCO: minBLEP group delay just under BLEP_DELAY samples",
              "%.4f samples", fresh.blepDelay);
    }

    // No frequency-dependent offset: the mean of a sawtooth stays ~0 up to
    // 15 kHz, either direction (the minBLEP's group delay is compensated).
    {
        float fs = 48000.f;
        double worst = 0.0;
        for (float hz : {1000.f, 5000.f, 10000.f, 15000.f, -15000.f}) {
            LunarVCOCore vco;
            double sum = 0.0;
            for (int i = 0; i < 48000; i++) {
                simd::float_4 y = vco.process(1.f / fs, fs, simd::float_4(-30.f), simd::float_4(0.f), simd::float_4(hz),
                                              LunarVCOCore::WAVE_INV_SAW, simd::float_4(0.f), false, false, simd::float_4(0.f));
                if (i >= 1000) {
                    sum += y[0];
                }
            }
            worst = std::max(worst, std::fabs(sum / 47000.0));
        }
        check(worst < 0.001, "LunarVCO: sawtooth has no frequency-dependent offset", "worst mean %.5f", worst);
    }

    // Normal playing (1 kHz, every waveform, with and without sub): the
    // output stays within +-1, like the fastTanh-capped 2.5.0 output did.
    {
        float fs = 48000.f;
        float peak = 0.f;
        for (int w = 0; w < LunarVCOCore::NUM_WAVEFORMS; w++) {
            for (int sub = 0; sub < 2; sub++) {
                for (float shape : {0.f, 0.1f, 0.5f, 0.9f, 1.f}) {
                    LunarVCOCore vco;
                    for (int i = 0; i < 4800; i++) {
                        simd::float_4 y = vco.process(1.f / fs, fs, simd::float_4(std::log2(1000.f / dsp::FREQ_C4)), simd::float_4(0.f),
                                                      simd::float_4(0.f), w, simd::float_4(shape), false, sub != 0, simd::float_4(0.f));
                        peak = std::max(peak, std::fabs(y[0]));
                    }
                }
            }
        }
        check(peak < 1.f, "LunarVCO: output within +-1 in normal playing", "peak %.3f", peak);
    }

    // Deep through-zero linear FM (+-15 kHz at audio rate), sync, sub, every
    // waveform: always finite and bounded. Without the old output-stage
    // fastTanh (now applied before band-limiting), such pathological input
    // can overshoot +-1 briefly: e.g. right after a sync reset at a ~0.4
    // phase step, the reset's minBLEP is still settling while the waveform
    // already sweeps back across its range.
    {
        bool ok = true;
        float peak = 0.f;
        float fs = 48000.f;
        for (int w = 0; w < LunarVCOCore::NUM_WAVEFORMS; w++) {
            for (int sub = 0; sub < 2; sub++) {
                LunarVCOCore vco;
                Noise noise;
                float masterPhase = 0.f;
                for (int i = 0; i < 48000; i++) {
                    float fm = 15000.f * std::sin(2.f * (float) M_PI * 1234.5f * i / fs);
                    masterPhase += 377.f / fs;
                    masterPhase -= std::floor(masterPhase);
                    float shape = 0.5f + 0.5f * noise.next();
                    simd::float_4 y = vco.process(1.f / fs, fs, simd::float_4(1.f), simd::float_4(0.f), simd::float_4(fm),
                                                  w, simd::float_4(shape), true, sub != 0, simd::float_4(masterPhase * 10.f - 5.f));
                    if (!std::isfinite(y[0]) || std::fabs(y[0]) > 3.f) {
                        ok = false;
                    }
                    peak = std::max(peak, std::fabs(y[0]));
                }
            }
        }
        check(ok, "LunarVCO: finite and bounded under deep through-zero FM, sync, random shape", "peak %.3f", peak);
    }
}

// Lunar50Drone's hardware tone: the same roll-off in Hz at every pitch
// (measured on the hardware at 41, 100 and 634 Hz), matching the analog
// pair of poles (1.7 kHz, 10 kHz) within 0.25 dB up to 4 kHz and 1 dB at
// 8 kHz (the TPT poles are pre-warped, so the 10 kHz one bends slightly
// below it at 48 kHz). Its fundamental included.
static std::vector<float> renderDrone(double f0, bool hardwareTone, float fs, int skip, int n, int switchAt = -1) {
    DroneVoice drone;
    drone.hardwareTone = hardwareTone;
    float pitches[DroneVoice::NUM_OSC] = {(float) std::log2(f0 / dsp::FREQ_C4), 0.f, 0.f, 0.f, 0.f};
    bool active[DroneVoice::NUM_OSC] = {true, false, false, false, false};
    bool mod[DroneVoice::NUM_OSC] = {};
    std::vector<float> x;
    for (int i = 0; i < skip + n; i++) {
        if (i == switchAt) {
            drone.hardwareTone = !drone.hardwareTone;
        }
        float y = drone.process(1.f / fs, fs, pitches, active, mod, 0.f, 0.f);
        if (i >= skip) {
            x.push_back(y);
        }
    }
    return x;
}

static void testDroneTone() {
    auto analogDb = [](double f) {
        return -10 * std::log10(1 + std::pow(f / DroneVoice::TONE_POLE1_HZ, 2))
               - 10 * std::log10(1 + std::pow(f / DroneVoice::TONE_POLE2_HZ, 2));
    };
    double worst = 0.0, top = 0.0, fundamental = 0.0;
    const double pitches[] = {41.0, 100.4, 634.5};
    for (double f0 : pitches) {
        std::vector<float> dark = renderDrone(f0, true, 48000.f, 24000, 48000);
        std::vector<float> bright = renderDrone(f0, false, 48000.f, 24000, 48000);
        fundamental = std::fmax(fundamental, std::fabs(20 * std::log10(toneAmplitude(dark, f0) / toneAmplitude(bright, f0)) - analogDb(f0)));
        const double bands[] = {1000.0, 2500.0, 4000.0};
        for (double f : bands) {
            double h = std::round(f / f0) * f0;
            double db = 20 * std::log10(toneAmplitude(dark, h) / toneAmplitude(bright, h));
            worst = std::fmax(worst, std::fabs(db - analogDb(h)));
        }
        double h8 = std::round(8000.0 / f0) * f0;
        top = std::fmax(top, std::fabs(20 * std::log10(toneAmplitude(dark, h8) / toneAmplitude(bright, h8)) - analogDb(h8)));
    }
    check(worst < 0.25 && top < 1.0, "DroneVoice: hardware tone = 1.7 kHz + 10 kHz poles at 41/100/634 Hz",
          "worst %.2f dB to 4 kHz, %.2f dB at 8 kHz", worst, top);
    check(fundamental < 0.05, "DroneVoice: hardware tone at the fundamental as the analog poles", "%.3f dB", fundamental);

    // Switching the tone on or off mid-note: no step larger than the saw's
    // own steps (its edge is 2).
    for (int on = 0; on < 2; on++) {
        std::vector<float> x = renderDrone(100.4, on == 1, 48000.f, 0, 24000, 12000);
        float worstJump = 0.f;
        for (int i = 11990; i < 12010; i++) {
            float d = std::fabs(x[i + 1] - x[i]);
            // Ignore the saw's own edge, which can fall in the window.
            if (d < 1.f) {
                worstJump = std::fmax(worstJump, d);
            }
        }
        check(worstJump < 0.2f, on ? "DroneVoice: tone switched off mid-note without a click" : "DroneVoice: tone switched on mid-note without a click",
              "largest step %.3f", worstJump);
    }

    // Low sample rate: the 10 kHz pole is held below 0.45 fs, still stable.
    std::vector<float> low = renderDrone(1000.0, true, 8000.f, 0, 8000);
    float peak = 0.f;
    bool finite = true;
    for (float v : low) {
        finite = finite && std::isfinite(v);
        peak = std::fmax(peak, std::fabs(v));
    }
    check(finite && peak < 1.5f, "DroneVoice: hardware tone stable at 8 kHz sample rate", "peak %.2f", peak);
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
    testVcoAliasing();
    testVcoRobustness();
    testVcoSubRounding();
    testVcoSubLock();
    testDroneTone();
    std::printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures;
}
