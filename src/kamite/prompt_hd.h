#pragma once

#include "kamite/button_prompt_sheets.h"
#include "kamite/hd_textures.h"

#include <cstdint>
#include <string>

// Button prompts at full resolution from Xelu's free prompt pack (CC0, resources/prompts/xelu: "Xelu's FREE
// Controllers & Keyboard PROMPTS" by Nicolae "Xelu" Berbece, Those Awesome Guys).
//
// The game draws every button prompt from two 128x128 sheets (button_prompt_sheets.h): the text icons (texture 0x18)
// and the floating action prompt (texture 0x0F, its bottom row). The port keeps writing its own PS2-resolution version
// of them into the game's memory (the bound keys or buttons, drawn small), marked in one unused CLUT entry per art set
// so every set has a texture name of its own; for that name it writes a full-resolution image made from the pack into
// cache/hd-textures and hands it to the GS (texture_add). So the prompts are full resolution with or without a
// texture pack, a pack's own prompt images never apply over them, and with replacements off the small version shows.
namespace kamite::prompt_hd
{
    // Which art a controller gets: [Game] ButtonPrompts=auto|xbox|playstation|nintendo|steam (KAMITE_BUTTON_PROMPTS).
    // Auto follows the controller in use (SDL's type, then its name): PlayStation pads keep the game's own art.
    enum class Choice : uint8_t
    {
        Auto,
        Xbox,
        PlayStation,
        Nintendo,
        Steam
    };
    Choice choice();
    // The F2 menu (any thread): the choice from now on (button_prompts::requestRebuild() redraws the sheets).
    void setChoice(Choice choice);
    const char *choiceName(Choice choice); // the kamite.ini value: auto, xbox, playstation, nintendo, steam

    // The art sets. PlayStation is the game's own art (no image is made); Keyboard is for keys and mouse buttons.
    enum class Set : uint8_t
    {
        PlayStation,
        Xbox360,
        SteamDeck,
        Switch,
        Keyboard
    };
    const char *setName(Set set);

    // The full-resolution image (kScale x the sheet) of sheet 0 (icons) or 1 (action prompt) for these labels: the PS2
    // sheet given (as the game has it, patched or not) scaled up underneath, every cell the pack has art for drawn over
    // it. Alpha in the PS2's scale (0x80 = opaque), as PCSX2's replacement images keep it. False when no cell could be
    // drawn (the pack's files are missing).
    constexpr int kScale = 3;
    bool compose(Set set, int sheet, const uint8_t *payload, const uint8_t *clut, const prompt_sheets::PromptLabels &labels,
                 hd_textures::Image &out);
}
