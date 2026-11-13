#pragma once

#include "ps2_runtime.h"

#include <filesystem>

// The OPTIONS screen of God Hand: Rearmed (claude/rearmed-options.md). The title screen's and the pause menu's Options
// links open our own screen in place of the game's (whose row names are painted into its background): the game's
// object (cSubScrOption in the pause menu, cSubScrOptionT on the title screen) keeps its opening, closing and pause
// flow, and its move / update / draw are ours while its list is up.
//
// The look: K's painted brick wall (resources/options/wall.jpg, his art: no credit) fills the screen at any aspect
// ratio, our text pinned to the picture's edges (the text area is capped at 21:9). The wall goes in place of the
// game's own backdrop texture: a PS2-resolution stand-in in the game's memory, swapped by the GS for the full image
// (cache/hd-options, texture_add) as the button prompts are.
//
// The list of categories (CONTROLS, DISPLAY, INTERFACE, GAMEPLAY, SOUND, pause only LOAD GAME and RESET GAME, RESTORE
// DEFAULTS, EXIT); each category opens its page of settings (settings_table.cpp, the launcher's own list): the live
// ones, changed with left / right, saved to GHR.ini and applied at once. RESET GAME and RESTORE DEFAULTS ask Yes / No.
namespace kamite::options_screen
{
    // After the runtime is initialised and hd_textures::install (exeDir: the folder holding GHR.exe).
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);
    // Game thread, once a game frame (frame_patches): the game's own options (Enemy Life Gauge, the sound mode,
    // vibration) written into the game whenever they differ from GHR.ini's (a loaded save brings its own), and the
    // playing streams' volumes re-applied after a volume change.
    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
