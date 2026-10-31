#pragma once
#include <rack.hpp>

using namespace rack;

// Classic 2-piece PolyBLEP correction (Valimaki/Huovilainen-style), applied
// near a waveform's hard discontinuity (t = phase distance from the edge,
// dt = phase increment per sample) to band-limit it without oversampling.
// Used by LunarVCOCore. Templated so the same formula works both scalar
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

// Matching 2-piece PolyBLAMP: the integral of polyBlep's residual, for a
// corner (an instant change of slope) instead of a jump — same t/dt
// convention and same branchless masking as polyBlep above. Returns the
// residual for a unit slope change measured in signal units per SAMPLE, so
// the caller scales it by (slope after - slope before, per unit phase) * dt.
// Derivation: unit-step residual is (1-n)^2/2 just before the edge and
// -(1-n)^2/2 just after (n = distance in samples); integrating gives
// (1-n)^3/6 on both sides, continuous (1/6) at the corner itself. Being
// linear, corrections for several corners simply add, even when two of
// them land closer together than one sample (they then sum to the right
// smeared step) — that is what keeps LunarVCOCore's Saw morph correct at
// its near-vertical shape extremes.
template <typename T>
static inline T polyBlamp(T t, T dt) {
    T before = T(1.f) - t / dt;
    before = before * before * before * (1.f / 6.f);
    T after = T(1.f) - (T(1.f) - t) / dt;
    after = after * after * after * (1.f / 6.f);
    T result = simd::ifelse(t > (T(1.f) - dt), after, T(0.f));
    result = simd::ifelse(t < dt, before, result);
    return result;
}
