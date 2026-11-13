#pragma once

#include <filesystem>

class PS2Runtime;

namespace kamite
{
    // The gameplay camera's field of view and framing, and the enemies' facing cones that follow it (camera.cpp).
    // [Video] FieldOfView, FightFieldOfView, CameraDistance, CameraTilt and [Game] EnemiesFollowView, read at
    // install; the F2 menu's Camera tab changes them while playing and can save them back to GHR.ini.
    // aspect = the picture's width / height (widescreen.cpp).
    void installCamera(float aspect);
    // [Video] SteadyIdleCamera (0/1, default 0): the player camera neither rocks nor shakes with Gene's idle; F2 >
    // Camera changes it. After the function hooks are attached (main.cpp).
    void installSteadyIdleCamera(PS2Runtime &runtime);

    // The in-game OPTIONS (any thread): the camera, See Through and depth of field values back from GHR.ini (F2's
    // Revert, after the screen wrote a change there).
    void reloadCameraSettings();

    // GHR.ini, for the F2 menu's Save / Revert (main.cpp, before installWidescreen).
    void setCameraIniPath(const std::filesystem::path &path);
    [[nodiscard]] const std::filesystem::path &cameraIniPath();
}
