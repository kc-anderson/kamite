#pragma once

#include <filesystem>

struct Pcsx2GsUi;

namespace kamite::hotkeys
{
    // The port's shortcut keys (runtime/ps2_hotkeys.h) as kamite.ini [Hotkeys] lines, one per action id
    // (SaveState=F5, DebugDump=Shift+F8, Menu=F2 ...; "none" unbinds). Read at start into the runtime's table;
    // the F2 Controls tab's Hotkeys section and the launcher edit them, and saving writes them back.
    void install(const std::filesystem::path &iniPath);
    bool save(std::string &status);

    // The F2 menu's "Hotkeys" section (GS thread).
    void drawSection(const Pcsx2GsUi *ui);
}
