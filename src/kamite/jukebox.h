#pragma once

#include <cstdint>

class PS2Runtime;

// The shop's jukebox (JukeBox_Execute 0x1F5CC0: the radio's song list over the shop). On the PS2 the game fades, sets
// 0x747A78's player-off bits (func_001F6C40: 0x80000000 | 0x40000000 | ..., so the player's Move 0x100530 returns at
// once and Gene is neither updated nor drawn) and the camera stands still (cCamera_stepState skips on bit 31). With the
// Modern controls the free camera kept turning while Gene was gone (Oct 25). Here: while the jukebox is open
// (0x7474B0 != 0, set by JukeBox_Load, cleared by JukeBox_Release) the game has the camera (modern_controls), and
// Gene stays in the picture: his Move runs with the two player-off bits cleared for its duration (his input copy,
// another object's Move, still sees them and copies nothing, so he stands and idles).
namespace kamite::jukebox
{
    bool open(const uint8_t *rdram);
    void install(PS2Runtime &runtime);
}
