#pragma once

#include <cstdint>

// Texture slots that fit (a fix to the game's per-frame VRAM allocator, exposed by the wider view).
//
// Each frame the game packs the textures of the models it draws into VRAM from block 0x2800 up: func_00151398 picks a
// cursor, and the TIM2 loader func_00151990 uploads the picture there and moves the cursor on by the picture's size in
// bytes / 256 (0x151C68..0x151C78). A GS texture narrower than a page and taller than one (a T8 64x128: page 128x64)
// takes more blocks than its bytes fill, so the next texture lands inside it: in the shop the jukebox's T8 64x128 at
// 0x2840 really covers 0x2840..0x284F and 0x2860..0x286F, and the window glass put at 0x2860 had its top 64 rows
// overwritten (Oct 25, gif_76568). On the PS2's 4:3 view the jukebox is off screen there; the wider view brings both
// in. The fix: at 0x151C70 (srl v1,v1,8; $v1 the picture's ImageSize, its TIM2 picture header at $s5 - 0x30) the size
// becomes the picture's real footprint in blocks x 256 when that is larger. The game checks no end for the cursor;
// the region up to 0x3800 is free (the dump's peak was 0x2B32), and only narrow tall textures grow.
namespace kamite::texture_slots
{
    void install();
    // Blocks a texture of this PSM (GS code: 0x00 CT32, 0x01 CT24, 0x02 CT16, 0x13 T8, 0x14 T4) and size takes at
    // TBW tbw (64-texel units), from its base block: the highest block it touches + 1.
    uint32_t footprintBlocks(uint32_t psm, uint32_t width, uint32_t height, uint32_t tbw);
}
