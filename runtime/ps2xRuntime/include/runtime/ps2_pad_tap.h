#pragma once

#include "ps2x/iop/iop_pad.h"

#include <cstdint>

// A host tap on the pad the emulated IOP polls (ps2_iop_host.cpp readPad / readPadSlot: PADMAN's SIO2 commands ask
// the host for the DualShock's state once a vblank or so): called on the IOP thread with the state the host
// produced, which the tap may read or rewrite (the game's libpad sees what it leaves). One tap at a time; nullptr
// removes it. For input recording and playback (kamite/test_mode.cpp). The HLE scePadRead stub is not on this
// path: a game whose own libpad runs (the usual case) never calls it.
namespace ps2x::pad
{
    using StateTap = void (*)(uint32_t port, uint32_t slot, ps2x::iop::PadInputState &state);
    void setStateTap(StateTap tap);
}
