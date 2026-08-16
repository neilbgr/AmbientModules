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
