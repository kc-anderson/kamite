#pragma once

#include <cstdint>

// Which controls are in effect, shown on the radar (radar_glow.cpp): with [Game] ControlStyle=modern the radar's
// floor gets a yellow glow along the inside of its rim while the original controls are in effect (L1 held or toggled,
// Auto in a fight, the game holding the camera); none while the free camera is on. Classic players see the PS2 art.
//
// The radar's texture lives in the core block (core_usa.bin 0x11C830, 128x256 PSMT8, its CLUT at 0x124930). The glow
// is a second version of it written over it in RAM, as the button prompts do. With a texture pack that replaces the
// radar (its image named after the radar's content), the second version only differs in one unused CLUT entry, and a
// full-resolution image of it (the pack's radar with the glow painted on) goes into the port's own replacement folder
// (hd_textures), so the glow is drawn at full resolution on the pack's art. Without one, the texels themselves get the
// glow at the PS2's resolution.
namespace kamite::radar_glow
{
    // Before the game starts (after hd_textures::install, before the first frame), with the EE's RDRAM; the work runs
    // on the EE thread at every vblank.
    void install(uint8_t *rdram);
}
