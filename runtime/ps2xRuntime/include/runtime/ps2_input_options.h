#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ps2_host_input
{
    constexpr uint32_t kInvertRightX = 1u;
    constexpr uint32_t kInvertRightY = 2u;

    struct MouseCameraInput
    {
        float x = 0.0f;
        float y = 0.0f;
        bool active = false;
    };

    void setRightStickInvertDefault(uint32_t axes);
    void setMouseCameraEnabled(bool enabled);
    void setMouseStickEnabled(bool enabled);
    [[nodiscard]] MouseCameraInput consumeMouseCameraInput();

    // Controller pad (0-based) settings when controls.ini has no [Pad<pad+1>] section: PCSX2's [PadN] keys in file
    // order, a key repeated once per binding. urban.exe writes controls.ini with them.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> defaultPadSettings(uint32_t pad);

    // The controls file ([Pad1]-[Pad8] in PCSX2's syntax) the Controls screen (F1) edits; call before the run loop.
    // Missing: written from `legacy` (the PS2X_PAD<n>=1 / PS2X_PAD<n>_<KEY> settings an older kamite.ini carried),
    // else from the defaults. Reloaded while the game runs when it changes. A pad with PS2X_PAD<n> in the environment
    // keeps using those variables. An empty path: no file (the old behaviour).
    void setControlsFile(const std::string &path, const std::vector<std::pair<std::string, std::string>> &legacy);
}
