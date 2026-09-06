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

// Same small-diameter red Rogan Valley/Plateau uses for its "Predelay" knob
// (RoganSmallRed in plugins/ValleyAudio/src/gui/ValleyComponents.hpp:140-146)
// — ~5.2mm, noticeably smaller than the stock Rogan1PSRed (~10.5mm) or even
// RoganMedSmallWhite/Black above (~8.4mm). See res/knobs/NOTICE.md.
struct RoganSmallRed : Rogan {
    RoganSmallRed() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSRedSmall.svg")));
        bg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSSmall-bg.svg")));
        fg->setSvg(Svg::load(asset::plugin(pluginInstance, "res/knobs/Rogan1PSRedSmall-fg.svg")));
    }
};

// LunarJoystick's CV input-range selector (4 detents, paired with
// configSwitch so it snaps — see Knob::onDragMove in the Rack SDK, which
// reads ParamQuantity::snapEnabled rather than a widget-level flag). Same
// art as RoganSmallRed, just confined to a tight left-side arc instead of
// Rogan's default near-full-circle sweep (-0.83pi..0.83pi), so all 4
// positions land on the left (9 o'clock-ish) — Neil wants the range labels
// printed to the left of the knob, pointer aimed at each in turn.
struct RoganRangeSelector : RoganSmallRed {
    RoganRangeSelector() {
        minAngle = -0.75f * (float)M_PI;
        maxAngle = -0.25f * (float)M_PI;
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
    // True while this one pad is being held down "by hand" via Ctrl+click,
    // overriding `momentary` for this pad only so a second pad can still be
    // pressed/released normally at the same time (simulates a second
    // finger). Cleared as soon as the pad goes back to off, whichever way
    // that happens — see onDragEnd below and LunarPadsWidget::step().
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

    // Releasing any *other* Ctrl-held pad (file-explorer-style "select only
    // this one") happens right away at mousedown, not delayed until this
    // press resolves — Neil expects the other pads to visibly let go the
    // instant he presses a new one (if Ctrl isn't held), same instant a
    // real explorer click narrows the selection. Independent of whatever
    // *this* pad's own press turns into (decided later, at mouseup below).
    void onDragStart(const widget::Widget::DragStartEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT && onPlainClick) {
            bool ctrlHeld = (APP->window->getMods() & RACK_MOD_MASK) == RACK_MOD_CTRL;
            if (!ctrlHeld) {
                onPlainClick(this);
            }
        }
        SvgSwitch::onDragStart(e);
    }

    // Whether *this* pad itself turns into a Ctrl-held sticky pad is decided
    // here, at mouseup, rather than at mousedown — queried live via
    // getMods() instead of a press-time snapshot, so you can start a plain
    // press and still turn it into a hold by pressing Ctrl before releasing
    // (not just before clicking). onDragStart above always runs the base
    // class's normal momentary press unconditionally, so the pad visually
    // presses the same way either way while held; only what happens at
    // release differs.
    void onDragEnd(const widget::Widget::DragEndEvent& e) override {
        if (e.button == GLFW_MOUSE_BUTTON_LEFT) {
            bool ctrlHeld = (APP->window->getMods() & RACK_MOD_MASK) == RACK_MOD_CTRL;
            if (ctrlHeld && momentary) {
                // Currently momentary (global latch mode is off): handle this
                // release as a one-shot toggle-on instead of Switch's usual
                // "spring back to min" — same effect as flipping this pad's
                // own latch on, without touching the other pads or the
                // persisted menu setting. Flipping momentary off BEFORE
                // calling the base class below means Switch::onDragEnd (via
                // SvgSwitch::onDragEnd) sees momentary == false and never
                // arms its own auto-release-to-min for this press.
                momentary = false;
                individualLatch = true;
            }
        }
        SvgSwitch::onDragEnd(e);
        if (individualLatch) {
            engine::ParamQuantity* pq = getParamQuantity();
            if (pq && pq->isMin()) {
                // Toggled back off (by a plain click or another Ctrl+click,
                // both take the toggle branch in onDragStart once momentary
                // is false) — drop the override; LunarPadsWidget::step()
                // puts momentary back to the global setting on its next pass.
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

// Square touch-pad control driving 2 params (X/Y) at once from a single
// click+drag gesture, for modules that want one combined manual/CV position
// control instead of 2 separate knobs (currently LunarJoystick). Not tied to
// any specific module type: the owning ModuleWidget wires `getPosition`
// (read the *effective* current value for axis 0=X/1=Y — same value the
// module's own process() uses, e.g. LunarJoystick::axisPosition(), so the
// crosshair never disagrees with what's actually driving the output) and
// `setDragging` (flip a module-side "manual overrides CV while held" flag —
// LunarJoystick::xyPadDragging). Deliberately not an app::ParamWidget: that
// class assumes a single paramId per widget, which doesn't fit a control
// that sets 2 params from one 2D position.
//
// Absolute-position dragging (click here = jump straight there, unlike a
// Knob's relative-delta drag) is done the same way plugins/JW-Modules/src/
// XYPad.cpp's own display widget does it: accumulate mouseDelta into a
// local drag position rather than reading it in one shot, and divide by
// getAbsoluteZoom() — Rack's DragMoveEvent delta is in screen pixels, so at
// any canvas zoom other than 100% an unscaled delta drifts out of sync with
// the actual cursor position.
struct XYPad : widget::OpaqueWidget {
    engine::Module* module = nullptr;
    int paramIdX = -1, paramIdY = -1;
    std::function<float(int axis)> getPosition; // axis: 0 = X, 1 = Y
    std::function<void(bool)> setDragging;
    float minValue = -5.f, maxValue = 5.f;

    Vec dragPos;
    float dragOldX = 0.f, dragOldY = 0.f;
    // The 2nd press of a double-click still runs onButton (jump + start a
    // drag) before onDoubleClick fires on top of it — without this flag,
    // the reset would push its own history entry and then the same
    // drag/click's onDragEnd would push a second, redundant one for what is
    // really one user gesture.
    bool suppressDragEndHistory = false;

    void onButton(const widget::Widget::ButtonEvent& e) override {
        OpaqueWidget::onButton(e);
        if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
            dragPos = e.pos; // always inside: Rack only delivers the press to the widget actually under the cursor
            if (module) {
                dragOldX = module->params[paramIdX].getValue();
                dragOldY = module->params[paramIdY].getValue();
            }
            if (setDragging) {
                setDragging(true);
            }
            setPosFromPixels(dragPos);
        }
    }

    void onDragMove(const widget::Widget::DragMoveEvent& e) override {
        if (e.button != GLFW_MOUSE_BUTTON_LEFT) {
            return;
        }
        Vec oldPos = dragPos; // raw position before this delta, may itself be outside if already frozen
        Vec newPos = oldPos.plus(e.mouseDelta.div(getAbsoluteZoom()));
        dragPos = newPos; // keep tracking the real cursor even while outside, so re-entry resumes from it exactly

        if (isInsideBox(newPos)) {
            setPosFromPixels(newPos);
        }
        else if (isInsideBox(oldPos)) {
            // Just crossed out this frame: freeze exactly where the
            // oldPos->newPos line crosses the box edge, rather than at
            // oldPos itself (which can lag behind on a fast drag) or at the
            // nearest edge point ignoring the drag's actual direction.
            setPosFromPixels(clipExitPoint(oldPos, newPos));
        }
        // else: was already outside and still is — stay frozen, nothing to apply.
    }

    void onDragEnd(const widget::Widget::DragEndEvent& e) override {
        if (e.button != GLFW_MOUSE_BUTTON_LEFT) {
            return;
        }
        if (setDragging) {
            setDragging(false);
        }
        if (suppressDragEndHistory) {
            suppressDragEndHistory = false;
            return;
        }
        pushXYHistory("move XY pad", dragOldX, dragOldY);
    }

    // Same convention as app::ParamWidget::resetAction (src/Rack/src/app/
    // ParamWidget.cpp) — double-click resets to the params' configured
    // default value, which for X_PARAM/Y_PARAM is already the pad's center
    // (0V), so this needs no special-cased "center" constant of its own.
    void onDoubleClick(const widget::Widget::DoubleClickEvent& e) override {
        if (!module) {
            return;
        }
        float oldX = module->params[paramIdX].getValue();
        float oldY = module->params[paramIdY].getValue();
        engine::ParamQuantity* pqX = module->getParamQuantity(paramIdX);
        engine::ParamQuantity* pqY = module->getParamQuantity(paramIdY);
        if (pqX) {
            pqX->reset();
        }
        if (pqY) {
            pqY->reset();
        }
        // The 2nd click of the double-click is still physically held down
        // when this fires (see onButton), so any further mouse movement
        // before release still calls onDragMove — which was about to
        // recompute the position from `dragPos`, still holding the click
        // spot from *before* this reset. Re-sync it to the box center so
        // the tiniest hand tremor during that still-held click doesn't snap
        // the pad straight back to where it was clicked, undoing the reset
        // before the mouse is even released.
        dragPos = box.size.div(2.f);
        pushXYHistory("reset XY pad", oldX, oldY);
        suppressDragEndHistory = true;
    }

    // One combined undo step for both axes at once, same convention as
    // app::Knob::onDragEnd (src/Rack/src/app/Knob.cpp) pushing a single
    // history::ParamChange per drag rather than one per pixel moved.
    void pushXYHistory(const std::string& name, float oldX, float oldY) {
        if (!module) {
            return;
        }
        float newX = module->params[paramIdX].getValue();
        float newY = module->params[paramIdY].getValue();
        if (oldX == newX && oldY == newY) {
            return;
        }
        history::ComplexAction* complexAction = new history::ComplexAction;
        complexAction->name = name;
        if (oldX != newX) {
            history::ParamChange* h = new history::ParamChange;
            h->moduleId = module->id;
            h->paramId = paramIdX;
            h->oldValue = oldX;
            h->newValue = newX;
            complexAction->push(h);
        }
        if (oldY != newY) {
            history::ParamChange* h = new history::ParamChange;
            h->moduleId = module->id;
            h->paramId = paramIdY;
            h->oldValue = oldY;
            h->newValue = newY;
            complexAction->push(h);
        }
        APP->history->push(complexAction);
    }

    bool isInsideBox(Vec p) const {
        return p.x >= 0.f && p.x <= box.size.x && p.y >= 0.f && p.y <= box.size.y;
    }

    // `from` must be inside the box; `to` may not be. Returns the point
    // where segment from->to first crosses the box edge (Liang-Barsky,
    // simplified for a box starting at the origin) — i.e. the actual pixel
    // where the cursor left the sensitive zone, not just wherever `from`
    // happened to be sampled.
    Vec clipExitPoint(Vec from, Vec to) const {
        float dx = to.x - from.x;
        float dy = to.y - from.y;
        float t = 1.f;
        if (dx > 0.f) {
            t = std::min(t, (box.size.x - from.x) / dx);
        }
        else if (dx < 0.f) {
            t = std::min(t, (0.f - from.x) / dx);
        }
        if (dy > 0.f) {
            t = std::min(t, (box.size.y - from.y) / dy);
        }
        else if (dy < 0.f) {
            t = std::min(t, (0.f - from.y) / dy);
        }
        t = clamp(t, 0.f, 1.f);
        return Vec(from.x + t * dx, from.y + t * dy);
    }

    void setPosFromPixels(Vec p) {
        if (!module) {
            return;
        }
        float nx = p.x / box.size.x;
        float ny = 1.f - p.y / box.size.y; // screen Y grows downward, voltage grows upward
        module->params[paramIdX].setValue(rescale(nx, 0.f, 1.f, minValue, maxValue));
        module->params[paramIdY].setValue(rescale(ny, 0.f, 1.f, minValue, maxValue));
    }

    void draw(const widget::Widget::DrawArgs& args) override {
        nvgBeginPath(args.vg);
        nvgRect(args.vg, 0.f, 0.f, box.size.x, box.size.y);
        nvgFillColor(args.vg, nvgRGBA(0x00, 0x00, 0x00, 0x30));
        nvgFill(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(0xff, 0xff, 0xff, 0x50));
        nvgStrokeWidth(args.vg, 1.f);
        nvgStroke(args.vg);

        float x = 0.f, y = 0.f;
        if (getPosition) {
            x = getPosition(0);
            y = getPosition(1);
        }
        float px = rescale(x, minValue, maxValue, 0.f, box.size.x);
        float py = rescale(y, minValue, maxValue, box.size.y, 0.f);

        NVGcolor crossColor = nvgRGB(0xff, 0xff, 0xff);
        nvgStrokeColor(args.vg, crossColor);
        nvgStrokeWidth(args.vg, 1.f);
        nvgBeginPath(args.vg);
        nvgMoveTo(args.vg, 0.f, py);
        nvgLineTo(args.vg, box.size.x, py);
        nvgMoveTo(args.vg, px, 0.f);
        nvgLineTo(args.vg, px, box.size.y);
        nvgStroke(args.vg);

        nvgFillColor(args.vg, crossColor);
        nvgBeginPath(args.vg);
        nvgCircle(args.vg, px, py, 2.5f);
        nvgFill(args.vg);
    }
};
