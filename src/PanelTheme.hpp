#pragma once
#include <rack.hpp>
#include <functional>
#include <string>
#include <vector>

using namespace rack;

// Shared named-theme mechanism: each module keeps a `theme` index (persisted
// via dataToJson/dataFromJson) resolved to "res/<ModuleName>_<ThemeName>.svg".
static const std::vector<std::string> PANEL_THEMES = {"Cream", "Black", "Pink", "Yellow", "Blue"};

static std::string panelThemePath(const std::string& moduleName, int themeIndex) {
    if (themeIndex < 0 || themeIndex >= (int)PANEL_THEMES.size())
        themeIndex = 0;
    return "res/" + moduleName + "_" + PANEL_THEMES[themeIndex] + ".svg";
}

// Cardinal's Engine::fromJson (src/override/Engine.cpp) creates the
// ModuleWidget *before* calling Module::fromJson(), unlike vanilla Rack —
// so the panel set in the widget constructor can be stale by the time the
// module's persisted `theme` is actually loaded. Widgets call this from both
// their constructor and step() (cheap int compare, no-op unless the theme
// actually changed) so the panel self-corrects as soon as the real theme is
// known, regardless of load order.
static inline void syncPanelTheme(rack::app::ModuleWidget* w, const std::string& moduleName, int wantedTheme, int& appliedTheme) {
    if (wantedTheme == appliedTheme) return;
    appliedTheme = wantedTheme;
    w->setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath(moduleName, wantedTheme))));
}

// createIndexSubmenuItem/createIndexPtrSubmenuItem (Rack's context-menu
// helpers) call their setter directly with no undo/redo or dirty-patch side
// effect at all — history::State::isSaved() only looks at whether an
// history::Action was ever pushed, so a menu-only field change (theme, FM
// topology, ...) never marks the patch as modified and can be silently lost
// on close. Push one of these instead of writing the field directly to fix
// that, matching how knob/param changes already behave.
struct IntFieldChange : history::ModuleAction {
    int oldValue = 0;
    int newValue = 0;
    std::function<void(engine::Module*, int)> apply;

    void undo() override {
        if (engine::Module* m = APP->engine->getModule(moduleId)) apply(m, oldValue);
    }
    void redo() override {
        if (engine::Module* m = APP->engine->getModule(moduleId)) apply(m, newValue);
    }
};

static inline void pushIntFieldChange(engine::Module* module, const std::string& actionName, int oldValue, int newValue,
                                       std::function<void(engine::Module*, int)> apply,
                                       history::ComplexAction* complexAction = nullptr) {
    if (oldValue == newValue) return;
    IntFieldChange* h = new IntFieldChange;
    h->name = actionName;
    h->moduleId = module->id;
    h->oldValue = oldValue;
    h->newValue = newValue;
    h->apply = apply;
    if (complexAction) complexAction->push(h);
    else APP->history->push(h);
}

// Mixin implemented by every AmbientModules ModuleWidget that has a theme, so
// "apply to all" can reach each one without a per-type dynamic_cast chain.
struct ThemedModuleWidget {
    virtual ~ThemedModuleWidget() = default;
    virtual void applyTheme(int theme, history::ComplexAction* complexAction = nullptr) = 0;
};

// Batches every module's theme change into one history::ComplexAction, so a
// single undo reverts the whole "apply to all" instead of one step per module.
static inline void appendApplyThemeToAllItem(Menu* menu, int currentTheme) {
    menu->addChild(createMenuItem("Apply theme to all AmbientModules", "", [=]() {
        history::ComplexAction* complexAction = new history::ComplexAction;
        complexAction->name = "apply theme to all panels";
        for (ModuleWidget* mw : APP->scene->rack->getModules()) {
            if (!mw->model || mw->model->plugin != pluginInstance) continue;
            if (ThemedModuleWidget* themed = dynamic_cast<ThemedModuleWidget*>(mw))
                themed->applyTheme(currentTheme, complexAction);
        }
        if (!complexAction->isEmpty())
            APP->history->push(complexAction);
        else
            delete complexAction;
    }));
}
