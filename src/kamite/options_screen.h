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
// Build step 1 (Nov 12): the list of categories (CONTROLS, DISPLAY, INTERFACE, GAMEPLAY, SOUND, pause only LOAD GAME
// and RESET GAME, RESTORE DEFAULTS, EXIT), the cursor, the help box, the game's menu sounds, the Menu controls
// styles. CONTROLS opens the game's own controller page for now; RESET GAME uses the game's own Yes / No; the
// category pages and RESTORE DEFAULTS come next.
namespace kamite::options_screen
{
    // After the runtime is initialised and hd_textures::install (exeDir: the folder holding GHR.exe).
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);
}
