#include "plugin.hpp"
#include "dsp/AREnvelope.hpp"
#include "dsp/DroneVoice.hpp"
#include "PanelTheme.hpp"

struct Lunar50Drone : Module {
    static const int NUM_OSC = DroneVoice::NUM_OSC;
    static constexpr float OUTPUT_VOLTAGE = 5.f; // Eurorack ±5V audio convention

    enum MixMode { MIX_FIXED = 0, MIX_AVERAGE_ACTIVE = 1, MIX_SOFT_CLIP = 2 };
    int mixMode = 0; // 0 = Fixed /5 (legacy), keeps existing patches sounding the same

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
        ENV_INPUT,
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
        ENV_LIGHT,
        NUM_LIGHTS
    };

    DroneVoice voice[PORT_MAX_CHANNELS];
    dsp::SchmittTrigger trigTrigger[NUM_OSC];
    AREnvelope envelope[PORT_MAX_CHANNELS];
    dsp::SchmittTrigger gateTrigger[PORT_MAX_CHANNELS];

    Lunar50Drone() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        for (int i = 0; i < NUM_OSC; i++) {
            configInput(TRIG_INPUT + i, string::f("Oscillator %d activation trigger (mono)", i + 1));
            configSwitch(ACTIVE_PARAM + i, 0.f, 1.f, (i == 0 ? 1.f : 0.f), string::f("Oscillator %d active", i + 1), {"Inactive", "Active"});
            configParam(FREQ_PARAM + i, -8.f, 4.f, -1.f, string::f("Oscillator %d frequency", i + 1), " Hz", 2.f, dsp::FREQ_C4);
            configSwitch(MOD_PARAM + i, 0.f, 1.f, (i == 0 ? 1.f : 0.f), string::f("Oscillator %d modulation", i + 1), {"Off", "On"});
        }
        configParam(ATTEN_PARAM, -1.f, 1.f, 1.f, "CV Attenuverter", "%", 0.f, 100.f);
        configInput(CV_INPUT, "Frequency CV (applies to all oscillators)");
        configOutput(SAW_OUTPUT, "Sawtooth mix");

        configSwitch(HOLD_PARAM, 0.f, 1.f, 0.f, "Hold (manual gate)", {"Off", "On"});
        configParam(ATTACK_PARAM, 0.f, 1.f, 0.4f, "Envelope attack", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configParam(RELEASE_PARAM, 0.f, 1.f, 0.6f, "Envelope release", " ms", AREnvelope::MAX_TIME / AREnvelope::MIN_TIME, AREnvelope::MIN_TIME * 1000.f);
        configInput(GATE_INPUT, "Envelope gate");
        configOutput(ENV_OUTPUT, "Envelope");
        configInput(ENV_INPUT, "Envelope CV (overrides internal envelope, gate and hold when connected — for chaining several drones on one envelope)");

        configParam(VOLT_PARAM, 0.f, 1.f, 0.f, "Volt (detune / FM)", "%", 0.f, 100.f);
        configInput(VOLT_CV_INPUT, "Volt CV");
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "fmTopology", json_integer(voice[0].fmTopology));
        json_object_set_new(rootJ, "mixMode", json_integer(mixMode));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* fmTopologyJ = json_object_get(rootJ, "fmTopology");
        if (fmTopologyJ) {
            int fmTopology = json_integer_value(fmTopologyJ);
            for (int c = 0; c < PORT_MAX_CHANNELS; c++) voice[c].fmTopology = fmTopology;
        }
        json_t* mixModeJ = json_object_get(rootJ, "mixMode");
        if (mixModeJ) {
            mixMode = json_integer_value(mixModeJ);
        }
    }

    void process(const ProcessArgs& args) override {
        // Poly channel count driven by CV, gate, VOLT CV, AND the envelope
        // input, so that chaining another module's poly ENV_OUTPUT into
        // ENV_INPUT (overriding the internal envelope/gate entirely) still
        // drives all of its channels even when the other inputs are mono or
        // unpatched.
        int channels = std::max({inputs[CV_INPUT].getChannels(), inputs[GATE_INPUT].getChannels(),
                                  inputs[VOLT_CV_INPUT].getChannels(), inputs[ENV_INPUT].getChannels(), 1});

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

        int activeCount = 0;
        for (int i = 0; i < NUM_OSC; i++) {
            if (active[i]) {
                activeCount++;
            }
        }

        // VOLT knob, per the official doc: "transposes down all 5 voice
        // generators at the same time. After half the stroke of the knob,
        // generators start to modulate each other creating FM synthesis
        // effect" — see DroneVoice::process() for the detune/FM curve.
        // Knob value cached here; the CV itself (0..10V, added to the knob and
        // clamped back into the knob's own 0..1 range) is read per-channel
        // below via getPolyVoltage(c), same as the other poly CV inputs.
        float voltParam = params[VOLT_PARAM].getValue();

        bool holdActive = params[HOLD_PARAM].getValue() > 0.f;
        // Attack/Release knobs are non-poly (same for every channel) — compute
        // the lambdas once here instead of recomputing approxExp2_taylor5
        // per channel inside the loop below.
        float attackLambda = AREnvelope::lambdaFromKnob(params[ATTACK_PARAM].getValue());
        float releaseLambda = AREnvelope::lambdaFromKnob(params[RELEASE_PARAM].getValue());
        bool envInputConnected = inputs[ENV_INPUT].isConnected();
        bool gateInputConnected = inputs[GATE_INPUT].isConnected();
        float atten = params[ATTEN_PARAM].getValue();

        float maxEnvValue = 0.f;
        for (int c = 0; c < channels; c++) {
            // Same CV offset (in octaves) applied to every oscillator, so it shifts
            // all 5 frequencies together while preserving the intervals between them.
            float cv = inputs[CV_INPUT].getPolyVoltage(c) * atten;
            float volt = clamp(voltParam + inputs[VOLT_CV_INPUT].getPolyVoltage(c) / 10.f, 0.f, 1.f);

            float envValue;
            if (envInputConnected) {
                envValue = inputs[ENV_INPUT].getPolyVoltage(c) / 10.f;
            } else {
                gateTrigger[c].process(inputs[GATE_INPUT].getPolyVoltage(c));
                bool gateHigh = holdActive || (gateInputConnected && gateTrigger[c].isHigh());

                envValue = envelope[c].process(args.sampleTime, gateHigh, attackLambda, releaseLambda);
            }
            float envAmount = clamp(envValue, 0.f, 1.f);
            maxEnvValue = std::max(maxEnvValue, envAmount);

            // Envelope silent -> output would be zero anyway, skip the 5-oscillator engine entirely.
            float sawOut = 0.f;
            if (envAmount > 0.f) {
                float mix = voice[c].process(args.sampleTime, args.sampleRate, pitchParams, active, mod, cv, volt);
                switch (mixMode) {
                    case MIX_AVERAGE_ACTIVE:
                        sawOut = OUTPUT_VOLTAGE * mix / std::max(activeCount, 1) * envAmount;
                        break;
                    case MIX_SOFT_CLIP:
                        sawOut = OUTPUT_VOLTAGE * std::tanh(mix) * envAmount;
                        break;
                    default: // MIX_FIXED
                        sawOut = OUTPUT_VOLTAGE * mix / NUM_OSC * envAmount; // average of NUM_OSC oscillators, scaled to OUTPUT_VOLTAGE
                        break;
                }
            }

            outputs[ENV_OUTPUT].setVoltage(envValue * 10.f, c);
            outputs[SAW_OUTPUT].setVoltage(sawOut, c);
        }
        outputs[ENV_OUTPUT].setChannels(channels);
        outputs[SAW_OUTPUT].setChannels(channels);

        lights[HOLD_LIGHT].setBrightness(holdActive ? 1.f : 0.f);
        lights[ENV_LIGHT].setBrightness(maxEnvValue);
    }
};

struct Lunar50DroneWidget : ModuleWidget {
    int appliedTheme = -1;

    Lunar50DroneWidget(Lunar50Drone* module) {
        setModule(module);
        syncPanelTheme(this, "Lunar50Drone", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float oscX[4] = {12.f, 24.f, 36.f, 48.f};
        const float oscY[Lunar50Drone::NUM_OSC] = {22.f, 32.f, 42.f, 52.f, 62.f};
        for (int i = 0; i < Lunar50Drone::NUM_OSC; i++) {
            addInput(createInputCentered<PJ301MPort>(mm2px(Vec(oscX[0], oscY[i])), module, Lunar50Drone::TRIG_INPUT + i));
            addParam(createLightParamCentered<VCVLightBezelLatch<RedLight>>(mm2px(Vec(oscX[1], oscY[i])), module, Lunar50Drone::ACTIVE_PARAM + i, Lunar50Drone::ACTIVE_LIGHT + i));
            addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(oscX[2], oscY[i])), module, Lunar50Drone::FREQ_PARAM + i));
            addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(oscX[3], oscY[i])), module, Lunar50Drone::MOD_PARAM + i, Lunar50Drone::MOD_LIGHT + i));
        }

        addParam(createParamCentered<Trimpot>(mm2px(Vec(48.f, 72.f)), module, Lunar50Drone::ATTEN_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(48.f, 83.f)), module, Lunar50Drone::CV_INPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.f, 76.75f)), module, Lunar50Drone::VOLT_CV_INPUT));
        addParam(createParamCentered<Rogan1PSBlue>(mm2px(Vec(28.75f, 76.75f)), module, Lunar50Drone::VOLT_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.f, 98.f)), module, Lunar50Drone::ENV_INPUT));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(24.f, 98.f)), module, Lunar50Drone::ATTACK_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(36.f, 98.f)), module, Lunar50Drone::RELEASE_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(48.f, 98.f)), module, Lunar50Drone::ENV_OUTPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.f, 113.f)), module, Lunar50Drone::GATE_INPUT));
        addChild(createLightCentered<MediumLight<YellowLight>>(mm2px(Vec(18.5f, 113.f - 2.f )), module, Lunar50Drone::ENV_LIGHT));
        addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(24.f, 113.f)), module, Lunar50Drone::HOLD_PARAM, Lunar50Drone::HOLD_LIGHT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(48.f, 113.f)), module, Lunar50Drone::SAW_OUTPUT));
    }

    void step() override {
        syncPanelTheme(this, "Lunar50Drone", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        Lunar50Drone* module = dynamic_cast<Lunar50Drone*>(this->module);
        assert(module);

        appendAmbientThemeMenu(menu);
        menu->addChild(createIndexSubmenuItem("FM topology", {"Average of active others", "Circular chain"},
            [=]() { return module->voice[0].fmTopology; },
            [=](int topology) {
                pushIntFieldChange(module, "change FM topology", module->voice[0].fmTopology, topology,
                    [](engine::Module* m, int v) {
                        Lunar50Drone* mm = dynamic_cast<Lunar50Drone*>(m);
                        for (int c = 0; c < PORT_MAX_CHANNELS; c++) mm->voice[c].fmTopology = v;
                    });
                for (int c = 0; c < PORT_MAX_CHANNELS; c++) module->voice[c].fmTopology = topology;
            }
        ));
        menu->addChild(createIndexSubmenuItem("Oscillator mix",
            {"Fixed sum / 5 (legacy)", "Average of active oscillators", "Soft-clip saturated sum"},
            [=]() { return module->mixMode; },
            [=](int mode) {
                pushIntFieldChange(module, "change oscillator mix", module->mixMode, mode,
                    [](engine::Module* m, int v) { dynamic_cast<Lunar50Drone*>(m)->mixMode = v; });
                module->mixMode = mode;
            }
        ));
    }
};

Model* modelLunar50Drone = createModel<Lunar50Drone, Lunar50DroneWidget>("Lunar50Drone");
