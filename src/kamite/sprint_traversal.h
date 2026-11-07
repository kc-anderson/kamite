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
    // choice, Oct 30, the height is not checked). `depth`: the wall's far face, metres along (-dz, dx) from the marked one
    // (a thick wall is two faces, often two groups); the window covers both and the space between.
    struct Window
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, dx = 1.0f, dz = 0.0f;
        float offset = 0.0f, sill = 0.4f, width = 1.2f, height = 1.4f, depth = 0.0f;
        int passes = kJump; // which sprint attacks go through it: kJump (sprint + X / Y), kSlide (sprint + A), both
        // One-way (K, Nov 6: new windows are, windows before that stay two-way): Gene passes only heading the way he
        // faced when it was marked, side = +1 along (-dz, dx), -1 against it.
        bool twoWay = true;
        int side = 1;
        // Linked to a door (K, Nov 7): a window into the space behind a door that leads to the next room. When Gene lands
        // from a dive through it, that door's own trigger runs (the same jump, fade and SAVE / CONTINUE screen as walking
        // through it), so he never stands where the room has no collision. The door's trigger is found by its two bytes
        // (cSceAtUnit +0x5E and +0x79, what func_002C2078 passes cDoor_getJumpData); -1 = not linked. A linked window is
        // one-way.
        int doorRoom = -1, doorPoint = -1;
        bool linked() const { return doorPoint >= 0; }
    };
    // The side a window marked by Gene heading (fx, fz) lets him through from.
    inline int windowSide(const Window &w, float fx, float fz) { return fx * -w.dz + fz * w.dx >= 0.0f ? 1 : -1; }
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
    // The room's doors (scenario trigger units of type 1, handler func_002C2078), refreshed each frame by onFrame.
    struct Door
    {
        int room = 0, point = 0; // cSceAtUnit +0x5E, +0x79
        float x = 0.0f, y = 0.0f, z = 0.0f; // the trigger area's centre
        bool locked = false;     // +0x7A
    };
    std::vector<Door> doors();
    // Game frame start (frame_patches.cpp): the door list, and a dive through a linked window runs its door on landing.
    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    // Every room back to the marks and windows the port ships (resources/sprint_traversal.txt): the user's file keeps
    // only its header. false = could not write.
    bool restoreShipped();
}
