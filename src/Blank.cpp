#include "plugin.hpp"
#include "PanelTheme.hpp"

struct Blank : Module {
    int theme = 0;

    Blank() {
        config(0, 0, 0, 0);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "theme", json_integer(theme));
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* themeJ = json_object_get(rootJ, "theme");
        if (themeJ) {
            theme = json_integer_value(themeJ);
        }
    }
};

struct BlankWidget : ModuleWidget, ThemedModuleWidget {
    int appliedTheme = -1;

    BlankWidget(Blank* module) {
        setModule(module);
        syncPanelTheme(this, "Blank", module ? module->theme : 0, appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
    }

    void step() override {
        if (module) syncPanelTheme(this, "Blank", dynamic_cast<Blank*>(module)->theme, appliedTheme);
        ModuleWidget::step();
    }

    void applyTheme(int theme, history::ComplexAction* complexAction = nullptr) override {
        Blank* module = dynamic_cast<Blank*>(this->module);
        pushIntFieldChange(module, "change theme", module->theme, theme,
            [](engine::Module* m, int v) { dynamic_cast<Blank*>(m)->theme = v; }, complexAction);
        module->theme = theme;
        appliedTheme = theme;
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("Blank", theme))));
    }

    void appendContextMenu(Menu* menu) override {
        Blank* module = dynamic_cast<Blank*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
            [=]() { return module->theme; },
            [=](int theme) { applyTheme(theme); }
        ));
        appendApplyThemeToAllItem(menu, module->theme);
    }
};

Model* modelBlank = createModel<Blank, BlankWidget>("Blank");
