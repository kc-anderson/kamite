#pragma once

#include "ps2_runtime.h"

namespace kamite
{
    // The 60 FPS mode and the L3 dash ([Game] Fps60 / L3Dash in kamite.ini; KAMITE_FPS60 / KAMITE_L3_DASH), and
    // the per-frame hook at the top of the game's main loop (also counts game frames for the F3 overlay). Before
    // runtime.run().
    void installFramePatches(PS2Runtime &runtime);

    // The F2 menu (any thread). The 60 FPS mode switches between two game frames; the others take effect at once.
    bool fps60();
    void requestFps60(bool on);
    bool l3Dash();
    void setL3Dash(bool on);
    bool rouletteTimerDouble();
    // Game thread: the roulette wheel is on screen (cCockRoulette's move ran within the last two game frames).
    bool rouletteUp();
    void setRouletteTimerDouble(bool on);
}
