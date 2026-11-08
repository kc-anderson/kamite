#pragma once

#include <cstdint>

class PS2Runtime;

// Buck Breaker ([Game] BuckBreaker, launcher Gameplay, off by default): the punish Gene gives stunned enemies, picked on
// the Techniques screen in a sixth right-column slot under Down + A, "[circle] Punish: Normal". The menu's OK cycles it:
// Normal (the game's own), Pummel, Spanking, Suplex, Stinger, Cobra Twist. Without Style switching it is one pick (the
// base, None's); with it each style (None, S, E, X, Y) keeps its own. The picks live in the save (cCoreSaveData +0x1480).
//
// In play the pick replaces only the five generic punishes (an enemy whose own is Stake Driver, Gorilla Suplex, Poke of
// God or a Pummel of its own keeps it; no prompt stays no prompt): func_002DCD50's switch on the enemy number is sent
// to the picked punish's case. Research and design: the project's claude/punish-notes.md.
namespace kamite::buck_breaker
{
    // Guest memory for the punishes' motions a room did not load (the DLL slot hole's free top: function_hooks.cpp
    // keeps its guard off this range).
    inline constexpr uint32_t kMotionArea = 0x007E0000u;
    inline constexpr uint32_t kMotionAreaEnd = 0x00810000u;

    // Before the game starts: the gameplay hook, the Techniques screen's slot, the per-vblank pick.
    void install(PS2Runtime &runtime);
    // style_presets (EE thread): a style first visited took the equipped moves of position `from` (0 None, 1..4 S..Y)
    // into position `to`; it takes that position's punish too.
    void styleCreated(int from, int to);
}
