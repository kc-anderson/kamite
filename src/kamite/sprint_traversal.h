#pragma once

// Sprint attack traversal ([Game] SprintTraversal, off by default): during the sprint attacks, Gene passes the stage
// collision groups marked for them (railings he can jump over or slide through), room by room. The marks are set in
// F2 > Debug > Stage collision and kept in sprint_traversal.txt. See sprint_traversal.cpp.

#include "ps2_runtime.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

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

    // Windows: an opening in a wall (one plane of collision, the wall's group) that Gene dives through with sprint + X
    // or sprint + Y. Any number per wall. Marked from where Gene stood facing the wall: (x, y, z) the point hit on the
    // wall with y = Gene's floor, (dx, dz) the wall's direction (unit, horizontal); the opening is `width` wide centred
    // `offset` metres along the wall from that point, from `sill` to `sill + height` above the floor (drawn only: K's
    // choice, Oct 30, the height is not checked).
    struct Window
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, dx = 1.0f, dz = 0.0f;
        float offset = 0.0f, sill = 0.4f, width = 1.2f, height = 1.4f;
    };
    struct WindowRef
    {
        std::string group;
        int index = 0; // within the group's windows
        Window window;
    };
    std::vector<WindowRef> windows(uint16_t room);
    // Add (saved at once), change (kept in memory; flushWindows() saves), remove (saved at once). false = not saved.
    bool addWindow(uint16_t room, const std::string &group, const Window &window);
    void setWindow(uint16_t room, const std::string &group, int index, const Window &window);
    bool removeWindow(uint16_t room, const std::string &group, int index);
    bool flushWindows(); // saves pending changes (call when no slider is being dragged); false = could not write
}
