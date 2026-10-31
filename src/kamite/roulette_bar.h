#pragma once

#include <cstdint>

// The roulette's hint bar (cCockRoulette's panel: a block sprite with one text line, message 0x30F2, drawn centred and
// squeezed to the block's width). With [Game] MenuButtons=modern the line is reworded ("[R1]Cycle [cross]OK
// [circle]Cancel") and laid out again here: full letter width (the x scale made equal to the y scale), its right end
// a little inside the bar's right edge. The bar keeps the game's length (cutting its pieces by drawX / drawSclX did
// not show; the F6 HUD dump would give the pieces' real draws). Each frame's glyph and sprite draws give every
// entry's extents; the next frame's draw of the text entry (cIDBase_trans, before it draws) gets its drawX / drawSclX
// set from them. Classic: untouched. hud_layout.cpp calls these.
namespace kamite::roulette_bar
{
    // cIDBase_trans is about to draw `entry` of `layout` (guest addresses). Adjusts the bar's text entry in RAM.
    void onEntry(uint8_t *rdram, uint32_t layout, uint32_t entry);
    // A 2D primitive of `entry` in `layout` was converted: its screen x extent (4:3 screen units, before any
    // narrowing); glyph = a text glyph or inline icon.
    void onDraw(uint32_t layout, uint32_t entry, float x0, float x1, float y0, float y1, bool glyph);
    // Top of every game frame.
    void onFrame();
}
