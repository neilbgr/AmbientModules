#include "plugin.hpp"
#include "PanelTheme.hpp"

struct Blank : Module {
    Blank() {
        config(0, 0, 0, 0);
    }
};

struct BlankWidget : ModuleWidget {
    int appliedTheme = -1;

    BlankWidget(Blank* module) {
        setModule(module);
        syncPanelTheme(this, "Blank", appliedTheme);

        addChild(createWidget<ThemedScrew>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, 0)));
        addChild(createWidget<ThemedScrew>(Vec(box.size.x / 2 - RACK_GRID_WIDTH / 2, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
    }

    void step() override {
        syncPanelTheme(this, "Blank", appliedTheme);
        ModuleWidget::step();
    }

    void appendContextMenu(Menu* menu) override {
        appendAmbientThemeMenu(menu);
    }
};

Model* modelBlank = createModel<Blank, BlankWidget>("Blank");
