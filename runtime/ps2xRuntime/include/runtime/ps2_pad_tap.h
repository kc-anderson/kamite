#pragma once

#include <cstdint>

// A host tap on scePadRead (Kernel/Stubs/Pad.cpp): called on the game thread right after the host filled the 32-byte
// pad status the guest asked for (DS2 layout: +0 status, +1 mode/id, +2..3 buttons inverted, +4..7 right/left stick,
// +8.. pressures), with the buffer itself, which the tap may read or rewrite: the guest sees what it leaves. One tap
// at a time; nullptr removes it. For input recording and playback (kamite/test_mode.cpp).
namespace ps2x::pad
{
    using ReadTap = void (*)(int port, int slot, uint8_t *status32);
    void setReadTap(ReadTap tap);
}
