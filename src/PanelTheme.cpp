#include "PanelTheme.hpp"
#include <cstdio>

int ambientTheme = 0;

void loadAmbientTheme() {
    std::string path = asset::user("AmbientModules.json");
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        return;

    json_error_t error;
    json_t* rootJ = json_loadf(f, 0, &error);
    std::fclose(f);
    if (!rootJ)
        return;

    if (json_t* themeJ = json_object_get(rootJ, "theme")) {
        int theme = json_integer_value(themeJ);
        if (theme >= 0 && theme < (int)PANEL_THEMES.size())
            ambientTheme = theme;
    }
    json_decref(rootJ);
}

void setAmbientTheme(int theme) {
    ambientTheme = theme;

    json_t* rootJ = json_object();
    json_object_set_new(rootJ, "theme", json_integer(theme));
    std::string path = asset::user("AmbientModules.json");
    FILE* f = std::fopen(path.c_str(), "w");
    if (f) {
        json_dumpf(rootJ, f, JSON_INDENT(2));
        std::fclose(f);
    }
    json_decref(rootJ);
}
