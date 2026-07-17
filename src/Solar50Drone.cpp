#include "plugin.hpp"
#include "dsp/AREnvelope.hpp"
#include "dsp/DroneVoice.hpp"

struct Solar50Drone : Module {
    static const int NUM_OSC = DroneVoice::NUM_OSC;

    enum ParamIds {
        ENUMS(FREQ_PARAM, NUM_OSC),
        ENUMS(ACTIVE_PARAM, NUM_OSC),
        ENUMS(MOD_PARAM, NUM_OSC),
        ATTEN_PARAM,
        HOLD_PARAM,
        ATTACK_PARAM,
        RELEASE_PARAM,
        VOLT_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        CV_INPUT,
        ENUMS(TRIG_INPUT, NUM_OSC),
        GATE_INPUT,
        VOLT_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        SAW_OUTPUT,
        ENV_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        ENUMS(ACTIVE_LIGHT, NUM_OSC),
        ENUMS(MOD_LIGHT, NUM_OSC),
        HOLD_LIGHT,
        NUM_LIGHTS
    };

    DroneVoice voice;
    dsp::SchmittTrigger trigTrigger[NUM_OSC];
    AREnvelope envelope;
    dsp::SchmittTrigger gateTrigger;

    Solar50Drone() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        for (int i = 0; i < NUM_OSC; i++) {
            configInput(TRIG_INPUT + i, string::f("Oscillator %d activation trigger", i + 1));
            configSwitch(ACTIVE_PARAM + i, 0.f, 1.f, 0.f, string::f("Oscillator %d active", i + 1), {"Inactive", "Active"});
            configParam(FREQ_PARAM + i, -8.f, 4.f, -1.f, string::f("Oscillator %d frequency", i + 1), " Hz", 2.f, dsp::FREQ_C4);
            configSwitch(MOD_PARAM + i, 0.f, 1.f, 0.f, string::f("Oscillator %d modulation", i + 1), {"Off", "On"});
        }
        configParam(ATTEN_PARAM, -1.f, 1.f, 0.f, "CV Attenuverter", "%", 0.f, 100.f);
        configInput(CV_INPUT, "Frequency CV (applies to all oscillators)");
        configOutput(SAW_OUTPUT, "Sawtooth mix");

        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold (manual gate)", {"Off", "On"});
        configParam(ATTACK_PARAM, 0.f, 1.f, 0.2f, "Envelope attack");
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.2f, "Envelope release");
        configInput(GATE_INPUT, "Envelope gate");
        configOutput(ENV_OUTPUT, "Envelope");

        configParam(VOLT_PARAM, -1.f, 1.f, 0.f, "Volt (detune / FM)");
        configInput(VOLT_CV_INPUT, "Volt CV");
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "fmTopology", json_integer(voice.fmTopology));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* fmTopologyJ = json_object_get(rootJ, "fmTopology");
        if (fmTopologyJ) {
            voice.fmTopology = json_integer_value(fmTopologyJ);
        }
    }

    void process(const ProcessArgs& args) override {
        // Same CV offset (in octaves) applied to every oscillator, so it shifts
        // all 5 frequencies together while preserving the intervals between them.
        float cv = inputs[CV_INPUT].getVoltage() * params[ATTEN_PARAM].getValue();

        bool active[NUM_OSC];
        bool mod[NUM_OSC];
        float pitchParams[NUM_OSC];
        for (int i = 0; i < NUM_OSC; i++) {
            if (trigTrigger[i].process(inputs[TRIG_INPUT + i].getVoltage())) {
                params[ACTIVE_PARAM + i].setValue(params[ACTIVE_PARAM + i].getValue() > 0.f ? 0.f : 1.f);
            }
            active[i] = params[ACTIVE_PARAM + i].getValue() > 0.f;
            lights[ACTIVE_LIGHT + i].setBrightness(active[i] ? 1.f : 0.f);

            mod[i] = params[MOD_PARAM + i].getValue() > 0.f;
            lights[MOD_LIGHT + i].setBrightness(mod[i] ? 1.f : 0.f);

            pitchParams[i] = params[FREQ_PARAM + i].getValue();
        }

        // VOLT knob: negative half detunes all 5 oscillators down together;
        // positive half cross-modulates active oscillators (FM synthesis effect).
        // ±5V CV, added to the knob and clamped back into the knob's own -1..1 range.
        float volt = clamp(params[VOLT_PARAM].getValue() + inputs[VOLT_CV_INPUT].getVoltage() / 5.f, -1.f, 1.f);

        float mix = voice.process(args.sampleTime, args.sampleRate, pitchParams, active, mod, cv, volt);

        gateTrigger.process(inputs[GATE_INPUT].getVoltage());
        bool holdActive = params[HOLD_PARAM].getValue() > 0.f;
        bool gateHigh = gateTrigger.isHigh() || holdActive;
        
        envelope.updateCoefficients(params[ATTACK_PARAM].getValue(), params[RELEASE_PARAM].getValue());
        float envValue = envelope.process(args.sampleTime, gateHigh);

        lights[HOLD_LIGHT].setBrightness(envValue);

        outputs[ENV_OUTPUT].setVoltage(envValue * 10.f);
        outputs[SAW_OUTPUT].setVoltage(5.f * mix / NUM_OSC * envValue);
    }
};

struct Solar50DroneWidget : ModuleWidget {
    Solar50DroneWidget(Solar50Drone* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Solar50Drone.svg")));

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float oscX[4] = {12.f, 24.f, 36.f, 48.f};
        const float oscY[Solar50Drone::NUM_OSC] = {22.f, 32.f, 42.f, 52.f, 62.f};
        for (int i = 0; i < Solar50Drone::NUM_OSC; i++) {
            addInput(createInputCentered<PJ301MPort>(mm2px(Vec(oscX[0], oscY[i])), module, Solar50Drone::TRIG_INPUT + i));
            addParam(createLightParamCentered<VCVLightBezelLatch<RedLight>>(mm2px(Vec(oscX[1], oscY[i])), module, Solar50Drone::ACTIVE_PARAM + i, Solar50Drone::ACTIVE_LIGHT + i));
            addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(oscX[2], oscY[i])), module, Solar50Drone::FREQ_PARAM + i));
            addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(oscX[3], oscY[i])), module, Solar50Drone::MOD_PARAM + i, Solar50Drone::MOD_LIGHT + i));
        }

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(36.f, 75.f)), module, Solar50Drone::CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(48.f, 75.f)), module, Solar50Drone::ATTEN_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(48.8f, 108.f)), module, Solar50Drone::SAW_OUTPUT));

        // Envelope section — placeholder coordinates, panel layout still WIP in Inkscape.
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.f, 90.f)), module, Solar50Drone::GATE_INPUT));
        addParam(createLightParamCentered<VCVLightBezelLatch<YellowLight>>(mm2px(Vec(18.f, 90.f)), module, Solar50Drone::HOLD_PARAM, Solar50Drone::HOLD_LIGHT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(8.f, 105.f)), module, Solar50Drone::ATTACK_PARAM));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(18.f, 105.f)), module, Solar50Drone::RELEASE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(8.f, 118.f)), module, Solar50Drone::ENV_OUTPUT));

        // VOLT knob — placeholder coordinates, panel layout still WIP in Inkscape.
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(30.f, 90.f)), module, Solar50Drone::VOLT_CV_INPUT));
        addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(36.f, 90.f)), module, Solar50Drone::VOLT_PARAM));
    }

    void appendContextMenu(Menu* menu) override {
        Solar50Drone* module = dynamic_cast<Solar50Drone*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("FM topology", {"Average of active others", "Circular chain"}, &module->voice.fmTopology));
    }
};

Model* modelSolar50Drone = createModel<Solar50Drone, Solar50DroneWidget>("Solar50Drone");
