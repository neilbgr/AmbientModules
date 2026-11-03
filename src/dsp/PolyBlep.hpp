#pragma once
#include <rack.hpp>

using namespace rack;

// Classic 2-piece PolyBLEP correction (Valimaki/Huovilainen-style), applied
// near a waveform's hard discontinuity (t = phase distance from the edge,
// dt = phase increment per sample) to band-limit it without oversampling.
// Used by DroneVoice and PapaSrapaCore (LunarVCOCore uses a minBLEP for its
// edges). Templated so the same formula works both scalar
// (T=float) and 4-wide SIMD (T=simd::float_4, batching 4 poly channels) —
// written branchless via simd::ifelse() (a bitwise select) instead of
// if/else so it vectorizes without per-lane divergence; a lane past a hard
// edge that produces Inf/NaN (e.g. dt==0 for a channel that's momentarily
// silent) never leaks into the selected result, since the unselected side
// is masked out at the bit level rather than added/subtracted.
template <typename T>
static inline T polyBlep(T t, T dt) {
    T before = t / dt;
    before = before + before - before * before - 1.f;
    T after = (t - 1.f) / dt;
    after = after * after + after + after + 1.f;
    T result = simd::ifelse(t > (T(1.f) - dt), after, T(0.f));
    result = simd::ifelse(t < dt, before, result);
    return result;
}

// 4-point PolyBLAMP, for a corner (an instant change of slope) instead of a
// jump: residual of a unit slope change (per sample) when the band-limiting
// kernel is the cubic B-spline, i.e. 2 samples on each side of the corner
// instead of the classic 2-piece PolyBLAMP's 1 -- much stronger suppression
// of the harmonics near Nyquist. s = signed time
// from the corner in samples; the residual is even, so only |s| matters:
//   |s| < 1:      7/30 - a/2 + a^2/3 - a^4/12 + a^5/40
//   1 <= |s| < 2: (2 - a)^5 / 120
//   beyond:       0
// (exact double integral of the B-spline minus the ideal ramp; it vanishes
// with zero slope at |s| = 2, so it truncates cleanly). Add
// slopeChangePerSample * polyBlamp4(s) to the naive signal; being linear,
// corrections for several corners simply add. Linear phase, so the 2 samples BEFORE a corner need it predicted
// in advance -- fine for a free-running phase, which LunarVCOCore's corners
// are.
template <typename T>
static inline T polyBlamp4(T s) {
    T a = simd::fabs(s);
    T a2 = a * a;
    T inner = 7.f / 30.f - 0.5f * a + a2 / 3.f - a2 * a2 / 12.f + a2 * a2 * a / 40.f;
    T b = 2.f - a;
    T b2 = b * b;
    T outer = b2 * b2 * b / 120.f;
    return simd::ifelse(a < 1.f, inner, simd::ifelse(a < 2.f, outer, T(0.f)));
}

// Both instances of a corner at phase c that can be within 2 samples of
// phase p (the last one crossed and the next one ahead), as polyBlamp4()
// residuals. dt = |phase step| (> 0.25 puts both within reach at once,
// which simply adds up).
template <typename T>
static inline T polyBlamp4Corner(T p, T c, T dt) {
    T d = p - c;
    d -= simd::floor(d);
    T safeDt = simd::fmax(dt, T(1e-9f));
    return polyBlamp4(d / safeDt) + polyBlamp4((T(1.f) - d) / safeDt);
}
