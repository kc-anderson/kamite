#pragma once

// The launcher: the first screen in the game window, before the game boots. Install (pick and check the player's own
// disc image), then Play / Options, with "don't show again" ([Launcher] Show=0 in kamite.ini; holding Shift
// while the game starts shows it again). Options edit kamite.ini in place. Drawn with Dear ImGui (rlImGui) on the
// raylib window the game then keeps; mouse, keyboard and any controller.
#include <filesystem>
#include <string>

namespace kamite::launcher
{
    struct Context
    {
        std::filesystem::path exeDir;
        std::filesystem::path iniPath;  // kamite.ini
        std::filesystem::path cacheDir; // game_data (the checked boot ELF is written there)
        std::filesystem::path iso;      // the disc image found so far (may be empty or unusable)
        std::string message;            // shown on the Install screen, e.g. why the configured image failed
    };

    enum class Result
    {
        Play, // kamite.ini holds the chosen image and settings; read the settings again and boot
        Quit,
    };

    // Whether to show it: [Launcher] Show (KAMITE_LAUNCHER, default 1), or Shift held at start, or no usable image.
    bool wanted(bool haveImage);

    // Main thread. Makes the game window (ps2_host_display::openHostWindow) and returns when the player chose.
    Result run(const Context &context);
}
