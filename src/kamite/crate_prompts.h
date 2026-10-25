#pragma once
// Pirate ship cannon minigame (stage 3, r301): the four crates on the ship show the PS2 triangle/circle/square/cross.
// They come from one TIM3 texture in guest RAM, 128x128 PSMT4 that the game uploads every frame as a CT32 64x32 DBW 1
// image (port_prep/pirate: field 15300 recording, upload of 0x2000 bytes from 0x011CB8A0, CLUT right after it). This
// rebuilds that payload with each crate's symbol replaced by the bound key, like button_prompt_sheets does for the HUD.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace kamite::crate_prompts
{
    inline constexpr size_t kPayloadBytes = 0x2000;
    inline constexpr size_t kClutBytes = 0x40;

    // Cells of the 128x128 texture, 64x64 each: triangle and circle on top, square and cross below.
    enum Cell
    {
        Triangle,
        Circle,
        Square,
        Cross,
    };

    // `original` = the 0x2000-byte payload as the game uploads it. Writes the patched payload to `out` (may not alias
    // `original`). Returns false and leaves `out` untouched when the payload does not have the expected layout (each
    // cell's own symbol colour present).
    bool build(const uint8_t *original, const std::array<std::string, 4> &labels, uint8_t *out);
}
