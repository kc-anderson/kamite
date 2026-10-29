// Dev tools in the F2 menu's Debug tab: cheats for testing, a room jump and the game's display switches (dev-only:
// strip-list).
//
// Cheats (the preview disc's DBG_PL_NO_DEATH / DBG_EM_NO_DEATH / DBG_EM_DIRECT_DEATH clamped HP inside the damage
// step; retail dropped those checks, so the same clamps are hooks in the retail damage steps, plus per-frame refills):
//   God mode           Gene's HP (cGameObj +0x54A) never falls below 1 in his damage step (func_0012C0F8, hook
//                      km_dbg_pl_damage at 0x12C2A4, right after the subtraction) and is put back to his maximum
//                      (+0x548) every frame. A pit or a fall is not a damage step.
//   Enemies can't die  an enemy's HP never falls below 1 in the enemies' damage step (func_0028FB88, the vtable
//                      method; hook km_dbg_em_damage at 0x28FD38) and is put back to its maximum once it falls to a
//                      quarter: their bars move, they do not die.
//   One-hit kills      the damage step sets the HP to 0 (the enemy dies of the first hit), and every live enemy's HP
//                      is held at 1 between hits.
//   Dizzy at once      the enemies' add-stun (func_0028ED28; hook km_dbg_em_dizzy at 0x28ED3C) fills the gauge on any
//                      stun damage (the preview's DBG_EM_KIZETSU).
//
// Developer switches: the preview's DBG group lives on in retail at GameWork +0x58C..+0x59C (game::Dbg; the preview had
// it at 0x718CA0) and retail still reads about 30 of its bits. Ticked boxes set their bit at every frame start (as the
// Hide switches below; the game never sets these itself). PL MUTEKI lost its check in the hit loop (the preview's
// func_001D6910 skipped Gene's take units; retail func_001FCF20), so km_dbg_muteki at 0x1FD038 puts it back: the hit
// record whose target object is Gene is skipped as if he were in his own invulnerable frames.
// Enemies: cEmManage's list (0x5864F0 + 0x500: slot = {+4 next, +8 object}; kinds 0x200..0x27F are enemies, alive
// while HP > 0 and +0x250 has no release bit), as modern_controls' nearest-enemy scan reads it.
//
// Display switches. GameWork +0x5E0 (0x747A80) is the preview's DSP group (game::Dsp): one bit per part of the game,
// and a SET bit HIDES that part (the dog race and the jukebox set them to hide the world and Gene, the door jump sets
// DSP_ACTBTN to hide the prompt; func_002BF708 sets them all). Retail still reads most of them: Gene (0x1020F8,
// 0x107F28), the enemies (12 enemy draw functions), the objects (0x1B80C0, IsTargetVisible_14B470), the stage
// (0x319848), the screen filters (classCFILTER_TAG_Filter_main), the shadows (cObjBase_KageDraw), the coat
// (IsTargetVisibleOrForced_1004C8), the action-button prompt (0x1F7360). A ticked box sets its bit at every frame
// start; untick it and the bit gets the value the game last gave it (a clear by the game while hidden counts; a set
// by the game meanwhile is missed, so a part the game hid meanwhile may show again: a dev tool).
//
// Room jump. RoomJmpTbl.dat (etc/debugres, loaded by retail into D_00747A44 for the casino's jump points): u32 group
// count, u32 group table offset, u32 record offset; groups of (u16 room, u8 count, u8 first); 0x30-byte records of
// position (metres), heading (radians when it fits -PI..PI, else degrees: cRoomJump_setNextPos) and the developers'
// Shift-JIS name. The jump is the game's forced door jump func_001FFF30 (cScenario_beginRoomJumpForce's body:
// a0 = the door D_00583EC0, a1 = the room id, a2 = the landing position, f12 = the heading, a3 = arg, t0 = the fade
// (1 white, 2 black), t1 = doorArg), the one func_002BF568 uses to restart a room where Gene stands. Only rooms whose
// module is on the retail disc are listed (the table also has the developers' test rooms). The landing point's story
// flags are not set: saving after a jump can leave the save in a state the game never makes.
#include "kamite/debug_tools.h"
#include "kamite/disc_files.h"
#include "kamite/hitbox_overlay.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h" // GPR_U32 / SET_GPR_U32
#include "runtime/ps2_memory.h"

#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_hotkeys.h"
#include "runtime/ps2_time_control.h"
#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kamite::debug_tools
{
    namespace
    {
        using namespace kamite::game;

        constexpr uint32_t kForcedDoorJump = 0x001FFF30u; // func_001FFF30 (cScenario_beginRoomJumpForce's body)
        constexpr uint32_t kDoor = 0x00583EC0u;           // D_00583EC0, the door jump's cDoor
        constexpr uint32_t kLanding = 0x000FFF00u;        // 16 bytes of guest scratch for the landing position
                                                          // (below the ELF, past npc_poses' slots, unused by the game)
        constexpr uint32_t kFadeBlack = 2u;
        constexpr float kPi = 3.1415927f;

        struct Switch
        {
            uint32_t bit;
            const char *label;
        };
        constexpr Switch kSwitches[] = {
            {Dsp::kPl, "Gene (DSP_PL)"},
            {Dsp::kPl01, "Gene's coat (DSP_PL01)"},
            {Dsp::kEm, "Enemies and NPCs (DSP_EM)"},
            {Dsp::kOm, "Objects (DSP_OM)"},
            {Dsp::kObj, "Game objects (DSP_OBJ)"},
            {Dsp::kScr, "Stage (DSP_SCR)"},
            {Dsp::kEff, "Effects (DSP_EFF)"},
            {Dsp::kEmitter, "Emitters (DSP_EMITTER)"},
            {Dsp::kKage, "Shadows (DSP_KAGE)"},
            {Dsp::kFilter, "Screen filters: blur, haze (DSP_FILTER)"},
            {Dsp::kWorldLight, "World light (DSP_WORLD_LIGHT)"},
            {Dsp::kCock, "HUD (DSP_COCK)"},
            {Dsp::kActBtn, "Action-button prompt (DSP_ACTBTN)"},
        };

        std::atomic<bool> g_godMode{false};
        std::atomic<bool> g_enemiesNoDeath{false};
        std::atomic<bool> g_oneHitKills{false};
        std::atomic<bool> g_enemiesDizzy{false};
        std::atomic<int> g_enemiesAlive{0};   // game thread -> UI: live enemies this frame
        constexpr uint32_t kEnemyList = 0x5864F0u + 0x500u; // cEmManage: the first used slot
        constexpr uint32_t kObjHpMax = 0x548u, kObjHp = 0x54Au; // cGameObj: shorts

        // A flag word some of whose bits the Debug tab keeps set while ticked. Untick a bit and it gets the value the
        // game last gave it (a clear by the game while forced counts; a set by the game meanwhile is missed).
        struct ForcedBits
        {
            std::atomic<uint32_t> wanted{0}; // UI thread writes, game thread reads
            uint32_t forcing = 0;            // game thread: the bits it is keeping set now
            uint32_t gameWants = 0;          // game thread: of those, the ones the game had set

            void apply(uint8_t *rdram, uint32_t address)
            {
                const uint32_t want = wanted.load(std::memory_order_relaxed);
                if (!want && !forcing)
                    return;
                uint32_t w = 0;
                std::memcpy(&w, rdram + (address & 0x01FFFFFFu), 4);
                const uint32_t starting = want & ~forcing, stopping = forcing & ~want;
                gameWants = (gameWants & ~starting) | (w & starting); // what the game had when the forcing began
                gameWants &= ~(~w & want & forcing);                   // cleared by the game while forced
                w = (w | want) & ~(stopping & ~gameWants);
                gameWants &= ~stopping;
                forcing = want;
                std::memcpy(rdram + (address & 0x01FFFFFFu), &w, 4);
            }
        };
        ForcedBits g_hide;       // GameWork DSP: a set bit hides that part
        ForcedBits g_dbg[4];     // GameWork DBG words 0..3

        struct DbgSwitch
        {
            uint32_t word;
            uint32_t bit;
            const char *label;
        };
        constexpr DbgSwitch kDbgSwitches[] = {
            {0, Dbg::kPlMuteki, "Invincible: hits, grabs and hazards pass through Gene (PL MUTEKI)##muteki"},
            {0, Dbg::kGhCostZero, "God Hand and roulette for free: tension and roulette orbs stay full (DBG_PL_GH_COST_ZERO)##ghfree"},
            {2, Dbg::kAllGodReel, "Every roulette reel owned: the roulette offers all its moves (DBG_ALL_GOD_REEL)##godreel"},
            {2, Dbg::kWazaFullOpen, "Every technique owned (DBG_WAZA_FULL_OPEN)##wazaopen"},
            {2, Dbg::kMaxGold, "Gold at 999,999: written into the save data (MAX_GOLD)##maxgold"},
            {3, Dbg::kHpTpMax, "Health and tension upgrades at max: written into the save data (DBG_HP_TP_MAX)##hptpmax"},
            {2, Dbg::kWazaComboMax, "Combo length at max: written into the save data (WAZA_COMBO_MAX)##combomax"},
            {1, Dbg::kEmSetOff, "No enemies placed when a room loads (DBG_EM_SET_OFF)##emsetoff"},
            {1, Dbg::kOmSetOff, "No objects placed when a room loads (DBG_OM_SET_OFF)##omsetoff"},
            {3, Dbg::kPathfindingOff, "Enemy pathfinding off (PATHFINDING OFF)##pathoff"},
            {1, Dbg::kNoHitBlur, "No hit blur (DBG_NO_HIT_BLUR)##nohitblur"},
            {0, Dbg::kVibrateOff, "No vibration (VIBRATE OFF)##vibeoff"},
        };

        struct Point
        {
            uint16_t room = 0;
            uint8_t index = 0;
            float pos[3] = {};
            float heading = 0.0f; // radians
            std::string label;
        };
        std::vector<Point> g_points;
        std::vector<std::string> g_labels; // for the combo
        std::atomic<int> g_request{-1};     // a point to jump to, taken at the next frame start
        int g_selected = 0;                 // UI thread
        std::mutex g_statusMutex;
        std::string g_status;
        PS2Runtime::RecompiledFunction g_jump = nullptr;
        uint16_t g_room = 0xFFFFu;          // the room Gene is in (game thread writes)

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & 0x01FFFFFFu), &v, 4); }
        bool inRam(uint32_t address, uint32_t size)
        {
            const uint32_t a = address & 0x1FFFFFFFu;
            return a >= 0x100000u && a + size <= PS2_RAM_SIZE;
        }

        // km_dbg_muteki at 0x1FD038 (func_001FCF20, the hit loop: `addiu s3,zero,1`, then `beql v0,s3` skips the hit
        // record while $v0 = 1). s1 = the hit's target object.
        void mutekiHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 19, 1u);
            if ((g_dbg[0].wanted.load(std::memory_order_relaxed) & Dbg::kPlMuteki) &&
                GPR_U32(ctx, 17) == rd32(rdram, GameWork::address(GameWork::kPlayerObj)))
                SET_GPR_U32(ctx, 2, 1u);
        }

        // km_dbg_em_dizzy at 0x28ED3C (func_0028ED28, the enemies' add-stun: +0x5B4 += a1, clamped to 0..100000, then
        // +0x5B8 = 90). The preview's DBG_EM_KIZETSU set the gauge to 100000 when a1 > 0; this puts it there before the
        // clamp reads it.
        void dizzyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (g_enemiesDizzy.load(std::memory_order_relaxed) && GPR_S32(ctx, 5) > 0 && inRam(obj, 0x600u))
                wr32(rdram, obj + 0x5B4u, 100000u);
        }

        void setHp(uint8_t *rdram, uint32_t obj, int16_t hp) { std::memcpy(rdram + ((obj + kObjHp) & 0x01FFFFFFu), &hp, 2); }
        int16_t getHp(const uint8_t *rdram, uint32_t obj)
        {
            int16_t hp = 0;
            std::memcpy(&hp, rdram + ((obj + kObjHp) & 0x01FFFFFFu), 2);
            return hp;
        }
        // km_dbg_pl_damage at 0x12C2A4 (Gene's damage step, after `sh v0,0x54A(s1)` / the clamp to 0); s1 = Gene.
        void playerDamageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t obj = GPR_U32(ctx, 17);
            if (g_godMode.load(std::memory_order_relaxed) && inRam(obj, 0x600u) && getHp(rdram, obj) < 1)
                setHp(rdram, obj, 1);
        }
        // km_dbg_em_damage at 0x28FD38 (the enemies' damage step, after the subtraction); s1 = the enemy.
        void enemyDamageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t obj = GPR_U32(ctx, 17);
            if (!inRam(obj, 0x600u))
                return;
            if (g_oneHitKills.load(std::memory_order_relaxed))
                setHp(rdram, obj, 0);
            else if (g_enemiesNoDeath.load(std::memory_order_relaxed) && getHp(rdram, obj) < 1)
                setHp(rdram, obj, 1);
        }

        // KAMITE_ADX_PROBE=1 (diagnostic, claude/audio-notes.md): the music doubling. The ADMA dump of Oct 28 showed the
        // right channel jumping 8192 samples (one 16 KB lap) ahead of the left after a hitch. CRI's EE transfer
        // server (func_00340130, once a vblank) moves each stream channel from its EE ring to the IOP in its own slot
        // (32 slots of 0x40 at 0x3EBC20: +0 on, +4 source SJ, +8 destination SJ, +0x1C/+0x20/+0x24 the transfer's
        // source, destination and size, +0x2C its DMA id; a stereo stream takes two slots side by side). Hook
        // km_adx_xfer at 0x3400E0 (func_0033FDC0 right after a successful sceSifSetDma, s2 = the slot): the bytes each
        // slot has moved since its stream started; a line whenever a pair's left-minus-right count moves by 2 KB or
        // more (at most 400 lines). A jump of 16 KB at the hitch = the EE side; a steady count = the IOP side.
        void adxDumpRecord(const uint8_t *rdram, char kind, uint32_t slot, uint32_t sj, uint32_t addr, uint32_t len);
        void adxRingPairLine(const uint8_t *rdram, uint32_t k);
        uint64_t g_adxMoved[32] = {};
        uint32_t g_adxSource[32] = {};
        int64_t g_adxLogged[16] = {};
        void adxTransferHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            constexpr uint32_t kSlots = 0x3EBC20u;
            uint64_t *moved = g_adxMoved;
            uint32_t *source = g_adxSource;
            int64_t *logged = g_adxLogged;
            static uint32_t linesLeft = 400u;
            const uint32_t slot = GPR_U32(ctx, 18);
            if (slot < kSlots || slot >= kSlots + 32u * 0x40u || (slot - kSlots) % 0x40u != 0u)
                return;
            const uint32_t k = (slot - kSlots) / 0x40u, pair = k / 2u;
            adxDumpRecord(rdram, 'T', k, rd32(rdram, slot + 4u), rd32(rdram, slot + 0x1Cu), rd32(rdram, slot + 0x24u));
            if ((k & 1u) != 0u && rdram[kSlots + (k - 1u) * 0x40u] == 1u)
                adxRingPairLine(rdram, k);
            if (rd32(rdram, slot + 4u) != source[k])
            {
                source[k] = rd32(rdram, slot + 4u);
                moved[k] = 0u;
                logged[pair] = 0;
                if (linesLeft != 0u && (--linesLeft, true))
                    KM_INFO(Game, "adx probe: slot " << k << " starts, source SJ 0x" << std::hex << source[k] << " -> 0x"
                                                     << rd32(rdram, slot + 8u) << std::dec);
            }
            const uint32_t size = rd32(rdram, slot + 0x24u);
            moved[k] += size;
            const uint32_t partner = k ^ 1u;
            if (static_cast<int8_t>(rdram[kSlots + partner * 0x40u]) != 1)
            {
                if (linesLeft != 0u && (--linesLeft, true))
                    KM_INFO(Game, "adx probe: slot " << k << " moved " << size << " while slot " << partner << " is off (flags "
                                                     << int(rdram[kSlots + partner * 0x40u]) << "/" << int(rdram[kSlots + partner * 0x40u + 1u])
                                                     << "/" << int(rdram[kSlots + partner * 0x40u + 2u]) << ", source SJ 0x" << std::hex
                                                     << rd32(rdram, kSlots + partner * 0x40u + 4u) << std::dec << ")");
                return;
            }
            if ((k & 1u) == 0u) // the left slot goes first in each pass: compare after the right one
                return;
            const int64_t diff = static_cast<int64_t>(moved[pair * 2u]) - static_cast<int64_t>(moved[pair * 2u + 1u]);
            const int64_t change = diff - logged[pair];
            if ((change >= 2048 || change <= -2048) && linesLeft != 0u && (--linesLeft, true))
            {
                KM_INFO(Game, "adx probe: pair " << pair << " left-right " << diff << " bytes (was " << logged[pair] << "); slot " << k
                                                 << " moved " << size << " to 0x" << std::hex << rd32(rdram, slot + 0x20u)
                                                 << ", partner's last " << std::dec << rd32(rdram, kSlots + partner * 0x40u + 0x24u)
                                                 << " to 0x" << std::hex << rd32(rdram, kSlots + partner * 0x40u + 0x20u) << std::dec
                                                 << "; totals " << moved[pair * 2u] << " / " << moved[pair * 2u + 1u]);
                logged[pair] = diff;
            }
        }

        // The DTR slot life cycle for the ADX probe: create (DTR_Create 0x3401F0, at 0x3402BC: s1 = the new slot),
        // destroy (0x3402E0, at 0x3402F8: s0), start (0x340308, at 0x34030C: a0) and stop (DTR_Stop 0x340318, at
        // 0x340334: s2), with the caller (ra where it is still live).
        void adxSlotLine(const char *what, uint8_t *rdram, uint32_t slot, uint32_t ra)
        {
            constexpr uint32_t kSlots = 0x3EBC20u;
            static uint32_t linesLeft = 300u;
            if (slot < kSlots || slot >= kSlots + 32u * 0x40u || linesLeft == 0u)
                return;
            --linesLeft;
            KM_INFO(Game, "adx probe: " << what << " slot " << (slot - kSlots) / 0x40u << " (source SJ 0x" << std::hex
                                        << rd32(rdram, slot + 4u) << " -> 0x" << rd32(rdram, slot + 8u) << ", ra 0x" << ra << std::dec
                                        << ", flags " << int(rdram[slot]) << "/" << int(rdram[slot + 1u]) << "/" << int(rdram[slot + 2u]) << ")");
        }
        void adxCreateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t slot = GPR_U32(ctx, 17);
            adxSlotLine("create", rdram, slot, 0u);
            if (slot >= 0x3EBC20u && slot < 0x3EBC20u + 32u * 0x40u)
            {
                const uint32_t k = (slot - 0x3EBC20u) / 0x40u;
                adxDumpRecord(rdram, 'C', k, rd32(rdram, slot + 4u), rd32(rdram, slot + 8u), 0u);
                g_adxMoved[k] = 0u;
                g_adxSource[k] = rd32(rdram, slot + 4u);
                g_adxLogged[k / 2u] = 0;
                // The two rings (source on the EE, destination for the IOP): their first words, to learn base/size/positions.
                for (const uint32_t sj : {rd32(rdram, slot + 4u), rd32(rdram, slot + 8u)})
                {
                    if (!inRam(sj, 0x40u))
                        continue;
                    std::string words;
                    char text[16];
                    for (uint32_t i = 0; i < 16u; ++i)
                    {
                        std::snprintf(text, sizeof(text), " %X", rd32(rdram, sj + 4u * i));
                        words += text;
                    }
                    KM_INFO(Game, "adx probe:   SJ 0x" << std::hex << sj << std::dec << ":" << words);
                }
            }
        }
        // KAMITE_ADX_DUMP=<file> (with KAMITE_ADX_PROBE=1; diagnostic, claude/audio-notes.md, strip-list): the bytes of
        // every stream channel at two points, to find where the right channel gains its 16 KB lap. 'P' = a chunk the
        // ADX decoder commits to its source ring (the EE SJRBF PutChunk wrapper 0x339A98, hook km_adx_put: a0 = SJ,
        // a1 = line, 1 = data, a2 -> {addr, len}; only SJs that are a DTR slot's source); 'T' = a chunk the transfer
        // server sends to the IOP (km_adx_xfer: slot +0x1C source, +0x24 size). 'C' / 'X' = slot create / destroy (sj =
        // source SJ, addr = destination SJ). Record: char kind, u8 slot, u16 0, u32 sj, u32 addr, u32 len, f64 seconds
        // (the log's clock), then len bytes for P and T. tools/audio/adx_dump.py reads it. KAMITE_ADX_DUMP_MB caps the
        // file (default 512).
        std::mutex g_adxDumpLock;
        std::FILE *g_adxDump = nullptr;
        uint64_t g_adxDumpLeft = 0u;
        void adxDumpRecord(const uint8_t *rdram, char kind, uint32_t slot, uint32_t sj, uint32_t addr, uint32_t len)
        {
            if (g_adxDump == nullptr)
                return;
            const bool payload = kind == 'P' || kind == 'T';
            if (payload && (len == 0u || len > 0x10000u || !inRam(addr, len)))
                return;
            std::lock_guard<std::mutex> lock(g_adxDumpLock);
            const uint64_t size = 24u + (payload ? len : 0u);
            if (g_adxDump == nullptr || g_adxDumpLeft < size)
            {
                if (g_adxDump != nullptr)
                {
                    std::fclose(g_adxDump);
                    g_adxDump = nullptr;
                    KM_INFO(Game, "adx dump: size cap reached, file closed");
                }
                return;
            }
            g_adxDumpLeft -= size;
            uint8_t head[24] = {};
            head[0] = static_cast<uint8_t>(kind);
            head[1] = static_cast<uint8_t>(slot);
            std::memcpy(head + 4, &sj, 4);
            std::memcpy(head + 8, &addr, 4);
            std::memcpy(head + 12, &len, 4);
            const double seconds = kamite::log::elapsedSeconds();
            std::memcpy(head + 16, &seconds, 8);
            std::fwrite(head, 1, sizeof(head), g_adxDump);
            if (payload)
                std::fwrite(rdram + (addr & 0x01FFFFFFu), 1, len, g_adxDump);
            static uint64_t sinceFlush = 0u;
            if ((sinceFlush += size) >= (256u << 10)) // the game is often closed, not quit: keep the file current
            {
                sinceFlush = 0u;
                std::fflush(g_adxDump);
            }
        }
        // km_adx_put at 0x339A98 (the EE SJRBF PutChunk wrapper's first instruction).
        void adxPutHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            constexpr uint32_t kSlots = 0x3EBC20u;
            if (g_adxDump == nullptr || GPR_U32(ctx, 5) != 1u)
                return;
            const uint32_t sj = GPR_U32(ctx, 4), chunk = GPR_U32(ctx, 6);
            for (uint32_t k = 0; k < 32u; ++k)
                if (rdram[kSlots + k * 0x40u] == 1u && rd32(rdram, kSlots + k * 0x40u + 4u) == sj)
                {
                    if (inRam(chunk, 8u))
                        adxDumpRecord(rdram, 'P', k, sj, rd32(rdram, chunk), rd32(rdram, chunk + 4u));
                    return;
                }
        }
        // The source rings of a stereo pair (slots k-1, k), at each transfer of the right slot: a line when the two
        // rings' data or free positions part (mod 16 KB) or the decoder's / transfer server's byte totals part by
        // 1 KB. EE SJRBF: +0xC data bytes, +0x10 free bytes, +0x14 free pos, +0x18 data pos, +0x28/+0x2C free taken /
        // returned, +0x30/+0x34 data taken / committed (the IOP SJRBF's layout).
        void adxRingPairLine(const uint8_t *rdram, uint32_t k)
        {
            static uint32_t linesLeft = 300u;
            static uint32_t lastPos[16][2] = {};
            static int64_t lastTotals[16][2] = {};
            const uint32_t left = rd32(rdram, 0x3EBC20u + (k - 1u) * 0x40u + 4u), right = rd32(rdram, 0x3EBC20u + k * 0x40u + 4u);
            if (linesLeft == 0u || !inRam(left, 0x40u) || !inRam(right, 0x40u))
                return;
            const auto f = [rdram](uint32_t sj, uint32_t off) { return rd32(rdram, sj + off); };
            const uint32_t dataPos = (f(left, 0x18u) - f(right, 0x18u)) & 0x3FFFu, freePos = (f(left, 0x14u) - f(right, 0x14u)) & 0x3FFFu;
            const int64_t put = int64_t(f(left, 0x34u)) - int64_t(f(right, 0x34u)), sent = int64_t(f(left, 0x30u)) - int64_t(f(right, 0x30u));
            const uint32_t pair = k / 2u;
            uint32_t *pos = lastPos[pair];
            int64_t *totals = lastTotals[pair];
            if (dataPos == pos[0] && freePos == pos[1] && std::llabs(put - totals[0]) < 1024 && std::llabs(sent - totals[1]) < 1024)
                return;
            pos[0] = dataPos;
            pos[1] = freePos;
            totals[0] = put;
            totals[1] = sent;
            --linesLeft;
            KM_INFO(Game, "adx probe: rings " << (k - 1u) << "/" << k << " data pos 0x" << std::hex << f(left, 0x18u) << "/0x" << f(right, 0x18u)
                                              << " free pos 0x" << f(left, 0x14u) << "/0x" << f(right, 0x14u) << std::dec << " data "
                                              << f(left, 0xCu) << "/" << f(right, 0xCu) << " free " << f(left, 0x10u) << "/" << f(right, 0x10u)
                                              << "; decoder put L-R " << put << ", sent L-R " << sent << " (totals put " << f(left, 0x34u)
                                              << "/" << f(right, 0x34u) << ", sent " << f(left, 0x30u) << "/" << f(right, 0x30u) << ")");
        }
        void adxDestroyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t slot = GPR_U32(ctx, 16);
            adxSlotLine("destroy", rdram, slot, 0u);
            if (slot >= 0x3EBC20u && slot < 0x3EBC20u + 32u * 0x40u)
                adxDumpRecord(rdram, 'X', (slot - 0x3EBC20u) / 0x40u, rd32(rdram, slot + 4u), rd32(rdram, slot + 8u), 0u);
        }
        void adxStartHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { adxSlotLine("start", rdram, GPR_U32(ctx, 4), GPR_U32(ctx, 31)); }
        void adxStopHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { adxSlotLine("stop", rdram, GPR_U32(ctx, 18), 0u); }

        // The developers' names (Shift-JIS in the table), in English. "なまえをいれてください" ("please enter a
        // name") and points without a name here are "(unnamed)".
        const char *nameOf(uint16_t room, uint8_t index)
        {
            struct Name
            {
                uint16_t room;
                uint8_t index;
                const char *name;
            };
            static constexpr Name kNames[] = {
                {0x000, 0, "Casino entrance"}, {0x001, 0, "Before the casino door"}, {0x002, 0, "Meeting room entrance"},
                {0x003, 0, "Test point"}, {0x005, 0, "Second floor entrance"}, {0x006, 0, "Third floor entrance"},
                {0x040, 0, "Ring 1"}, {0x040, 1, "Ring 2"}, {0x040, 2, "Ring 3"},
                {0x100, 0, "Starting place"}, {0x100, 1, "Second half"}, {0x101, 0, "Elvis's mansion entrance"},
                {0x102, 0, "Western town entrance"}, {0x102, 1, "Western town, before the boss"}, {0x102, 2, "Test point"},
                {0x104, 0, "Elvis's room"}, {0x105, 0, "Western cliff"}, {0x105, 1, "In front of the inn"},
                {0x105, 2, "Cliff"}, {0x110, 0, "Start"},
                {0x200, 0, "Start"}, {0x201, 0, "Start"}, {0x201, 1, "Second half start"}, {0x202, 0, "Start"},
                {0x203, 0, "Start"}, {0x204, 0, "Event square"}, {0x205, 0, "Start"}, {0x206, 0, "Start"}, {0x207, 0, "Start"},
                {0x300, 0, "Merry-go-round square entrance"}, {0x301, 1, "Cannon game"}, {0x304, 1, "Before the attraction"},
                {0x400, 0, "Drill room"}, {0x401, 0, "Beginning"}, {0x402, 0, "Event room"}, {0x403, 0, "Mid-boss room"},
                {0x404, 0, "Press room"}, {0x405, 0, "Steel bridge room"}, {0x405, 1, "Cage"}, {0x406, 0, "Devil Hand room"},
                {0x407, 0, "Lift room"}, {0x408, 0, "Box-throwing room"}, {0x409, 0, "Cage room"},
                {0x500, 0, "Water city"}, {0x501, 0, "Signboard"}, {0x502, 0, "Floodgate"}, {0x503, 0, "Entrance"},
                {0x504, 0, "Church"}, {0x505, 0, "Before the boss room"}, {0x506, 0, "The band"}, {0x507, 0, "Back alley"},
                {0x508, 0, "Start"}, {0x509, 0, "Harbour town 2"},
                {0x600, 0, "Stepping stones"}, {0x601, 0, "Drop point"}, {0x601, 1, "Uphill slope"}, {0x602, 0, "Boss room"},
                {0x603, 0, "Showdown: the sentai Five"}, {0x604, 0, "Before the lift"}, {0x605, 0, "Showdown: Psycho Chibi"},
                {0x606, 0, "Cave 1"}, {0x607, 0, "Cave 2"}, {0x608, 0, "Drawbridge"},
                {0x700, 0, "Desert"}, {0x701, 0, "Mirage town"}, {0x702, 0, "Pyramid"}, {0x704, 0, "Boss room"},
                {0x800, 0, "Tower entrance"}, {0x801, 0, "Satan's room"}, {0x802, 0, "Mirage town, second half"},
                {0x803, 0, "Boss entrance 1"}, {0x804, 0, "Boss entrance 2"}, {0x805, 0, "Boss entrance 3"},
                {0x806, 0, "Tower top"},
            };
            for (const Name &n : kNames)
                if (n.room == room && n.index == index)
                    return n.name;
            return "(unnamed)";
        }

        // A room needs its module and its map data. r502 (Floodgate) is cut: its module is a stub and st5/r502.dat is
        // not on the disc, so jumping there calls a load routine that is not there.
        bool roomModuleExists(uint16_t room)
        {
            const unsigned stage = (room >> 8) & 0xF, index = room & 0xFF;
            char path[48];
            std::snprintf(path, sizeof(path), "dll/debugoptimize/r%x%02x.rel", stage, index);
            if (!disc_files::find(path))
                return false;
            std::snprintf(path, sizeof(path), "st%x/r%x%02x.dat", stage, stage, index);
            return disc_files::find(path).has_value();
        }

        void loadPoints()
        {
            if (!disc_files::open())
                return;
            const auto file = disc_files::find("etc/debugres/roomjmptbl.dat");
            std::vector<uint8_t> d;
            if (!file || !disc_files::read(*file, d) || d.size() < 16)
                return;
            uint32_t groups = 0, groupOffset = 0, recordOffset = 0;
            std::memcpy(&groups, d.data(), 4);
            std::memcpy(&groupOffset, d.data() + 4, 4);
            std::memcpy(&recordOffset, d.data() + 8, 4);
            for (uint32_t g = 0; g < groups && groupOffset + 4 * g + 4 <= d.size(); ++g)
            {
                uint16_t room = 0;
                std::memcpy(&room, d.data() + groupOffset + 4 * g, 2);
                const uint8_t count = d[groupOffset + 4 * g + 2], first = d[groupOffset + 4 * g + 3];
                if (!roomModuleExists(room))
                    continue;
                for (uint8_t k = 0; k < count; ++k)
                {
                    // r601 #2 ("Uphill slope", -40,-145,45) is stale: it lands outside the finished map.
                    if (room == 0x601 && k == 1)
                        continue;
                    const size_t r = recordOffset + 0x30u * (first + k);
                    if (r + 16 > d.size())
                        break;
                    Point p;
                    p.room = room;
                    p.index = k;
                    std::memcpy(p.pos, d.data() + r, 12);
                    float w = 0.0f;
                    std::memcpy(&w, d.data() + r + 12, 4);
                    p.heading = (w < -kPi || w > kPi) ? w * (kPi / 180.0f) : w;
                    char label[128];
                    std::snprintf(label, sizeof(label), "r%x%02x  %s %s%s", (room >> 8) & 0xF, room & 0xFF,
                                  ((room >> 8) & 0xF) == 0 ? "Casino & rings:" : ("Stage " + std::to_string((room >> 8) & 0xF) + ":").c_str(),
                                  nameOf(room, k), count > 1 && room != 0x601 ? (" (#" + std::to_string(k + 1) + ")").c_str() : "");
                    p.label = label;
                    g_points.push_back(std::move(p));
                }
            }
            for (const Point &p : g_points)
                g_labels.push_back(p.label);
        }

        // Gene in play: a player, no cutscene, door jump, talk, soft event or sub screen, the player not switched off.
        bool canJump(const uint8_t *rdram, std::string &why)
        {
            const uint32_t player = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
            if (!inRam(player, 0x700u))
            {
                why = "No Gene in this room.";
                return false;
            }
            const uint32_t sta = rd32(rdram, GameWork::address(GameWork::kFlags5E4));
            if (sta & (Sta::kEvent | Sta::kRoomJump | Sta::kTalk | Sta::kSoftEvent | Sta::kSubScr))
            {
                why = "Not now: a cutscene, a door, a talk or a menu is running.";
                return false;
            }
            if (rd32(rdram, GameWork::address(GameWork::kFlags5D8)) & kFlags5D8PlayerOff)
            {
                why = "Not now: the game has the player switched off.";
                return false;
            }
            const uint32_t subScr = rd32(rdram, SubScr::kPointer);
            if (inRam(subScr, 0x60u) && static_cast<int32_t>(rd32(rdram, subScr + SubScr::kKind)) != -1)
            {
                why = "Not now: close the game's menu first.";
                return false;
            }
            return true;
        }

        void jump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Point &p)
        {
            std::string why;
            if (!canJump(rdram, why))
            {
                setStatus(why);
                return;
            }
            if (!g_jump && runtime->hasFunction(kForcedDoorJump))
                g_jump = runtime->lookupFunction(kForcedDoorJump);
            if (!g_jump)
            {
                setStatus("The game's door jump is not in this build.");
                return;
            }
            for (int i = 0; i < 3; ++i)
                std::memcpy(rdram + kLanding + 4 * i, &p.pos[i], 4);
            wr32(rdram, kLanding + 12, 0);
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 4, kDoor);
            SET_GPR_U32(ctx, 5, p.room);
            SET_GPR_U32(ctx, 6, kLanding);
            SET_GPR_U32(ctx, 7, 0u);
            SET_GPR_U32(ctx, 8, kFadeBlack);
            SET_GPR_U32(ctx, 9, 0u);
            ctx->f[12] = p.heading;
            g_jump(rdram, ctx, runtime);
            *ctx = saved;
            KM_INFO(Game, "debug: room jump to " << p.label);
            setStatus("Jumped to " + p.label + ".");
        }

#if defined(KAMITE_HAS_GS_MENU)
        void drawPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 2u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again.");
                return;
            }
            ui->separator_text("Cheats");
            auto toggle = [&](const char *label, std::atomic<bool> &setting) {
                int on = setting.load() ? 1 : 0;
                if (ui->checkbox(label, &on))
                    setting = on != 0;
            };
            toggle("God mode: Gene's HP never falls below 1 and is refilled every frame##god", g_godMode);
            toggle("Enemies can't die: their HP never falls below 1 and is refilled at a quarter##emnodeath", g_enemiesNoDeath);
            toggle("One-hit kills: an enemy dies of its first hit##onehit", g_oneHitKills);
            toggle("Dizzy at once: any hit that stuns fills an enemy's stun gauge (DBG_EM_KIZETSU)##dizzy", g_enemiesDizzy);
            char enemies[64];
            std::snprintf(enemies, sizeof(enemies), "%d live enemies in the room.", g_enemiesAlive.load());
            ui->text_disabled(enemies);

            ui->separator_text("Developer switches");
            ui->text_disabled("The game's own debug flags (names from the preview disc), still read by retail.");
            for (const DbgSwitch &s : kDbgSwitches)
            {
                uint32_t want = g_dbg[s.word].wanted.load();
                int on = (want & s.bit) ? 1 : 0;
                if (ui->checkbox(s.label, &on))
                    g_dbg[s.word].wanted.store(on ? (want | s.bit) : (want & ~s.bit));
            }
            if ((g_dbg[2].wanted.load() & (Dbg::kMaxGold | Dbg::kWazaComboMax)) || (g_dbg[3].wanted.load() & Dbg::kHpTpMax))
                ui->text_disabled("The ones written into the save data stay after unticking: don't save unless you want to keep them.");

            ui->separator_text("Room jump");
            if (g_points.empty())
                ui->text_disabled("The jump table (etc/debugres/RoomJmpTbl.dat) was not found on the disc image.");
            else
            {
                char here[64];
                std::snprintf(here, sizeof(here), "Gene is in r%x%02x.", (g_room >> 8) & 0xF, g_room & 0xFF);
                ui->text(here);
                std::vector<const char *> labels;
                for (const std::string &l : g_labels)
                    labels.push_back(l.c_str());
                ui->set_next_width(420.0f);
                ui->combo("##jumppoint", &g_selected, labels.data(), static_cast<int>(labels.size()));
                ui->same_line();
                if (ui->button("Jump##jump"))
                {
                    g_request.store(g_selected);
                    setStatus("Jumping...");
                }
                ui->text_disabled("The game's door jump with a black fade. The story flags of the place are not set: "
                                  "don't save after a jump you don't want in your save. Other stages: untested.");
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                    ui->text(g_status.c_str());
            }

            ui->separator_text("Slow motion and frame step");
            {
                namespace tc = ps2x::timecontrol;
                namespace hk = ps2x::hotkeys;
                static const char *const kSpeeds[] = {"Full speed", "1/2", "1/4", "1/8"};
                static constexpr uint32_t kDivisors[] = {1u, 2u, 4u, 8u};
                int speed = 0;
                for (int i = 0; i < 4; ++i)
                    if (kDivisors[i] == tc::slowDivisor())
                        speed = i;
                ui->set_next_width(160.0f);
                if (ui->combo("Speed##slowmo", &speed, kSpeeds, 4))
                    tc::setSlowDivisor(kDivisors[speed]);
                ui->same_line();
                int paused = tc::paused() ? 1 : 0;
                if (ui->checkbox("Paused##pause", &paused))
                    tc::setPaused(paused != 0);
                ui->same_line();
                if (ui->button("Step one frame##step"))
                    tc::step();
                ui->same_line();
                int mute = tc::muted() ? 1 : 0;
                if (ui->checkbox("Mute the game##mute", &mute))
                    tc::setMuted(mute != 0);
                char keys[192];
                std::snprintf(keys, sizeof(keys), "Keys: %s speed, %s pause, %s step (Controls > Hotkeys rebinds them). "
                              "The game runs exactly as at full speed, only slower; the sound crackles.",
                              hk::text(hk::binding(hk::Action::SlowMotion)).c_str(), hk::text(hk::binding(hk::Action::Pause)).c_str(),
                              hk::text(hk::binding(hk::Action::FrameStep)).c_str());
                ui->text_disabled(keys);
            }

            hitbox_overlay::drawPanel(ui);

            ui->spacing();
            if (ui->version >= 4u ? ui->collapsing_header("Hide", 0) != 0 : true)
            {
                ui->text_disabled("The game's own display switches (names from the preview disc's debug menu).");
                uint32_t hide = g_hide.wanted.load();
                for (const Switch &s : kSwitches)
                {
                    int on = (hide & s.bit) ? 1 : 0;
                    if (ui->checkbox(s.label, &on))
                        hide = on ? (hide | s.bit) : (hide & ~s.bit);
                }
                g_hide.wanted.store(hide);
                if (hide && ui->button("Show everything##showall"))
                    g_hide.wanted.store(0);
            }
        }
#endif
    }

    void install(PS2Runtime &)
    {
        loadPoints();
        KM_INFO(Game, "debug tools: " << g_points.size() << " room jump points");
        ps2xRegisterHostHook("km_dbg_muteki", &mutekiHook);
        ps2xRegisterHostHook("km_dbg_pl_damage", &playerDamageHook);
        ps2xRegisterHostHook("km_dbg_em_damage", &enemyDamageHook);
        ps2xRegisterHostHook("km_dbg_em_dizzy", &dizzyHook);
        ps2xSetPatchFlag("debug_cheats", true); // the hooks test their switches themselves
        if (const char *probe = std::getenv("KAMITE_ADX_PROBE"); probe && *probe == '1')
        {
            ps2xRegisterHostHook("km_adx_xfer", &adxTransferHook);
            ps2xRegisterHostHook("km_adx_create", &adxCreateHook);
            ps2xRegisterHostHook("km_adx_destroy", &adxDestroyHook);
            ps2xRegisterHostHook("km_adx_start", &adxStartHook);
            ps2xRegisterHostHook("km_adx_stop", &adxStopHook);
            ps2xRegisterHostHook("km_adx_put", &adxPutHook);
            ps2xSetPatchFlag("adx_probe", true);
            KM_INFO(Game, "adx probe: on (KAMITE_ADX_PROBE)");
            if (const char *path = std::getenv("KAMITE_ADX_DUMP"); path && *path)
            {
                const char *mb = std::getenv("KAMITE_ADX_DUMP_MB");
                g_adxDumpLeft = static_cast<uint64_t>(mb && *mb ? std::strtoul(mb, nullptr, 10) : 512ul) << 20;
                g_adxDump = std::fopen(path, "wb");
                KM_INFO(Game, "adx dump: " << (g_adxDump ? "writing " : "can't open ") << path << " (cap " << (g_adxDumpLeft >> 20) << " MB)");
            }
        }
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("Debug", &drawPanel);
#endif
    }

    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::memcpy(&g_room, rdram + GameWork::address(GameWork::kRoom), 2);
        // A frame step is one game frame: the game's vblanks per frame (2 at 30 fps, 1 at 60).
        ps2x::timecontrol::setStepVblanks(rdram[GameWork::address(GameWork::kVblankDivider)] == 1u ? 1u : 2u);
        // Cheats.
        if (g_godMode.load(std::memory_order_relaxed))
        {
            const uint32_t player = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
            if (inRam(player, 0x700u))
            {
                int16_t max = 0, hp = 0;
                std::memcpy(&max, rdram + ((player + kObjHpMax) & 0x01FFFFFFu), sizeof(max));
                std::memcpy(&hp, rdram + ((player + kObjHp) & 0x01FFFFFFu), sizeof(hp));
                if (max > 0 && hp > 0 && hp < max)
                    std::memcpy(rdram + ((player + kObjHp) & 0x01FFFFFFu), &max, sizeof(max));
            }
        }
        {
            const bool noDeath = g_enemiesNoDeath.load(std::memory_order_relaxed);
            const bool oneHit = g_oneHitKills.load(std::memory_order_relaxed);
            int alive = 0;
            uint32_t slot = rd32(rdram, kEnemyList);
            for (int guard = 0; guard < 64 && inRam(slot, 16u); ++guard)
            {
                const uint32_t em = rd32(rdram, slot + 8u);
                slot = rd32(rdram, slot + 4u);
                if (!inRam(em, 0x600u))
                    continue;
                uint16_t kind = 0;
                int16_t hp = 0, max = 0;
                std::memcpy(&kind, rdram + ((em + 0x2FEu) & 0x01FFFFFFu), sizeof(kind));
                std::memcpy(&hp, rdram + ((em + kObjHp) & 0x01FFFFFFu), sizeof(hp));
                std::memcpy(&max, rdram + ((em + kObjHpMax) & 0x01FFFFFFu), sizeof(max));
                const int32_t flags = static_cast<int32_t>(rd32(rdram, em + 0x250u));
                if (kind < 0x200u || kind >= 0x280u || hp <= 0 || flags < 0)
                    continue;
                ++alive;
                if (oneHit)
                {
                    const int16_t one = 1; // wins over "can't die": otherwise the two would trade HP every frame
                    if (hp > 1)
                        std::memcpy(rdram + ((em + kObjHp) & 0x01FFFFFFu), &one, sizeof(one));
                }
                else if (noDeath && max > 0 && hp <= max / 4)
                    std::memcpy(rdram + ((em + kObjHp) & 0x01FFFFFFu), &max, sizeof(max));
            }
            g_enemiesAlive.store(alive, std::memory_order_relaxed);
        }
        // Display switches and developer switches.
        g_hide.apply(rdram, GameWork::address(GameWork::kFlagsDsp));
        for (uint32_t i = 0; i < 4; ++i)
            g_dbg[i].apply(rdram, GameWork::address(Dbg::word(i)));
        // Room jump.
        const int request = g_request.exchange(-1);
        if (request >= 0 && request < static_cast<int>(g_points.size()) && ctx && runtime)
            jump(rdram, ctx, runtime, g_points[static_cast<size_t>(request)]);
    }
}
