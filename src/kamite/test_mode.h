#pragma once

// The test mode (dev-only; claude/strip-list.md): the case recorder (the RecordCase hotkey, the right stick press:
// press, do the move, press again; one cases\<date-time>.case file with a state of its own, the pad and a live per-frame log), the case
// replay (KAMITE_REPLAY_CASE / --replay-case <file> [--fps60 0|1]: the game's logic plays the case back at 30 or 60
// and logs every frame to <case>.30.csv / .60.csv, then quits) and scripted runs (KAMITE_TEST=<script>, [Run] Test,
// --test). For measuring the game at 30 and at 60 FPS instead of counting frames by eye: claude/test-mode.md.

#include "ps2_runtime.h"

#include <filesystem>
#include <string>
#include <vector>

namespace kamite::test_mode
{
    // Before the game loop, after installFramePatches (the script may switch the frame rate). Relative paths in the
    // script are taken from exeDir.
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);
    // A case replay (KAMITE_REPLAY_CASE), right after kamite.ini was read and before anything reads the settings: the
    // recorder's gameplay settings kept in the case's meta (env.<variable>=<value>) replace this machine's. Returns
    // log lines.
    std::vector<std::string> applyCaseSettings(const std::filesystem::path &exeDir);
    // From the main-loop hook (frame_patches.cpp frameStart), once a game frame.
    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // After the run: the test could not be carried out (the state did not load, a file could not be written).
    [[nodiscard]] bool failed();
    // A case is being recorded (any thread; for menus).
    [[nodiscard]] bool recording();

    // The Modern controls' host side (modern_controls.cpp updateCamera, game thread, once a game frame): the free
    // camera and the turn it gives Gene (his yaw +0x104) are not in a save state, so a case keeps them per frame (its
    // `host` section) and a replay plays them back instead of computing them.
    struct HostCamera
    {
        bool free = false;
        float yaw = 0.0f;
        float pitch = 0.0f;
        bool turned = false; // Gene's yaw was written this frame...
        float geneYaw = 0.0f; // ... to this
    };
    // A replay of a case with a host section: the recorded values for this frame (true), else false.
    [[nodiscard]] bool replayHost(HostCamera &out);
    // While a case records: keep this frame's values.
    void recordHost(const HostCamera &in);

    // The game's random rolls (frame_patches.cpp randInt / randFloat, game thread; site = the caller's ra, object =
    // the one MoveObj is updating or 0). rollBefore gets the twister's number before the 60 FPS pairing: a replay at
    // the other frame rate than the case's hands back the recorded value of the same roll (the AI makes the tester's
    // choices); otherwise the number is returned as it is. rollAfter gets the value the game is given: a recording
    // keeps it, a replay counts it per site (the report's roll check).
    [[nodiscard]] uint32_t rollBefore(uint32_t site, uint32_t object, uint32_t drawn);
    void rollAfter(uint32_t value);
}
