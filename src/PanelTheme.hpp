#pragma once
#include <rack.hpp>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "plugin.hpp"

using namespace rack;

// AmbientModules-wide panel theme: a single value persisted process-wide (in
// Rack's user asset dir), same pattern as Aluminium's own PanelTheme
// (plugins/Aluminium/src/PanelTheme.cpp) — rather than per-module/per-patch,
// so every module, already placed or added later, always shows the same
// currently-chosen theme with no separate "apply to all" step needed.
static const std::vector<std::string> PANEL_THEMES = {"Cream", "Black", "Pink", "Yellow", "Blue"};

static std::string panelThemePath(const std::string& moduleName, int themeIndex) {
    if (themeIndex < 0 || themeIndex >= (int)PANEL_THEMES.size()) {
        themeIndex = 0;
    }
    return "res/" + moduleName + "_" + PANEL_THEMES[themeIndex] + ".svg";
}

extern int ambientTheme;
// Plugin-wide too, in the same file: oscillator LEDs follow their
// oscillator (true) or are plain on/off (false). Read by the audio thread.
extern std::atomic<bool> ambientPitchLights;

void loadAmbientSettings();
void setAmbientTheme(int theme);
void setAmbientPitchLights(bool on);
// A module's own defaults for new instances ("Use these settings for new
// ..."), as its dataToJson(): saved in the same file, never in a patch.
// copy: a copy the caller decrefs, nullptr if none. set: takes ownership
// of dataJ.
json_t* copyAmbientModuleDefaults(const std::string& slug);
void setAmbientModuleDefaults(const std::string& slug, json_t* dataJ);

// Cardinal's Engine::fromJson (src/override/Engine.cpp) creates the
// ModuleWidget *before* calling Module::fromJson(), unlike vanilla Rack —
// widgets call this from both their constructor and step() (cheap int
// compare, no-op unless the theme actually changed) so the panel
// self-corrects as soon as the plugin's theme file has been loaded,
// regardless of load order.
static inline void syncPanelTheme(rack::app::ModuleWidget* w, const std::string& moduleName, int& appliedTheme) {
    if (ambientTheme == appliedTheme) {
        return;
    }
    appliedTheme = ambientTheme;
    w->setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath(moduleName, ambientTheme))));
}

// createIndexSubmenuItem/createIndexPtrSubmenuItem (Rack's context-menu
// helpers) call their setter directly with no undo/redo or dirty-patch side
// effect at all — history::State::isSaved() only looks at whether an
// history::Action was ever pushed, so a menu-only field change (FM topology,
// CV range, ...) never marks the patch as modified and can be silently lost
// on close. Push one of these instead of writing the field directly to fix
// that, matching how knob/param changes already behave.
struct IntFieldChange : history::ModuleAction {
    int oldValue = 0;
    int newValue = 0;
    std::function<void(engine::Module*, int)> apply;

    void undo() override {
        if (engine::Module* m = APP->engine->getModule(moduleId)) {
            apply(m, oldValue);
        }
    }
    void redo() override {
        if (engine::Module* m = APP->engine->getModule(moduleId)) {
            apply(m, newValue);
        }
    }
};

static inline void pushIntFieldChange(engine::Module* module, const std::string& actionName, int oldValue, int newValue,
                                       std::function<void(engine::Module*, int)> apply,
                                       history::ComplexAction* complexAction = nullptr) {
    if (oldValue == newValue) {
        return;
    }
    IntFieldChange* h = new IntFieldChange;
    h->name = actionName;
    h->moduleId = module->id;
    h->oldValue = oldValue;
    h->newValue = newValue;
    h->apply = apply;
    if (complexAction) {
        complexAction->push(h);
    }
    else {
        APP->history->push(h);
    }
}

// Appends the single "Theme" submenu shared by every AmbientModules module's
// right-click menu — changes the plugin-wide ambientTheme immediately,
// repainting every open module (old or new) since they all read the same
// global on their next step().
// globalLabel: heads it with a greyed "Global" label, for a module that
// lists other pack-wide settings right after it.
inline void appendAmbientThemeMenu(Menu* menu, bool globalLabel = false) {
    menu->addChild(new MenuSeparator);
    if (globalLabel) {
        menu->addChild(createMenuLabel("Global"));
    }
    menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
        [=]() { return ambientTheme; },
        [=](int theme) { setAmbientTheme(theme); }
    ));
}
