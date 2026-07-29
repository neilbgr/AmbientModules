#include "plugin.hpp"
#include "dsp/TriSquareLFO.hpp"
#include "PanelTheme.hpp"

struct LunarLFO : Module {
    int theme = 0;
    int cvRangeIndex = 0; // index into cvRanges, default matches hardware (0V to +5V, oscilloscope-verified)

    static constexpr float cvRanges[4][2] = { {0.f, 5.f}, {0.f, 10.f}, {-5.f, 5.f}, {-10.f, 10.f} };

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
        json_object_set_new(rootJ, "theme", json_integer(theme));
        json_object_set_new(rootJ, "cvRangeIndex", json_integer(cvRangeIndex));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* themeJ = json_object_get(rootJ, "theme");
        if (themeJ) {
            theme = json_integer_value(themeJ);
        }
        json_t* cvRangeJ = json_object_get(rootJ, "cvRangeIndex");
        if (cvRangeJ) {
            cvRangeIndex = json_integer_value(cvRangeJ);
        }
    }

    void process(const ProcessArgs& args) override {
        float rangeMin = cvRanges[cvRangeIndex][0];
        float rangeMax = cvRanges[cvRangeIndex][1];

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

constexpr float LunarLFO::cvRanges[4][2];

struct LunarLFOWidget : ModuleWidget, ThemedModuleWidget {
    int appliedTheme = -1;

    LunarLFOWidget(LunarLFO* module) {
        setModule(module);
        syncPanelTheme(this, "LunarLFO", module ? module->theme : 0, appliedTheme);

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
        if (module) syncPanelTheme(this, "LunarLFO", dynamic_cast<LunarLFO*>(module)->theme, appliedTheme);
        ModuleWidget::step();
    }

    void applyTheme(int theme, history::ComplexAction* complexAction = nullptr) override {
        LunarLFO* module = dynamic_cast<LunarLFO*>(this->module);
        pushIntFieldChange(module, "change theme", module->theme, theme,
            [](engine::Module* m, int v) { dynamic_cast<LunarLFO*>(m)->theme = v; }, complexAction);
        module->theme = theme;
        appliedTheme = theme;
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("LunarLFO", theme))));
    }

    void appendContextMenu(Menu* menu) override {
        LunarLFO* module = dynamic_cast<LunarLFO*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
            [=]() { return module->theme; },
            [=](int theme) { applyTheme(theme); }
        ));
        appendApplyThemeToAllItem(menu, module->theme);

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
