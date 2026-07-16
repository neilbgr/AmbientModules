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

    // Param value is in octaves (exponent of 2), not Hz directly — same convention
    // as Fundamental's LFO-1/2 — so the knob's rotation is spread evenly across
    // the audible range instead of being crammed into the high end. -8..10 octaves
    // matches Fundamental's 0.0039..1024 Hz range exactly (2^-8 .. 2^10).
    static constexpr float RATE_MIN_OCT = -8.f;
    static constexpr float RATE_MAX_OCT = 10.f;

    static float octavesToHz(float octaves) {
        return dsp::approxExp2_taylor5(octaves + 30.f) / std::pow(2.f, 30.f);
    }

    SolarLFO() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
        configParam(RATE_A_PARAM, RATE_MIN_OCT, RATE_MAX_OCT, 0.f, "LFO A rate", " Hz", 2.f, 1.f);
        configParam(WAVE_A_PARAM, 0.f, 1.f, 0.5f, "LFO A wave (triangle..square)");
        configOutput(LFO_A_OUTPUT, "LFO A");

        configParam(RATE_B_PARAM, RATE_MIN_OCT, RATE_MAX_OCT, 0.f, "LFO B rate", " Hz", 2.f, 1.f);
        configParam(WAVE_B_PARAM, 0.f, 1.f, 0.5f, "LFO B wave (triangle..square)");
        configOutput(LFO_B_OUTPUT, "LFO B");
    }

    void process(const ProcessArgs& args) override {
        float freqA = octavesToHz(params[RATE_A_PARAM].getValue());
        float valueA = lfoA.process(args.sampleTime, freqA, params[WAVE_A_PARAM].getValue());
        outputs[LFO_A_OUTPUT].setVoltage(valueA * 10.f);

        float freqB = octavesToHz(params[RATE_B_PARAM].getValue());
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
