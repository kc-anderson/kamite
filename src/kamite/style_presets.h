#pragma once

#include <cstdint>

// Style presets (stage 1 of the Techniques screen's presets: the data and a testing tab, no in-game controls yet).
//
// The moves Gene has equipped are six "combo sets" in the save data (decomp include/godhand/cCoreSave.h, cCoreSaveData
// +0x1B0, 0x24 bytes each: six move ids, six levels, the used length): set 0 the square chain (up to six moves), sets
// 1-5 the single slots (triangle, cross, down+square, down+triangle, down+cross). The save data is reached through the
// cCoreSave at 0x569B70 (its first word points at it; 0x5686D0 in the Cheat Engine table that switches combos the same
// way). A style is a copy of those 0xD8 bytes. Loading one keeps out moves the save does not own (skill[id] < 0): a
// single slot stays empty, the chain closes up.
//
// For now four styles live in memory only (lost when the game closes) and F2 > Styles saves and loads them; the next
// stages put them in the save file, on the Techniques screen (Y + d-pad tap loads, held 1.5 s saves) and in its UI.
namespace kamite::style_presets
{
    // Before the game starts, with the EE's RDRAM: the per-vblank work and the F2 tab.
    void install(uint8_t *rdram);
}
