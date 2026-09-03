#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "Widgets.hpp"

// 9-voice panoramic mixer (SOLAR 42F manual p.21: Drone1, Drone2, Drone3,
// Ext.Audio, VCO A, VCO B, Preamp, Drone4, Drone5, Drone6 — 10 physical jacks
// on the hardware, but EXT.AUDIO and PREAMP are mutually exclusive there
// (patching EXT.AUDIO mutes the internal piezo), so only 9 channels are
// actually useful here). Straight sum, no automatic normalization by channel
// count and no soft-clip: the hardware mixer page shows only PAN+VOL per
// channel, and that's also the standard Eurorack mixer convention — the user
// gain-stages via the VOL knobs, same as on real hardware.
struct LunarMixer : Module {
    static const int NUM_CHANNELS = 9;

    enum ParamIds {
        ENUMS(PAN_PARAM, NUM_CHANNELS),
        ENUMS(VOL_PARAM, NUM_CHANNELS),
        MASTER_VOL_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        ENUMS(CHANNEL_INPUT, NUM_CHANNELS),
        NUM_INPUTS
    };
    enum OutputIds {
        OUTPUT_L,
        OUTPUT_R,
        NUM_OUTPUTS
    };

    float gainL[NUM_CHANNELS] = {};
    float gainR[NUM_CHANNELS] = {};
    // Constant-power pan law recomputed only every 16 samples (see process())
    // — same throttling convention as AREnvelope::lambdaFromKnob, negligible
    // either way for 9 channels but kept consistent with the project's
    // "throttle expensive math" priority.
    dsp::ClockDivider panDivider;

    LunarMixer() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
        panDivider.setDivision(16);
        static const char* labels[NUM_CHANNELS] = {
            "Drone 1 (Solar50)", "Drone 2 (Solar50)", "Drone 3 (PapaSrapa)",
            "VCO A", "Ext. Audio / Preamp (mutually exclusive on the real hardware)", "VCO B",
            "Drone 4 (Solar50)", "Drone 5 (Solar50)", "Drone 6 (PapaSrapa)"
        };
        for (int i = 0; i < NUM_CHANNELS; i++) {
            configInput(CHANNEL_INPUT + i, labels[i]);
            configParam(PAN_PARAM + i, -1.f, 1.f, 0.f, string::f("%s pan", labels[i]));
            configParam(VOL_PARAM + i, 0.f, 1.f, 0.8f, string::f("%s level", labels[i]), "%", 0.f, 100.f);
            updatePanGains(i);
        }
        configParam(MASTER_VOL_PARAM, 0.f, 1.f, 1.f, "Master level", "%", 0.f, 100.f);
        configOutput(OUTPUT_L, "Left");
        configOutput(OUTPUT_R, "Right");
    }

    void updatePanGains(int i) {
        float pan = params[PAN_PARAM + i].getValue();
        gainL[i] = std::cos((pan + 1.f) * (float)M_PI / 4.f);
        gainR[i] = std::sin((pan + 1.f) * (float)M_PI / 4.f);
    }

    void process(const ProcessArgs& args) override {
        if (panDivider.process()) {
            for (int i = 0; i < NUM_CHANNELS; i++) {
                updatePanGains(i);
            }
        }

        float outL = 0.f, outR = 0.f;
        for (int i = 0; i < NUM_CHANNELS; i++) {
            // getVoltageSum(), not getVoltage(): each channel has a single level/pan
            // knob, so a polyphonic cable is summed to mono before it, same
            // convention as Bogaudio's Mix4 and Venom/MindMeld MixMaster's default
            // "poly sum" mode (their own bus is likewise non-polyphonic per channel).
            float signal = inputs[CHANNEL_INPUT + i].getVoltageSum() * params[VOL_PARAM + i].getValue();
            outL += signal * gainL[i];
            outR += signal * gainR[i];
        }
        float masterVol = params[MASTER_VOL_PARAM].getValue();
        outputs[OUTPUT_L].setVoltage(outL * masterVol);
        outputs[OUTPUT_R].setVoltage(outR * masterVol);
    }
};

struct LunarMixerWidget : ModuleWidget {
    int appliedTheme = -1;

    LunarMixerWidget(LunarMixer* module) {
        setModule(module);
        syncPanelTheme(this, "LunarMixer", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Placeholder layout: one row per channel (IN, VOL, PAN left to
        // right), output column (L / master vol / R, stacked) to the right.
        // Panel WIP in Inkscape.
        // PAN/VOL use RoganMedSmallWhite/Black (Widgets.hpp), real diameter
        // 8.387mm (Rogan1PSWhiteMedSmall-fg.svg's declared width); PJ301MPort
        // is ~8.03mm (23.7px, res/ComponentLibrary/PJ301M.svg).
        const float subPitch = 10.f;    // mm between sub-columns/rows, ~1.6mm clearance around the ~8.4mm components
        const float edgeMargin = 10.5f; // same left-edge X as LunarPapaSrapa's inputs, for consistency across the pack
        const float topMargin = 15.f, bottomMargin = 9.5f;
        const float usableHeight = 128.5f - topMargin - bottomMargin;
        const float rowPitch = usableHeight / LunarMixer::NUM_CHANNELS;

        float xIn = edgeMargin;
        float xVol = xIn + subPitch;
        float xPan = xIn + 2.5f * subPitch;

        for (int i = 0; i < LunarMixer::NUM_CHANNELS; i++) {
            float y = topMargin + rowPitch * (i + 0.5f);
            addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xIn, y)), module, LunarMixer::CHANNEL_INPUT + i));
            addParam(createParamCentered<RoganMedSmallBlack>(mm2px(Vec(xVol, y)), module, LunarMixer::VOL_PARAM + i));
            addParam(createParamCentered<RoganMedSmallWhite>(mm2px(Vec(xPan, y)), module, LunarMixer::PAN_PARAM + i));
        }

        // Output column: L / master volume / R stacked vertically, centered
        // in the same 12mm/9mm usable band as the channel rows.
        float xOut = 47.28f;
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xOut, topMargin + rowPitch * (3 + 0.5f))), module, LunarMixer::OUTPUT_L));
        addParam(createParamCentered<RoganMedSmallBlack>(mm2px(Vec(xOut, topMargin + rowPitch * (4 + 0.5f))), module, LunarMixer::MASTER_VOL_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xOut, topMargin + rowPitch * (5 + 0.5f))), module, LunarMixer::OUTPUT_R));

        // Panel width: xOut + edgeMargin = 55.4mm, rounded up to 11HP (55.88mm)
        // — was 12HP (60.96mm) before this pass, 13HP (66.04mm) with the
        // VCO A/B dry-thru column before that. Set directly in
        // res/LunarMixer_*.svg (box.size.x is already in px).
    }

    void step() override {
        syncPanelTheme(this, "LunarMixer", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        appendAmbientThemeMenu(menu);
    }
};

Model* modelLunarMixer = createModel<LunarMixer, LunarMixerWidget>("LunarMixer");
