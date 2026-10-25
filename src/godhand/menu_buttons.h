#pragma once

#include <cstdint>
#include <vector>

namespace ps2x::iop
{
    struct PadInputState;
}

// Menu buttons (menu_buttons.cpp): Classic, as on the PS2 (circle and cross confirm, triangle cancels: the Japanese
// layout the US release kept), or Modern (the south button confirms, the east one cancels: A/B on an Xbox pad,
// cross/circle on a PlayStation pad). Modern trades the game's circle and triangle: the pad filter swaps the two
// buttons on their way in, the prompt sheets show each cell's new button, and the few hint strings that list both
// confirm buttons or a lone circle are reworded (the roulette's bar reads "Cycle / OK / Cancel" with one button each).
namespace godhand::menu_buttons
{
    // [Game] MenuButtons=classic|modern (GODHAND_MENU_BUTTONS), read here; call before the game starts.
    void install();
    bool modern();

    // Modern: circle <-> triangle (buttons and pressures). Classic: nothing. The pad filter calls it first.
    void swapPad(ps2x::iop::PadInputState &state);

    // Text patches in core_usa.bin as the game keeps it in RAM (16-bit glyph codes: A=0x40.., a=0x5A.., space 0x10,
    // the button icons 0..13, end 0x8001). `original` must be there before `modern` is written; both the same length,
    // so the string table around them is untouched.
    struct TextPatch
    {
        uint32_t offset;                // from the core block's base
        std::vector<uint16_t> original;
        std::vector<uint16_t> modern;
    };
    const std::vector<TextPatch> &textPatches();
}
