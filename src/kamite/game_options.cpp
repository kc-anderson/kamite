// Optional changes to the game ([Game] UnlockHard / SkipIntro / MuteMusic / RouletteNoOrbs) and Dead Pan. See game_options.h.
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
// Dead Pan (always on): the Japanese release's free roulette move (reel 0xB; TCRF "Dead Pan / Pan Drop"). The US build
// keeps it but hides it outside Japanese: func_00125048 tests GameWork +0x56C (language, 0 = Japanese) at 0x1250A8,
// 0x1251CC, 0x125264 and 0x125338 (the PCSX2 restore patch makes all four read 0). Here they read 0 once it is earned:
// the game-clear room r022 sets 0x747A28 (GameWork +0x588) |= 0x20000000 when the Kick Me Sign is still on (cCoreSave
// ckPaper; the same clear gives the KMS jukebox CD), or with F2's "Every roulette reel owned" (DBG_ALL_GOD_REEL).
// As in Japanese it then joins the roulette without being bought, at level 3 and up (0x125338: getGameLevel >= 3).
//
// Dead Pan's invincibility shown (always on with Dead Pan; F2 > Gameplay can drop the red face): from the move's
// state (Gene phase 41, func_00113CF8: post-hit guard +0x54C held at 30) until that guard has run out after it, Gene
// gets the taunted enemies' berserk look (func_00268788, the anger state +0x16EC): the steam, SetEffect(0x58, 7, obj,
// params, 5, -1) + SetEffect(0x58, 0xF, obj, params, -1, -1) once, KillEffect(obj, 5, 2) when it ends; the red face,
// the face meshes' colour (+0x3A0/+0x3A4/+0x3A8) eased 0.1 a frame toward (0.9, 0.5, 0.5) and back to white. Gene's
// head is the mesh HD_00 (pl00.dat's scr; looked up like cModel_getMeshPtr 0x14B730: obj +0x25C list, +0x404 next,
// +0x414 -> +8 = the name packed in a u64). Driven from the frame hook (deadPanFrame), host calls as debug_tools does.
#include "kamite/game_options.h"
#include "kamite/frame_patches.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "runtime/ps2_save_state.h"

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

        // Before sb zero,0x621(s1) at 0x114904 (the roulette's cancel, flag modern_roulette): the circle that cancelled
        // must not also start the circle attack (+0x625, its 3-frame window).
        void rouletteCancelCircle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            rdram[(GPR_U32(ctx, 17) + 0x625u) & 0x1FFFFFFu] = 0u;
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
        std::atomic<bool> g_deadPanRedFace{true};
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
        if (inMove)
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
        const bool red = look.active && g_deadPanRedFace.load(std::memory_order_relaxed);
        const float before = look.fade;
        look.fade = red ? std::max(0.0f, look.fade - step) : std::min(1.0f, look.fade + step);
        if (look.fade < 1.0f || before < 1.0f)
            paintFace(rdram, gene, look.fade);
    }

    bool deadPanRedFace() { return g_deadPanRedFace.load(); }
    void setDeadPanRedFace(bool on) { g_deadPanRedFace.store(on); }

    void installGameOptions(PS2Runtime &)
    {
        const bool unlockHard = envOn("KAMITE_UNLOCK_HARD");
        const bool muteMusic = envOn("KAMITE_MUTE_MUSIC");
        const bool skipIntro = envOn("KAMITE_SKIP_INTRO");
        const bool rouletteAnyOrbs = envOn("KAMITE_ROULETTE_NO_ORBS");
        ps2xSetPatchFlag("skip_intro", skipIntro);
        ps2xRegisterHostHook("km_roulette_no_orbs", &rouletteNoOrbs);
        ps2xRegisterHostHook("km_roulette_grey_no_orbs", &rouletteGreyWithoutOrbs);
        ps2xRegisterHostHook("km_roulette_cancel_circle", &rouletteCancelCircle);
        ps2xSetPatchFlag("roulette_no_orbs", rouletteAnyOrbs);
        ps2xRegisterHostHook("km_dead_pan_v0", &deadPanLanguage<2>);
        ps2xRegisterHostHook("km_dead_pan_v1", &deadPanLanguage<3>);
        ps2xRegisterHostHook("km_dead_pan_a0", &deadPanLanguage<4>);
        ps2xSetPatchFlag("dead_pan", true);
        ps2xRegisterHostHook("km_mute_music_volume", &muteMusicVolume);
        ps2xSetPatchFlag("unlock_hard", unlockHard);
        ps2xSetPatchFlag("mute_music", muteMusic);
        KM_INFO(Game, "options: Hard " << (unlockHard ? "unlocked from the start" : "as saved") << "; music "
                                       << (muteMusic ? "muted" : "on") << (skipIntro ? "; boot straight to the main menu" : "")
                                       << (rouletteAnyOrbs ? "; roulette without orbs" : ""));
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
}
