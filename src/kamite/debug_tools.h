#pragma once

// Dev tools in the F2 menu's Debug tab (dev-only: claude/strip-list.md), built from what the 2006-06-21 preview disc's
// debug menu showed of the game: its display switches (game::Dsp: hide Gene, the enemies, the stage, the HUD,
// the shadows ...) and a room jump to any room's jump point (etc/debugres/RoomJmpTbl.dat, which retail still loads,
// through the game's own door jump). See debug_tools.cpp.

#include "ps2_runtime.h"

#include <cstdint>

namespace kamite::debug_tools
{
    // Before the game loop (after PS2Runtime::setIoPaths: it reads the jump table from the disc): the F2 tab.
    void install(PS2Runtime &runtime);
    // Game frame start (frame_patches.cpp): keeps the hidden parts hidden and starts a requested jump. The registers
    // are the game's at that point; the jump runs on them and gives them back after.
    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // Shared with the NPCs tab (npc_tools.cpp).
    bool pathfindingOff();
    void setPathfindingOff(bool on);
}
