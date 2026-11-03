#pragma once

#include <cstdint>

// Tutorial popups reworded for the launcher's options (tutorial_text.cpp). With [Game] ControlStyle=modern, Tutorial 1
// (the right stick's dodge, stage 1) also explains Select (free camera / brawl mode) and, with ModernAuto, Auto.
namespace kamite::tutorial_text
{
    // Starts the RAM scan for the tutorial's message bank and the frame hook that points its text at ours.
    void install(uint8_t *rdram);
}
