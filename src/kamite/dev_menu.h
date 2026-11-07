#pragma once

#include <filesystem>
#include <string>

namespace kamite::dev_menu
{
    // The F2 menu as the dev menu: its tab order, and the Display, Graphics and Gameplay sections that switch the
    // launcher's options while the game runs (the ones that can be: the rest are shown with "set in the launcher").
    // Before the modules that add their own tabs; the menu's title and hint are set here too.
    void install(const std::filesystem::path &iniPath);
    // Writes one value into kamite.ini (any thread; false when the file can't be read or written).
    bool saveIniValue(const char *section, const char *key, const std::string &value);
}
