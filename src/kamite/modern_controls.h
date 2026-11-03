#pragma once

#include <cstdint>

// Control Style (modern_controls.cpp): Classic, or Modern (a free camera on the right stick; R3 switches to the
// original controls and back, the choice kept across rooms; with Auto a fight's battle camera switches to them; L1 is
// always the game's quick turn). The input side runs on the window thread (a pad filter), the camera side in the camera hooks
// (camera.cpp, game thread); they meet in the atomics behind these calls.
namespace kamite::modern
{
    // [Game] ControlStyle, ModernAuto (and the camera speeds), read at install; registers the pad filter
    // and the F2 menu's Controls tab.
    void install();

    // Camera side (game thread, once per player camera update). battle: the battle (or a boss) preset camera is on
    // (+0x51C > 0); gameCamera: the game has taken the camera (a close-up preset, the second update path).
    void noteCamera(const uint8_t *rdram, bool battle, bool gameCamera);

    // The free camera: false = the original camera (behind Gene). While free, yaw is the camera's direction round
    // Gene in world radians and pitch the extra pitch to add. Advances the camera by the right stick (dt seconds),
    // and turns Gene toward the left stick's direction (rdram: the player object) while he walks or stands.
    // gameYaw: where the original camera would be this frame, which the free camera eases back to while the original
    // controls are in effect; geneYaw: Gene's own yaw.
    struct FreeCamera
    {
        bool free = false;
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    FreeCamera updateCamera(uint8_t *rdram, float gameYaw, float geneYaw, float dt);
    // The lowest extra pitch that keeps the camera above the ground at its current distance (camera.cpp, every update).
    void setPitchFloor(float floor);
    // setCamUpdate (a cut, a new room): back behind Gene.
    void resetCamera();

    // [Game] ControlStyle=modern is on (whatever is in effect now).
    bool enabled();
    // Modern's Auto is on (F2 or [Game] ModernAuto). Any thread.
    bool autoEnabled();
    // A hit between Gene and an enemy landed (either way; game thread, the damage steps): with Auto, brawl mode.
    void noteContact();

    // Modern is on and the free camera is in effect now (not the original controls by Select / Auto, and the game not
    // holding the camera). For the radar's glow (radar_glow.h). Any thread.
    bool freeCameraNow();
}
