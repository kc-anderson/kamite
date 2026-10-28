#pragma once

// Dash logger (temporary diagnostic: claude/strip-list.md). Logs which words of Gene's object and GameWork change
// around left-stick taps, to find the double-tap window of the stick dash. See dash_probe.cpp.

#include <cstdint>

struct Pcsx2GsUi;

namespace kamite::dash_probe
{
    // Game frame start (frame_patches.cpp), after the stick writes. Does nothing while the logger is off.
    void onFrame(const uint8_t *rdram);
    // Its checkbox in F2 > Debug. GS thread.
    void drawPanel(const Pcsx2GsUi *ui);
}
