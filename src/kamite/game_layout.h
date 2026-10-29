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
        inline constexpr uint32_t kFrameCount = 0x210u; // ours: game frames, +1 each main loop (func_0030AC40 0x30AF5C)
        inline constexpr uint32_t kTrg = 0x1A0u;        // decomp: trg, the menu command word (held)
        inline constexpr uint32_t kRep = 0x1B0u;        // decomp: rep, the menu command word (repeat)
        inline constexpr uint32_t kLangData = 0x558u;   // decomp: langData, the language data file (core_usa.bin)
        inline constexpr uint32_t kVblankDivider = 0x569u; // decomp: unk569; ours: vblanks per game frame (1 = 60/s)
        inline constexpr uint32_t kLanguage = 0x56Cu;   // decomp: language (0 = Japanese)
        inline constexpr uint32_t kSaveFlags = 0x588u;  // decomp: pad588; ours: bit 0x200000 = Hard unlocked
        inline constexpr uint32_t kDebugFlags = 0x594u; // decomp: debugFlags
        inline constexpr uint32_t kRoom = 0x5B0u;       // decomp: room (u16, the current room id)
        inline constexpr uint32_t kFlags5D8 = 0x5D8u;   // decomp: flags5D8; the preview's STP group (Stp:: below)
        inline constexpr uint32_t kFlagsDsp = 0x5E0u;   // the preview's DSP group (Dsp:: below)
        inline constexpr uint32_t kFlags5E4 = 0x5E4u;   // decomp: flags5E4; the preview's STA group (Sta:: below)
        inline constexpr uint32_t kRoomJumpTable = 0x5A4u; // D_00747A44: the loaded etc/debugres/RoomJmpTbl.dat
        inline constexpr uint32_t kPlayerObj = 0x5F4u;  // decomp: playerObj
        inline constexpr uint32_t address(uint32_t field) { return kAddress + field; }
    }
    // The game's flag words, named by the 2006-06-21 preview disc's debug "Flag Edit" table (its .data 0x384808:
    // group name, bit names, word, bit count). Bit i of a group is 0x80000000 >> (i % 32) in word i / 32. The retail
    // words sit where the preview's do (right below ZeroMat); the names agree with what the decomp and the port worked
    // out on their own (STP_EM = the decomp's "enemies skip their update", STP_KEY = our key stop, STA EVENT = "a
    // cutscene is running", STA_WORLD_SLOW = its slow-mo, STA SOFT_EVENT = our casino/dog-race soft event, and the
    // door jump sets STP_CAM / VIB / ACTBTN / SCEAT and DSP_ACTBTN). A bit with no name was unnamed in the preview.
    namespace Stp // GameWork.flags5D8 (0x747A78; word 2 at 0x747A7C): "stop" - a part of the game is not updated
    {
        inline constexpr uint32_t kPl = 0x80000000u;          // STP_PL
        inline constexpr uint32_t kEm = 0x40000000u;          // STP_EM: enemies skip their update
        inline constexpr uint32_t kOm = 0x20000000u;          // STP_OM
        inline constexpr uint32_t kEff = 0x10000000u;         // STP_EFF
        inline constexpr uint32_t kScr = 0x08000000u;         // STP_SCR
        inline constexpr uint32_t kVib = 0x04000000u;         // STP_VIB
        inline constexpr uint32_t kKey = 0x02000000u;         // STP_KEY: the controls are off, the player still updates
        inline constexpr uint32_t kPad2 = 0x01000000u;        // STP_PAD2
        inline constexpr uint32_t kCam = 0x00800000u;         // STP_CAM: the camera stays where it is
        inline constexpr uint32_t kActBtn = 0x00400000u;      // STP_ACTBTN
        inline constexpr uint32_t kCamThru = 0x00200000u;     // STP_CAMTHRU: the hide-model manager (func_00307B28) is skipped
        inline constexpr uint32_t kSceAt = 0x00100000u;       // STP_SCEAT
        inline constexpr uint32_t kMess = 0x00080000u;        // STP_MESS
        inline constexpr uint32_t kCockpit = 0x00040000u;     // STP_COCKPIT
        inline constexpr uint32_t kObj = 0x00020000u;         // STP_OBJ
        inline constexpr uint32_t kSce = 0x00010000u;         // STP_SCE
        inline constexpr uint32_t kDeflicker = 0x00008000u;   // STP_DEFLICKER
        inline constexpr uint32_t kWorldLight = 0x00004000u;  // STP_WORLD_LIGHT
        inline constexpr uint32_t kTime = 0x00002000u;        // STP_TIME
        inline constexpr uint32_t kShadowChkLine = 0x00001000u; // STP_SHADOWCHKLINE
        inline constexpr uint32_t kSeRenew = 0x00000040u;     // STP_SE_RENEW
        inline constexpr uint32_t kPlayKey = 0x00000020u;     // STP_PLAYKEY
        inline constexpr uint32_t kPlStopKey = 0x00000010u;   // STP_PLSTOPKEY
        inline constexpr uint32_t kSubScr2 = 0x00100000u;     // STP_SUBSCR, in word 2 (0x747A7C)
    }
    namespace Dsp // GameWork +0x5E0 (0x747A80): "display" - a SET bit hides that part (the dog race, the jukebox and
                  // the door jump set them; func_002BF708 sets them all)
    {
        inline constexpr uint32_t kPl = 0x80000000u;          // DSP_PL: Gene
        inline constexpr uint32_t kEm = 0x40000000u;          // DSP_EM: enemies
        inline constexpr uint32_t kOm = 0x20000000u;          // DSP_OM: objects (om)
        inline constexpr uint32_t kEff = 0x10000000u;         // DSP_EFF: effects
        inline constexpr uint32_t kScr = 0x08000000u;         // DSP_SCR: the stage
        inline constexpr uint32_t kFilter = 0x04000000u;      // DSP_FILTER: the screen filters (classCFILTER_TAG_Filter_main)
        inline constexpr uint32_t kCock = 0x02000000u;        // DSP_COCK: the HUD ("cockpit")
        inline constexpr uint32_t kEmitter = 0x01000000u;     // DSP_EMITTER
        inline constexpr uint32_t kObj = 0x00800000u;         // DSP_OBJ (IsTargetVisible_14B470)
        inline constexpr uint32_t kKage = 0x00400000u;        // DSP_KAGE: shadows (cObjBase_KageDraw)
        inline constexpr uint32_t kPl01 = 0x00200000u;        // DSP_PL01: Gene's coat (pl01)
        inline constexpr uint32_t kActBtn = 0x00100000u;      // DSP_ACTBTN: the action-button prompt
        inline constexpr uint32_t kWorldLight = 0x00080000u;  // DSP_WORLD_LIGHT
        inline constexpr uint32_t kNpcHpBar = 0x00040000u;    // DSP_NPC_HPBAR (no reader left in retail)
    }
    namespace Sta // GameWork.flags5E4 (0x747A84; word 2 at 0x747A88): "state"
    {
        inline constexpr uint32_t kCockpit = 0x80000000u;     // COCKPIT
        inline constexpr uint32_t kScenario = 0x40000000u;    // SCENARIO (cScenario_isEventStartOk: events locked out)
        inline constexpr uint32_t kEvent = 0x20000000u;       // EVENT: a cutscene is running
        inline constexpr uint32_t kTalk = 0x10000000u;        // TALK
        inline constexpr uint32_t kSubScr = 0x08000000u;      // SUBSCR
        inline constexpr uint32_t kRoomJump = 0x02000000u;    // ROOMJUMP: a door jump is running (cDoor_setDoorJump)
        inline constexpr uint32_t kSuspend = 0x01000000u;     // STA_SUSPEND
        inline constexpr uint32_t kStageStartJump = 0x00800000u; // STA_STAGE_START_JUMP
        inline constexpr uint32_t kLvMaxFix = 0x00400000u;    // STA_LV_MAX_FIX
        inline constexpr uint32_t kReleaseEm = 0x00100000u;   // RELEASE_EM
        inline constexpr uint32_t kEvent2 = 0x00080000u;      // STA_EVENT2
        inline constexpr uint32_t kWorldSlow = 0x00020000u;   // STA_WORLD_SLOW: slow motion
        inline constexpr uint32_t kDoorJump = 0x00008000u;    // STA_DOOR_JUMP
        inline constexpr uint32_t kTitle = 0x00004000u;       // STA_TITLE
        inline constexpr uint32_t kMovie = 0x00002000u;       // (unnamed in the preview) a movie plays (decomp MOVIE_SYS_PLAYING)
        inline constexpr uint32_t kSescrOff = 0x00000800u;    // SESCR_OFF
        inline constexpr uint32_t kWipe = 0x00000080u;        // WIPE
        inline constexpr uint32_t kSoftEvent = 0x00000040u;   // SOFT_EVENT: a scripted soft event (casino slots, dog race)
        inline constexpr uint32_t kRoomJumpNoWipe = 0x00000020u; // ROOMJUMP_NOWIPE
        inline constexpr uint32_t kSoftEventCancel = 0x00000010u; // SOFT EVENT CANCEL
        inline constexpr uint32_t kSceMovieCancel = 0x00000004u;  // SCE MOVIE CANCEL
    }
    namespace Dbg // GameWork +0x58C..+0x59C (0x747A2C..0x747A3C): the preview's DBG group (its 0x718CA0..), five words
                  // moved here in retail (word 2 is the decomp's debugFlags). Retail still reads ~30 of the bits; the
                  // ones the Debug tab sets (claude/preview-debug-flags.md has the rest).
    {
        inline constexpr uint32_t word(uint32_t index) { return 0x58Cu + 4u * index; }
        inline constexpr uint32_t kPlMuteki = 0x00000008u;    // word 0, PL MUTEKI: the grab checks and hazards skip Gene
        inline constexpr uint32_t kGhCostZero = 0x00080000u;  // word 0, DBG_PL_GH_COST_ZERO: tension and orbs kept full
        inline constexpr uint32_t kVibrateOff = 0x08000000u;  // word 0, VIBRATE OFF
        inline constexpr uint32_t kEmSetOff = 0x10000000u;    // word 1, DBG_EM_SET_OFF: no enemy placement on room load
        inline constexpr uint32_t kOmSetOff = 0x08000000u;    // word 1, DBG_OM_SET_OFF: no object placement on room load
        inline constexpr uint32_t kNoHitBlur = 0x00000800u;   // word 1, DBG_NO_HIT_BLUR
        inline constexpr uint32_t kMaxGold = 0x02000000u;     // word 2, MAX_GOLD: the gold getters write 999999
        inline constexpr uint32_t kAllGodReel = 0x00000002u;  // word 2, DBG_ALL_GOD_REEL: cCoreSave_getGodReel says yes
        inline constexpr uint32_t kWazaComboMax = 0x01000000u; // word 2, WAZA_COMBO_MAX: cCoreSave_getComboST writes 6
        inline constexpr uint32_t kWazaFullOpen = 0x00800000u; // word 2, DBG_WAZA_FULL_OPEN: cCoreSave_getSkill says owned
        inline constexpr uint32_t kPathfindingOff = 0x00080000u; // word 3, PATHFINDING OFF
        inline constexpr uint32_t kHpTpMax = 0x08000000u;     // word 3, DBG_HP_TP_MAX: the save's +0x154..0x156 maxed
    }
    // GameWork.flags5D8 bits the port uses (ours, before the names above).
    inline constexpr uint32_t kFlags5D8PlayerOff = Stp::kPl | Stp::kEm; // the player's Move returns at once (func_001F6C40)
    inline constexpr uint32_t kFlags5D8KeyStop = Stp::kKey;             // the controls are off, the player still updates

    // cGameObj, the base of the game's actors (cGameObj.h).
    namespace GameObj
    {
        inline constexpr uint32_t kPos = 0x0F0u;        // decomp: pos (cVec *, the live position)
        inline constexpr uint32_t kRot = 0x100u;        // decomp: rot[3] (rot[1], +0x104, is the heading)
        inline constexpr uint32_t kMode = 0x2F4u;       // decomp: mode (with phase, step, stepArg after it)
        inline constexpr uint32_t kMotionData = 0x304u; // decomp: motionData (the loaded model's data block)
    }

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
    static_assert(GameWork::address(GameWork::kFlagsDsp) == 0x747A80u);
    static_assert(GameWork::address(GameWork::kRoomJumpTable) == 0x747A44u);
    static_assert(kFlags5D8PlayerOff == 0xC0000000u && kFlags5D8KeyStop == 0x02000000u);
    static_assert(GameWork::address(GameWork::kPlayerObj) == 0x747A94u);
    static_assert(GameWork::address(GameWork::kLangData) == 0x7479F8u);
    static_assert(GameWork::address(0x10u) == 0x7474B0u);
}
