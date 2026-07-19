#include "plugin.hpp"
#include "dsp/SolarVCOCore.hpp"
#include "dsp/ADSREnvelope.hpp"
#include "PanelTheme.hpp"

struct SolarVCO : Module {
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
        NUM_INPUTS
    };
    enum OutputIds {
        VCO_OUTPUT,           // enveloped (VCA'd) main output
        DRY_OUTPUT,           // raw, unenveloped oscillator output
        ENV_OUTPUT,
        NUM_OUTPUTS
    };

    SolarVCOCore vco;
    ADSREnvelope env;

    SolarVCO() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);

        configSwitch(WAVEFORM_PARAM, 0.f, 5.f, 0.f, "Waveform",
            {"Sine", "Triangle", "Inverted saw", "Square", "Saw to inv. saw (shape = blend)", "Sine to triangle (shape = blend)"});
        configParam(TUNE_PARAM, 0.f, 1.f, 0.5f, "Tune");
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
        configOutput(DRY_OUTPUT, "Dry");
        configOutput(VCO_OUTPUT, "VCO (enveloped)");

        configParam(ATTACK_PARAM, 0.f, 1.f, 0.2f, "Attack", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configParam(DECAY_PARAM, 0.f, 1.f, 0.2f, "Decay", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configParam(SUSTAIN_PARAM, 0.f, 1.f, 0.7f, "Sustain", "%", 0.f, 100.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.2f, "Release", " ms", ADSREnvelope::MAX_TIME / ADSREnvelope::MIN_TIME, ADSREnvelope::MIN_TIME * 1000.f);
        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold", {"Off", "On"});
        configSwitch(SELFGEN_PARAM, 0.f, 1.f, 0.f, "Self-generation", {"Off", "On"});
        configInput(GATE_INPUT, "Gate");
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
        float octaveOffset = params[OCTAVE_PARAM].getValue() > 0.f ? 3.f : 0.f;
        float pitch = params[TUNE_PARAM].getValue() + octaveOffset + inputs[VOCT_INPUT].getVoltage();

        bool expMode = params[LINEXP_PARAM].getValue() > 0.f;
        float fmCv = inputs[FM_INPUT].getVoltage() * params[FM_CV_ATTEN_PARAM].getValue();
        float expFm = expMode ? fmCv : 0.f;
        float linFm = expMode ? 0.f : fmCv * 100.f; // Hz scale, adjustable by ear

        float shape = clamp(params[SHAPE_PARAM].getValue()
            + inputs[SHAPE_CV_INPUT].getVoltage() / 10.f * params[SHAPE_CV_ATTEN_PARAM].getValue(), 0.f, 1.f);

        float dry = vco.process(args.sampleTime, args.sampleRate, pitch, expFm, linFm,
            (int)params[WAVEFORM_PARAM].getValue(), shape,
            params[SUB_PARAM].getValue() > 0.f, inputs[SYNC_INPUT].getVoltage());

        bool gate = inputs[GATE_INPUT].getVoltage() >= 1.f;
        float envValue = env.process(args.sampleTime, gate,
            params[SELFGEN_PARAM].getValue() > 0.f, params[HOLD_PARAM].getValue() > 0.f,
            params[ATTACK_PARAM].getValue(), params[DECAY_PARAM].getValue(),
            params[SUSTAIN_PARAM].getValue(), params[RELEASE_PARAM].getValue());

        outputs[DRY_OUTPUT].setVoltage(dry * 5.f);
        outputs[VCO_OUTPUT].setVoltage(dry * 5.f * envValue);
        outputs[ENV_OUTPUT].setVoltage(envValue * 10.f);
    }
};

struct SolarVCOWidget : ModuleWidget {
    SolarVCOWidget(SolarVCO* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("SolarVCO", module ? module->theme : 0))));

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        
        addParam(createParamCentered<Rogan3PSGreen>(mm2px(Vec(25.5f, 25.5f)), module, SolarVCO::WAVEFORM_PARAM));
        addParam(createParamCentered<Rogan2PSGreen>(mm2px(Vec(50.f, 25.5f)), module, SolarVCO::TUNE_PARAM));

        addParam(createParamCentered<CKSS>(mm2px(Vec(8.f, 44.f)), module, SolarVCO::OCTAVE_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(15.f, 44.f)), module, SolarVCO::SUB_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(22.f, 44.f)), module, SolarVCO::LINEXP_PARAM));

        addParam(createParamCentered<Rogan2PSGreen>(mm2px(Vec(8.f, 56.f)), module, SolarVCO::SHAPE_PARAM));
        addParam(createParamCentered<Rogan1PSGreen>(mm2px(Vec(15.f, 56.f)), module, SolarVCO::SHAPE_CV_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.f, 56.f)), module, SolarVCO::SHAPE_CV_INPUT));

        addParam(createParamCentered<Rogan1PSGreen>(mm2px(Vec(8.f, 66.f)), module, SolarVCO::FM_CV_ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.f, 66.f)), module, SolarVCO::FM_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.f, 66.f)), module, SolarVCO::SYNC_INPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.f, 76.f)), module, SolarVCO::VOCT_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.f, 76.f)), module, SolarVCO::DRY_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(22.f, 76.f)), module, SolarVCO::VCO_OUTPUT));

        addParam(createParamCentered<Rogan1PSGreen>(mm2px(Vec(6.f, 90.f)), module, SolarVCO::ATTACK_PARAM));
        addParam(createParamCentered<Rogan1PSGreen>(mm2px(Vec(14.f, 90.f)), module, SolarVCO::DECAY_PARAM));
        addParam(createParamCentered<Rogan1PSGreen>(mm2px(Vec(22.f, 90.f)), module, SolarVCO::SUSTAIN_PARAM));
        addParam(createParamCentered<Rogan1PSGreen>(mm2px(Vec(30.f, 90.f)), module, SolarVCO::RELEASE_PARAM));

        addParam(createParamCentered<CKSS>(mm2px(Vec(8.f, 102.f)), module, SolarVCO::HOLD_PARAM));
        addParam(createParamCentered<CKSS>(mm2px(Vec(18.f, 102.f)), module, SolarVCO::SELFGEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(28.f, 102.f)), module, SolarVCO::GATE_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.f, 114.f)), module, SolarVCO::ENV_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        SolarVCO* module = dynamic_cast<SolarVCO*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
            [=]() { return module->theme; },
            [=](int theme) {
                module->theme = theme;
                setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("SolarVCO", theme))));
            }
        ));
    }
};

Model* modelSolarVCO = createModel<SolarVCO, SolarVCOWidget>("SolarVCO");
