#pragma once

// Every player setting, once: the launcher's Options tabs and the in-game OPTIONS screen are both drawn from this list
// (K, Nov 13: "one settings table"), so their names, order, categories and descriptions can't drift apart. Data only:
// the launcher (launcher.cpp) draws it with ImGui, the game (options_screen.cpp) with its overlay and applies the live
// ones. GHR.ini stays the one store; `fallback` is each setting's shipped default (GHR.ini in the repo).

#include <cstddef>
#include <cstdint>
#include <string>

namespace kamite
{
    class IniFile;
}

namespace kamite::settings
{
    enum class Tab : uint8_t
    {
        Controls,
        Display,
        Interface,
        Gameplay,
        Sound,
        Mods, // launcher only
    };
    inline constexpr Tab kTabs[] = {Tab::Controls, Tab::Display, Tab::Interface, Tab::Gameplay, Tab::Sound, Tab::Mods};
    const char *tabName(Tab tab); // "Controls"...

    enum class Kind : uint8_t
    {
        Toggle, // 0 / 1
        Choice, // one of `choices` (by value)
        Slider, // one of `values` (a number; the volumes)
        Custom, // drawn by hand (the launcher's Display Mode, Character, texture packs)
    };

    struct Choice
    {
        const char *value; // in GHR.ini
        const char *label; // the launcher's
        const char *game;  // the in-game screen's (caps, short); a number for a slider-like choice
    };

    struct Setting
    {
        const char *id = "";          // unique (GHR.ini's key where there is one)
        Tab tab = Tab::Gameplay;
        const char *category = "";    // the heading it sits under (upper case in game)
        const char *name = "";        // the launcher's name (upper case in game)
        const char *description = ""; // shared: the launcher's help, the in-game help line
        const char *note = nullptr;   // the launcher's "NOTE:" line
        const char *gameNote = nullptr; // in game only, after the description ("Applies in the next room.")
        const char *section = "";
        const char *key = "";
        const char *fallback = "";    // the shipped default
        Kind kind = Kind::Toggle;
        const Choice *choices = nullptr;
        int choiceCount = 0;
        const int *values = nullptr; // a Slider's stops
        int valueCount = 0;
        const char *unit = "";       // after a number ("%", degrees)
        const char *parent = nullptr;      // a sub-option of this id: listed while it is on (a Toggle) or set to
        const char *parentValue = nullptr; // this value (a Choice)
        bool live = false;       // on the in-game screen, applied at once
        bool greyed = false;     // the launcher shows it greyed (not hidden) while its parent is off
        bool wideOnly = false;   // greyed in the launcher / left out in game at 4:3
        bool gameSlider = false; // a Choice of numbers: a slider in game (its choices' values are the stops)
    };

    extern const Setting kSettings[];
    extern const size_t kSettingCount;

    const Setting *find(const char *id);
    // The stored value (the fallback when unset). [Sound] MusicVolume falls back to 0 for an old MuteMusic=1.
    std::string value(const IniFile &ini, const Setting &setting);
    bool on(const IniFile &ini, const Setting &setting); // a Toggle's value
    // Whether a sub-option's parent lets it show.
    bool parentAllows(const IniFile &ini, const Setting &setting);
    int choiceIndex(const Setting &setting, const std::string &value); // -1: none of them
    // Camera & FOV ([Video] CameraPreset): the preset four camera values match ("custom" for none), and a pick written
    // (a preset's four values; Custom starts from Action's).
    const char *cameraPresetId(float fov, float fight, float zoomOut, float tilt);
    void writeCameraPreset(IniFile &ini, const std::string &id);
}
