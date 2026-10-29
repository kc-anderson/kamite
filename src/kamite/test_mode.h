#pragma once

// The test mode (dev-only; claude/strip-list.md): KAMITE_TEST=<script> ([Run] Test in kamite.ini, --test on the
// command line) loads a save state, records the pad or plays a recording back frame by frame, writes one CSV row per
// game frame (Gene, the enemies, the collision pool) and quits by itself. For measuring the game at 30 and at 60
// FPS instead of counting frames by eye. The script format and the CSV columns: claude/test-mode.md.

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
}
