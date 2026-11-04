// Optional changes to the game ([Game] UnlockHard / SkipIntro / MuteMusic / RouletteNoOrbs / LiftChairs) and Dead Pan.
// See game_options.h.
//
// LiftChairs: stage 1's chairs (om04, cOmBase, actor 0x304), bar stools (om38) and pub sign (om36, both cOmScrfix) have
// no carry or throw states; the crates in the same rooms (om0a, cOmThrow, 0x30A) do. All of them are one model: part 0
// whole, parts 1..N the break pieces (hidden), mesh 0 hidden / mesh 1 shown, which the crate's setup sets up alike. The object factory func_0030FE68(id a0, data id a1) switches on s0 = id to each class's
// alloc + constructor, then stores the id (+0x2FE / +0x300) and picks the data (texture, mesh group) by it. Under
// "lift_chairs" 0x304 / 0x336 / 0x338 is read as 0x30A for the switch (before sq s1 at 0x30FE7C) and put back once the object exists
// (before daddu a0,s2 at 0x3125B4): a crate-class object with the chair's id, mesh (Fx_chrNN), texture and break
// effect. claude/chair-notes.md.
// Held, the crate class's carry (cOmThrow phase 0, func_001C8020) poses the object with its own carry motion (part
// angles +0x100, translation +0x30) and then
// moves part 0 (+0x278[0] +0xD0, the translation in Gene's frame) by a per-id offset x the blend +0x624 (crate 0x30A:
// -0.1, 0.1, 0.2; none for 0x304). A chair's part 0 sits at the top of its backrest (1.0 m up, 0.27 m back), so it rode
// ~0.4 m over Gene's hands and swung out of them as he walked: the motion turns the object round its origin, the
// crate's middle but the chair's backrest top (case chair, Nov 2). km_lift_chairs_hold (before ori at 0x1C8248, every
// path after the offsets) re-centres the pose on the chair's middle, with an offset and turn F2 > Gameplay > Wild
// Curry Funk sets live.
//
// UnlockHard: the difficulty select is in the title room DLL r020 (cR020::titleLevelSel, 0x79B0E0). It reads the save's
// "Hard unlocked" bit (0x747A28 & 0x200000, set by clearing the game) three times: for the menu's item animation
// (0x79B25C), the confirm (0x79B374, a locked Hard buzzes) and the item sprites (0x79B924, Hard or the locked plate).
// recomp/modules/r020.toml turns each of those 'and' into 'or rX,zero,0x200000' under the flag "unlock_hard".
//
// SkipIntro: also r020 (symbols in the module: cR020::Initialize, MoveWait, MoveLogo, titlePressWait). The room plays
// the logos (state 1, MoveLogo) only while GameWork +0x584 (0x747A24) bit 0x800 is clear, then sets it: coming back
// to the title skips them already. Under "skip_intro" both tests of the bit (Initialize 0x7995A4, MoveWait 0x799970)
// read it as set, so the room waits 40 frames and opens the title. titlePressWait tests the pad's new presses
// (0x747640 & Start/Circle) at 0x79A204: read as pressed, the title goes on to the main menu by itself, with its own
// sound and animation.
//
// MuteMusic: the music is CRI ADX streams driven by the SDE sound library. Every stream volume change goes through
// func_00373FF0(node a0, category a1, volume f12) for a one-channel node, or func_003745C0(node a0, category a1,
// channel a2, volume f12) for a node with 2+ channels (node+0x58 > 1; the stereo music takes this one, which the
// first version of this option missed): volume x the cSnd category levels (0x5FEE00) -> the dB table at 0x3F2060 ->
// ADXT_SetOutVol (0x32A9F0) / func_0032EC78 per channel. Music streams are the cSndBgmNode pool inside cSnd
// (0x601180, 16 nodes of 0xDC bytes; decomp cSnd.h); for those the volume is made 0 at both functions' entry
// (table[0] = -96.0 dB, silent), except bank 1, the cutscene streams (their voices are in the same stream).
// Voices and sound effects are other node types; movies use Sofdec's own ADXT handles.
// KAMITE_TRACE=GAME logs the first calls (node, vtable, category, volume) to check the match.
//
// RouletteNoOrbs: R1 opens the roulette from Gene's action chooser (func_00123938 0x123B2C: cCoreSave_getGodItem, the
// first orb slot at save +0xA8, non-zero -> state 0x2B). getGodItem has no other caller (main ELF and room DLLs). Under
// "roulette_no_orbs" its first load (lw v1,0(a0) at 0x1FAC58) points v1 at a constant non-zero byte when the slots are
// empty, so the roulette opens. The list builder (func_00125048) greys out moves by cost only for reels costing 2 or 3
// (getGodItemNum at 0x125B44 / 0x125B58; a 1-orb reel was never checked: the entry needed an orb), so at 0x125B84,
// where every reel's check ends, an empty first slot greys the entry (+8 = 1) too, except the free Dead Pan (0xB).
//
// TensionPlacable / OffTheCuff (launcher: Experimental, "Tension is Placable" / its sub-option "Off the Cuff"): the
// tension gauge is cHeatSys at 0x5CB000 (+0 max 120, +4 the gauge, +8 the floor SubHeatGage stops at, +0xC the full
// mark, +0x10 the mode byte: 1 while the God Hand is unleashed). Gene's action chooser func_00123938 starts the unleash
// on an R2 press (GameWork +8 & 2 at 0x123AE0) when the mode is 0 and cHeatSys_GetHeatLv (0x2A9BC0: gauge >= full) says
// so: phase 8 (func_00114120 sets the mode to 1 and Gene +0x15F4 |= 0x10 at its animation event). The meter running
// out ends it at the top of the same chooser: cHeatSys_SetHeatMode(0), phase 9 (func_001142E0, the end pose; clears
// 0x10), +0x54C = 3.0, return 1. Placable: an R2 press while unleashed (mode byte read at 0x123AF4, read as 0) goes on
// to the GetHeatLv call, which says yes (wrapper, its call at 0x123B00 only), and the delay slot at 0x123B0C picks
// phase 9 instead of 8 after the game's own SetHeatMode(0) and +0x54C = 3.0: the same end, with the gauge left as it
// is (SetHeatMode(0) moves the floor to max(gauge - full, 0) = 0). Off the Cuff: the same GetHeatLv call also says
// yes with any tension (gauge > 0); the unleash then drains what there is. Unleashed, Gene's update func_00108AB0 holds
// his guard +0x54C at 5.0 after his state handler whenever the mode byte is set (0x1093A4: the invulnerability); an
// Off the Cuff unleash from below the full mark is marked in guest RAM (kPartialUnleash, so save states and cases keep
// it) and that read (lbu at 0x109398) sees 0 for it: no invulnerability (K: only a full gauge's unleash gives it). Case 2026-11-04_11-50-15 (an unleash run
// down: phase 8 at 1.2 s, the gauge 120 -> 0 at 12 a second, phase 9 at 12.3 s).
//
// Dead Pan (always on): the Japanese release's free roulette move (reel 0xB; TCRF "Dead Pan / Pan Drop"). The US build
// keeps it but hides it outside Japanese: func_00125048 tests GameWork +0x56C (language, 0 = Japanese) at 0x1250A8,
// 0x1251CC, 0x125264 and 0x125338 (the PCSX2 restore patch makes all four read 0). Here they read 0 once it is earned:
// the game-clear room r022 sets 0x747A28 (GameWork +0x588) |= 0x20000000 when the Kick Me Sign is still on (cCoreSave
// ckPaper; the same clear gives the KMS jukebox CD), or with F2's "Every roulette reel owned" (DBG_ALL_GOD_REEL).
// As in Japanese it then joins the roulette without being bought (the first reel list, 0x1250BC), and at Level 3 and
// DIE a second copy goes on the wheel (0x125338: getGameLevel >= 3 appends 0xB to the third list; Japanese code).
//
// Dead Pan's invincibility shown (always on with Dead Pan): from the pan's hit (Gene phase 41 step 2, func_00113CF8;
// the post-hit guard +0x54C is held at 30 through the state) until that guard has run out after it, Gene
// gets the taunted enemies' berserk look (func_00268788, the anger state +0x16EC): the steam, SetEffect(0x58, 7, obj,
// params, 5, -1) + SetEffect(0x58, 0xF, obj, params, -1, -1) once, KillEffect(obj, 5, 2) when it ends; the red face,
// the face meshes' colour (+0x3A0/+0x3A4/+0x3A8) eased 0.1 a frame toward (0.9, 0.5, 0.5) and back to white. Gene's
// head is the mesh HD_00 (pl00.dat's scr; looked up like cModel_getMeshPtr 0x14B730: obj +0x25C list, +0x404 next,
// +0x414 -> +8 = the name packed in a u64). Driven from the frame hook (deadPanFrame), host calls as debug_tools does.
#include "kamite/game_options.h"
#include "kamite/frame_patches.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include "runtime/ps2_save_state.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

namespace kamite
{
    namespace
    {
        // The BGM nodes are the pool cSnd::node[16] (0xDC bytes each) at cSnd (0x5FEE00) + 0x2380 (the decomp's
        // cSnd.h); the volume setters get a pointer to one of them. The first version of this option looked for a
        // vtable at node+0x80, which these nodes do not have (KAMITE_TRACE=GAME showed node 0x601180, vtable 0).
        constexpr uint32_t kBgmPool = 0x5FEE00u + 0x2380u;
        constexpr uint32_t kBgmNodeSize = 0xDCu;
        constexpr uint32_t kBgmNodeCount = 16u;

        bool envOn(const char *name)
        {
            const char *value = std::getenv(name);
            return value && *value && *value != '0';
        }

        // A live switch (F2) also sets its variable, so a test case recorded after it keeps the setting.
        void setenvFlag(const char *name, bool on)
        {
#ifdef _WIN32
            _putenv_s(name, on ? "1" : "0");
#else
            setenv(name, on ? "1" : "0", 1);
#endif
        }

        uint32_t ram32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        // Replaces lw v1,0x0(a0) at 0x1FAC58 (cCoreSave_getGodItem; flag roulette_no_orbs).
        constexpr uint32_t kNonZeroByte = 0x429890u; // '8' of the class name "8cOmGlass" (.rodata)
        void rouletteNoOrbs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t save = ram32(rdram, GPR_U32(ctx, 4));
            if (save != 0u && rdram[(save + 0xA8u) & 0x1FFFFFFu] == 0u)
                save = kNonZeroByte - 0xA8u;
            SET_GPR_U32(ctx, 3, save);
        }

        // Before lw a1,0x650(sp) at 0x125B84 (func_00125048, flag roulette_no_orbs): s2 = the entry, s5 = the reel ids.
        constexpr uint32_t kCoreSave = 0x569B70u;
        void rouletteGreyWithoutOrbs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t save = ram32(rdram, kCoreSave);
            if (save == 0u || rdram[(save + 0xA8u) & 0x1FFFFFFu] != 0u)
                return;
            const uint32_t index = GPR_U32(ctx, 18);
            if (ram32(rdram, GPR_U32(ctx, 21) + index * 0xCu) == 0xBu) // Dead Pan costs nothing
                return;
            rdram[(GPR_U32(ctx, 29) + 0x30u + index * 0xCu + 8u) & 0x1FFFFFFu] = 1u;
        }

        // Replaces the language loads in func_00125048 (flag dead_pan, always on): 0 (Japanese) once Dead Pan is earned.
        constexpr uint32_t kLanguage = 0x747A0Cu;     // GameWork +0x56C
        constexpr uint32_t kSaveFlags = 0x747A28u;    // GameWork +0x588
        constexpr uint32_t kKickMeSignClear = 0x20000000u;
        constexpr uint32_t kDbgWord2 = 0x747A34u;     // DBG_ALL_GOD_REEL = 0x2
        template <int Reg>
        void deadPanLanguage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const bool earned = (ram32(rdram, kSaveFlags) & kKickMeSignClear) != 0u || (ram32(rdram, kDbgWord2) & 0x2u) != 0u;
            SET_GPR_U32(ctx, Reg, earned ? 0u : ram32(rdram, kLanguage));
        }

        // Before the first instruction of func_00373FF0 (the stream volume setter).
        void muteMusicVolume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t node = GPR_U32(ctx, 4);
            bool music = node >= kBgmPool && node < kBgmPool + kBgmNodeSize * kBgmNodeCount &&
                         (node - kBgmPool) % kBgmNodeSize == 0u;
            if (music)
            {
                // Cutscene streams are BGM nodes too: bank 1 (stored biased, 0x81 at +0x8C), started per cut by
                // cSnd_BgmEvCutSet from the event's bgm data. They carry the cutscene's voices and effects mixed
                // with its music, so they stay; the stage, boss, menu and minigame music (bank 0) is muted.
                int32_t bank = 0;
                std::memcpy(&bank, rdram + node + 0x8Cu, sizeof(bank));
                if (bank - 0x80 == 1)
                    music = false;
            }
            static int logged = 0;
            if (logged < 40)
            {
                ++logged;
                KM_TRACE(Game, "[mute] volume call: node 0x" << std::hex << node << std::dec << " category " << GPR_U32(ctx, 5)
                                   << " volume " << ctx->f[12] << (music ? " (music: muted)" : ""));
            }
            if (music)
                ctx->f[12] = 0.0f;
        }
    }

    namespace
    {
        constexpr uint32_t kGameWork = 0x7474A0u;
        constexpr uint32_t kSetEffect = 0x306528u, kKillEffect = 0x306200u;
        constexpr uint8_t kDeadPanPhase = 41u;
        struct DeadPanLook
        {
            uint32_t gene = 0;
            bool active = false, steam = false;
            float fade = 1.0f; // 1 = Gene's own colours, 0 = full red (the enemies' +0x16F0)
        } g_look;

        float ramF(const uint8_t *rdram, uint32_t address)
        {
            float v = 0.0f;
            std::memcpy(&v, rdram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        void putF(uint8_t *rdram, uint32_t address, float v) { std::memcpy(rdram + (address & 0x1FFFFFFu), &v, sizeof(v)); }
        void put32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & 0x1FFFFFFu), &v, sizeof(v)); }

        uint32_t meshByName(const uint8_t *rdram, uint32_t obj, const char *name)
        {
            uint64_t key = 0;
            for (int i = 0; i < 8 && name[i]; ++i)
                key |= static_cast<uint64_t>(static_cast<uint8_t>(name[i])) << (8 * i);
            uint32_t mesh = ram32(rdram, obj + 0x25Cu);
            for (int guard = 0; guard < 256 && mesh >= 0x100000u && mesh < 0x2000000u; ++guard)
            {
                const uint32_t info = ram32(rdram, mesh + 0x414u);
                uint64_t have = 0;
                if (info >= 0x100000u && info < 0x2000000u)
                    std::memcpy(&have, rdram + info + 8u, sizeof(have));
                if (have == key)
                    return mesh;
                mesh = ram32(rdram, mesh + 0x404u);
            }
            return 0u;
        }

        // A guest function run to its end on a copy of the registers (debug_tools' room jump does the same).
        bool callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address,
                       std::initializer_list<uint32_t> args, uint32_t stack = 0u)
        {
            if (!runtime || !runtime->hasFunction(address))
                return false;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return false;
            const R5900Context saved = *ctx;
            int reg = 4;
            for (uint32_t a : args)
            {
                if (reg > 11)
                    break;
                SET_GPR_U32(ctx, reg, a);
                ++reg;
            }
            if (stack)
                SET_GPR_U32(ctx, 29, stack);
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            *ctx = saved;
            return true;
        }

        void startSteam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t gene)
        {
            // The parameter block func_00268788 builds on its stack (0x80 bytes), here below the caller's stack.
            const uint32_t block = (GPR_U32(ctx, 29) - 0x400u) & ~0xFu;
            std::memset(rdram + (block & 0x1FFFFFFu), 0, 0x80);
            for (uint32_t o : {0x00u, 0x04u, 0x08u, 0x0Cu, 0x30u, 0x34u, 0x38u, 0x3Cu})
                putF(rdram, block + o, 1.0f);
            for (uint32_t o : {0x1Cu, 0x2Cu, 0x6Cu}) // vf0 = (0, 0, 0, 1) at +0x10, +0x20, +0x60
                putF(rdram, block + o, 1.0f);
            putF(rdram, block + 0x40u, ramF(rdram, gene + 0x114u));
            rdram[(block + 0x4Cu) & 0x1FFFFFFu] = 0xFFu;
            rdram[(block + 0x4Fu) & 0x1FFFFFFu] = 0xFFu;
            callGuest(rdram, ctx, runtime, kSetEffect, {0x58u, 7u, gene, block, 5u, 0xFFFFFFFFu}, block);
            callGuest(rdram, ctx, runtime, kSetEffect, {0x58u, 0xFu, gene, block, 0xFFFFFFFFu, 0xFFFFFFFFu}, block);
        }

        void paintFace(uint8_t *rdram, uint32_t gene, float fade)
        {
            const uint32_t head = meshByName(rdram, gene, "HD_00");
            if (!head)
                return;
            const float base[3] = {0.9f, 0.5f, 0.5f}; // the enemies' red (0x3BF8AC)
            for (int i = 0; i < 3; ++i)
                putF(rdram, head + 0x3A0u + 4u * i, base[i] + fade * (1.0f - base[i]));
        }
    }

    void deadPanFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t loads = 0;
        DeadPanLook &look = g_look;
        const uint32_t gene = ram32(rdram, kGameWork + 0x5F4u);
        const uint32_t now = ps2x::savestate::loadCount();
        if (now != loads || gene != look.gene || gene < 0x100000u || gene >= 0x2000000u)
        {
            loads = now;
            look = DeadPanLook{};
            look.gene = (gene >= 0x100000u && gene < 0x2000000u) ? gene : 0u;
            return; // a new Gene (room change, load): his meshes and effects are new too
        }
        const bool inMove = rdram[(gene + 0x2F5u) & 0x1FFFFFFu] == kDeadPanPhase;
        if (inMove && rdram[(gene + 0x2F6u) & 0x1FFFFFFu] >= 2u) // from the pan's hit on (step 2), not the wind-up
            look.active = true;
        else if (look.active && ramF(rdram, gene + 0x54Cu) <= 0.0f)
            look.active = false;
        if (look.active && !look.steam)
        {
            startSteam(rdram, ctx, runtime, gene);
            look.steam = true;
        }
        else if (!look.active && look.steam)
        {
            callGuest(rdram, ctx, runtime, kKillEffect, {gene, 5u, 2u});
            look.steam = false;
        }
        const float step = fps60() ? 0.05f : 0.1f; // the enemies' 0.1 a frame, at the 30 FPS pace
        const bool red = look.active;
        const float before = look.fade;
        look.fade = red ? std::max(0.0f, look.fade - step) : std::min(1.0f, look.fade + step);
        if (look.fade < 1.0f || before < 1.0f)
            paintFace(rdram, gene, look.fade);
    }


    namespace
    {
        constexpr uint32_t kChairId = 0x304u, kCrateId = 0x30Au;
        constexpr uint32_t kPubSignId = 0x336u, kBarStoolId = 0x338u; // r102 (stage 1), cOmScrfix
        uint32_t g_liftSp = 0, g_liftId = 0; // the factory call (its sp) building a prop as a crate, and the prop's id

        // Before sq s1,0x30(sp) at 0x30FE7C (func_0030FE68, flag lift_chairs): s0 = the id the switch reads.
        void liftChairsSwitch(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t id = GPR_U32(ctx, 16);
            if (id != kChairId && id != kPubSignId && id != kBarStoolId)
                return;
            g_liftSp = GPR_U32(ctx, 29);
            g_liftId = id;
            SET_GPR_U32(ctx, 16, kCrateId);
        }

        // What a new carriable is attached to: Gene's two hands (default), one hand, or the crate's carry motion.
        enum PropFollow : int { kFollowBothHands, kFollowLeftHand, kFollowRightHand, kFollowMotion };
        constexpr uint32_t kGeneLeftHand = 10u, kGeneRightHand = 16u; // each other's mirror (part +0x14C)
        uint8_t *g_ram = nullptr; // for the F2 panel (what Gene holds)

        using Mat3 = std::array<std::array<float, 3>, 3>;
        Mat3 mul(const Mat3 &a, const Mat3 &b)
        {
            Mat3 m{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
            return m;
        }
        // Row-vector rotation (p' = p R) by x, then y, then z, in radians.
        Mat3 turnMatrix(float x, float y, float z)
        {
            const float cx = std::cos(x), sx = std::sin(x), cy = std::cos(y), sy = std::sin(y), cz = std::cos(z), sz = std::sin(z);
            const Mat3 rx{{{1, 0, 0}, {0, cx, sx}, {0, -sx, cx}}};
            const Mat3 ry{{{cy, 0, -sy}, {0, 1, 0}, {sy, 0, cy}}};
            const Mat3 rz{{{cz, sz, 0}, {-sz, cz, 0}, {0, 0, 1}}};
            return mul(mul(rx, ry), rz);
        }
        Mat3 transpose(const Mat3 &a)
        {
            Mat3 m{};
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    m[r][c] = a[c][r];
            return m;
        }
        // The mean of two rotations (their quaternions summed in one hemisphere): follows both when both turn alike
        // (mean(A D, B D) = mean(A, B) D), unlike a fixed axis taken from either.
        std::array<float, 4> toQuat(const Mat3 &m)
        {
            const float tr = m[0][0] + m[1][1] + m[2][2];
            std::array<float, 4> q{};
            if (tr > 0.0f)
            {
                const float s = std::sqrt(tr + 1.0f) * 2.0f;
                q = {0.25f * s, (m[1][2] - m[2][1]) / s, (m[2][0] - m[0][2]) / s, (m[0][1] - m[1][0]) / s};
            }
            else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
            {
                const float s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
                q = {(m[1][2] - m[2][1]) / s, 0.25f * s, (m[1][0] + m[0][1]) / s, (m[2][0] + m[0][2]) / s};
            }
            else if (m[1][1] > m[2][2])
            {
                const float s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
                q = {(m[2][0] - m[0][2]) / s, (m[1][0] + m[0][1]) / s, 0.25f * s, (m[2][1] + m[1][2]) / s};
            }
            else
            {
                const float s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
                q = {(m[0][1] - m[1][0]) / s, (m[2][0] + m[0][2]) / s, (m[2][1] + m[1][2]) / s, 0.25f * s};
            }
            return q;
        }
        Mat3 fromQuat(std::array<float, 4> q)
        {
            const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            for (float &v : q)
                v /= n;
            const float w = q[0], x = q[1], y = q[2], z = q[3];
            return Mat3{{{1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w)},
                         {2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w)},
                         {2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)}}};
        }
        Mat3 meanRotation(const Mat3 &a, const Mat3 &b)
        {
            std::array<float, 4> qa = toQuat(a), qb = toQuat(b);
            const float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
            for (int i = 0; i < 4; ++i)
                qa[i] += dot < 0.0f ? -qb[i] : qb[i];
            return fromQuat(qa);
        }

        // The reference: case chair (fb14772), snapshot 240, Gene standing with the chair (carry idle E1F460), in the
        // held object's root frame (Gene's frame): each attachment's frame (rotation, origin) there. A prop's reference
        // pose (angles, middle) is taken at the same moment; its sliders read in Gene's frame from that pose.
        struct HandRef
        {
            Mat3 rotation;
            float origin[3];
        };
        const HandRef kRefBoth{{{{0.78761f, 0.21788f, 0.57636f}, {0.43872f, -0.85510f, -0.27627f}, {0.43265f, 0.47045f, -0.76908f}}},
                               {-0.2971f, 1.5561f, 0.1598f}};
        const HandRef kRefLeft{{{{0.85850f, -0.49624f, -0.12933f}, {-0.50877f, -0.79258f, -0.33610f}, {0.06428f, 0.35434f, -0.93290f}}},
                               {-0.5694f, 1.4848f, -0.1355f}};
        const HandRef kRefRight{{{{-0.12519f, 0.57068f, 0.81157f}, {0.89176f, -0.29380f, 0.34415f}, {0.43484f, 0.76682f, -0.47213f}}},
                                {-0.0247f, 1.6274f, 0.4551f}};

        // New carriables, one entry per actor id (K, Nov 3: values per item kind, set in F2 while it is held).
        // middle: the point it turns round, in its part 0's frame; refAngles (deg) / refMiddle: its pose in the
        // reference; follow / hold (m) / turn (deg) / size: the defaults K picked.
        struct PropKind
        {
            uint32_t id;
            const char *name;
            int follow;
            float hold[3];
            float turn[3];
            float size;
            float middle[3];
            float refAngles[3];
            float refMiddle[3];
        };
        const PropKind kProps[] = {
            // Stage 1 chair (om04): part 0 = the top of its backrest (1.0 m up, 0.27 m back of the seat); the crate
            // motion's pose at the reference, 0.3 m lower. K's values, Nov 3.
            {kChairId, "Chair", kFollowBothHands, {0.07f, -0.07f, -0.16f}, {-2.0f, -2.0f, 11.0f}, 1.0f, {0.0f, -0.5f, 0.27f},
             {-75.7558f, -6.0369f, -10.1902f}, {-0.40877f, 2.15203f - 0.3f, 0.41078f}},
            // Bar stool (om38, r102 x4, r600): part 0 = (-0.17, 0.63, -0.01) of its root (the seat), pieces 0.2..0.63 m up;
            // middle ~0.38 m up. Pub sign (om36, r102 x1): part 0 = (-0.38, 1.47, 0.08) (its top), ~0.8 m wide, pieces
            // 0.5..1.33 m up; middle ~0.75 m up. Both start from the chair's reference pose (cases bar_stool / pub_sign,
            // Nov 4) until K sets them.
            {kBarStoolId, "Bar stool", kFollowBothHands, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f, {0.174f, -0.246f, 0.014f},
             {-75.7558f, -6.0369f, -10.1902f}, {-0.40877f, 2.15203f - 0.3f, 0.41078f}},
            {kPubSignId, "Pub sign", kFollowBothHands, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f, {0.381f, -0.72f, -0.083f},
             {-75.7558f, -6.0369f, -10.1902f}, {-0.40877f, 2.15203f - 0.3f, 0.41078f}},
        };
        constexpr int kPropCount = static_cast<int>(sizeof(kProps) / sizeof(kProps[0]));
        struct PropLive
        {
            std::atomic<int> follow{0};
            std::atomic<float> hold[3];
            std::atomic<float> turn[3];
            std::atomic<float> size{1.0f};
        };
        PropLive g_props[kPropCount];
        void resetProp(int i)
        {
            g_props[i].follow.store(kProps[i].follow, std::memory_order_relaxed);
            g_props[i].size.store(kProps[i].size, std::memory_order_relaxed);
            for (int k = 0; k < 3; ++k)
            {
                g_props[i].hold[k].store(kProps[i].hold[k], std::memory_order_relaxed);
                g_props[i].turn[k].store(kProps[i].turn[k], std::memory_order_relaxed);
            }
        }
        int propIndex(uint32_t id)
        {
            for (int i = 0; i < kPropCount; ++i)
                if (kProps[i].id == id)
                    return i;
            return -1;
        }

        // Before ori at,at,0xCCCD at 0x1C8248 (func_001C8020, the held phase; s0 = the object, s1 = its part 0).
        void liftChairsHold(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t obj = GPR_U32(ctx, 16), part = GPR_U32(ctx, 17);
            const int pi = part ? propIndex(ram32(rdram, obj + 0x2FCu) >> 16) : -1;
            if (pi < 0)
                return; // the game's own carriables keep their carry motion
            const PropKind &kind = kProps[pi];
            PropLive &live = g_props[pi];
            // Part 0's pose: angles part +0x100 (x, y, z rad, matrix Rx Ry Rz, rebuilt after this point) and translation
            // +0xD0 -> part +0x30, in the chair's root frame (Gene's position *(+0xF0) and angles +0x100: world = local x
            // Root + pos). The crate's carry motion sets it; its sway doesn't follow Gene's hands (a big crate hides
            // that; the chair floated over his shoulder when he ran, K's video Nov 2). Attached to the hands instead:
            // the frame of both hands (their mean rotation, the point between them) or of one, taken relative to the
            // reference pose (D = Ref^T A): R = Turn Ref D, middle = origin + (Ref middle - Ref origin + offset) D.
            // Gene's parts are this frame's (he updates before MoveObj). Blended in from the motion's pose by +0x624.
            const uint32_t at = ram32(rdram, part + 0xD0u) & 0x1FFFFFFu;
            const uint32_t angles = (part + 0x100u) & 0x1FFFFFFu;
            auto vec = [&](uint32_t address, float *out) { std::memcpy(out, rdram + (address & 0x1FFFFFFu), sizeof(float) * 3); };
            float e[3], t[3], blend = 0.0f;
            vec(angles, e);
            vec(at, t);
            std::memcpy(&blend, rdram + ((obj + 0x624u) & 0x1FFFFFFu), sizeof(blend));
            constexpr float kDeg = 3.14159265f / 180.0f;
            const Mat3 turn = turnMatrix(live.turn[0].load(std::memory_order_relaxed) * kDeg,
                                         live.turn[1].load(std::memory_order_relaxed) * kDeg,
                                         live.turn[2].load(std::memory_order_relaxed) * kDeg);
            float offset[3];
            for (int k = 0; k < 3; ++k)
                offset[k] = live.hold[k].load(std::memory_order_relaxed);
            const int follow = live.follow.load(std::memory_order_relaxed);
            Mat3 r{};
            float middle[3];
            const uint32_t gene = ram32(rdram, obj + 0x644u);
            const uint32_t geneParts = gene ? ram32(rdram, gene + 0x278u) : 0u;
            if (follow == kFollowMotion || !geneParts || rdram[(gene + 0x2B4u) & 0x1FFFFFFu] <= kGeneRightHand)
            {
                // The crate's way: the motion's pose re-centred on the chair's middle, at the motion's origin + offset.
                r = mul(turn, turnMatrix(e[0], e[1], e[2]));
                for (int k = 0; k < 3; ++k)
                    middle[k] = t[k] + offset[k];
            }
            else
            {
                float pos[3], rootAngles[3];
                vec(ram32(rdram, obj + 0xF0u), pos);
                vec(obj + 0x100u, rootAngles);
                const Mat3 rootT = transpose(turnMatrix(rootAngles[0], rootAngles[1], rootAngles[2]));
                // A hand part's rotation (world +0x80 rows) and position (+0xB0) in the root frame.
                auto hand = [&](uint32_t index, Mat3 &rot, float *origin) {
                    const uint32_t p = ram32(rdram, geneParts + 4u * index) & 0x1FFFFFFu;
                    Mat3 w{};
                    float wp[3];
                    for (int row = 0; row < 3; ++row)
                        std::memcpy(w[row].data(), rdram + p + 0x80u + 0x10u * row, sizeof(float) * 3);
                    vec(p + 0xB0u, wp);
                    rot = mul(w, rootT);
                    for (int k = 0; k < 3; ++k)
                    {
                        origin[k] = 0.0f;
                        for (int j = 0; j < 3; ++j)
                            origin[k] += (wp[j] - pos[j]) * rootT[j][k];
                    }
                };
                Mat3 a{};
                float origin[3];
                const HandRef *ref = &kRefBoth;
                if (follow == kFollowLeftHand)
                {
                    hand(kGeneLeftHand, a, origin);
                    ref = &kRefLeft;
                }
                else if (follow == kFollowRightHand)
                {
                    hand(kGeneRightHand, a, origin);
                    ref = &kRefRight;
                }
                else
                {
                    Mat3 left{}, right{};
                    float lo[3], ro[3];
                    hand(kGeneLeftHand, left, lo);
                    hand(kGeneRightHand, right, ro);
                    a = meanRotation(left, right);
                    for (int k = 0; k < 3; ++k)
                        origin[k] = (lo[k] + ro[k]) * 0.5f;
                }
                const Mat3 d = mul(transpose(ref->rotation), a);
                r = mul(mul(turn, turnMatrix(kind.refAngles[0] * kDeg, kind.refAngles[1] * kDeg, kind.refAngles[2] * kDeg)), d);
                float local[3];
                for (int k = 0; k < 3; ++k)
                    local[k] = kind.refMiddle[k] - ref->origin[k] + offset[k];
                for (int k = 0; k < 3; ++k)
                {
                    middle[k] = origin[k];
                    for (int j = 0; j < 3; ++j)
                        middle[k] += local[j] * d[j][k];
                }
            }
            const float target[3] = {std::atan2(r[1][2], r[2][2]), std::asin(std::clamp(-r[0][2], -1.0f, 1.0f)),
                                     std::atan2(r[0][1], r[0][0])};
            // Size: the object's root scale (+0x110, used by cModel_calcParts' root matrix; nothing else writes it), eased
            // in by the blend from whatever it is. It scales part 0's translation too, so the middle is placed at
            // middle / size. It stays when the item is thrown (it flies and breaks at that size).
            float size = 1.0f;
            std::memcpy(&size, rdram + ((obj + 0x110u) & 0x1FFFFFFu), sizeof(size));
            if (!(size > 0.1f && size < 10.0f))
                size = 1.0f;
            size += (live.size.load(std::memory_order_relaxed) - size) * blend;
            const float sizes[3] = {size, size, size};
            std::memcpy(rdram + ((obj + 0x110u) & 0x1FFFFFFu), sizes, sizeof(sizes));
            for (int k = 0; k < 3; ++k)
            {
                float placed = middle[k] / size;
                for (int j = 0; j < 3; ++j)
                    placed -= kind.middle[j] * r[j][k];
                t[k] += (placed - t[k]) * blend;
                e[k] += std::remainder(target[k] - e[k], 2.0f * 3.14159265f) * blend;
            }
            std::memcpy(rdram + angles, e, sizeof(e));
            std::memcpy(rdram + at, t, sizeof(t));
        }

#ifdef KAMITE_HAS_GS_MENU
        // The new carriable Gene holds now (Gene +0x6A0 / +0x698, held: +0x5B0 bit 2, its parent +0x644 = Gene), or -1.
        int heldProp()
        {
            if (!g_ram)
                return -1;
            const uint32_t gene = ram32(g_ram, 0x747A94u); // GameWork +0x5F4 playerObj
            if (!gene)
                return -1;
            for (uint32_t slot : {0x6A0u, 0x698u})
            {
                const uint32_t obj = ram32(g_ram, gene + slot);
                if (obj && (ram32(g_ram, obj + 0x5B0u) & 0x4u) && ram32(g_ram, obj + 0x644u) == gene)
                    return propIndex(ram32(g_ram, obj + 0x2FCu) >> 16);
            }
            return -1;
        }

        void drawChairPanel(const Pcsx2GsUi *ui)
        {
            if (!envOn("KAMITE_LIFT_CHAIRS"))
                return;
            if (ui->version >= 4u && !ui->collapsing_header("Held items (Wild Curry Funk)", 0))
                return;
            const int pi = heldProp();
            if (pi < 0)
            {
                ui->text_disabled("Pick up a chair, bar stool or pub sign to set how it sits in Gene's hands. Each "
                                  "kind of item keeps its own values.");
                return;
            }
            PropLive &live = g_props[pi];
            char line[96];
            std::snprintf(line, sizeof(line), "Holding: %s", kProps[pi].name);
            ui->text(line);
            {
                const char *const follows[4] = {"Both hands", "Left hand", "Right hand", "Carry animation (as a crate)"};
                int follow = live.follow.load(std::memory_order_relaxed);
                ui->set_next_width(340.0f);
                if (ui->combo("Held by##propfollow", &follow, follows, 4))
                    live.follow.store(follow, std::memory_order_relaxed);
            }
            const char *names[3] = {"Sideways##propx", "Up##propy", "Forward##propz"};
            for (int i = 0; i < 3; ++i)
            {
                float v = live.hold[i].load(std::memory_order_relaxed);
                ui->set_next_width(340.0f);
                if (ui->slider_float(names[i], &v, -1.0f, 1.0f, "%.2f m"))
                    live.hold[i].store(v, std::memory_order_relaxed);
            }
            const char *turns[3] = {"Tip forward / back##proprx", "Spin##propry", "Roll##proprz"};
            for (int i = 0; i < 3; ++i)
            {
                float v = live.turn[i].load(std::memory_order_relaxed);
                ui->set_next_width(340.0f);
                if (ui->slider_float(turns[i], &v, -180.0f, 180.0f, "%.0f deg"))
                    live.turn[i].store(v, std::memory_order_relaxed);
            }
            {
                float v = live.size.load(std::memory_order_relaxed);
                ui->set_next_width(340.0f);
                if (ui->slider_float("Size##propsize", &v, 0.5f, 2.0f, "%.2fx"))
                    live.size.store(v, std::memory_order_relaxed);
            }
            if (ui->button("Reset to defaults##propreset"))
                resetProp(pi);
            ui->text_disabled("Held by: what the item is fixed to (it moves and turns with it). Move / turn: from its "
                              "starting pose, in Gene's own directions while he stands (metres / degrees). Size: how big it is "
                              "while held and thrown. The values "
                              "last until the game closes; tell Claude the ones that look right to make them the "
                              "defaults.");
        }
#endif

        // Before daddu a0,s2,zero at 0x3125B4 (the object exists): the prop's id back for +0x2FE / +0x300 / the data.
        void liftChairsId(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (g_liftSp && GPR_U32(ctx, 29) == g_liftSp && GPR_U32(ctx, 16) == kCrateId)
            {
                SET_GPR_U32(ctx, 16, g_liftId);
                g_liftSp = 0;
            }
        }

        // ---- Tension is Placable / Off the Cuff (see the top of the file) ------------------------------------------
        constexpr uint32_t kHeat = 0x5CB000u;          // cHeatSys
        constexpr uint32_t kHeatSetMode = 0x2A9CC8u;   // cHeatSys_SetHeatMode(heat, mode)
        constexpr uint32_t kHeatLv = 0x2A9BC0u;        // cHeatSys_GetHeatLv(heat)
        constexpr uint32_t kHeatLvR2Return = 0x123B08u; // its call in Gene's R2 check
        constexpr uint32_t kPlUnleashed = 0x10u;        // Gene +0x15F4
        std::atomic<bool> g_tensionPlacable{false}, g_offTheCuff{false};
        GuestFunction g_heatLv = nullptr;
        constexpr uint32_t kPartialUnleash = 0x000FFFF0u; // guest word below the ELF: 1 = this unleash began below full
        bool g_placeEnd = false; // game thread: this R2 press ends the unleash

        float ramFloat(const uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        void updateTensionFlag()
        {
            ps2xSetPatchFlag("tension", g_heatLv && g_tensionPlacable.load(std::memory_order_relaxed));
        }

        // Replaces lbu v1,0x10(a0) at 0x123AF4 (a0 = cHeatSys, s1 = Gene; R2 was just pressed).
        void tensionR2Mode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t mode = rdram[(GPR_U32(ctx, 4) + 0x10u) & 0x1FFFFFFu];
            g_placeEnd = mode != 0u && (ram32(rdram, GPR_U32(ctx, 17) + 0x15F4u) & kPlUnleashed) != 0u;
            if (g_placeEnd)
                mode = 0u;
            SET_GPR_U32(ctx, 3, mode);
        }

        // Replaces addiu v0,zero,8 at 0x123B0C (the delay slot of bnez v0 after GetHeatLv; v0 = its answer).
        void tensionR2Phase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            uint32_t phase = 8u;
            if (g_placeEnd && GPR_U32(ctx, 2) != 0u)
            {
                callGuest(rdram, ctx, runtime, kHeatSetMode, {kHeat, 0u});
                const float guard = 3.0f;
                std::memcpy(rdram + ((GPR_U32(ctx, 17) + 0x54Cu) & 0x1FFFFFFu), &guard, sizeof(guard));
                phase = 9u;
            }
            g_placeEnd = false;
            SET_GPR_U32(ctx, 2, phase);
        }

        // Replaces lbu v1,0x10(v0) at 0x109398 (func_00108AB0, v0 = cHeatSys): an Off the Cuff unleash holds no guard.
        void tensionGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t mode = rdram[(GPR_U32(ctx, 2) + 0x10u) & 0x1FFFFFFu];
            if (mode != 0u && ram32(rdram, kPartialUnleash) != 0u)
                mode = 0u;
            SET_GPR_U32(ctx, 3, mode);
        }

        // cHeatSys_GetHeatLv, wrapped: at Gene's R2 check, yes for a placable end, and with Off the Cuff for any tension.
        void heatLvWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            if (ra != kHeatLvR2Return || !g_tensionPlacable.load(std::memory_order_relaxed))
                return g_heatLv(rdram, ctx, runtime);
            if (g_placeEnd)
            {
                SET_GPR_U32(ctx, 2, 1u);
                ctx->pc = ra;
                return;
            }
            const float gauge = ramFloat(rdram, GPR_U32(ctx, 4) + 4u);
            g_heatLv(rdram, ctx, runtime); // the game's answer (and its +0x14)
            uint32_t partial = 0u;
            if (GPR_U32(ctx, 2) == 0u && g_offTheCuff.load(std::memory_order_relaxed) && gauge > 0.0f)
            {
                SET_GPR_U32(ctx, 2, 1u);
                partial = 1u;
            }
            if (GPR_U32(ctx, 2) != 0u) // R2 starts an unleash (phase 8)
                std::memcpy(rdram + kPartialUnleash, &partial, sizeof(partial));
            ctx->pc = ra;
        }

    }

    void installGameOptions(PS2Runtime &runtime)
    {
        g_ram = runtime.memory().getRDRAM();
        for (int i = 0; i < kPropCount; ++i)
            resetProp(i);
        const bool unlockHard = envOn("KAMITE_UNLOCK_HARD");
        const bool muteMusic = envOn("KAMITE_MUTE_MUSIC");
        const bool skipIntro = envOn("KAMITE_SKIP_INTRO");
        const bool rouletteAnyOrbs = envOn("KAMITE_ROULETTE_NO_ORBS");
        const bool liftChairs = envOn("KAMITE_LIFT_CHAIRS");
        ps2xRegisterHostHook("km_lift_chairs_switch", &liftChairsSwitch);
        ps2xRegisterHostHook("km_lift_chairs_id", &liftChairsId);
        ps2xRegisterHostHook("km_lift_chairs_hold", &liftChairsHold);
#ifdef KAMITE_HAS_GS_MENU
        ps2x::menu::addPanel("Gameplay", &drawChairPanel);
#endif
        ps2xSetPatchFlag("lift_chairs", liftChairs);
        ps2xSetPatchFlag("skip_intro", skipIntro);
        ps2xRegisterHostHook("km_roulette_no_orbs", &rouletteNoOrbs);
        ps2xRegisterHostHook("km_roulette_grey_no_orbs", &rouletteGreyWithoutOrbs);
        ps2xSetPatchFlag("roulette_no_orbs", rouletteAnyOrbs);
        ps2xRegisterHostHook("km_dead_pan_v0", &deadPanLanguage<2>);
        ps2xRegisterHostHook("km_dead_pan_v1", &deadPanLanguage<3>);
        ps2xRegisterHostHook("km_dead_pan_a0", &deadPanLanguage<4>);
        ps2xSetPatchFlag("dead_pan", true);
        ps2xRegisterHostHook("km_mute_music_volume", &muteMusicVolume);
        ps2xRegisterHostHook("km_tension_r2_mode", &tensionR2Mode);
        ps2xRegisterHostHook("km_tension_r2_phase", &tensionR2Phase);
        ps2xRegisterHostHook("km_tension_guard", &tensionGuard);
        g_tensionPlacable.store(envOn("KAMITE_TENSION_PLACABLE"), std::memory_order_relaxed);
        g_offTheCuff.store(envOn("KAMITE_OFF_THE_CUFF"), std::memory_order_relaxed);
        if (!runtime.hasFunction(kHeatLv) || !(g_heatLv = runtime.lookupFunction(kHeatLv)) ||
            !FunctionHooks::instance().replace(kHeatLv, "cHeatSys_GetHeatLv (Tension is Placable)", &heatLvWrapped,
                                               ReplacementStatus::Native, "R2 ends the unleash / Off the Cuff"))
        {
            g_heatLv = nullptr;
            KM_WARN(Game, "Tension is Placable: cHeatSys_GetHeatLv 0x2A9BC0 is not hookable; the option is off");
        }
        updateTensionFlag();
        ps2xSetPatchFlag("unlock_hard", unlockHard);
        ps2xSetPatchFlag("mute_music", muteMusic);
        KM_INFO(Game, "options: Hard " << (unlockHard ? "unlocked from the start" : "as saved") << "; music "
                                       << (muteMusic ? "muted" : "on") << (skipIntro ? "; boot straight to the main menu" : "")
                                       << (rouletteAnyOrbs ? "; roulette without orbs" : "")
                                       << (liftChairs ? "; chairs liftable" : "")
                                       << (g_tensionPlacable ? (g_offTheCuff ? "; tension placable, off the cuff" : "; tension placable") : ""));
    }

    bool muteMusic()
    {
        return ps2xPatchFlag("mute_music");
    }

    void setMuteMusic(bool on)
    {
        // The flag is read at every stream volume call, so the change lands at the next one (a room or track change).
        ps2xSetPatchFlag("mute_music", on);
    }

    bool tensionPlacable()
    {
        return g_tensionPlacable.load(std::memory_order_relaxed);
    }

    void setTensionPlacable(bool on)
    {
        g_tensionPlacable.store(on, std::memory_order_relaxed);
        updateTensionFlag();
        setenvFlag("KAMITE_TENSION_PLACABLE", on);
    }

    bool offTheCuff()
    {
        return g_offTheCuff.load(std::memory_order_relaxed);
    }

    void setOffTheCuff(bool on)
    {
        g_offTheCuff.store(on, std::memory_order_relaxed);
        setenvFlag("KAMITE_OFF_THE_CUFF", on);
    }
}
