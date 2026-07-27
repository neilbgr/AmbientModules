#include "plugin.hpp"
#include "dsp/PapaSrapaCore.hpp"
#include "dsp/AREnvelope.hpp"
#include "PanelTheme.hpp"

struct LunarPapaSrapa : Module {
    static constexpr float OUTPUT_VOLTAGE = 5.f; // Eurorack +-5V audio convention
    static constexpr float RATE_MIN_OCT = -8.f;
    static constexpr float RATE_MAX_OCT = 4.f;

    int theme = 0;

    enum ParamIds {
        RATE_PARAM,
        RATE_RANGE_PARAM,   // switch: 0 = low, 1 = +5 octaves
        PITCH_PARAM,
        PITCH_RANGE_PARAM,  // switch: 0 = low, 1 = +3 octaves
        FM_PARAM,           // switch: modulator -> audio frequency
        AM_PARAM,           // switch: modulator -> audio amplitude
        MOD_PARAM,          // modulation depth
        DIVIDER_PARAM,      // modulator frequency divider
        NOISE_PARAM,        // independent white noise mix
        ATTACK_PARAM,
        RELEASE_PARAM,
        HOLD_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        GATE_INPUT,
        SH_CLOCK_INPUT,
        SH_INPUT,           // sample & hold source, normalled to the internal noise
        NUM_INPUTS
    };
    enum OutputIds {
        DRY_OUTPUT,
        MAIN_OUTPUT,
        ENV_OUTPUT,
        SH_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        HOLD_LIGHT,
        NUM_LIGHTS
    };

    PapaSrapaCore core;
    AREnvelope envelope;
    dsp::SchmittTrigger gateTrigger;
    dsp::SchmittTrigger clockTrigger;
    float shValue = 0.f;

    LunarPapaSrapa() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configParam(RATE_PARAM, RATE_MIN_OCT, RATE_MAX_OCT, -2.f, "Modulator rate", " Hz", 2.f, 1.f);
        configSwitch(RATE_RANGE_PARAM, 0.f, 1.f, 0.f, "Modulator range", {"Low", "+5 oct"});
        configParam(PITCH_PARAM, -4.f, 3.f, -1.f, "Pitch", " Hz", 2.f, dsp::FREQ_C4);
        configSwitch(PITCH_RANGE_PARAM, 0.f, 1.f, 0.f, "Pitch range", {"Low", "+3 oct"});
        configSwitch(FM_PARAM, 0.f, 1.f, 0.f, "FM", {"Off", "On"});
        configSwitch(AM_PARAM, 0.f, 1.f, 0.f, "AM", {"Off", "On"});
        configParam(MOD_PARAM, 0.f, 1.f, 0.5f, "Modulation depth", "%", 0.f, 100.f);
        configParam(DIVIDER_PARAM, 0.f, 1.f, 0.f, "Divider", "%", 0.f, 100.f);
        configParam(NOISE_PARAM, 0.f, 1.f, 0.f, "Noise", "%", 0.f, 100.f);

        configParam(ATTACK_PARAM, 0.f, 1.f, 0.2f, "Attack", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.2f, "Release", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold (manual gate)", {"Off", "On"});
        configInput(GATE_INPUT, "Gate");

        configInput(SH_CLOCK_INPUT, "Sample & hold clock");
        configInput(SH_INPUT, "Sample & hold input (normalled to internal noise; connect to override)");
        configOutput(SH_OUTPUT, "Sample & hold");

        configOutput(DRY_OUTPUT, "Dry (unenveloped)");
        configOutput(MAIN_OUTPUT, "Main (enveloped)");
        configOutput(ENV_OUTPUT, "Envelope");
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
        float rateOctaves = params[RATE_PARAM].getValue() + (params[RATE_RANGE_PARAM].getValue() > 0.f ? 5.f : 0.f);
        float pitchOctaves = params[PITCH_PARAM].getValue() + (params[PITCH_RANGE_PARAM].getValue() > 0.f ? 3.f : 0.f);

        bool fm = params[FM_PARAM].getValue() > 0.f;
        bool am = params[AM_PARAM].getValue() > 0.f;
        PapaSrapaCore::ModMode mode = fm && am ? PapaSrapaCore::MODE_FM_AM
                                    : fm ? PapaSrapaCore::MODE_FM
                                    : am ? PapaSrapaCore::MODE_AM
                                    : PapaSrapaCore::MODE_OFF;

        // Unlike Lunar50Drone's oscillator bank, this core is already cheap
        // (2 square oscillators + a noise sample, no sin/asin) and the noise
        // sample must stay continuously fresh for the S&H below even when
        // the envelope is at 0 (silent gate) or DRY_OUTPUT is the only thing
        // patched — so it always runs, matching LunarVCO's approach rather
        // than Lunar50Drone's envelope-gated skip.
        float dry = core.process(args.sampleTime, args.sampleRate, rateOctaves,
            params[DIVIDER_PARAM].getValue(), pitchOctaves, params[MOD_PARAM].getValue(),
            mode, params[NOISE_PARAM].getValue());

        if (clockTrigger.process(inputs[SH_CLOCK_INPUT].getVoltage())) {
            shValue = inputs[SH_INPUT].isConnected() ? inputs[SH_INPUT].getVoltage() / OUTPUT_VOLTAGE : core.noise;
        }
        outputs[SH_OUTPUT].setVoltage(shValue * OUTPUT_VOLTAGE);

        gateTrigger.process(inputs[GATE_INPUT].getVoltage());
        bool holdActive = params[HOLD_PARAM].getValue() > 0.f;
        bool gateHigh = gateTrigger.isHigh() || holdActive;

        envelope.updateCoefficients(params[ATTACK_PARAM].getValue(), params[RELEASE_PARAM].getValue());
        float envValue = envelope.process(args.sampleTime, gateHigh);
        float envAmount = clamp(envValue, 0.f, 1.f);
        lights[HOLD_LIGHT].setBrightness(envAmount);

        outputs[DRY_OUTPUT].setVoltage(dry * OUTPUT_VOLTAGE);
        outputs[MAIN_OUTPUT].setVoltage(dry * OUTPUT_VOLTAGE * envAmount);
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

        addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(10.f, 20.f)), module, LunarPapaSrapa::RATE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(20.f, 20.f)), module, LunarPapaSrapa::RATE_RANGE_PARAM));
        addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(32.f, 20.f)), module, LunarPapaSrapa::PITCH_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(42.f, 20.f)), module, LunarPapaSrapa::PITCH_RANGE_PARAM));

        addParam(createParamCentered<CKSS>(mm2px(Vec(12.f, 35.f)), module, LunarPapaSrapa::FM_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(22.f, 35.f)), module, LunarPapaSrapa::AM_PARAM));
        addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(32.f, 35.f)), module, LunarPapaSrapa::MOD_PARAM));
        addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(42.f, 35.f)), module, LunarPapaSrapa::DIVIDER_PARAM));

        addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(15.f, 50.f)), module, LunarPapaSrapa::NOISE_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(10.f, 65.f)), module, LunarPapaSrapa::GATE_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(20.f, 65.f)), module, LunarPapaSrapa::SH_CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.f, 65.f)), module, LunarPapaSrapa::SH_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(40.f, 65.f)), module, LunarPapaSrapa::SH_OUTPUT));

        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.f, 80.f)), module, LunarPapaSrapa::ATTACK_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(22.f, 80.f)), module, LunarPapaSrapa::RELEASE_PARAM));
        addParam(createLightParamCentered<VCVLightBezelLatch<YellowLight>>(mm2px(Vec(34.f, 80.f)), module, LunarPapaSrapa::HOLD_PARAM, LunarPapaSrapa::HOLD_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(46.f, 80.f)), module, LunarPapaSrapa::ENV_OUTPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.f, 100.f)), module, LunarPapaSrapa::DRY_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(35.f, 100.f)), module, LunarPapaSrapa::MAIN_OUTPUT));
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
