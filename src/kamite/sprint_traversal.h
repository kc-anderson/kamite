#pragma once

// Sprint attack traversal ([Game] SprintTraversal, off by default): during the sprint attacks, Gene passes the stage
// collision groups marked for them (railings he can jump over or slide through), room by room. The marks are set in
// F2 > Debug > Stage collision and kept in sprint_traversal.txt. See sprint_traversal.cpp.

#include "ps2_runtime.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace kamite::sprint_traversal
{
    // Mark bits: which sprint attacks pass a group.
    inline constexpr int kJump = 1;  // sprint + X (diving attack), sprint + Y (flying kick)
    inline constexpr int kSlide = 2; // sprint + A (the slide)

    // Before the game loop: reads resources/sprint_traversal.txt (shipped) and sprint_traversal.txt (K's marks, next
    // to the exe, which win), and with the option on registers the two hooks round Gene's wall check.
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);
    bool enabled();
    // The F2 panel (GS thread): a group's marks in a room, and changing them (saved to sprint_traversal.txt at once;
    // false if the file could not be written).
    int marks(uint16_t room, const std::string &group);
    bool setMarks(uint16_t room, const std::string &group, int marks);
}
