#include "plugin.hpp"

struct Solar50Drone : Module {
    enum ParamIds {
        FREQ_PARAM,
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

    float phase = 0.f;

    Solar50Drone() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, 0);
        configParam(FREQ_PARAM, -8.f, 4.f, -1.f, "Frequency", " Hz", 2.f, dsp::FREQ_C4);
        configParam(ATTEN_PARAM, -1.f, 1.f, 0.f, "CV Attenuverter", "%", 0.f, 100.f);
        configInput(CV_INPUT, "Frequency CV");
        configOutput(SAW_OUTPUT, "Sawtooth");
    }

    void process(const ProcessArgs& args) override {
        float pitch = params[FREQ_PARAM].getValue();
        pitch += inputs[CV_INPUT].getVoltage() * params[ATTEN_PARAM].getValue();

        float freq = dsp::FREQ_C4 * dsp::approxExp2_taylor5(pitch + 30.f) / std::pow(2.f, 30.f);
        freq = clamp(freq, 0.f, args.sampleRate / 2.f);

        float deltaPhase = std::fmin(freq * args.sampleTime, 0.5f);
        phase += deltaPhase;
        phase -= std::trunc(phase);

        float saw = 2.f * (phase - std::round(phase)); // sawtooth naïf, pas d'anti-aliasing
        outputs[SAW_OUTPUT].setVoltage(5.f * saw);
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

        addParam(createParamCentered<RoundHugeBlackKnob>(mm2px(Vec(15.24, 30)), module, Solar50Drone::FREQ_PARAM));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24, 55)), module, Solar50Drone::CV_INPUT));
        addParam(createParamCentered<Trimpot>(mm2px(Vec(15.24, 78)), module, Solar50Drone::ATTEN_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.24, 102)), module, Solar50Drone::SAW_OUTPUT));
    }
};

Model* modelSolar50Drone = createModel<Solar50Drone, Solar50DroneWidget>("Solar50Drone");
