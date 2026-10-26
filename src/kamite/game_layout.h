#pragma once
// The game's structures as the decomp names them (LucasPicoli/god-hand-decomp, include/godhand/*.h), for the port's
// hooks: field offsets under the decomp's field names, so code here reads like the decomp's C and a rename there is
// one search away. Where the decomp still has a field as unkNN and the port has worked out what it is, the name is
// ours and the comment says so (those are the ones offered back to the decomp: docs/decomp/).
//
// Values only: every offset below is the one the port used as a literal before (asserted at the end), so code that
// switches to these names behaves exactly as before.
#include <cstdint>

namespace kamite::game
{
    // GameWork, the game work block D_007474A0 (gameWork.h).
    namespace GameWork
    {
        inline constexpr uint32_t kAddress = 0x7474A0u;
        // The pad words func_0031A808 fills (ours; the decomp names only 0x08, padBtn): the raw button word held,
        // last frame's, the bits pressed this frame ((held ^ previous) & held) and released.
        inline constexpr uint32_t kPadHeld = 0x00u;
        inline constexpr uint32_t kPadPrevious = 0x04u;
        inline constexpr uint32_t kPadBtn = 0x08u;     // decomp: padBtn (the pressed bits)
        inline constexpr uint32_t kPadReleased = 0x0Cu;
        inline constexpr uint32_t kTrg = 0x1A0u;        // decomp: trg, the menu command word (held)
        inline constexpr uint32_t kRep = 0x1B0u;        // decomp: rep, the menu command word (repeat)
        inline constexpr uint32_t kLangData = 0x558u;   // decomp: langData, the language data file (core_usa.bin)
        inline constexpr uint32_t kVblankDivider = 0x569u; // decomp: unk569; ours: vblanks per game frame (1 = 60/s)
        inline constexpr uint32_t kLanguage = 0x56Cu;   // decomp: language (0 = Japanese)
        inline constexpr uint32_t kSaveFlags = 0x588u;  // decomp: pad588; ours: bit 0x200000 = Hard unlocked
        inline constexpr uint32_t kDebugFlags = 0x594u; // decomp: debugFlags
        inline constexpr uint32_t kFlags5D8 = 0x5D8u;   // decomp: flags5D8 (bits: FLAGS5D8_* below, ours)
        inline constexpr uint32_t kFlags5E4 = 0x5E4u;   // decomp: flags5E4
        inline constexpr uint32_t kPlayerObj = 0x5F4u;  // decomp: playerObj
        inline constexpr uint32_t address(uint32_t field) { return kAddress + field; }
    }
    // GameWork.flags5D8 bits (ours).
    inline constexpr uint32_t kFlags5D8PlayerOff = 0xC0000000u; // the player's Move returns at once (func_001F6C40)
    inline constexpr uint32_t kFlags5D8KeyStop = 0x02000000u;   // the controls are off, the player still updates

    // cIDBase, the on-screen message and icon display object (cIDBase.h).
    namespace IDBase
    {
        inline constexpr uint32_t kEnt = 0x04u;    // cIDBaseEnt *ent
        inline constexpr uint32_t kEntNum = 0x0Cu; // int entNum
    }
    // cIDBaseEnt, one display entry (cIDBase.h): IDBASE_ENT_SIZE bytes.
    namespace IDBaseEnt
    {
        inline constexpr uint32_t kSize = 0xACu;      // IDBASE_ENT_SIZE
        inline constexpr uint32_t kFlags = 0x2Cu;     // IDENT_FLAG_*
        inline constexpr uint32_t kDrawX = 0x50u;
        inline constexpr uint32_t kDrawY = 0x54u;
        inline constexpr uint32_t kDrawSclX = 0x58u;
        inline constexpr uint32_t kDrawSclY = 0x5Cu;
        inline constexpr uint32_t kKind = 0x88u;      // IDENT_KIND_*
        inline constexpr uint32_t kAlign = 0x89u;     // decomp: b89; ours: text alignment (0 = block centred)
        inline constexpr uint32_t kColourSet = 0x8Bu; // decomp: b8B; ours: the entry's colour set
        inline constexpr uint32_t kMsg = 0x90u;       // message id, 0xFFFF = none
    }
    inline constexpr uint32_t kIdentFlagNoDraw = 0x08000000u; // IDENT_FLAG_NO_DRAW
    inline constexpr uint8_t kIdentKindText = 2u;             // IDENT_KIND_TEXT

    // cSubScr, the sub screen manager (cSubScr.h); D_003BD930 points at the one instance.
    namespace SubScr
    {
        inline constexpr uint32_t kPointer = 0x3BD930u; // D_003BD930
        inline constexpr uint32_t kKind = 0x48u;        // the open screen, -1 when closed
        inline constexpr uint32_t kOpen = 0x50u;        // short open
    }

    // cMessDrawFont, the message text drawer (cMessDrawFont.h); D_005E7510 is the one the text entries draw with.
    namespace MessDrawFont
    {
        inline constexpr uint32_t kInstance = 0x5E7510u;
        inline constexpr uint32_t kCharGapX = 0x2Cu;  // decomp: unk2C[0]; ours: letter spacing (times zoomX)
        inline constexpr uint32_t kCharGapY = 0x30u;  // decomp: unk2C[1]; ours: line spacing (times zoomY)
        inline constexpr uint32_t kZoomX = 0x44u;     // decomp: zoomX
        inline constexpr uint32_t kZoomY = 0x48u;     // decomp: zoomY
        inline constexpr uint32_t kPass = 0x4Cu;      // decomp: unk4C[0]; ours: 3 while the shadow copy is drawn
        inline constexpr uint32_t kCursor = 0x64u;    // decomp: cursor, the code being laid out
    }

    // cCoreSave / cCoreSaveData, the save record (cCoreSave.h).
    namespace CoreSaveData
    {
        inline constexpr uint32_t kSize = 0x14A0u;
        inline constexpr uint32_t kSkill = 0xB0u;     // char skill[0x80], -1 = not owned
        inline constexpr uint32_t kCombo = 0x1B0u;    // cCoreSaveCombo combo[CORESAVE_COMBO_SETS]
        inline constexpr uint32_t kComboSize = 0x24u; // sizeof(cCoreSaveCombo)
        inline constexpr uint32_t kUsedEnd = 0xBC4u;  // oliviaCostumeNo ends here; unkBC4[0x8DC] after it is untouched
    }

    // The literals the port used before these names (behaviour is unchanged by construction).
    static_assert(GameWork::address(GameWork::kVblankDivider) == 0x747A09u);
    static_assert(GameWork::address(GameWork::kLanguage) == 0x747A0Cu);
    static_assert(GameWork::address(GameWork::kSaveFlags) == 0x747A28u);
    static_assert(GameWork::address(GameWork::kDebugFlags) == 0x747A34u);
    static_assert(GameWork::address(GameWork::kFlags5D8) == 0x747A78u);
    static_assert(GameWork::address(GameWork::kFlags5E4) == 0x747A84u);
    static_assert(GameWork::address(GameWork::kPlayerObj) == 0x747A94u);
    static_assert(GameWork::address(GameWork::kLangData) == 0x7479F8u);
    static_assert(GameWork::address(0x10u) == 0x7474B0u);
}
