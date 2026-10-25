#pragma once

// God Hand's button-prompt sheets redrawn with the player's PC bindings (port_prep/prompts/DESIGN_2026-10-18.md,
// option A: patch the sheets where they sit in guest RAM; the game re-sends them from EE RAM on every draw).
//
// Pure functions: given a sheet's original 16 KB payload and its 1 KB CLUT (both read from guest RAM) and the labels,
// they write the patched payload. No runtime state, no guest RAM access, and the CLUT is never changed: every new
// texel is the nearest palette entry (premultiplied RGBA) among the entries the original cell already uses, plus
// index 0 (transparent), plus (icon sheet) the sheet's own white lettering ramp.
//
// Both sheets are 128x128 PSMT8 with TBW 2 (TEX0 0x20000001DD308000), stored pre-swizzled as the CT32 64x64 (DBW 1)
// image the game uploads, so texel (x, y) lives at payload byte inv32[addrPSMT8(0, 2, x, y) >> 2] * 4 + (addr & 3),
// inv32 being the inverse of addrPSMCT32(0, 1, .) over 64x64 (port_prep/prompts/poc_patch_sheet.py proves it offline).

#include <cstddef>
#include <cstdint>
#include <string>

namespace godhand::prompt_sheets
{
    // ---- where the sheets are (SLUS_215.03, US; checked in all five timeout_ram_*.bin dumps, 2026-10-17/18) ----
    // core_usa.bin (AFS entry 8) is read by the boot task func_0030D358 with cDvd_ReadAlloc; the pointer is stored at
    // D_007479F8 (0x8F0280 in every dump). The bytes in RAM match the disc.
    constexpr uint32_t kCorePointer = 0x7479F8u;
    // Texture 0x18 of the system IDD = the text-icon sheet (func_002B1300 passes a1 = 0x18 to cIDManager_getTexAddr
    // 0x2ACE28). Record at core+0xBEAF0: TEX0 at +0x20 (= data - 0xC0), payload at +0xE0, CLUT 0x4100 after the
    // payload (16x16 CT32, CSM1). Payload sha256 prefix 4d625e757c635e93.
    constexpr uint32_t kIconSheetData = 0xBEBD0u;
    constexpr uint32_t kIconSheetClut = 0xC2CD0u;
    // Texture 0x0F (cCockActButton, the floating action prompt; layout #5 works 1-4 = the four shapes). Record at
    // core+0xAA330, same layout. Payload sha256 prefix dc3f1442ad970cb2.
    constexpr uint32_t kActionSheetData = 0xAA410u;
    constexpr uint32_t kActionSheetClut = 0xAE510u;
    constexpr uint32_t kTex0Offset = 0xC0u; // TEX0 qword at data - 0xC0
    constexpr uint64_t kSheetTex0 = 0x20000001DD308000ull;

    constexpr size_t kPayloadBytes = 16384u; // 128 x 128 x 8 bit
    constexpr size_t kClutBytes = 1024u;     // 256 x CT32
    constexpr int kSheetSize = 128;
    constexpr int kCellSize = 32;

    // Text icon codes 0x0000..0x000D (func_002B12A0: code < 0xE). Each is drawn from one 32x32 cell of texture 0x18
    // (switch table D_0044B150), in this order.
    enum Icon : int
    {
        Circle,     // (0,0)   red ring
        Triangle,   // (32,0)  green triangle
        Square,     // (64,0)  pink square
        Cross,      // (96,0)  blue cross
        L1,         // (64,32)
        R1,         // (0,32)
        L2,         // (96,32)
        R2,         // (32,32)
        L3,         // (32,64)
        R3,         // (0,64)
        LeftStick,  // (96,64) "LEFT" over a stick
        RightStick, // (64,64) "RIGHT" over a stick
        Start,      // (32,96) "START"
        Select,     // (0,96)  "SELECT"
        IconCount
    };

    // The colour of a cell's label. White: the port's key caps. The others: an Xbox pad's lettering (A green, B red,
    // X blue, Y yellow), drawn from the sheet's own colour ramps (the red circle's, the green triangle's, the blue
    // cross's, and the pink square's ramp recoloured to yellow in the CLUT: nothing else uses it).
    enum class Tint : uint8_t
    {
        White,
        Red,
        Green,
        Blue,
        Yellow
    };

    struct PromptLabels
    {
        // What each icon cell shows: prompt_art::Label::compact of the bound key or button ("F", "Spc", "Ctrl", "LB").
        // Empty = keep the original art of that cell. LeftStick/RightStick are not used here (see the sticks below).
        std::string icon[IconCount];
        Tint tint[IconCount] = {};
        // The stick cells: the four directions as an inverted T, in controls.ini's LUp LRight LDown LLeft order
        // (keyboard: W D S A). All four empty = keep the original stick art (right for a controller's stick).
        std::string leftStick[4];
        std::string rightStick[4];
    };

    // Texture 0x18. icon[0..13] go to their cells: the PS2 symbol, L1/R1/L2/R2/L3/R3 letters or START/SELECT word is
    // replaced by the label on the cell's own key cap (the circle or rounded-rectangle rim is kept; START/SELECT get
    // the R1 cap). origPayload must be the unpatched sheet (the hook keeps its own copy of it). False (outPayload
    // untouched) when an argument is null, the payload does not look like the sheet, or a label could not be drawn.
    // outPayload may equal origPayload. outClut (1 KB) gets the CLUT to use with it: the original, with the pink ramp
    // turned yellow when a label asks for yellow. Takes a few milliseconds (GDI): build on a worker thread.
    bool buildIconSheet(const uint8_t *origPayload, const uint8_t *clut, const PromptLabels &labels, uint8_t *outPayload,
                        uint8_t *outClut);

    // Texture 0x0F. Its bottom row holds the four outline shapes the action prompt puts on its grey button
    // (func_001F7680 kinds 0-3); each is replaced by icon[Circle/Triangle/Square/Cross] drawn in that shape's own
    // colour, so the red circle becomes a red "F". Same contract as buildIconSheet.
    bool buildActionSheet(const uint8_t *origPayload, const uint8_t *clut, const PromptLabels &labels, uint8_t *outPayload,
                          uint8_t *outClut);


    // Sheet <-> 128x128 palette indices (row-major), and the CLUT as 256 RGBA entries in index order (alpha as
    // stored, 0x80 = opaque). For the hook's checks and offline tools.
    void unswizzle(const uint8_t *payload, uint8_t *indices);
    void swizzle(const uint8_t *indices, uint8_t *payload);
    void paletteRgba(const uint8_t *clut, uint8_t *rgba);
}
