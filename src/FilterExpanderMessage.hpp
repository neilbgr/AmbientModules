#pragma once

// Written by LunarFilter into its own leftExpander buffer each sample, so
// an adjacent LunarMixer directly to its left can read it via its own
// rightExpander.module->leftExpander.consumerMessage.
struct FilterExpanderMessage {
    float outL = 0.f;
    float outR = 0.f;
};
