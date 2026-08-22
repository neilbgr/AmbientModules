#include "plugin.hpp"
#include "PanelTheme.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
    pluginInstance = p;
    loadAmbientTheme();
    p->addModel(modelBlank);
    p->addModel(modelLunar50Drone);
    p->addModel(modelLunarLFO);
    p->addModel(modelLunarVCO);
    p->addModel(modelLunarPapaSrapa);
    p->addModel(modelLunarSequencer);
}
