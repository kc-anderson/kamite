#pragma once

// The F2 menu's NPCs tab, top sections (dev-only: claude/strip-list.md): pathfinding off, AI off (practice dummies),
// Kill all and the Target enemy fold-out (can't die, a motion played over and over). See npc_tools.cpp.

#include "ps2_runtime.h"

#include <cstdint>

namespace kamite::npc_tools
{
    // Before npc_poses::install (the NPCs tab draws its sections in registration order: these go above Pose NPC).
    void install(PS2Runtime &runtime);
    // Game frame start (frame_patches.cpp, after debug_tools::onFrame): Kill all, the target and its motion.
    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // The enemies' damage step (debug_tools' km_dbg_em_damage): the locked target with "Can't die" keeps 1 HP.
    bool cantDie(uint32_t enemy);
}
