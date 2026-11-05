#include "PanelTheme.hpp"
#include <cstdio>
#include <mutex>

int ambientTheme = 0;
std::atomic<bool> ambientPitchLights{true};
// "moduleDefaults": { "<module slug>": <that module's dataToJson()> }.
// Guarded: a host may build a module (which reads it) off the UI thread.
static json_t* moduleDefaultsJ = nullptr;
static std::mutex moduleDefaultsMutex;

static std::string settingsPath() {
    return asset::user("AmbientModules.json");
}

// The settings file as it is on disk now, or nullptr if missing/unreadable.
static json_t* readSettingsFile() {
    FILE* f = std::fopen(settingsPath().c_str(), "r");
    if (!f) {
        return nullptr;
    }
    json_error_t error;
    json_t* rootJ = json_loadf(f, 0, &error);
    std::fclose(f);
    if (rootJ && !json_is_object(rootJ)) {
        json_decref(rootJ);
        return nullptr;
    }
    return rootJ;
}

void loadAmbientSettings() {
    json_t* rootJ = readSettingsFile();
    if (!rootJ) {
        return;
    }

    if (json_t* themeJ = json_object_get(rootJ, "theme")) {
        int theme = json_integer_value(themeJ);
        if (theme >= 0 && theme < (int)PANEL_THEMES.size()) {
            ambientTheme = theme;
        }
    }
    if (json_t* lightsJ = json_object_get(rootJ, "pitchLights")) {
        ambientPitchLights = json_boolean_value(lightsJ);
    }
    json_t* defaultsJ = json_object_get(rootJ, "moduleDefaults");
    if (json_is_object(defaultsJ)) {
        std::lock_guard<std::mutex> lock(moduleDefaultsMutex);
        if (moduleDefaultsJ) {
            json_decref(moduleDefaultsJ);
        }
        moduleDefaultsJ = json_deep_copy(defaultsJ);
    }
    json_decref(rootJ);
}

// Changes one setting in the file as it is on disk now, keeping the others:
// another Rack or Cardinal instance may have saved its own since we loaded.
// Takes ownership of valueJ.
static void saveAmbientSetting(const char* key, json_t* valueJ) {
    json_t* rootJ = readSettingsFile();
    if (!rootJ) {
        rootJ = json_object();
    }
    json_object_set_new(rootJ, key, valueJ);
    FILE* f = std::fopen(settingsPath().c_str(), "w");
    if (f) {
        json_dumpf(rootJ, f, JSON_INDENT(2));
        std::fclose(f);
    }
    json_decref(rootJ);
}

void setAmbientTheme(int theme) {
    ambientTheme = theme;
    saveAmbientSetting("theme", json_integer(theme));
}

void setAmbientPitchLights(bool on) {
    ambientPitchLights = on;
    saveAmbientSetting("pitchLights", json_boolean(on));
}

json_t* copyAmbientModuleDefaults(const std::string& slug) {
    std::lock_guard<std::mutex> lock(moduleDefaultsMutex);
    json_t* dataJ = moduleDefaultsJ ? json_object_get(moduleDefaultsJ, slug.c_str()) : nullptr;
    return dataJ ? json_deep_copy(dataJ) : nullptr;
}

void setAmbientModuleDefaults(const std::string& slug, json_t* dataJ) {
    {
        std::lock_guard<std::mutex> lock(moduleDefaultsMutex);
        if (!moduleDefaultsJ) {
            moduleDefaultsJ = json_object();
        }
        json_object_set(moduleDefaultsJ, slug.c_str(), dataJ);
    }
    // Merged into the file's own moduleDefaults, keeping other modules'.
    json_t* fileJ = readSettingsFile();
    json_t* fileDefaultsJ = fileJ ? json_object_get(fileJ, "moduleDefaults") : nullptr;
    json_t* mergedJ = json_is_object(fileDefaultsJ) ? json_deep_copy(fileDefaultsJ) : json_object();
    if (fileJ) {
        json_decref(fileJ);
    }
    json_object_set_new(mergedJ, slug.c_str(), dataJ);
    saveAmbientSetting("moduleDefaults", mergedJ);
}
