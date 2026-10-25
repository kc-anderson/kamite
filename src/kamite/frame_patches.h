#pragma once

#include "ps2_runtime.h"

namespace kamite
{
    // The 60 FPS mode and the L3 dash ([Game] Fps60 / L3Dash in kamite.ini; KAMITE_FPS60 / KAMITE_L3_DASH), and
    // the per-frame hook at the top of the game's main loop (also counts game frames for the F3 overlay). Before
    // runtime.run().
    void installFramePatches(PS2Runtime &runtime);
}
