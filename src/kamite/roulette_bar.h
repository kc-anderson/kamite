#pragma once

#include <cstdint>

// The roulette's hint bar (cCockRoulette's panel: a block sprite with one text line, message 0x30F2, drawn centred and
// squeezed to the block's width). With [Game] MenuButtons=modern the line is reworded to one button per action and
// gets shorter, so it is laid out again here: full letter width (the x scale made equal to the y scale, or as wide as
// the block allows) and its right end on the block's right edge. The glyph and sprite draws of one frame give the
// block's and the text's extents; the next frame's draw of the text entry (cIDBase_trans, before cIDBase_transText)
// gets its drawX/drawSclX set from them. hud_layout.cpp calls these from its hooks.
namespace kamite::roulette_bar
{
    // cIDBase_trans is about to draw `entry` of `layout` (guest addresses). Adjusts the bar's text entry in RAM.
    void onEntry(uint8_t *rdram, uint32_t layout, uint32_t entry);
    // A 2D primitive of `entry` in `layout` was converted: its screen x extent (4:3 screen units, before any
    // narrowing); glyph = a text glyph or inline icon.
    void onDraw(uint32_t layout, uint32_t entry, float x0, float x1, bool glyph);
    // Top of every game frame.
    void onFrame();
}
