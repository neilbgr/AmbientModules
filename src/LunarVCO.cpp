#include "plugin.hpp"
#include "dsp/LunarVCOCore.hpp"
#include "dsp/ADSREnvelope.hpp"
#include "PanelTheme.hpp"

struct LunarVCO : Module {
    int theme = 0;

    enum ParamIds {
        WAVEFORM_PARAM,
        TUNE_PARAM,
        OCTAVE_PARAM,         // switch: 0 = low, 1 = +3 octaves
        SUB_PARAM,            // switch: sub-oscillator on/off
        LINEXP_PARAM,         // switch: 0 = lin, 1 = exp (secondary CV input)
        SHAPE_PARAM,          // pulse width OR morph blend, depending on waveform
        SHAPE_CV_ATTEN_PARAM,
        FM_CV_ATTEN_PARAM,
        ATTACK_PARAM,
        DECAY_PARAM,
        SUSTAIN_PARAM,
        RELEASE_PARAM,
        HOLD_PARAM,
        SELFGEN_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        VOCT_INPUT,
        FM_INPUT,             // secondary "cv" jack (linear or exponential FM)
        SHAPE_CV_INPUT,
        SYNC_INPUT,
        GATE_INPUT,
        ENV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        VCO_OUTPUT,           // enveloped (VCA'd) main output
        ENV_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        OCTAVE_LIGHT,
        SUB_LIGHT,
        LINEXP_LIGHT,
        HOLD_LIGHT,
        SELFGEN_LIGHT,
        NUM_LIGHTS
    };

    LunarVCOCore vco;
    ADSREnvelope env;

    LunarVCO() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        configSwitch(WAVEFORM_PARAM, 0.f, 5.f, 0.f, "Waveform",
            {"Sine", "Triangle", "Inverted saw", "Square", "Saw to inv. saw (shape = blend)", "Sine to triangle (shape = blend)"});
        configParam(TUNE_PARAM, -0.5f, 0.5f, 0.f, "Tune (1 octave range)");
        configSwitch(OCTAVE_PARAM, 0.f, 1.f, 0.f, "Octave", {"Low", "+3"});
        configSwitch(SUB_PARAM, 0.f, 1.f, 0.f, "Sub oscillator", {"Off", "On (-1 octave)"});
        configSwitch(LINEXP_PARAM, 0.f, 1.f, 0.f, "FM mode", {"Linear", "Exponential"});
        configParam(SHAPE_PARAM, 0.f, 1.f, 0.5f, "Shape (pulse width / morph blend)");
        configParam(SHAPE_CV_ATTEN_PARAM, -1.f, 1.f, 0.f, "Shape CV amount", "%", 0.f, 100.f);
        configParam(FM_CV_ATTEN_PARAM, -1.f, 1.f, 0.f, "FM amount", "%", 0.f, 100.f);
        configInput(VOCT_INPUT, "V/oct");
        configInput(FM_INPUT, "FM");
        configInput(SHAPE_CV_INPUT, "Shape CV");
        configInput(SYNC_INPUT, "Sync");
        configOutput(VCO_OUTPUT, "Audio Out");

        configParam(ATTACK_PARAM, 0.f, 1.f, 0.2f, "Attack", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.2f, "Decay", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.7f, "Sustain", "%", 0.f, 100.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.2f, "Release", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold", {"Off", "On"});
        configSwitch(SELFGEN_PARAM, 0.f, 1.f, 0.f, "Self-generation", {"Off", "On"});
        configInput(GATE_INPUT, "Gate");
        configOutput(ENV_OUTPUT, "Envelope Out");
        configInput(ENV_INPUT, "Envelope In (overrides internal envelope, gate and hold when connected)");
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
        float pitch = params[TUNE_PARAM].getValue() + inputs[VOCT_INPUT].getVoltage();

        bool expMode = params[LINEXP_PARAM].getValue() > 0.f;
        float fmCv = inputs[FM_INPUT].getVoltage() * params[FM_CV_ATTEN_PARAM].getValue();
        float expFm = expMode ? fmCv : 0.f;
        float linFm = expMode ? 0.f : fmCv * 100.f; // Hz scale, adjustable by ear

        float shape = clamp(params[SHAPE_PARAM].getValue()
            + inputs[SHAPE_CV_INPUT].getVoltage() / 10.f * params[SHAPE_CV_ATTEN_PARAM].getValue(), 0.f, 1.f);

        float dry = vco.process(args.sampleTime, args.sampleRate, pitch, expFm, linFm,
            (int)params[WAVEFORM_PARAM].getValue(), shape,
            params[OCTAVE_PARAM].getValue() > 0.f, params[SUB_PARAM].getValue() > 0.f, inputs[SYNC_INPUT].getVoltage());

        float envValue;
        if (inputs[ENV_INPUT].isConnected()) {
            envValue = clamp(inputs[ENV_INPUT].getVoltage() / 10.f, 0.f, 1.f);
        } else {
            bool gate = inputs[GATE_INPUT].getVoltage() >= 1.f;
            envValue = env.process(args.sampleTime, gate,
                params[SELFGEN_PARAM].getValue() > 0.f, params[HOLD_PARAM].getValue() > 0.f,
                params[ATTACK_PARAM].getValue(), params[DECAY_PARAM].getValue(),
                params[SUSTAIN_PARAM].getValue(), params[RELEASE_PARAM].getValue());
        }

        lights[OCTAVE_LIGHT].setBrightness(params[OCTAVE_PARAM].getValue() > 0.f ? 1.f : 0.f);
        lights[SUB_LIGHT].setBrightness(params[SUB_PARAM].getValue() > 0.f ? 1.f : 0.f);
        lights[LINEXP_LIGHT].setBrightness(params[LINEXP_PARAM].getValue() > 0.f ? 1.f : 0.f);
        lights[HOLD_LIGHT].setBrightness(envValue);
        lights[SELFGEN_LIGHT].setBrightness(params[SELFGEN_PARAM].getValue() > 0.f ? 1.f : 0.f);

        outputs[VCO_OUTPUT].setVoltage(dry * 5.f * envValue);
        outputs[ENV_OUTPUT].setVoltage(envValue * 10.f);
    }
};

struct LunarVCOWidget : ModuleWidget {
    LunarVCOWidget(LunarVCO* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("LunarVCO", module ? module->theme : 0))));

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        
        const float x1div3 = 11.37f;
        const float x2div3 = 22.84f;
        const float x3div3 = 34.39f;

        const float x1div4 = 7.3f;
        const float x2div4 = 17.57f;
        const float x3div4 = 28.f;
        const float x4div4 = 38.29f;

        const float pnl1_y1 = 24.f;
        const float pnl1_y2 = 34.5f;
        const float pnl1_y3 = 43.f;

        const float pnl2_y1 = 62.f;
        const float pnl2_y2 = 73.f;

        const float pnl3_y1 = 90.f;
        const float pnl3_y2 = 100.f;
        const float pnl3_y3 = 113.f;

        addParam(createParamCentered<Rogan2PSGreen>(mm2px(Vec(14.5f, 27.5f)), module, LunarVCO::WAVEFORM_PARAM));
        
        addParam(createParamCentered<Rogan1PGreen>(mm2px(Vec(x3div3, pnl1_y1)), module, LunarVCO::SHAPE_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(x3div3, pnl1_y2)), module, LunarVCO::SHAPE_CV_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl1_y3)), module, LunarVCO::SHAPE_CV_INPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl1_y3)), module, LunarVCO::SYNC_INPUT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl1_y3)), module, LunarVCO::SUB_PARAM, LunarVCO::SUB_LIGHT));

        addParam(createParamCentered<Rogan1PGreen>(mm2px(Vec(x1div3, pnl2_y1)), module, LunarVCO::TUNE_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl2_y2)), module, LunarVCO::VOCT_INPUT));  

        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl2_y1)), module, LunarVCO::OCTAVE_PARAM, LunarVCO::OCTAVE_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl2_y2)), module, LunarVCO::LINEXP_PARAM, LunarVCO::LINEXP_LIGHT));

        addParam(createParamCentered<Rogan1PGreen>(mm2px(Vec(x3div3, pnl2_y1)), module, LunarVCO::FM_CV_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl2_y2)), module, LunarVCO::FM_INPUT));
        
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x1div4, pnl3_y1)), module, LunarVCO::ATTACK_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x2div4, pnl3_y1)), module, LunarVCO::DECAY_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x3div4, pnl3_y1)), module, LunarVCO::SUSTAIN_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(x4div4, pnl3_y1)), module, LunarVCO::RELEASE_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl3_y2)), module, LunarVCO::ENV_INPUT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(x2div3, pnl3_y2)), module, LunarVCO::SELFGEN_PARAM, LunarVCO::SELFGEN_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl3_y2)), module, LunarVCO::ENV_OUTPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x1div3, pnl3_y3)), module, LunarVCO::GATE_INPUT));
        addParam(createLightParamCentered<VCVLightBezelLatch<YellowLight>>(mm2px(Vec(x2div3, pnl3_y3)), module, LunarVCO::HOLD_PARAM, LunarVCO::HOLD_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x3div3, pnl3_y3)), module, LunarVCO::VCO_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        LunarVCO* module = dynamic_cast<LunarVCO*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
            [=]() { return module->theme; },
            [=](int theme) {
                module->theme = theme;
                setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("LunarVCO", theme))));
            }
        ));
    }
};

Model* modelLunarVCO = createModel<LunarVCO, LunarVCOWidget>("LunarVCO");
