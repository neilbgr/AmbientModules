#include "plugin.hpp"
#include "dsp/PapaSrapaCore.hpp"
#include "dsp/AREnvelope.hpp"
#include "PanelTheme.hpp"

// A plain octave-linear (dB-per-mm) knob spends as much rotation on the last
// octave (top half of the range in Hz) as on the first — fine for a wide
// pitch control, but it left too little of the LFO knob's travel usable near
// its upper end. minOct/maxOct/curveExp are set right after configParam<>();
// raw param value is a normalized knob position in [0, 1], reshaped by
// pow(pos, curveExp) before being mapped onto the octave range. curveExp < 1
// front-loads octaves at low knob positions, leaving more travel for the top.
struct RateParamQuantity : ParamQuantity {
    float minOct = 0.f, maxOct = 1.f, curveExp = 1.f;

    float posToOctaves(float pos) const {
        return minOct + (maxOct - minOct) * std::pow(clamp(pos, 0.f, 1.f), curveExp);
    }
    float octavesToPos(float octaves) const {
        float pos = (octaves - minOct) / (maxOct - minOct);
        return std::pow(clamp(pos, 0.f, 1.f), 1.f / curveExp);
    }
    float getDisplayValue() override {
        return std::pow(2.f, posToOctaves(getValue()));
    }
    void setDisplayValue(float hz) override {
        setValue(octavesToPos(std::log2(std::max(hz, 1e-6f))));
    }
};

struct LunarPapaSrapa : Module {
    static constexpr float OUTPUT_VOLTAGE = 5.f; // Eurorack +-5V audio convention
    static constexpr float RATE_MIN_OCT = 0.f;     // 1 Hz: below this, FM/AM cross-mod barely reads as motion
    static constexpr float RATE_MAX_OCT = 8.0314f; // C4 (~261.63 Hz): upper bound
    // NOTE: the core treats RATE_PARAM as octaves relative to 1 Hz (not C4),
    // so 0.f here means 1 Hz, not 1 Hz-relative-to-C4 — hence the explicit
    // log2(261.63) value for the upper bound.
    static constexpr float RATE_CURVE_EXP = 0.6f; // <1: more knob travel near the top of the range
    static constexpr float PITCH_MIN_OCT = -4.f;      // C0 (~16.35 Hz)
    static constexpr float PITCH_MAX_OCT = 10.f / 3.f; // E7 (~2637 Hz), per solar42f_instruct_03_25_v9.pdf p.8

    int theme = 0;

    enum ParamIds {
        RATE_PARAM,         // LFO rate, single knob from 1 Hz up to C4
        PITCH_PARAM,        // audio oscillator pitch, single knob spanning C0..E7
        FM_PARAM,           // switch: modulator -> audio frequency
        AM_PARAM,           // switch: modulator -> audio amplitude
        MOD_PARAM,          // modulation depth
        DIVIDER_PARAM,      // modulator frequency divider
        NOISE_PARAM,        // independent white noise mix
        NOISE_ONLY_PARAM,   // switch: force clean white-noise-only output
        ATTACK_PARAM,
        RELEASE_PARAM,
        HOLD_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        GATE_INPUT,
        PITCH_CV_INPUT,     // 1V/oct, sums onto PITCH_PARAM
        SH_CLOCK_INPUT,
        SH_INPUT,           // sample & hold source, normalled to the internal noise
        ENV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        LFO_OUTPUT,
        VCO_OUT,
        ENV_OUTPUT,
        SH_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        HOLD_LIGHT,
        ENV_LIGHT,
        NOISE_ONLY_LIGHT,
        FM_LIGHT,
        AM_LIGHT,
        NUM_LIGHTS
    };

    PapaSrapaCore core;
    AREnvelope envelope;
    dsp::SchmittTrigger gateTrigger;
    dsp::SchmittTrigger clockTrigger;
    float shValue = 0.f;

    LunarPapaSrapa() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        float rateDefaultPos = std::pow((2.f - RATE_MIN_OCT) / (RATE_MAX_OCT - RATE_MIN_OCT), 1.f / RATE_CURVE_EXP);
        RateParamQuantity* rateQ = configParam<RateParamQuantity>(RATE_PARAM, 0.f, 1.f, rateDefaultPos, "LFO rate", " Hz");
        rateQ->minOct = RATE_MIN_OCT;
        rateQ->maxOct = RATE_MAX_OCT;
        rateQ->curveExp = RATE_CURVE_EXP;
        configParam(PITCH_PARAM, PITCH_MIN_OCT, PITCH_MAX_OCT, 0.f, "Pitch", " Hz", 2.f, dsp::FREQ_C4);
        configInput(PITCH_CV_INPUT, "Pitch CV (1V/oct)");
        configSwitch(FM_PARAM, 0.f, 1.f, 0.f, "FM", {"Off", "On"});
        configSwitch(AM_PARAM, 0.f, 1.f, 0.f, "AM", {"Off", "On"});
        configParam(MOD_PARAM, 0.f, 1.f, 0.5f, "Modulation depth", "%", 0.f, 100.f);
        configParam(DIVIDER_PARAM, 0.f, 1.f, 0.f, "Divider", "%", 0.f, 100.f);
        configParam(NOISE_PARAM, 0.f, 1.f, 0.f, "Noise", "%", 0.f, 100.f);
        configSwitch(NOISE_ONLY_PARAM, 0.f, 1.f, 0.f, "Noise only", {"Off", "On"});

        configParam(ATTACK_PARAM, 0.f, 1.f, 0.2f, "Attack", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.2f, "Release", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold (manual gate)", {"Off", "On"});
        configInput(GATE_INPUT, "Gate");

        configInput(SH_CLOCK_INPUT, "Sample & hold clock");
        configInput(SH_INPUT, "Sample & hold input (normalled to internal noise; connect to override)");
        configOutput(SH_OUTPUT, "Sample & hold");

        configOutput(LFO_OUTPUT, "LFO");
        configOutput(VCO_OUT, "VCO (enveloped)");
        configOutput(ENV_OUTPUT, "Envelope");
        configInput(ENV_INPUT, "Envelope CV input (normalled to internal envelope; connect to override)");
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "theme", json_integer(theme));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* themeJ = json_object_get(rootJ, "theme");
        if (themeJ) {
            theme = json_integer_value(themeJ);
        }
    }

    void process(const ProcessArgs& args) override {
        float rateOctaves = RATE_MIN_OCT + (RATE_MAX_OCT - RATE_MIN_OCT) * std::pow(params[RATE_PARAM].getValue(), RATE_CURVE_EXP);
        float pitchOctaves = params[PITCH_PARAM].getValue() + inputs[PITCH_CV_INPUT].getVoltage();

        bool fm = params[FM_PARAM].getValue() > 0.f;
        bool am = params[AM_PARAM].getValue() > 0.f;
        PapaSrapaCore::ModMode mode = fm && am ? PapaSrapaCore::MODE_FM_AM
                                    : fm ? PapaSrapaCore::MODE_FM
                                    : am ? PapaSrapaCore::MODE_AM
                                    : PapaSrapaCore::MODE_OFF;
        lights[FM_LIGHT].setBrightness(fm ? 1.f : 0.f);
        lights[AM_LIGHT].setBrightness(am ? 1.f : 0.f);

        bool noiseOnly = params[NOISE_ONLY_PARAM].getValue() > 0.f;
        lights[NOISE_ONLY_LIGHT].setBrightness(noiseOnly ? 1.f : 0.f);

        // Unlike Lunar50Drone's oscillator bank, this core is already cheap
        // (2 square oscillators + a noise sample, no sin/asin) and the noise
        // sample must stay continuously fresh for the S&H below even when
        // the envelope is at 0 (silent gate) — so it always runs, matching
        // LunarVCO's approach rather than Lunar50Drone's envelope-gated skip.
        float vco = core.process(args.sampleTime, args.sampleRate, rateOctaves,
            params[DIVIDER_PARAM].getValue(), pitchOctaves, params[MOD_PARAM].getValue(),
            mode, params[NOISE_PARAM].getValue(), noiseOnly);

        if (clockTrigger.process(inputs[SH_CLOCK_INPUT].getVoltage())) {
            shValue = inputs[SH_INPUT].isConnected() ? inputs[SH_INPUT].getVoltage() / OUTPUT_VOLTAGE : core.noise;
        }
        outputs[SH_OUTPUT].setVoltage(shValue * OUTPUT_VOLTAGE);
        outputs[LFO_OUTPUT].setVoltage(core.lfoOut * OUTPUT_VOLTAGE);

        bool holdActive = params[HOLD_PARAM].getValue() > 0.f;
        bool envInputConnected = inputs[ENV_INPUT].isConnected();
        float envValue;
        if (envInputConnected) {
            envValue = inputs[ENV_INPUT].getVoltage() / 10.f;
        } else {
            gateTrigger.process(inputs[GATE_INPUT].getVoltage());
            bool gateHigh = inputs[GATE_INPUT].isConnected() ? gateTrigger.isHigh() : holdActive;
            envelope.updateCoefficients(params[ATTACK_PARAM].getValue(), params[RELEASE_PARAM].getValue());
            envValue = envelope.process(args.sampleTime, gateHigh);
        }

        float envAmount = clamp(envValue, 0.f, 1.f);
        lights[HOLD_LIGHT].setBrightness(holdActive ? 1.f : 0.f);
        lights[ENV_LIGHT].setBrightness(envAmount);

        outputs[VCO_OUT].setVoltage(vco * OUTPUT_VOLTAGE * envAmount);
        outputs[ENV_OUTPUT].setVoltage(envValue * 10.f);
    }
};

struct LunarPapaSrapaWidget : ModuleWidget, ThemedModuleWidget {
    int appliedTheme = -1;

    LunarPapaSrapaWidget(LunarPapaSrapa* module) {
        setModule(module);
        syncPanelTheme(this, "LunarPapaSrapa", module ? module->theme : 0, appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        float W = 52.318f;
        float C = 55.88f / 2.f;
        float W_6 = W / 6.f;
        
        float x1_5 = C - 2.f * W_6, x2_5 = C - W_6, x3_5 = C, x4_5 = C + W_6, x5_5 = C + 2.f * W_6;

        float x1_4 = x1_5, x2_4 = x1_5 + ((x5_5 - x1_5) / 3.f), x3_4 = x1_5 + ((x5_5 - x1_5) * 2.f / 3.f), x4_4 = x5_5;

        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x1_5, 24.f)), module, LunarPapaSrapa::RATE_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2_5, 30.f)), module, LunarPapaSrapa::FM_PARAM, LunarPapaSrapa::FM_LIGHT));
        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x3_5, 24.f)), module, LunarPapaSrapa::MOD_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x4_5, 30.f)), module, LunarPapaSrapa::AM_PARAM, LunarPapaSrapa::AM_LIGHT));
        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x5_5, 24.f)), module, LunarPapaSrapa::DIVIDER_PARAM));        
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x1_5, 38.f)), module, LunarPapaSrapa::LFO_OUTPUT));
        
        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x1_5, 55.25f)), module, LunarPapaSrapa::PITCH_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_5, 67.f)), module, LunarPapaSrapa::PITCH_CV_INPUT));

        addParam(createParamCentered<Rogan1PBlue>(mm2px(Vec(x3_5, 55.25f)), module, LunarPapaSrapa::NOISE_PARAM));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x3_5, 67.f)), module, LunarPapaSrapa::NOISE_ONLY_PARAM, LunarPapaSrapa::NOISE_ONLY_LIGHT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_5, 82.5f)), module, LunarPapaSrapa::SH_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3_5, 82.5f)), module, LunarPapaSrapa::SH_CLOCK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x5_5, 82.5f)), module, LunarPapaSrapa::SH_OUTPUT));
        
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_4, 98.f)), module, LunarPapaSrapa::ENV_INPUT));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x2_4, 98.f)), module, LunarPapaSrapa::ATTACK_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x3_4, 98.f)), module, LunarPapaSrapa::RELEASE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x4_4, 98.f)), module, LunarPapaSrapa::ENV_OUTPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1_4, 113.f)), module, LunarPapaSrapa::GATE_INPUT));
        addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(x2_4 - 5.5f, 113.f - 2.f )), module, LunarPapaSrapa::ENV_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2_4, 113.f)), module, LunarPapaSrapa::HOLD_PARAM, LunarPapaSrapa::HOLD_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x4_4, 113.f)), module, LunarPapaSrapa::VCO_OUT));

    }

    void step() override {
        if (module) syncPanelTheme(this, "LunarPapaSrapa", dynamic_cast<LunarPapaSrapa*>(module)->theme, appliedTheme);
        ModuleWidget::step();
    }

    void applyTheme(int theme, history::ComplexAction* complexAction = nullptr) override {
        LunarPapaSrapa* module = dynamic_cast<LunarPapaSrapa*>(this->module);
        pushIntFieldChange(module, "change theme", module->theme, theme,
            [](engine::Module* m, int v) { dynamic_cast<LunarPapaSrapa*>(m)->theme = v; }, complexAction);
        module->theme = theme;
        appliedTheme = theme;
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("LunarPapaSrapa", theme))));
    }

    void appendContextMenu(Menu* menu) override {
        LunarPapaSrapa* module = dynamic_cast<LunarPapaSrapa*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
            [=]() { return module->theme; },
            [=](int theme) { applyTheme(theme); }
        ));
        appendApplyThemeToAllItem(menu, module->theme);
    }
};

Model* modelLunarPapaSrapa = createModel<LunarPapaSrapa, LunarPapaSrapaWidget>("LunarPapaSrapa");
