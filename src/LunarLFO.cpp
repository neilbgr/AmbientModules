#include "plugin.hpp"
#include "dsp/TriSquareLFO.hpp"
#include "PanelTheme.hpp"
#include "dsp/Common.hpp"

struct LunarLFO : Module {
    int cvRangeIndex = 0; // index into ambient::CV_RANGES, default matches hardware (0V to +5V, oscilloscope-verified)

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
        return ambient::exp2Octaves(octaves);
    }

    LunarLFO() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
        configParam(RATE_A_PARAM, RATE_MIN_OCT, RATE_MAX_OCT, 0.f, "LFO A rate", " Hz", 2.f, 1.f);
        configParam(WAVE_A_PARAM, 0.f, 1.f, 0.f, "LFO A wave (triangle..square)");
        configOutput(LFO_A_OUTPUT, "LFO A");

        configParam(RATE_B_PARAM, RATE_MIN_OCT, RATE_MAX_OCT, 0.f, "LFO B rate", " Hz", 2.f, 1.f);
        configParam(WAVE_B_PARAM, 0.f, 1.f, 0.f, "LFO B wave (triangle..square)");
        configOutput(LFO_B_OUTPUT, "LFO B");
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "cvRangeIndex", json_integer(cvRangeIndex));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* cvRangeJ = json_object_get(rootJ, "cvRangeIndex");
        if (cvRangeJ) {
            // Clamped: indexes CV_RANGES directly in process(), so an
            // out-of-range value from a corrupt/hand-edited patch would read
            // past the table.
            cvRangeIndex = ambient::clampIndex(json_integer_value(cvRangeJ), ambient::NUM_CV_RANGES);
        }
    }

    void process(const ProcessArgs& args) override {
        float rangeMin = ambient::CV_RANGES[cvRangeIndex][0];
        float rangeMax = ambient::CV_RANGES[cvRangeIndex][1];

        if (outputs[LFO_A_OUTPUT].isConnected()) {
            float freqA = octavesToHz(params[RATE_A_PARAM].getValue());
            float valueA = lfoA.process(args.sampleTime, freqA, params[WAVE_A_PARAM].getValue());
            outputs[LFO_A_OUTPUT].setVoltage(rangeMin + valueA * (rangeMax - rangeMin));
        } else {
            lfoA.phase = 0.f;
        }

        if (outputs[LFO_B_OUTPUT].isConnected()) {
            float freqB = octavesToHz(params[RATE_B_PARAM].getValue());
            float valueB = lfoB.process(args.sampleTime, freqB, params[WAVE_B_PARAM].getValue());
            outputs[LFO_B_OUTPUT].setVoltage(rangeMin + valueB * (rangeMax - rangeMin));
        } else {
            lfoB.phase = 0.f;
        }
    }
};

struct LunarLFOWidget : ModuleWidget {
    int appliedTheme = -1;

    LunarLFOWidget(LunarLFO* module) {
        setModule(module);
        syncPanelTheme(this, "LunarLFO", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Single narrow column — placeholder coordinates, panel layout WIP in Inkscape.
        const float x = 10.16f;
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 25.f)), module, LunarLFO::RATE_A_PARAM));
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 44.0f)), module, LunarLFO::WAVE_A_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 59.5f)), module, LunarLFO::LFO_A_OUTPUT));

        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 78.5f)), module, LunarLFO::RATE_B_PARAM));
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(x, 97.5f)), module, LunarLFO::WAVE_B_PARAM));        
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, 113.f)), module, LunarLFO::LFO_B_OUTPUT));
    }

    void step() override {
        syncPanelTheme(this, "LunarLFO", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        LunarLFO* module = dynamic_cast<LunarLFO*>(this->module);
        assert(module);

        appendAmbientThemeMenu(menu);

        menu->addChild(createIndexSubmenuItem("CV Range",
            {"0V to +5V", "0V to +10V", "-5V to +5V", "-10V to +10V"},
            [=]() { return module->cvRangeIndex; },
            [=](int index) {
                pushIntFieldChange(module, "change CV range", module->cvRangeIndex, index,
                    [](engine::Module* m, int v) { dynamic_cast<LunarLFO*>(m)->cvRangeIndex = v; });
                module->cvRangeIndex = index;
            }
        ));
    }
};

Model* modelLunarLFO = createModel<LunarLFO, LunarLFOWidget>("LunarLFO");
