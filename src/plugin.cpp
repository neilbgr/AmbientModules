#include "plugin.hpp"

Plugin *pluginInstance;

void init(Plugin *p) {
    pluginInstance = p;
    p->addModel(modelBlank);
    p->addModel(modelSolar50Drone);
    p->addModel(modelSolarLFO);
    p->addModel(modelSolarVCO);
}
