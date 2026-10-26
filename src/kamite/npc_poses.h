#pragma once

// NPC poses: the bystanders who stand at a counter can lean on it (a rail idle from Quaternius' Universal Animation
// Library 2, CC0), set per NPC in the F2 menu's NPCs tab and saved to npc_poses.txt. See npc_poses.cpp.

#include "ps2_runtime.h"

#include <cstdint>
#include <filesystem>

namespace kamite::npc_poses
{
    // Before the game loop: the motion hook and the F2 tab. `exeDir` holds npc_poses.txt.
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);
    // Game frame start (frame_patches.cpp): applies edits, keeps turned NPCs turned, tracks the nearest one.
    void onFrame(uint8_t *rdram);
}
