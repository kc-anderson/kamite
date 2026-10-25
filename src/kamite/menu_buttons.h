#pragma once

#include <cstdint>
#include <vector>

// Menu buttons (menu_buttons.cpp): Classic, as on the PS2 (circle and cross confirm, triangle cancels: the Japanese
// layout the US release kept), or Modern (the south button confirms, the east one cancels: A/B on an Xbox pad,
// cross/circle on a PlayStation pad). Modern changes the menus only; play is untouched.
//
// How: the game keeps two readings of the pad. Play reads the raw button word (the game work block at 0x7474A0: +0
// held, +8 pressed, PS2 bits: 0x1000 triangle, 0x2000 circle, 0x4000 cross, 0x8000 square), and it is read by the
// player code alone. The menus, the dialogs' choices, the roulette and the title screen read a 64-bit "command"
// word (+0x1A0 held, +0x1B0 repeated) that func_0031AFE0 builds from the raw word through a 64-entry table of raw
// masks (0x3C3D50; 0x3C3E50 is the same with the d-pad turned, for the slot machine): command bit 24 = triangle,
// 25 = cross, 26 = circle, 27 = square. Modern exchanges the masks of entries 24 and 26 in both tables, so a menu
// sees the circle as its "triangle" (cancel) and the triangle as its "circle" (confirm) while a move assigned to
// the triangle is still thrown with the triangle. The prompt sheets keep showing each cell's own button; the few
// hint strings that name the menu buttons are reworded to the buttons that now work them.
namespace kamite::menu_buttons
{
    // [Game] MenuButtons=classic|modern (KAMITE_MENU_BUTTONS), read here; registers the per-vblank patch (tables and
    // text) on the EE thread. Call before the game starts with the EE's RDRAM.
    void install(uint8_t *rdram);
    bool modern();

    // Text patches (16-bit glyph codes: A=0x40.., a=0x5A.., space 0x10 or 0x35, the button icons 0..13, end 0x8001).
    // `original` must be there before `modern` is written; both the same length, so the tables around them hold.
    struct TextPatch
    {
        uint32_t offset;                // from the core block's base (core_usa.bin as the game keeps it in RAM)
        std::vector<uint16_t> original;
        std::vector<uint16_t> modern;
    };
    const std::vector<TextPatch> &textPatches();

    // The same for strings in the menu files (id/local/us/*.idd), which the game loads where it likes. Both kinds are
    // written over the files on the disc image at start (PS2DiscOverlay, checked against the disc's bytes); only when
    // that is not possible (no image, another language, a changed file) are they rewritten in RAM: the core text every
    // 30 vblanks, the menu files found by a thread scanning RAM for `original` twice a second.
    struct TextRewrite
    {
        const char *where;
        std::vector<uint16_t> original;
        std::vector<uint16_t> modern;
    };
    const std::vector<TextRewrite> &textRewrites();
}
