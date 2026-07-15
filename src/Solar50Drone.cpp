#include "plugin.hpp"

struct Solar50Drone : Module {
    static const int NUM_OSC = 5;

    enum ParamIds {
        ENUMS(FREQ_PARAM, NUM_OSC),
        ATTEN_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        CV_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        SAW_OUTPUT,
        NUM_OUTPUTS
    };

    float phase[NUM_OSC] = {};

    Solar50Drone() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);
        for (int i = 0; i < NUM_OSC; i++) {
            configParam(FREQ_PARAM + i, -8.f, 4.f, -1.f, string::f("Oscillator %d frequency", i + 1), " Hz", 2.f, dsp::FREQ_C4);
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
            float pitch = params[FREQ_PARAM + i].getValue() + cv;

            float freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitch + 30.f) / std::pow(2.f, 30.f);
            freq = clamp(freq, 0.f, args.sampleRate / 2.f);

            float deltaPhase = std::fmin(freq * args.sampleTime, 0.5f);
            phase[i] += deltaPhase;
            phase[i] -= std::trunc(phase[i]);

            mix += 2.f * (phase[i] - std::round(phase[i])); // sawtooth naïf, pas d'anti-aliasing
        }

        outputs[SAW_OUTPUT].setVoltage(5.f * mix / NUM_OSC);
    }
};

struct Solar50DroneWidget : ModuleWidget {
    Solar50DroneWidget(Solar50Drone* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Solar50Drone.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float centerX = 20.32f;
        const float oscY[Solar50Drone::NUM_OSC] = {22.f, 36.f, 50.f, 64.f, 78.f};
        for (int i = 0; i < Solar50Drone::NUM_OSC; i++) {
            addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(centerX, oscY[i])), module, Solar50Drone::FREQ_PARAM + i));
        }

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(12.32f, 94.f)), module, Solar50Drone::CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(28.32f, 94.f)), module, Solar50Drone::ATTEN_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(centerX, 108.f)), module, Solar50Drone::SAW_OUTPUT));
    }
};

Model* modelSolar50Drone = createModel<Solar50Drone, Solar50DroneWidget>("Solar50Drone");
