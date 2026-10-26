#pragma once

#include <filesystem>

class PS2Runtime;

namespace kamite
{
    // The gameplay camera's field of view and framing, and the enemies' facing cones that follow it (camera.cpp).
    // [Video] FieldOfView, FightFieldOfView, CameraDistance, CameraTilt and [Game] EnemiesFollowView, read at
    // install; the F2 menu's Camera tab changes them while playing and can save them back to kamite.ini.
    // aspect = the picture's width / height (widescreen.cpp).
    void installCamera(float aspect);
    // [Video] CameraShake (percent, 0 = none) scales the game's camera shake; F2 > Camera changes it. After the
    // function hooks are attached (main.cpp).
    void installCameraShake(PS2Runtime &runtime);

    // kamite.ini, for the F2 menu's Save / Revert (main.cpp, before installWidescreen).
    void setCameraIniPath(const std::filesystem::path &path);
    [[nodiscard]] const std::filesystem::path &cameraIniPath();
}
