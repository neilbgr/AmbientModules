#include "plugin.hpp"
#include "PanelTheme.hpp"
#include "Widgets.hpp"

// Reproduces the "DRONE KEYS" pushbutton grid (manual p.6-7) that triggers
// the 6 real drone voices (Drone 1/2/4/5 = Lunar50Drone "Classic Solar50",
// Drone 3/6 = LunarPapaSrapa). VCO A/B aren't covered here — they already
// have their own gate/envelope, driven by the touch keyboard on the real
// hardware.
struct LunarPads : Module {
    static const int NUM_PADS = 6;

    // 0 = momentary (default, matches the hardware's pushbutton keyboard:
    // gate high only while held), 1 = latch (click toggles on/off, like a
    // mute button). Applies to all 6 pads at once.
    int latchMode = 0;

    enum ParamIds {
        ENUMS(PAD_PARAM, NUM_PADS),
        NUM_PARAMS
    };
    enum InputIds {
        ENUMS(GATE_INPUT, NUM_PADS),
        NUM_INPUTS
    };
    enum OutputIds {
        ENUMS(GATE_OUTPUT, NUM_PADS),
        NUM_OUTPUTS
    };
    enum LightIds {
        ENUMS(PAD_LIGHT, NUM_PADS),
        NUM_LIGHTS
    };

    LunarPads() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        static const char* labels[NUM_PADS] = {
            "Drone 1 (Solar50)", "Drone 2 (Solar50)", "Drone 3 (PapaSrapa)",
            "Drone 4 (Solar50)", "Drone 5 (Solar50)", "Drone 6 (PapaSrapa)"
        };
        for (int i = 0; i < NUM_PADS; i++) {
            configButton(PAD_PARAM + i, labels[i]);
            configInput(GATE_INPUT + i, string::f("%s external gate (e.g. MIDI)", labels[i]));
            configOutput(GATE_OUTPUT + i, string::f("%s gate", labels[i]));
        }
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "latchMode", json_integer(latchMode));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        if (json_t* j = json_object_get(rootJ, "latchMode")) {
            latchMode = json_integer_value(j);
        }
    }

    void process(const ProcessArgs& args) override {
        // The pad widget itself (VCVLightBezel, momentary or latch depending
        // on latchMode — toggled on the widget in LunarPadsWidget::step())
        // already reports the correct on/off state via its param value, native
        // to Rack's switch mechanics — no edge detection needed here.
        for (int i = 0; i < NUM_PADS; i++) {
            bool padOn = params[PAD_PARAM + i].getValue() > 0.f;
            // The external gate always ORs in, regardless of latch mode, so a
            // MIDI-driven gate source can trigger the pad alongside the mouse.
            bool externalOn = inputs[GATE_INPUT + i].getVoltage() >= 1.f;
            bool gateOn = padOn || externalOn;

            outputs[GATE_OUTPUT + i].setVoltage(gateOn ? 10.f : 0.f);
            lights[PAD_LIGHT + i].setBrightness(gateOn ? 1.f : 0.f);
        }
    }
};

struct LunarPadsWidget : ModuleWidget {
    int appliedTheme = -1;
    SquarePad* padSwitches[LunarPads::NUM_PADS] = {};

    LunarPadsWidget(LunarPads* module) {
        setModule(module);
        syncPanelTheme(this, "LunarPads", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ThemedScrew>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        // Placeholder layout: 7HP-wide panel (35.56mm), 2 columns (col 0 =
        // pads 1/2/3, col 1 = pads 4/5/6, same order as labels[] above).
        // Each column stacks, top to bottom: the 3 gate outputs, then the 3
        // pads (+ their square LEDs), then the 3 gate inputs — grouped by
        // function rather than by pad. 5HP (25.4mm) doesn't fit: a 13mm-wide
        // SquarePad needs its own half-width (6.5mm) plus clearance just to
        // clear the panel edge, so 5HP's ~5mm edge margin would run the pad
        // off the panel; 6HP (30.48mm) is still under the ~30.8mm minimum
        // (2 x (padHalfWidth + edge clearance) + (padWidth + inter-column
        // clearance)) once any reasonable clearance is added. 7HP is the
        // first size with comfortable margin. res/LunarPads_*.svg (all 5
        // themes) now match this width.
        const float bias = 0.75f;
        const float colX[2] = {9.f+bias, 26.56f-bias}; // 17.56mm column pitch on the 35.56mm-wide (7HP) panel
        const float topMargin = 15.f, bottomMargin = 11.f;
        const float usableHeight = 128.5f - topMargin - bottomMargin;
        const float jackPitch = 9.f;    // tightened pitch within the 3 stacked outs, and within the 3 stacked ins
        const float padPitch = 15.f;    // pitch within the 3 stacked pads, unchanged (already well spaced)
        // Remaining space split evenly into the out-block -> pad-block and
        // pad-block -> in-block gaps, left deliberately open for Neil's
        // planned silkscreen framing around each jack group. Subtracting an
        // extra jackPitch (not just the 2 x 2 x jackPitch inside the two
        // jack blocks) accounts for the half-jackPitch buffer added below
        // the top margin and above the bottom margin — createXCentered()
        // positions a widget's *center*, so without that buffer the first
        // output/last input would poke into the margin instead of sitting
        // inside it, same as every other component in this pack.
        const float sectionGap = (usableHeight - 5.f * jackPitch - 2.f * padPitch) / 2.f;
        const float ledOffsetY = -4.5f; // LED sits near the pad's top edge, not centered

        for (int col = 0; col < 2; col++) {
            float x = colX[col];
            float yOut = topMargin + jackPitch / 2.f; // buffer so the first jack's top edge lands on topMargin, not its center
            float yPad = yOut + 2.f * jackPitch + sectionGap;
            float yIn = yPad + 2.f * padPitch + sectionGap;

            for (int row = 0; row < 3; row++) {
                int i = col * 3 + row;

                addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(x, yOut + row * jackPitch)), module, LunarPads::GATE_OUTPUT + i));

                float y = yPad + row * padPitch;
                padSwitches[i] = createParamCentered<SquarePad>(mm2px(Vec(x, y)), module, LunarPads::PAD_PARAM + i);
                addParam(padSwitches[i]);
                addChild(createLightCentered<SquareLight>(mm2px(Vec(x, y + ledOffsetY)), module, LunarPads::PAD_LIGHT + i));

                addInput(createInputCentered<PJ301MPort>(mm2px(Vec(x, yIn + row * jackPitch)), module, LunarPads::GATE_INPUT + i));
            }
        }

        // File-explorer-style selection: a plain click on any pad releases
        // every *other* pad currently held via Ctrl+click (only meaningful
        // while the global latch mode is off — see LunarPads::latchMode —
        // since that's the only time individualLatch is ever set).
        for (int i = 0; i < LunarPads::NUM_PADS; i++) {
            padSwitches[i]->onPlainClick = [this](SquarePad* clicked) {
                LunarPads* m = dynamic_cast<LunarPads*>(this->module);
                if (!m || m->latchMode != 0) {
                    return;
                }
                for (auto* sw : padSwitches) {
                    if (sw && sw != clicked && sw->individualLatch) {
                        sw->individualLatch = false;
                        engine::ParamQuantity* pq = sw->getParamQuantity();
                        if (pq) {
                            pq->setMin();
                        }
                    }
                }
            };
        }
    }

    void step() override {
        syncPanelTheme(this, "LunarPads", appliedTheme);

        // Toggle `momentary` live to follow the persisted menu setting — same
        // Switch-level field VCVBezelLatch flips in its own constructor.
        // NB: SvgSwitch also has its own, differently-named `latch` field,
        // but that one means something else entirely: it makes onChange()
        // stop following the param value and instead show frame[1] only
        // while the mouse button is down (onDragStart/onDragEnd in
        // src/Rack/src/app/SvgSwitch.cpp), springing back to frame[0] on
        // release regardless of the actual toggled value — i.e. exactly the
        // "follows mousedown/up" bug reported. Leaving it at its default
        // `false` lets onChange() draw the frame from the param value
        // instead, so a latched-on pad now stays visually pressed after the
        // mouse is released, independently per pad (each SquarePad only
        // reacts to its own clicks, so one held/latched pad never blocks
        // another from being pressed — no shared "one finger" state).
        // A pad currently held via Ctrl+click (SquarePad::individualLatch,
        // see Widgets.hpp) keeps `momentary = false` regardless of the global
        // setting, until it's toggled back off — otherwise this loop would
        // stomp it back to `true` on the very next frame and the sticky pad
        // would auto-release on mouseup instead of staying down.
        LunarPads* m = dynamic_cast<LunarPads*>(module);
        if (m) {
            bool latch = m->latchMode != 0;
            for (auto* sw : padSwitches) {
                if (sw) {
                    sw->momentary = !latch && !sw->individualLatch;
                }
            }
        }

        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        LunarPads* module = dynamic_cast<LunarPads*>(this->module);
        assert(module);

        appendAmbientThemeMenu(menu);

        menu->addChild(new MenuSeparator);
        menu->addChild(createCheckMenuItem("Latch mode", "",
            [=]() { return module->latchMode != 0; },
            [=]() {
                int newValue = module->latchMode ? 0 : 1;
                pushIntFieldChange(module, "toggle pad latch mode", module->latchMode, newValue,
                    [](engine::Module* m, int v) { dynamic_cast<LunarPads*>(m)->latchMode = v; });
                module->latchMode = newValue;
            }
        ));
    }
};

Model* modelLunarPads = createModel<LunarPads, LunarPadsWidget>("LunarPads");
