#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "Widgets.hpp"

// Electrical equivalent of the SOLAR 42F Joystick block (manual p.10) without
// the physical stick: X and Y are each either a manual knob (-5V..+5V, used
// when nothing is patched) or an external CV (e.g. a MIDI CC->CV converter),
// plus an offset knob, exactly matching the 3 example ranges on p.10
// (offset left/center/right -> -10..0V / -5..+5V / 0..+10V).
struct LunarJoystick : Module {
    // Same 4 presets already used elsewhere in the pack (LunarLFO/LunarSequencer's
    // "CV Range"), reused here to convert an external CV source into the
    // joystick's internal bipolar -5V..+5V axis range. A MIDI CC->CV converter
    // is almost always unipolar 0V..+10V (CC values are 0..127, never negative),
    // which a plain attenuator can't recenter — hence a range preset instead.
    static constexpr float RANGES[4][2] = { {-5.f, 5.f}, {0.f, 10.f}, {0.f, 5.f}, {-10.f, 10.f} };
    static const int RANGE_DEFAULT = 1; // 0V to +10V (MIDI CC)

    int rangeX = RANGE_DEFAULT;
    int rangeY = RANGE_DEFAULT;
    // Runtime-only (not persisted, like LunarPads::individualLatch): true
    // while the XYPad is actively being dragged, forcing manual position to
    // win over a patched CV on both axes for the duration of that one 2D
    // gesture — see axisPosition() below.
    bool xyPadDragging = false;

    enum ParamIds {
        X_PARAM,
        Y_PARAM,
        X_OFFSET_PARAM,
        Y_OFFSET_PARAM,
        NUM_PARAMS
    };
    enum InputIds {
        X_INPUT,
        Y_INPUT,
        NUM_INPUTS
    };
    enum OutputIds {
        X_OUTPUT,
        Y_OUTPUT,
        NUM_OUTPUTS
    };

    LunarJoystick() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
        configParam(X_PARAM, -5.f, 5.f, 0.f, "X position (manual, used when X In isn't patched)", "V");
        configParam(Y_PARAM, -5.f, 5.f, 0.f, "Y position (manual, used when Y In isn't patched)", "V");
        configParam(X_OFFSET_PARAM, -5.f, 5.f, 0.f, "X offset", "V");
        configParam(Y_OFFSET_PARAM, -5.f, 5.f, 0.f, "Y offset", "V");
        configInput(X_INPUT, "X input (choose your range in the context menu)");
        configInput(Y_INPUT, "Y input (choose your range in the context menu)");
        configOutput(X_OUTPUT, "X");
        configOutput(Y_OUTPUT, "Y");
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "rangeX", json_integer(rangeX));
        json_object_set_new(rootJ, "rangeY", json_integer(rangeY));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* j = json_object_get(rootJ, "rangeX")) {
            rangeX = json_integer_value(j);
        }
        if (json_t* j = json_object_get(rootJ, "rangeY")) {
            rangeY = json_integer_value(j);
        }
    }

    // Input and knob/pad are mutually exclusive, never summed: once a CV is
    // patched it takes over entirely, since summing it with the knob would
    // just be a redundant second offset (X_OFFSET_PARAM already covers that).
    // `forceManual` additionally overrides a patched CV while the XYPad is
    // actively being dragged (see xyPadDragging above) — same function used
    // by process() and by the widget's draw(), so the displayed position and
    // the value actually used are always identical, never two separate
    // computations to keep in sync.
    // The param branch is already within [-5, 5] by construction (Rack's
    // engine clamps a configParam's value to its declared min/max, same as a
    // manual knob physically can't turn past its own limits) — no clamp
    // needed there. The CV branch has no such guarantee: a patched source can
    // send any voltage, including outside [rangeMin, rangeMax], which
    // rescale() extrapolates linearly past [-5, 5] instead of clamping. Left
    // unclamped, that both draws the XYPad's crosshair outside its own square
    // and lets CV push the axis further than a manual knob for the same
    // param ever could — clamp so CV and manual control represent the same
    // bounded position either way.
    static float axisPosition(Input& input, Param& param, int rangeIndex, bool forceManual) {
        if (input.isConnected() && !forceManual) {
            float rangeMin = RANGES[rangeIndex][0];
            float rangeMax = RANGES[rangeIndex][1];
            return clamp(rescale(input.getVoltage(), rangeMin, rangeMax, -5.f, 5.f), -5.f, 5.f);
        }
        return param.getValue();
    }

    void process(const ProcessArgs& args) override {
        float x = axisPosition(inputs[X_INPUT], params[X_PARAM], rangeX, xyPadDragging) + params[X_OFFSET_PARAM].getValue();
        float y = axisPosition(inputs[Y_INPUT], params[Y_PARAM], rangeY, xyPadDragging) + params[Y_OFFSET_PARAM].getValue();
        outputs[X_OUTPUT].setVoltage(x);
        outputs[Y_OUTPUT].setVoltage(y);
    }
};

constexpr float LunarJoystick::RANGES[4][2];

struct LunarJoystickWidget : ModuleWidget {
    int appliedTheme = -1;

    LunarJoystickWidget(LunarJoystick* module) {
        setModule(module);
        syncPanelTheme(this, "LunarJoystick", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Placeholder layout: one shared XY pad for X_PARAM/Y_PARAM up top
        // (replaces the 2 separate knobs), then a row per axis below (In,
        // offset knob, Out) — panel WIP in Inkscape. 9HP (45.72mm, was 8HP)
        // to fit the pad with a 2mm margin left/right; target width for
        // res/LunarJoystick_*.svg (still 8HP/40.64mm right now) — left to
        // Neil to resize, same as LunarMixer's panel background.
        const float panelWidth = 45.72f;
        const float padMargin = 2.f;
        const float padSize = panelWidth - 2.f * padMargin;
        const float topMargin = 15.f;

        XYPad* xyPad = createWidget<XYPad>(mm2px(Vec(padMargin, topMargin)));
        xyPad->box.size = mm2px(Vec(padSize, padSize));
        xyPad->module = module;
        xyPad->paramIdX = LunarJoystick::X_PARAM;
        xyPad->paramIdY = LunarJoystick::Y_PARAM;
        xyPad->getPosition = [module](int axis) -> float {
            if (!module) {
                return 0.f;
            }
            return axis == 0
                ? LunarJoystick::axisPosition(module->inputs[LunarJoystick::X_INPUT], module->params[LunarJoystick::X_PARAM], module->rangeX, module->xyPadDragging)
                : LunarJoystick::axisPosition(module->inputs[LunarJoystick::Y_INPUT], module->params[LunarJoystick::Y_PARAM], module->rangeY, module->xyPadDragging);
        };
        xyPad->setDragging = [module](bool dragging) {
            if (module) {
                module->xyPadDragging = dragging;
            }
        };
        addChild(xyPad);

        const float xL = 11.89693f;
        const float xR = 33.82307f;
        const float yIn = 72.f;
        const float yOffset = 89.f;
        const float yOut = 107.95455f;

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xL, yIn)), module, LunarJoystick::X_INPUT));
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(xL, yOffset)), module, LunarJoystick::X_OFFSET_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xL, yOut)), module, LunarJoystick::X_OUTPUT));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(xR, yIn)), module, LunarJoystick::Y_INPUT));
        addParam(createParamCentered<Rogan1PSRed>(mm2px(Vec(xR, yOffset)), module, LunarJoystick::Y_OFFSET_PARAM));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(xR, yOut)), module, LunarJoystick::Y_OUTPUT));
    }

    void step() override {
        syncPanelTheme(this, "LunarJoystick", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        LunarJoystick* module = dynamic_cast<LunarJoystick*>(this->module);
        assert(module);

        appendAmbientThemeMenu(menu);

        static const std::vector<std::string> rangeLabels = {
            "-5V to +5V", "0V to +10V (ie: MIDI CC)", "0V to +5V", "-10V to +10V"
        };

        menu->addChild(createIndexSubmenuItem("Input range X", rangeLabels,
            [=]() { return module->rangeX; },
            [=](int index) {
                pushIntFieldChange(module, "change X input range", module->rangeX, index,
                    [](engine::Module* m, int v) { dynamic_cast<LunarJoystick*>(m)->rangeX = v; });
                module->rangeX = index;
            }
        ));
        menu->addChild(createIndexSubmenuItem("Input range Y", rangeLabels,
            [=]() { return module->rangeY; },
            [=](int index) {
                pushIntFieldChange(module, "change Y input range", module->rangeY, index,
                    [](engine::Module* m, int v) { dynamic_cast<LunarJoystick*>(m)->rangeY = v; });
                module->rangeY = index;
            }
        ));
        // Convenience shortcut applying the same choice to both axes at once —
        // no separate persisted "linked" state, just sets both fields.
        menu->addChild(createIndexSubmenuItem("Input range (both)", rangeLabels,
            [=]() { return module->rangeX; },
            [=](int index) {
                if (module->rangeX != index) {
                    pushIntFieldChange(module, "change X input range", module->rangeX, index,
                        [](engine::Module* m, int v) { dynamic_cast<LunarJoystick*>(m)->rangeX = v; });
                    module->rangeX = index;
                }
                if (module->rangeY != index) {
                    pushIntFieldChange(module, "change Y input range", module->rangeY, index,
                        [](engine::Module* m, int v) { dynamic_cast<LunarJoystick*>(m)->rangeY = v; });
                    module->rangeY = index;
                }
            }
        ));
    }
};

Model* modelLunarJoystick = createModel<LunarJoystick, LunarJoystickWidget>("LunarJoystick");
