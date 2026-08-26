#include "plugin.hpp"
#include "PanelTheme.hpp"

struct LunarSequencer;

// Step CV knobs display plain volts, computed from the module's *current*
// cvRangeIndex — a plain configParam unit can't do this since the range is
// picked at runtime from the context menu, not fixed at construction time.
struct StepCvQuantity : ParamQuantity {
    std::string getDisplayValueString() override;
};

// A real 3-position toggle snaps to whichever side of the lever you push,
// rather than bumping one step per click like Rack's stock Switch. So instead
// of reacting to onDragStart (which has no click position), this reads the
// click's position within the widget on mouse-down and jumps straight to the
// third (top/middle/bottom, or left/middle/right) it falls in. onDragStart is
// neutralized so the base Switch's step-and-wrap logic can't fight it on the
// same click. Templated on the underlying SvgSwitch so both orientations
// share one behavior.
template <typename TBase>
struct ClickTargetSwitch : TBase {
    void onButton(const widget::Widget::ButtonEvent& e) override {
        TBase::onButton(e);
        if (e.action != GLFW_PRESS || e.button != GLFW_MOUSE_BUTTON_LEFT) {
            return;
        }

        engine::ParamQuantity* pq = this->getParamQuantity();
        if (!pq) {
            return;
        }

        // CKSSThreeHorizontal is wider than tall, CKSSThree the opposite.
        bool horizontal = this->box.size.x > this->box.size.y;
        float frac = horizontal ? e.pos.x / this->box.size.x : e.pos.y / this->box.size.y;
        frac = clamp(frac, 0.f, 1.f);
        // Vertical: value 0 is at the bottom (see STAGES_PARAM comment below),
        // so flip so frac still runs low-to-high with the value.
        if (!horizontal) {
            frac = 1.f - frac;
        }

        float oldValue = pq->getValue();
        float newValue = (frac < 1.f / 3.f) ? 0.f : (frac < 2.f / 3.f) ? 1.f : 2.f;
        if (newValue == oldValue) {
            return;
        }
        pq->setValue(newValue);

        history::ParamChange* h = new history::ParamChange;
        h->name = "move switch";
        h->moduleId = this->module->id;
        h->paramId = this->paramId;
        h->oldValue = oldValue;
        h->newValue = newValue;
        APP->history->push(h);
    }

    void onDragStart(const widget::Widget::DragStartEvent& e) override {
        ParamWidget::onDragStart(e);
    }
};
using ClickTargetCKSSThree = ClickTargetSwitch<CKSSThree>;
using ClickTargetCKSSThreeHorizontal = ClickTargetSwitch<CKSSThreeHorizontal>;

struct LunarSequencer : Module {
    static const int NUM_STEPS = 5;

    int cvRangeIndex = 0; // index into cvRanges

    enum ParamIds {
        ENUMS(STEP_CV_PARAM, NUM_STEPS),     // 0..1 normalized, rescaled by cvRangeIndex at output time
        ENUMS(STEP_GATE_PARAM, NUM_STEPS),   // per-step gate on/off
        STAGES_PARAM,                        // 0/1/2 -> 3/4/5 stages
        RATE_PARAM,                          // internal pulser rate, in octaves rel. to 1 Hz
        NUM_PARAMS
    };
    enum InputIds {
        CLOCK_INPUT,
        //RESET_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        CV_OUTPUT,
        GATE_OUTPUT,
        CLOCK_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightIds {
        ENUMS(STEP_GATE_LIGHT, NUM_STEPS),
        ENUMS(STEP_LIGHT, NUM_STEPS),
        NUM_LIGHTS
    };

    static constexpr float RATE_MIN_OCT = -4.3219f; // 0.05 Hz (20 s/step)
    static constexpr float RATE_MAX_OCT = 6.f;
    // Named constant instead of std::pow(2.f, 30.f) — 2^30 is exactly
    // representable in float, and a compile-time literal removes any
    // dependency on the compiler actually folding a runtime powf call.
    static constexpr float TWO_POW_30 = 1073741824.f;

    static float octavesToHz(float octaves) {
        return dsp::approxExp2_taylor5(octaves + 30.f) / TWO_POW_30;
    }

    static constexpr float cvRanges[4][2] = { {0.f, 5.f}, {0.f, 10.f}, {-5.f, 5.f}, {-10.f, 10.f} };

    dsp::SchmittTrigger clockTrigger;
    dsp::SchmittTrigger resetTrigger;
    float pulserPhase = 0.f;
    int stepIndex = 0;

    LunarSequencer() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

        // Hardware switch is a vertical 3-position toggle reading, top to
        // bottom: 4, 5, 3 (solar42f_instruct_03_25_v9.pdf, p.11) — not the
        // intuitive 3/4/5 order. CKSSThree's value 0/1/2 maps to bottom/
        // middle/top, so the label list below follows that same order.
        configSwitch(STAGES_PARAM, 0.f, 2.f, 1.f, "Stages", {"3", "5", "4"});
        configParam(RATE_PARAM, RATE_MIN_OCT, RATE_MAX_OCT, 1.f, "Pulser rate", " Hz", 2.f, 1.f);
        configInput(CLOCK_INPUT, "Clock In");
        //configInput(RESET_INPUT, "Reset (back to step 1)");
        configOutput(CLOCK_OUTPUT, "Clock Out");
        configOutput(CV_OUTPUT, "Step CV");
        configOutput(GATE_OUTPUT, "Step gate");

        for (int i = 0; i < NUM_STEPS; i++) {
            configParam<StepCvQuantity>(STEP_CV_PARAM + i, 0.f, 1.f, 0.f, string::f("Step %d CV", i + 1));
            configSwitch(STEP_GATE_PARAM + i, 0.f, 1.f, 1.f, string::f("Step %d gate", i + 1), {"Off", "On"});
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "cvRangeIndex", json_integer(cvRangeIndex));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* cvRangeJ = json_object_get(rootJ, "cvRangeIndex");
        if (cvRangeJ) {
            cvRangeIndex = json_integer_value(cvRangeJ);
        }
    }

    void process(const ProcessArgs& args) override {
        // Same bottom/middle/top -> 3/5/4 mapping as the STAGES_PARAM labels.
        static const int STAGES_FOR_VALUE[3] = {3, 5, 4};
        int stagesIdx = clamp((int)std::round(params[STAGES_PARAM].getValue()), 0, 2);
        int stages = STAGES_FOR_VALUE[stagesIdx];

        float effectiveClockVoltage;
        if( inputs[CLOCK_INPUT].isConnected())
        {
            effectiveClockVoltage = inputs[CLOCK_INPUT].getVoltage();
        }
        else
        {
            float freq = octavesToHz(params[RATE_PARAM].getValue());
            pulserPhase += freq * args.sampleTime;
            if (pulserPhase >= 1.f) {
                pulserPhase -= 1.f;
            }
            effectiveClockVoltage = (pulserPhase < 0.5f) ? 10.f : 0.f;
        }

        /*
        // Low threshold 0.1V (not the default exact 0V): an external clock coming
        // from a continuously-varying source (e.g. an LFO morphed towards
        // triangle) asymptotically approaches but essentially never samples at
        // exactly 0V, so the trigger would latch high forever after the first
        // edge and never re-arm.
        if (resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f)) {
            stepIndex = 0;
            pulserPhase = 0.f;
        }
        */
        if (clockTrigger.process(effectiveClockVoltage, 0.1f, 1.f)) {
            stepIndex = (stepIndex + 1) % stages;
        }
        if (stepIndex >= stages) {
            stepIndex = 0;
        }

        outputs[CLOCK_OUTPUT].setVoltage(effectiveClockVoltage);

        // According to the documentaiton : "Gates switches does not affect the CV voltage output."
        float rangeMin = cvRanges[cvRangeIndex][0];
        float rangeMax = cvRanges[cvRangeIndex][1];
        float cvNorm = params[STEP_CV_PARAM + stepIndex].getValue();
        outputs[CV_OUTPUT].setVoltage(rangeMin + cvNorm * (rangeMax - rangeMin));

        // Always a clean 0/10V logic gate, regardless of the external clock's
        // actual shape/duty-cycle/amplitude — clockTrigger.isHigh() reflects
        // its thresholded (0.1V/1V hysteresis) high/low state rather than
        // passing the raw voltage through, matching the internal pulser's
        // already-clean square wave (see CLOCK_OUTPUT above for the
        // pass-through of the raw clock shape instead, if that's wanted).
        bool gateEnabled = params[STEP_GATE_PARAM + stepIndex].getValue() > 0.f;
        outputs[GATE_OUTPUT].setVoltage(gateEnabled && clockTrigger.isHigh() ? 10.f : 0.f);

        for (int i = 0; i < NUM_STEPS; i++) {
            lights[STEP_GATE_LIGHT + i].setBrightness(params[STEP_GATE_PARAM + i].getValue() > 0.f ? 1.f : 0.f);
            lights[STEP_LIGHT + i].setBrightness((i < stages && i == stepIndex) ? 1.f : 0.f);
        }
    }
};

constexpr float LunarSequencer::cvRanges[4][2];

std::string StepCvQuantity::getDisplayValueString() {
    LunarSequencer* m = dynamic_cast<LunarSequencer*>(module);
    float rangeMin = LunarSequencer::cvRanges[m->cvRangeIndex][0];
    float rangeMax = LunarSequencer::cvRanges[m->cvRangeIndex][1];
    float volts = rangeMin + getValue() * (rangeMax - rangeMin);
    return string::f("%.2fV", volts);
}

struct LunarSequencerWidget : ModuleWidget {
    int appliedTheme = -1;

    LunarSequencerWidget(LunarSequencer* module) {
        setModule(module);
        syncPanelTheme(this, "LunarSequencer", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        const float xL = 10.00f, xC = 18.f, xR = 26.f;

        addParam(createParamCentered<Rogan1PRed>(mm2px(Vec(xC, 20.f)), module, LunarSequencer::RATE_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xL, 30.f)), module, LunarSequencer::CLOCK_INPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xR, 30.f)), module, LunarSequencer::CLOCK_OUTPUT));

        //addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xC, 50.f)), module, LunarSequencer::RESET_INPUT));

        addParam(createParamCentered<ClickTargetCKSSThreeHorizontal>(mm2px(Vec(xC, 44.5f)), module, LunarSequencer::STAGES_PARAM));

        //const float lightX[LunarSequencer::NUM_STEPS] = {5.36f, 10.30f, 15.24f, 20.18f, 25.12f};           

        const float stepY[LunarSequencer::NUM_STEPS] = {53.f, 65.f, 77.f, 89.f, 101.f};
        for (int i = 0; i < LunarSequencer::NUM_STEPS; i++) {
            addParam(createLightParamCentered<VCVLightLatch<MediumSimpleLight<WhiteLight>>>(mm2px(Vec(8.f, stepY[i])), module, LunarSequencer::STEP_GATE_PARAM + i, LunarSequencer::STEP_GATE_LIGHT + i));
            addParam(createParamCentered<Rogan1PRed>(mm2px(Vec(xC, stepY[i])), module, LunarSequencer::STEP_CV_PARAM + i));
            addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(28.f, stepY[i])), module, LunarSequencer::STEP_LIGHT + i));
        }

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xL, 113.f)), module, LunarSequencer::GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xR, 113.f)), module, LunarSequencer::CV_OUTPUT));
    }

    void step() override {
        syncPanelTheme(this, "LunarSequencer", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        LunarSequencer* module = dynamic_cast<LunarSequencer*>(this->module);
        assert(module);

        appendAmbientThemeMenu(menu);

        menu->addChild(createIndexSubmenuItem("CV Range",
            {"0V to +5V", "0V to +10V", "-5V to +5V", "-10V to +10V"},
            [=]() { return module->cvRangeIndex; },
            [=](int index) {
                pushIntFieldChange(module, "change CV range", module->cvRangeIndex, index,
                    [](engine::Module* m, int v) { dynamic_cast<LunarSequencer*>(m)->cvRangeIndex = v; });
                module->cvRangeIndex = index;
            }
        ));
    }
};

Model* modelLunarSequencer = createModel<LunarSequencer, LunarSequencerWidget>("LunarSequencer");
