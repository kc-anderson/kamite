#pragma once

// The test mode (dev-only; claude/strip-list.md): the case recorder (the RecordCase hotkey, R3: press, do the move,
// press again; one cases\<date-time>.case file with a state of its own, the pad and a live per-frame log), the case
// replay (KAMITE_REPLAY_CASE / --replay-case <file> [--fps60 0|1]: the game's logic plays the case back at 30 or 60
// and logs every frame to <case>.30.csv / .60.csv, then quits) and scripted runs (KAMITE_TEST=<script>, [Run] Test,
// --test). For measuring the game at 30 and at 60 FPS instead of counting frames by eye: claude/test-mode.md.

#include "ps2_runtime.h"

#include <filesystem>

namespace kamite::test_mode
{
    // Before the game loop, after installFramePatches (the script may switch the frame rate). Relative paths in the
    // script are taken from exeDir.
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);
    // From the main-loop hook (frame_patches.cpp frameStart), once a game frame.
    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    // After the run: the test could not be carried out (the state did not load, a file could not be written).
    [[nodiscard]] bool failed();
    // A case is being recorded (any thread; for menus).
    [[nodiscard]] bool recording();
}
