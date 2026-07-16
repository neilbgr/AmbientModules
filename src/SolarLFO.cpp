#include "plugin.hpp"
#include "dsp/TriSquareLFO.hpp"

struct SolarLFO : Module {
    enum ParamIds {
        WAVE_A_PARAM,
        RATE_A_PARAM,
        WAVE_B_PARAM,
        RATE_B_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        NUM_INPUTS
    };
    enum OutputIds {
        LFO_A_OUTPUT,
        LFO_B_OUTPUT,
        NUM_OUTPUTS
    };
    
    TriSquareLFO lfoA, lfoB;

    SolarLFO() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
        configParam(RATE_A_PARAM, 0.f, 1.f, 0.5f, "LFO A rate");
        configParam(WAVE_A_PARAM, 0.f, 1.f, 0.5f, "LFO A wave (square..triangle)");
        configOutput(LFO_A_OUTPUT, "LFO A");

        configParam(RATE_B_PARAM, 0.f, 1.f, 0.5f, "LFO B rate");
        configParam(WAVE_B_PARAM, 0.f, 1.f, 0.5f, "LFO B wave (square..triangle)");
        configOutput(LFO_B_OUTPUT, "LFO B");
    }

    static constexpr float RATE_A_MIN_HZ = 0.02f;
    static constexpr float RATE_A_LOG2_RATIO = 6.643856f; // log2(2/0.02)
    static constexpr float RATE_B_MIN_HZ = 0.5f;
    static constexpr float RATE_B_LOG2_RATIO = 5.321928f; // log2(20/0.5)

    // Same +30/pow(2,30) trick as DroneVoice's pitch-to-freq: keeps the
    // exponent argument non-negative for approxExp2_taylor5, cancelled out
    // afterwards. std::pow(2.f, 30.f) has literal args so it's folded at
    // compile time — no runtime powf call, unlike a direct std::pow(ratio, knob).
    static float mapRate(float knobValue, float minHz, float log2Ratio) {
        return minHz * dsp::approxExp2_taylor5(log2Ratio * knobValue + 30.f) / std::pow(2.f, 30.f);
    }

    void process(const ProcessArgs& args) override {
        float freqA = mapRate(params[RATE_A_PARAM].getValue(), RATE_A_MIN_HZ, RATE_A_LOG2_RATIO);
        float valueA = lfoA.process(args.sampleTime, freqA, params[WAVE_A_PARAM].getValue());
        outputs[LFO_A_OUTPUT].setVoltage(valueA * 10.f);

        float freqB = mapRate(params[RATE_B_PARAM].getValue(), RATE_B_MIN_HZ, RATE_B_LOG2_RATIO);
        float valueB = lfoB.process(args.sampleTime, freqB, params[WAVE_B_PARAM].getValue());
        outputs[LFO_B_OUTPUT].setVoltage(valueB * 10.f);
    }
};

struct SolarLFOWidget : ModuleWidget {
    SolarLFOWidget(SolarLFO* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/SolarLFO.svg")));

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        //addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        //addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Single narrow column — placeholder coordinates, panel layout WIP in Inkscape.
        const float x = 10.16f;
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 25.f)), module, SolarLFO::RATE_A_PARAM));
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 44.0f)), module, SolarLFO::WAVE_A_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 59.5f)), module, SolarLFO::LFO_A_OUTPUT));

        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 78.5f)), module, SolarLFO::RATE_B_PARAM));
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 97.5f)), module, SolarLFO::WAVE_B_PARAM));        
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 113.f)), module, SolarLFO::LFO_B_OUTPUT));
    }
};

Model* modelSolarLFO = createModel<SolarLFO, SolarLFOWidget>("SolarLFO");
