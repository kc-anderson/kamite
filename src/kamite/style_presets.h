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
// Style None is the game's own moves (the default: styles off). On the Techniques screen L2 / R2 cycle None, S, E, X,
// Y (wrapping); every change of the equipped moves saves to the style shown; a style first cycled to takes the moves
// equipped then; None's moves wait while a style is on. The screen shows "Style: None / S / E / X / Y" under its
// title, the d-pad hints right of it and "[L2] [R2] Cycle Style" in the bottom bar.
// In play (Gene under the player's control, mid-fight and mid-combo included), while a style is on: a d-pad tap
// (up, right, down, left = S, E, X, Y) switches to that style if it is filled; the HUD shows the letters.
// F2 > Styles shows the equipped moves and the styles and selects one too.
namespace kamite::style_presets
{
    // Before the game starts, with the EE's RDRAM: the per-vblank work, the screen's list and the F2 tab.
    void install(PS2Runtime &runtime);
    // hud_layout's cIDBase_trans hooks (EE thread): the entry about to be drawn, 0 after the layout.
    void noteEntry(uint32_t entry);
    // The in-play style indicator's entries (the HUD's money layout carries them): true for one of them, with its
    // draw position set; hud_layout then anchors it to the left edge like the level meter.
    bool onHudEntry(uint32_t entry);
    // The indicator round the minimap (K, Nov 10): true while Style switching is on (the minimap and the gold counter
    // then move down a little to make room). For a glyph of the indicator's entries (the index-th drawn in it): the
    // style it shows (0..3 = S E X Y: top, right, bottom, left of the minimap), else -1; for the outline's copies the
    // direction of its offset (-1..1 each).
    bool lettersOnRadar();
    int hudGlyph(uint32_t entry, int index, float &dx, float &dy);
    // The indicator is drawn in a pass of its own right after the minimap (so it sits over it): true with the money
    // layout and the two entries to draw (they stay hidden in the layout's own draw).
    bool hudPass(uint32_t &layout, uint32_t &text, uint32_t &shadow);

    // ps2_host_input's pad filter, player 1 (window thread): called by modern_controls' filter before its own work.
    void padFilter(uint32_t player, ps2x::iop::PadInputState &state);

    // EE thread: 0 for None (also with Style switching off), 1..4 for the style on (S..Y). Buck Breaker's per-style pick.
    int activePosition();
}
