#include "plugin.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
    pluginInstance = p;
    p->addModel(modelBlank);
    p->addModel(modelLunar50Drone);
    p->addModel(modelLunarLFO);
    p->addModel(modelLunarVCO);
    p->addModel(modelLunarPapaSrapa);
}
