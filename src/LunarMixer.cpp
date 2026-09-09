#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "Widgets.hpp"
#include "dsp/EnvelopeFollower.hpp"

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

    static const int NUM_METER_LEDS = 16;

    enum ParamIds {
        ENUMS(PAN_PARAM, NUM_CHANNELS),
        ENUMS(VOL_PARAM, NUM_CHANNELS),
        MASTER_VOL_PARAM,
        VU_PEAK_PARAM,
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
    enum LightIds {
        ENUMS(METER_L_LIGHT, NUM_METER_LEDS),
        ENUMS(METER_R_LIGHT, NUM_METER_LEDS),
        NUM_LIGHTS
    };

    float gainL[NUM_CHANNELS] = {};
    float gainR[NUM_CHANNELS] = {};
    // Constant-power pan law recomputed only every 16 samples (see process())
    // — same throttling convention as AREnvelope::lambdaFromKnob, negligible
    // either way for 9 channels but kept consistent with the project's
    // "throttle expensive math" priority.
    dsp::ClockDivider panDivider;

    // Post-master-volume level meter. The envelope itself must run every
    // sample for correct ballistics; only the 32 LED brightness updates are
    // throttled (see process()) — negligible cost either way, but kept
    // consistent with panDivider's "throttle the visual, not the DSP" style.
    EnvelopeFollower meterFollowerL, meterFollowerR;
    dsp::ClockDivider meterLightDivider;

    // -inf..+9dB in 3dB steps (16 LEDs), normalized so 1.0 == +9dB: the
    // module's usual 0dB/unity reference is 5V (matching LunarFilter's own
    // /5.f convention elsewhere in the pack), so full scale here is
    // 5V * 10^(+9dB/20dB) ~= 14.09V — this lets the meter show 3 LEDs of
    // headroom above 0dB before EnvelopeFollower's internal [0,1] clamp is
    // ever reached. Values are 10^((dB-9)/20) for dB = -36, -33, ..., +9.
    static constexpr float METER_FULLSCALE_V = 14.0919f;
    static constexpr float METER_THRESHOLDS[NUM_METER_LEDS] = {
        0.005623f, 0.007943f, 0.011220f, 0.015849f, 0.022387f, 0.031623f, 0.044668f, 0.063096f,
        0.089125f, 0.125893f, 0.177828f, 0.251189f, 0.354813f, 0.501187f, 0.707946f, 1.000000f
    };
    // idx 0-9: green (-36..-9dB), idx 10-12: yellow (-6..0dB), idx 13-15: red (+3..+9dB).
    static const int METER_YELLOW_START = 10;
    static const int METER_RED_START = 13;

    // Standard VU ballistics (ANSI C16.5): symmetric 300ms attack/release.
    static constexpr float VU_LAMBDA = 1.f / 0.3f;
    // Peak: near-instant attack, ~300ms release for readability — adjust by
    // ear/eye like every other envelope timing in this pack.
    static constexpr float PEAK_ATTACK_LAMBDA = 1.f / 0.001f;
    static constexpr float PEAK_RELEASE_LAMBDA = 1.f / 0.3f;

    LunarMixer() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        panDivider.setDivision(16);
        meterLightDivider.setDivision(32);
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
        configSwitch(VU_PEAK_PARAM, 0.f, 1.f, 0.f, "Meter ballistics", {"VU", "Peak"});
        configOutput(OUTPUT_L, "Left");
        configOutput(OUTPUT_R, "Right");
    }

    void updateMeterLights(int firstLightId, float level) {
        for (int i = 0; i < NUM_METER_LEDS; i++) {
            float low = (i == 0) ? 0.f : METER_THRESHOLDS[i - 1];
            float high = METER_THRESHOLDS[i];
            lights[firstLightId + i].setBrightness(clamp((level - low) / (high - low), 0.f, 1.f));
        }
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
        float finalL = outL * masterVol;
        float finalR = outR * masterVol;
        outputs[OUTPUT_L].setVoltage(finalL);
        outputs[OUTPUT_R].setVoltage(finalR);

        bool peakMode = params[VU_PEAK_PARAM].getValue() > 0.5f;
        float attackLambda = peakMode ? PEAK_ATTACK_LAMBDA : VU_LAMBDA;
        float releaseLambda = peakMode ? PEAK_RELEASE_LAMBDA : VU_LAMBDA;
        float envL = meterFollowerL.process(args.sampleTime, finalL / METER_FULLSCALE_V, attackLambda, releaseLambda);
        float envR = meterFollowerR.process(args.sampleTime, finalR / METER_FULLSCALE_V, attackLambda, releaseLambda);

        if (meterLightDivider.process()) {
            updateMeterLights(METER_L_LIGHT, envL);
            updateMeterLights(METER_R_LIGHT, envR);
        }
    }
};

// Out-of-class definition required in C++11 for a static constexpr array
// member that's odr-used (indexed in a loop, as METER_THRESHOLDS is above).
constexpr float LunarMixer::METER_THRESHOLDS[LunarMixer::NUM_METER_LEDS];

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

        float xOut = 47.28f;
        addParam(createParamCentered<RoganMedSmallBlack>(mm2px(Vec(xOut, topMargin + rowPitch * (6 + 0.5f))), module, LunarMixer::MASTER_VOL_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xOut, topMargin + rowPitch * (7 + 0.5f))), module, LunarMixer::OUTPUT_L));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xOut, topMargin + rowPitch * (8 + 0.5f))), module, LunarMixer::OUTPUT_R));

        // VU/Peak meter: 2 columns of 16 LEDs (L/R), in the reserved
        // 42.28-52.28mm x 13-78mm zone; ballistics switch above MASTER_VOL.
        addParam(createParamCentered<CKSSHorizontal>(mm2px(Vec(xOut, 79.f)), module, LunarMixer::VU_PEAK_PARAM));

        const float meterTop = 13.f, meterBottom = 73.f;
        const float meterPitch = (meterBottom - meterTop) / LunarMixer::NUM_METER_LEDS;
        const float xMeterL = 44.78f, xMeterR = 49.78f;
        for (int i = 0; i < LunarMixer::NUM_METER_LEDS; i++) {
            // i=0 at the bottom (lowest dB), i=NUM_METER_LEDS-1 at the top.
            float y = meterBottom - meterPitch * (i + 0.5f);
            if (i < LunarMixer::METER_YELLOW_START) {
                addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(xMeterL, y)), module, LunarMixer::METER_L_LIGHT + i));
                addChild(createLightCentered<SmallLight<GreenLight>>(mm2px(Vec(xMeterR, y)), module, LunarMixer::METER_R_LIGHT + i));
            } else if (i < LunarMixer::METER_RED_START) {
                addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(xMeterL, y)), module, LunarMixer::METER_L_LIGHT + i));
                addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(xMeterR, y)), module, LunarMixer::METER_R_LIGHT + i));
            } else {
                addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(xMeterL, y)), module, LunarMixer::METER_L_LIGHT + i));
                addChild(createLightCentered<SmallLight<RedLight>>(mm2px(Vec(xMeterR, y)), module, LunarMixer::METER_R_LIGHT + i));
            }
        }
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
