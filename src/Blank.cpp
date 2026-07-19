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

struct BlankWidget : ModuleWidget {
    BlankWidget(Blank* module) {
        setModule(module);
        setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("Blank", module ? module->theme : 0))));

        addChild(createWidget<ThemedScrew>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
    }

    void appendContextMenu(Menu* menu) override {
        Blank* module = dynamic_cast<Blank*>(this->module);
        assert(module);

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexSubmenuItem("Theme", PANEL_THEMES,
            [=]() { return module->theme; },
            [=](int theme) {
                module->theme = theme;
                setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, panelThemePath("Blank", theme))));
            }
        ));
    }
};

Model* modelBlank = createModel<Blank, BlankWidget>("Blank");
