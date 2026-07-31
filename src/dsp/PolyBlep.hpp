#pragma once

// Classic 2-piece PolyBLEP correction (Valimaki/Huovilainen-style), applied
// near a waveform's hard discontinuity (t = phase distance from the edge,
// dt = phase increment per sample) to band-limit it without oversampling.
// Shared by LunarVCOCore and PapaSrapaCore.
static inline float polyBlep(float t, float dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.f;
    } else if (t > 1.f - dt) {
        t = (t - 1.f) / dt;
        return t * t + t + t + 1.f;
    }
    return 0.f;
}
