#pragma once
#include <rack.hpp>
#include "PolyBlep.hpp"

using namespace rack;

// One "Papa Srapa" noise voice (Solar 42F drone voices 3/6): a low-frequency
// square modulator cross-modulating (FM and/or AM) a square audio oscillator
// (roughly C0..E7), plus an independent white-noise generator always mixed
// into the output. Not a literal circuit emulation of the real coupled
// Trigger-Schmidt oscillators (undocumented, chaotic) — an idiomatic
// approximation in the spirit of DroneVoice/LunarVCOCore/TriSquareLFO, tuned
// by ear against the real hardware. Pure DSP, no Module/param/light dependency.
struct PapaSrapaCore {
    enum ModMode { MODE_OFF, MODE_FM, MODE_AM, MODE_FM_AM };

    float modPhase = 0.f;
    float audioPhase = 0.f;
    uint32_t noiseState = 0x1234567u; // xorshift32 state, must stay non-zero
    float noise = 0.f; // last white-noise sample in [-1, 1], tapped by the S&H
    float lfoOut = 0.f; // last modulator square sample in [-1, 1], tapped by LFO_OUTPUT

    static float nextNoise(uint32_t& state) {
        // xorshift32: cheap, good enough for audio-rate white noise, no runtime pow/trig.
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float)(int32_t)state / 2147483648.f;
    }

    // rateOctaves: LF modulator frequency (octaves, same convention as
    // LunarLFO::octavesToHz). dividerAmount: 0..1, divides the modulator's
    // frequency down (1..8) before it reaches the FM/AM stage — richer,
    // slower beating at higher settings.
    // pitchOctaves: audio oscillator base frequency (octaves rel. C4, same
    // idiom as LunarVCOCore/DroneVoice).
    // modDepth: 0..1, FM/AM modulation depth. modMode: which of FM/AM (or
    // both/neither) is active. noiseAmount: 0..1, independent white-noise mix.
    // noiseOnly: bypasses the square oscillator and modulator entirely,
    // returning the raw noise sample at unity gain — a deterministic stand-in
    // for the hardware's fiddly "pitch to zero, mod and divider to max"
    // clean-noise knob combination.
    // Returns the mixed output in roughly [-1, 1].
    float process(float sampleTime, float sampleRate,
                   float rateOctaves, float dividerAmount,
                   float pitchOctaves, float modDepth, ModMode modMode,
                   float noiseAmount, bool noiseOnly) {
        float modFreq = dsp::approxExp2_taylor5(rateOctaves + 30.f) / std::pow(2.f, 30.f);
        float divider = 1.f + dividerAmount * 7.f; // 1..8
        modFreq /= divider;

        modPhase += modFreq * sampleTime;
        modPhase -= std::floor(modPhase);
        float modDt = modFreq * sampleTime;
        // PolyBLEP-soften both edges: the real hardware's Schmitt-trigger
        // oscillators don't switch instantaneously, so a perfectly hard
        // square reads as harsher than the original.
        float modSquare = (modPhase < 0.5f) ? 1.f : -1.f;
        modSquare += polyBlep(modPhase, modDt);
        modSquare -= polyBlep(std::fmod(modPhase - 0.5f + 1.f, 1.f), modDt);
        lfoOut = modSquare;

        noise = nextNoise(noiseState);
        if (noiseOnly)
            return noise;

        bool fmOn = (modMode == MODE_FM || modMode == MODE_FM_AM);
        bool amOn = (modMode == MODE_AM || modMode == MODE_FM_AM);

        float fmOctaves = fmOn ? modSquare * modDepth * 2.f : 0.f; // up to +-2 octaves
        float audioFreq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitchOctaves + fmOctaves + 30.f) / std::pow(2.f, 30.f);
        audioFreq = clamp(audioFreq, 0.f, sampleRate / 2.f);

        audioPhase += audioFreq * sampleTime;
        audioPhase -= std::floor(audioPhase);
        float audioDt = audioFreq * sampleTime;
        float audioSquare = (audioPhase < 0.5f) ? 1.f : -1.f;
        audioSquare += polyBlep(audioPhase, audioDt);
        audioSquare -= polyBlep(std::fmod(audioPhase - 0.5f + 1.f, 1.f), audioDt);

        float amGain = amOn ? (0.5f + 0.5f * modDepth * modSquare) : 1.f;
        float out = audioSquare * amGain;
        out += noise * noiseAmount;

        return out;
    }
};
