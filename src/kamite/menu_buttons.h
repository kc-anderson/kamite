#pragma once

#include <cstdint>
#include <vector>

// Menu buttons (menu_buttons.cpp): Classic, as on the PS2 (circle and cross confirm, triangle cancels: the Japanese
// layout the US release kept), Western (the south button confirms, the east one cancels: A/B on an Xbox pad,
// cross/circle on a PlayStation pad) or Japanese (east confirms, south cancels). They change the menus and the
// roulette only; play is untouched.
//
// How: the game keeps two readings of the pad (game work block 0x7474A0). Play reads the raw button word (+0 held,
// +8 pressed; the pad's two button bytes high-to-low, so 0x10 triangle, 0x20 circle, 0x40 cross, 0x80 square and the
// d-pad at 0x1000..0x8000), in the player code alone. The menus, the dialogs and the title screen read a 64-bit
// "command" word (+0x1A0 pressed, +0x1B0 repeated) that func_0031AFE0 builds from the raw word through a 64-entry table
// of raw masks (0x3C3D50, or 0x3C3E50 for the US release): command bit 28 is the menus' OK, bit 29 their Cancel
// (entries 24-27 are the d-pad, 32-35 the four face buttons one by one). Modern sets OK = cross and Cancel = circle
// in both tables. Menus that ask for one face button by name (the shop's orb tiers, the casino's bets) keep the game's
// buttons; the roulette follows the style too (kamite.toml "menu_roulette"). The hint strings that name the menu buttons are reworded to match.
namespace kamite::menu_buttons
{
    // The menu styles ([Game] MenuButtons; KAMITE_MENU_BUTTONS). Western (also "modern", the default): the south button
    // confirms, the east one cancels (A / B, cross / circle). Japanese: the east button confirms, the south one
    // cancels (circle / cross as on a Japanese PS2: B / A on an Xbox pad, A / B on a Nintendo one). Classic: the US
    // release's own (circle and cross confirm, triangle cancels). Everything here is in PS2 buttons: the button
    // prompts turn each into the controller's own button by its place, whatever the prompt setting.
    enum class Style : int
    {
        Classic = 0,
        Western = 1,
        Japanese = 2,
    };
    // Reads the setting; registers the per-vblank patch (tables and text) on the EE thread and the roulette's hooks.
    // Call before the game starts with the EE's RDRAM.
    void install(uint8_t *rdram);
    Style style();
    // Western or Japanese (the menus' buttons are not the game's own).
    bool modern();
    // F2 > Controls (any thread): the tables, the roulette and the core hint text follow at once; the save menu's text
    // (written over its file on the disc at start) at the next start.
    void setStyle(Style style);
    // "western" / "japanese" / "classic", as kamite.ini spells them.
    const char *styleName(Style style);

    // Text patches (16-bit glyph codes: A=0x40.., a=0x5A.., space 0x10 or 0x35, the button icons 0..13, end 0x8001).
    // `original` must be there before `modern` is written; both the same length, so the tables around them hold.
    struct TextPatch
    {
        uint32_t offset;                // from the core block's base (core_usa.bin as the game keeps it in RAM)
        std::vector<uint16_t> original;
        std::vector<uint16_t> modern; // Western; Japanese is this with cross and circle traded (japaneseOf)
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
