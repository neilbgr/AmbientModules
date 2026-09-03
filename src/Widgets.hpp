#pragma once
#include "plugin.hpp"
#include <functional>

using namespace rack;

// Same knob Valley/Plateau uses for its "Dry level"/"Wet level" controls
// (RoganMedSmallWhite in plugins/ValleyAudio/src/gui/ValleyComponents.hpp:46-52,
// itself just Rack SDK's `Rogan` pointed at Valley's own SVG variant) — see
// res/knobs/NOTICE.md for the artwork's origin/license. Black is a recolor of
// White's round face (fg) only; both share the same cap body (dark knurled
// rim + position-indicator tab) and bg ring unmodified, so the indicator tab
// stays the same color regardless of face color.
struct RoganMedSmallWhite : Rogan {
    RoganMedSmallWhite() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSWhiteMedSmall.svg")));
        bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSMedSmall-bg.svg")));
        fg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSWhiteMedSmall-fg.svg")));
    }
};

struct RoganMedSmallBlack : Rogan {
    RoganMedSmallBlack() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSWhiteMedSmall.svg")));
        bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSMedSmall-bg.svg")));
        fg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSBlackMedSmall-fg.svg")));
    }
};

// Square, near-black trigger pad (LunarPads "Drone Voices" grid) reproducing
// the SOLAR 42F hardware's pushbutton look, since every stock Rack SDK
// button/bezel (VCVButton, TL1105, CKD6, VCVBezel...) is round. Same
// two-frame SvgSwitch idiom as e.g. WSTD-Drums' DKPad/cf's PadButton — frame
// index follows the param value (0/1), so it shows pressed/released like any
// other Rack switch. `momentary`/`latch` are plain runtime flags on the base
// class, toggled live in LunarPadsWidget::step() same as it did for the
// previous VCVLightBezel.
struct SquarePad : app::SvgSwitch {
    // Set from the most recent left-button press (onButton carries mouse
    // modifiers; onDragStart doesn't), so onDragStart can tell a Ctrl+click
    // apart from a plain one.
    bool ctrlHeld = false;
    // True while this one pad is being held down "by hand" via Ctrl+click,
    // overriding `momentary` for this pad only so a second pad can still be
    // pressed/released normally at the same time (simulates a second
    // finger). Cleared as soon as the pad goes back to off, whichever way
    // that happens — see onDragStart below and LunarPadsWidget::step().
    bool individualLatch = false;
    // Wired by the owning ModuleWidget (LunarPadsWidget) to release any
    // *other* pad currently held via Ctrl+click, file-explorer-selection
    // style: a plain click "selects" only the clicked pad, Ctrl+click adds
    // to the held set instead of replacing it. Left null (no-op) for a
    // SquarePad used standalone; the owner itself gates this on the global
    // latch-mode setting, so nothing happens while it's checked.
    std::function<void(SquarePad*)> onPlainClick;

    SquarePad() {
        addFrame(Svg::load(asset::plugin(pluginInstance, "res/pads/SquarePad-off.svg")));
        addFrame(Svg::load(asset::plugin(pluginInstance, "res/pads/SquarePad-on.svg")));
        // SvgSwitch always adds its own round drop shadow (app::CircularShadow,
        // src/Rack/src/app/CircularShadow.cpp — an nvgCircle regardless of the
        // switch's actual shape) sized to the switch's box. The pad art already
        // bakes in its own offset shadow, so drop Rack's circular one.
        fb->removeChild(shadow);
        delete shadow;
        shadow = nullptr;
    }

    void onButton(const widget::Widget::ButtonEvent& e) override {
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            ctrlHeld = (e.mods & RACK_MOD_MASK) == RACK_MOD_CTRL;
        }
        SvgSwitch::onButton(e);
    }

    void onDragStart(const widget::Widget::DragStartEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT && !ctrlHeld && onPlainClick) {
            onPlainClick(this);
        }
        if (e.button == GLFW_MOUSE_BUTTON_LEFT && ctrlHeld && momentary) {
            // Currently momentary (global latch mode is off): handle this
            // click as a one-shot toggle instead of Switch's usual "press to
            // max, spring back to min on release" — same effect as flipping
            // this pad's own latch on, without touching the other pads or
            // the persisted menu setting.
            momentary = false;
            individualLatch = true;
        }
        SvgSwitch::onDragStart(e);
        if (individualLatch) {
            engine::ParamQuantity* pq = getParamQuantity();
            if (pq && pq->isMin()) {
                // Toggled back off (by a plain click or another Ctrl+click,
                // both take the toggle branch once momentary is false) —
                // drop the override; LunarPadsWidget::step() puts momentary
                // back to the global setting on its next pass.
                individualLatch = false;
            }
        }
    }
};

// Square gate-indicator LED to sit near the top of a SquarePad, matching the
// hardware's square LED (Rack's stock LightWidget::drawBackground/drawLight
// always draw a circle — see src/Rack/src/app/LightWidget.cpp — so this
// overrides both with nvgRoundedRect instead). Kept as its own sibling
// widget rather than fused into SquarePad via createLightParamCentered, so
// it can be positioned off-center (near the pad's top edge) instead of
// Rack's automatic dead-center placement.
struct SquareLight : WhiteLight {
    SquareLight() {
        box.size = mm2px(Vec(2.5f, 2.5f));
    }

    void drawBackground(const widget::Widget::DrawArgs& args) override {
        float r = std::min(box.size.x, box.size.y) * 0.2f;
        nvgBeginPath(args.vg);
        nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, r);
        if (bgColor.a > 0.f) {
            nvgFillColor(args.vg, bgColor);
            nvgFill(args.vg);
        }
        if (borderColor.a > 0.f) {
            nvgStrokeWidth(args.vg, 0.5f);
            nvgStrokeColor(args.vg, borderColor);
            nvgStroke(args.vg);
        }
    }

    void drawLight(const widget::Widget::DrawArgs& args) override {
        if (color.a > 0.f) {
            float r = std::min(box.size.x, box.size.y) * 0.2f;
            nvgBeginPath(args.vg);
            nvgRoundedRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, r);
            nvgFillColor(args.vg, color);
            nvgFill(args.vg);
        }
    }
};
