#pragma once
#include <rack.hpp>
#include <string>
#include <vector>

using namespace rack;

// Shared named-theme mechanism: each module keeps a `theme` index (persisted
// via dataToJson/dataFromJson) resolved to "res/<ModuleName>_<ThemeName>.svg".
// Swap is immediate (setPanel() again from the context-menu handler) — no
// need to poll every frame in step() since there's no external/global theme
// source to react to (menu-only selection, no Rack dark-mode link).
static const std::vector<std::string> PANEL_THEMES = {"WhiteCream", "Black", "Pink", "Yellow", "Blue"};

static std::string panelThemePath(const std::string& moduleName, int themeIndex) {
    if (themeIndex < 0 || themeIndex >= (int)PANEL_THEMES.size())
        themeIndex = 0;
    return "res/" + moduleName + "_" + PANEL_THEMES[themeIndex] + ".svg";
}
