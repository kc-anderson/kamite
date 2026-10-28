#pragma once

#include <cstdint>

// Slow motion, pause and frame step for testing (EeScheduler.cpp). Host-side only: the guest's own timing is not
// touched. Slow motion stretches the real time between vblanks (EE time, and the IOP and SPU2 that run on it, slow
// down with it, so the game behaves frame for frame as at full speed; the sound runs short of samples and crackles).
// Pause holds the next vblank back, so the game stops at a field boundary; the GS is meanwhile given a vsync with the
// same registers about 60 times a second, which presents the frame on screen again (the menu and overlays stay live).
// Any thread.
namespace ps2x::timecontrol
{
    // 1 = full speed, 2 = half, 4 = a quarter ... (1..16).
    void setSlowDivisor(uint32_t divisor);
    [[nodiscard]] uint32_t slowDivisor();

    void setPaused(bool paused);
    [[nodiscard]] bool paused();
    // While paused: let one step's vblanks through, then hold again (setStepVblanks: the game's vblanks per frame,
    // 1 or 2; default 1). Not paused: pauses.
    void step();
    void setStepVblanks(uint32_t vblanks);

    // Mutes the game's sound output (the host stream's master volume; ps2_audio.cpp), e.g. for slow motion.
    void setMuted(bool muted);
    [[nodiscard]] bool muted();
}
