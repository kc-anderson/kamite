#pragma once

#include <cstdint>

// Which controls are in effect, shown on the radar (radar_glow.cpp): with [Game] ControlStyle=modern the radar's
// floor gets a soft yellow-orange glow along the inside of its rim while the original controls are in effect (L1
// held or toggled, Auto in a fight, the game holding the camera); none while the free camera is on. Classic players
// see the PS2 art. The radar's texture lives in the core block (core_usa.bin 0x11C830, 128x256 PSMT8, its CLUT at
// 0x124930): the glow is a second version of the texels and CLUT written over it in RAM, as the button prompts do.
namespace kamite::radar_glow
{
    // Before the game starts, with the EE's RDRAM; the work runs on the EE thread at every vblank.
    void install(uint8_t *rdram);
}
