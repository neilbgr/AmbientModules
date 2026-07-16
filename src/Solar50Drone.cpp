#include "plugin.hpp"

struct Solar50Drone : Module {
    static const int NUM_OSC = 5;

    enum ParamIds {
        ENUMS(FREQ_PARAM, NUM_OSC),
        ENUMS(ACTIVE_PARAM, NUM_OSC),
        ENUMS(MOD_PARAM, NUM_OSC),
        ATTEN_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        CV_INPUT,
        ENUMS(TRIG_INPUT, NUM_OSC),
        NUM_INPUTS
    };
    enum OutputIds {
        SAW_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        ENUMS(ACTIVE_LIGHT, NUM_OSC),
        ENUMS(MOD_LIGHT, NUM_OSC),
        NUM_LIGHTS
    };

    float phase[NUM_OSC] = {};
    dsp::SchmittTrigger trigTrigger[NUM_OSC];

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
    }

    void process(const ProcessArgs& args) override {
        // Same CV offset (in octaves) applied to every oscillator, so it shifts
        // all 5 frequencies together while preserving the intervals between them.
        float cv = inputs[CV_INPUT].getVoltage() * params[ATTEN_PARAM].getValue();

        float mix = 0.f;
        for (int i = 0; i < NUM_OSC; i++) {
            if (trigTrigger[i].process(inputs[TRIG_INPUT + i].getVoltage())) {
                params[ACTIVE_PARAM + i].setValue(params[ACTIVE_PARAM + i].getValue() > 0.f ? 0.f : 1.f);
            }
            bool active = params[ACTIVE_PARAM + i].getValue() > 0.f;
            lights[ACTIVE_LIGHT + i].setBrightness(active ? 1.f : 0.f);

            bool mod = params[MOD_PARAM + i].getValue() > 0.f;
            lights[MOD_LIGHT + i].setBrightness(mod ? 1.f : 0.f);

            float pitch = params[FREQ_PARAM + i].getValue() + (mod ? cv : 0.f);

            float freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitch + 30.f) / std::pow(2.f, 30.f);
            freq = clamp(freq, 0.f, args.sampleRate / 2.f);

            // Phase keeps running even when inactive, so re-enabling an
            // oscillator doesn't cause an audible phase jump.
            float deltaPhase = std::fmin(freq * args.sampleTime, 0.5f);
            phase[i] += deltaPhase;
            phase[i] -= std::trunc(phase[i]);

            if (active) {
                mix += 2.f * (phase[i] - std::round(phase[i])); // sawtooth naïf, pas d'anti-aliasing
            }
        }

        outputs[SAW_OUTPUT].setVoltage(5.f * mix / NUM_OSC);
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
    }
};

Model* modelSolar50Drone = createModel<Solar50Drone, Solar50DroneWidget>("Solar50Drone");
