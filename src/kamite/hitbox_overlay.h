#pragma once

// The hitbox overlay (F2 > Debug > Hitboxes; dev-only: claude/strip-list.md): every damage shape the game tests this
// frame - attack volumes and hurt volumes of Gene, the enemies and the objects - drawn as wireframe over the picture.
// The preview disc's DBG_COLL_AT_DISP, done host-side: the game is read, not changed. See hitbox_overlay.cpp.

#include "ps2_runtime.h"

#include <cstdint>

struct Pcsx2GsUi;

namespace kamite::hitbox_overlay
{
    // Before the game loop: registers the collection hook (km_hitbox_collect, recomp/kamite.toml).
    void install();
    // The Debug tab's "Hitboxes" section (debug_tools.cpp draws it between its own sections). GS thread.
    void drawPanel(const Pcsx2GsUi *ui);
    // cCamera_update built a world-to-screen matrix (camera.cpp's km_cam_matrix hook): the one the overlay projects
    // with. Game thread.
    void noteScreenMatrix(const float *m16);
    // Frame start (frame_patches.cpp), game thread: clears the overlay when Move() did not run last frame.
    void onFrame();
}
