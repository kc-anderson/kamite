#pragma once

#include <cstddef> // iop_pad.h uses size_t

#include "ps2x/iop/iop_pad.h"

#include <cstdint>

class PS2Runtime;

// Style presets: four saved sets of Gene's equipped moves (Style S, E, X, Y; L, I, F, E as Xavier), kept in the save.
//
// The moves Gene has equipped are six "combo sets" in the save data (decomp include/godhand/cCoreSave.h, cCoreSaveData
// +0x1B0, 0x24 bytes each: six move ids, six levels, the used length): set 0 the square chain (up to six moves), sets
// 1-5 the single slots (triangle, cross, down+square, down+triangle, down+cross). The save data is reached through the
// cCoreSave at 0x569B70 (its first word points at it). A style is a copy of those 0xD8 bytes; the four live in the
// save data's unused tail (+0xC00), so they go to the memory card with the save, and a new game has none. Loading one
// keeps out moves the save does not own (skill[id] < 0): a single slot stays empty, the chain closes up.
//
// Controls (d-pad up, right, down, left = S, E, X, Y):
//   In play (Gene under the player's control, mid-fight and mid-combo included): a d-pad tap loads that style.
//   On the Techniques screen with the Modern menu buttons (Y does nothing there): Y + d-pad, let go before 1.5 s,
//   loads; held 1.5 s saves the equipped moves to it. The d-pad is hidden from the screen while Y is held.
// The Techniques screen lists the four under "Attack Str" (Modern menu buttons), with the bottom bar's hint.
// F2 > Styles shows the equipped moves and the styles and saves/loads them too.
namespace kamite::style_presets
{
    // Before the game starts, with the EE's RDRAM: the per-vblank work, the screen's list and the F2 tab.
    void install(PS2Runtime &runtime);
    // hud_layout's cIDBase_trans hooks (EE thread): the entry about to be drawn, 0 after the layout.
    void noteEntry(uint32_t entry);
    // The in-play style indicator's entries (the HUD's money layout carries them): true for one of them, with its
    // draw position set; hud_layout then anchors it to the left edge like the level meter.
    bool onHudEntry(uint32_t entry);

    // ps2_host_input's pad filter, player 1 (window thread): called by modern_controls' filter before its own work.
    void padFilter(uint32_t player, ps2x::iop::PadInputState &state);
}
