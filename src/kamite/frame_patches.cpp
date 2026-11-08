// 60 FPS mode and L3 dash: a port of the PCSX2 patch SLUS-21503_6FB69282.pnach ("60 Fps", by Puggsy + Claude Ai +
// Gemini; K's copy, 2026-10-22). PCSX2 writes the patch into the game's code; here the game code is recompiled C++,
// so the code changes are compiled in as switchable patches (recomp/kamite.toml [[patches.conditional]] and
// [[patches.hooks]], flag "fps60") and this file holds the parts that are C++:
//
//   pnach                                         here
//   00747A09 byte 01 (every vblank)               frameStart: the game's frame divider byte = 1 (60 frames/s)
//   302D04/10D694 lui at,0x3F00                   the effect emitter's frame: replaced (Nov 1) by the effects' own
//                                                  slow-motion path at rate 0.5 (km_fps60_effect_world_slow, 20
//                                                  sites; kamite.toml "Effects at 60 FPS"); the Unleash drain is
//                                                  km_fps60_tension_drain; the pnach's 10B7B8 (the sub-screen demo's
//                                                  motion step) is left out: moveMotion halves it already
//   2C81BC hook -> F0000 (hitbox sync)            hitboxCompare replaces c.eq.s f0,f2 at 0x2C81C0: the hitbox logic
//                                                  is skipped when the integer frame (cvt.w.s) of the last and the
//                                                  current position are equal, not only the exact floats
//   15765C nop (dodge sync)                       conditional patch: the add.s in the beql delay slot is not run
//   108FF8 / 108FE8 (ours, stick dash)            conditional patches: the double-tap window 10 -> 20 frames and the
//                                                  "dash requested" flag 2 -> 4 (counted in game frames)
//   mash meter (ours, eleven pl00 states)         +0x15B0 / +0x15B8 count every other frame (countupSelf /
//                                                  countdownLoad; recomp/kamite.toml "Button-mash meter"); the
//                                                  doubled limits of 1f88879 are gone
//   1147CC sw zero,0(at) (roulette time)          conditional patch: nop (the timer store is dropped; the pnach's
//                                                  store to whatever $at holds is left out)
//   29059C lui at,0x3F00 (enemy physics x0.5)     conditional patch
//   290584 hook -> F0038 (enemy animation x2)     conditional patch: mov.s f12,f1 -> add.s f12,f1,f1 (f1 * 2)
//   30ADF4 hook -> D0020 (left stick)             frameStart: a stick axis at full deflection last frame is held
//                                                  there for one more frame (state in g_stick* instead of 0xD0200;
//                                                  without the pnach's input loop, see frameStart)
//   D05E8242 == 2: 005E8147 = 0, 005E81C7 = 0     frameStart with L3Dash: L3 writes Gene's "dash requested" (+0x627)
//   (L3 dash, every vblank)                        as the stick's double tap does; a centred stick is pushed forward
//
// The frame hook runs before the instruction at 0x30ADF4 (lw v0,0x3D84(s3)), right after the main loop's wait for the
// next frame (0x30ADD8..0x30ADF0), where the pnach's stick hook ran; the per-vblank writes run there too, once per
// game frame.
#include "kamite/frame_patches.h"

#include <cmath>
#include "kamite/character_mods.h"
#include "kamite/hud_layout.h"
#include "kamite/npc_poses.h"
#include "kamite/debug_tools.h"
#include "kamite/npc_tools.h"
#include "kamite/sprint_traversal.h"
#include "kamite/hitbox_overlay.h"
#include "kamite/bone_tweaks.h"
#include "kamite/call_profile.h"
#include "kamite/counter_finder.h"
#include "kamite/test_mode.h"
#include "kamite/game_options.h"
#include "kamite/function_hooks.h"
#include "kamite/pc_sampler.h"
#include "kamite/log.h"
#include "kamite/game_layout.h"
#include "ps2_runtime_macros.h" // FPU_CVT_W_S (the recompiled CVT.W.S)
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_save_state.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace kamite
{
    namespace
    {
        constexpr uint32_t kFrameDivider = game::GameWork::address(game::GameWork::kVblankDivider); // GameWork.unk569: 1 = 60 frames/s
        constexpr uint32_t kStickBytes[4] = {0x5E8146u, 0x5E8147u, 0x5E81C6u, 0x5E81C7u}; // left stick X/Y, two pads
        constexpr uint32_t kPadButtons = 0x5E8242u;                                        // 16-bit; 2 = L3
        bool g_fps60 = false;     // the mode in effect this frame (the patch flag, the divider)
        bool g_fps60User = false; // the player's choice (Fps60, F2); off for the frames of a cutscene (frameStart)
        std::atomic<int> g_fps60Request{-1};   // F2 menu: 0 / 1, applied at the next frame start (game thread)
        std::atomic<bool> g_fps60Shown{false}; // for the F2 menu
        std::atomic<bool> g_l3Dash{false};
        std::atomic<bool> g_rouletteSlow{false}; // [Game] RouletteTimer=double
        std::atomic<int> g_rouletteRequest{0};   // F2 menu: the period wants working out again
        uint8_t g_stickHeld[4] = {}; // the axis was at 0 or 0xFF last frame
        uint8_t g_stickLast[4] = {};
        int g_stickInjected[4] = {-1, -1, -1, -1}; // the value written over the game's last frame (-1: none)
        uint8_t g_stickRaw[4] = {0x80u, 0x80u, 0x80u, 0x80u};
        uint32_t g_staleRestores = 0, g_holds = 0, g_frames = 0;
        uint64_t g_gameFrame = 0; // every frame hook; at 60 frames/s the odd ones are the extra frames

        bool envOn(const char *name)
        {
            const char *value = std::getenv(name);
            return value && *value && *value != '0';
        }

        uint8_t &ram8(uint8_t *rdram, uint32_t address)
        {
            return rdram[address & (PS2_RAM_SIZE - 1u)];
        }

        // The game's own frame counter (GameWork +0x210, +1 each main loop): it lives in RAM, so a save state carries
        // it. The 60 FPS hooks take their every-other-frame cadence from it, not from a host counter: with g_gameFrame a
        // state saved on one parity and loaded on the other ran every hook a frame out of step (a test case replayed
        // unfaithfully at 3.85 s on +0x1603, case 2026-10-29_15-45-23).
        uint32_t gameFrames(uint8_t *rdram)
        {
            uint32_t value = 0;
            std::memcpy(&value, &ram8(rdram, game::GameWork::address(game::GameWork::kFrameCount)), sizeof(value));
            return value;
        }
        // The second half of a 30 FPS frame (frame30 grid below): countdowns step on first halves only, and the step
        // holds (enemyStepHold) hold handlers on second halves only, so a held vblank can never eat a countdown's step.
        // (Until Nov 1 this was the counter's own parity, a grid of its own: when the two disagreed, a held handler
        // skipped the frame its countdowns were due to step.)
        bool thinkSkipped(uint8_t *rdram);
        bool skipFrame(uint8_t *rdram) { return thinkSkipped(rdram); }

        // KAMITE_CALL_PROFILE=1 with KAMITE_CALL_PROFILE_EVERY=<game frames>: a call count report of every function
        // called in each window of that many frames (finding the code behind something seen on screen: compare the
        // windows where it shows with the others).
        void profileWindow()
        {
            static const uint32_t every = []
            {
                const char *value = std::getenv("KAMITE_CALL_PROFILE_EVERY");
                return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
            }();
            static uint64_t frames = 0, window = 0;
            if (every == 0u || ++frames % every != 0u)
                return;
            KM_INFO(Game, "call profile window " << ++window << " (game frames " << frames - every << ".." << frames << ")");
            callprofile::report(100000);
        }

        // L3: a dash, the game's own way (Nov 2, K: the old double-tap macro felt less responsive than tapping the stick).
        // The stick's second forward tap only writes Gene +0x627 ("dash requested", 2 at 30, 4 in the 60 FPS mode:
        // the setter at 0x108FE8), and the action chooser (0x124104) turns a non-zero +0x627 into phase 6, the dash.
        // The macro tapped neutral / forward / neutral / forward, 2 frames each: the dash came 5-6 frames after L3 at
        // 30 (11-13 vblanks at 60; case L3_unresponsive) and Gene stopped for the neutral frames when already
        // running. Now L3 going down writes +0x627 itself and keeps it up for the game's tap window (10 frames) until
        // Gene is in phase 6, so a press during a move's last frames still dashes once he can. The stick stays the
        // player's; with the stick centred, L3 pushes it forward while held (a dash straight ahead, as before).
        uint32_t g_dashFrames = 0; // frames since L3 went down (0 = not held)
        bool g_dashArmed = false;
        // A stick value written over the game's: the reading is kept and the next frame puts it back.
        void injectStick(uint8_t *rdram, int axis, uint8_t value)
        {
            if (g_stickInjected[axis] < 0)
                g_stickRaw[axis] = ram8(rdram, kStickBytes[axis]);
            ram8(rdram, kStickBytes[axis]) = value;
            g_stickInjected[axis] = value;
        }

        // At 30 frames/s (the 60 FPS stick hold does this itself): stick values written last frame that the game did
        // not replace go back to the reading.
        void restoreInjected(uint8_t *rdram)
        {
            for (int axis = 0; axis < 4; ++axis)
            {
                if (g_stickInjected[axis] >= 0 && ram8(rdram, kStickBytes[axis]) == static_cast<uint8_t>(g_stickInjected[axis]))
                    ram8(rdram, kStickBytes[axis]) = g_stickRaw[axis];
                g_stickInjected[axis] = -1;
            }
        }

        // The in-game roulette (the move wheel in a fight): its list moves only with the left stick; while it is up, the
        // d-pad's up and down push the stick too. Seen through cCockRoulette move's dial step (0x145308, only while the
        // wheel is up), stamped with the game frame.
        uint64_t g_rouletteSeen = 0;
        bool g_rouletteBack = false; // the last wheel step went up (see rouletteSlideBegin)

        void rouletteSeen(uint8_t *, R5900Context *, PS2Runtime *) { g_rouletteSeen = g_gameFrame; }

        // Replaces addiu v1,v1,1 at 0x1458CC (cCockRoulette, the wheel's move: any of its command bits advances the
        // cursor +0x422 by one, wrapping at the count in $a0). Pushing up (stick or d-pad) goes back one instead, so
        // an overshot move is one step away, not a whole lap.
        // The direction comes from the command bit that made the step (GameWork +0x1A0, pressed: 49 = left stick up,
        // the pad code's Y < -0x50; 48 = down; 40 = R1), not from the stick as it is now: a flick up read a stick already
        // on its way back (the live byte past 0x40) and stepped down. The d-pad pushes the stick (rouletteDpad).
        void rouletteStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t cursor = GPR_S32(ctx, 3), count = GPR_S32(ctx, 4);
            uint16_t buttons = 0;
            std::memcpy(&buttons, &ram8(rdram, kPadButtons), sizeof(buttons));
            uint64_t pressed = 0;
            std::memcpy(&pressed, &ram8(rdram, 0x7474A0u + 0x1A0u), sizeof(pressed));
            const bool up = (pressed & (1ull << 49)) != 0u || ((pressed & (1ull << 48)) == 0u && (buttons & 0x10u) != 0u);
            g_rouletteBack = up && count > 1;
            SET_GPR_S32(ctx, 3, up && count > 0 ? (cursor + count - 1) % count : cursor + 1);
        }

        // The wheel's slide (the rest of that function: progress +0x338 from 1.0 to 0 and a 32.0 offset +0x2A4, both
        // x0.7 a frame) only knows forward steps. A back step to c is drawn as the forward step c -> c+1 played
        // backwards: cursor c+1 and progress 1-p while the slide runs, then put back after the slots are placed.
        bool g_rouletteSwapped = false;
        uint16_t g_rouletteSavedCursor = 0;
        float g_rouletteSavedP = 0.0f, g_rouletteSavedA = 0.0f;

        float ramF32(uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, &ram8(rdram, address), sizeof(value));
            return value;
        }

        void setRamF32(uint8_t *rdram, uint32_t address, float value) { std::memcpy(&ram8(rdram, address), &value, sizeof(value)); }
        bool isGene(uint8_t *rdram, uint32_t object);
        void forgetClothAfterLoad(uint8_t *rdram);
        bool inEnemyList(uint8_t *rdram, uint32_t obj);

        // Before lwc1 f0,0x2A4(s1) at 0x1458E8 (after the step), s1 = the wheel.
        void rouletteSlideBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_rouletteSwapped = false;
            const uint32_t wheel = GPR_U32(ctx, 17);
            const float p = ramF32(rdram, wheel + 0x338u), a = ramF32(rdram, wheel + 0x2A4u);
            if (!g_rouletteBack || !(a > 1.0f))
                return;
            int16_t count = 0;
            uint16_t cursor = 0;
            std::memcpy(&count, &ram8(rdram, wheel + 0x3B2u), sizeof(count));
            std::memcpy(&cursor, &ram8(rdram, wheel + 0x422u), sizeof(cursor));
            if (count <= 1)
                return;
            g_rouletteSavedCursor = cursor;
            g_rouletteSavedP = p;
            g_rouletteSavedA = a;
            g_rouletteSwapped = true;
            const uint16_t next = static_cast<uint16_t>((cursor + 1) % count);
            const float back = 1.0f - p;
            std::memcpy(&ram8(rdram, wheel + 0x422u), &next, sizeof(next));
            setRamF32(rdram, wheel + 0x338u, back);
            setRamF32(rdram, wheel + 0x2A4u, 32.0f * back);
            // The incoming slot (+0xE4) is hidden (+0x2C bit 8) whenever the wheel is at rest; same for the reversed
            // progress, so the first frame shows the old wheel exactly.
            uint32_t incoming = 0, flags = 0;
            std::memcpy(&incoming, &ram8(rdram, wheel + 0xE4u), sizeof(incoming));
            if (incoming == 0u)
                return;
            std::memcpy(&flags, &ram8(rdram, incoming + 0x2Cu), sizeof(flags));
            flags = 32.0f * back > 1.0f ? (flags & ~8u) : (flags | 8u);
            std::memcpy(&ram8(rdram, incoming + 0x2Cu), &flags, sizeof(flags));
        }

        // Before lw v0,0x90(s1) at 0x145C88 (after the slot loop).
        void rouletteSlideEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_rouletteSwapped)
                return;
            g_rouletteSwapped = false;
            const uint32_t wheel = GPR_U32(ctx, 17);
            std::memcpy(&ram8(rdram, wheel + 0x422u), &g_rouletteSavedCursor, sizeof(uint16_t));
            setRamF32(rdram, wheel + 0x338u, g_rouletteSavedP);
            setRamF32(rdram, wheel + 0x2A4u, g_rouletteSavedA);
        }

        void rouletteDpad(uint8_t *rdram)
        {
            if (g_rouletteSeen == 0u || g_gameFrame - g_rouletteSeen > 2u)
                return;
            uint16_t buttons = 0;
            std::memcpy(&buttons, &ram8(rdram, kPadButtons), sizeof(buttons));
            const bool up = (buttons & 0x10u) != 0u, down = (buttons & 0x40u) != 0u;
            if (up == down)
                return;
            injectStick(rdram, 1, up ? 0x00u : 0xFFu);
            injectStick(rdram, 3, up ? 0x00u : 0xFFu);
        }

        void l3Dash(uint8_t *rdram)
        {
            if (!g_l3Dash.load(std::memory_order_relaxed))
                return;
            uint16_t buttons = 0;
            std::memcpy(&buttons, &ram8(rdram, kPadButtons), sizeof(buttons));
            if ((buttons & 2u) == 0u)
            {
                g_dashFrames = 0;
                g_dashArmed = false;
                return;
            }
            const uint32_t perFrame = g_fps60 ? 2u : 1u; // game frames per 30 FPS frame
            if (g_dashFrames++ == 0u)
                g_dashArmed = true;
            uint32_t gene = 0;
            std::memcpy(&gene, &ram8(rdram, game::GameWork::address(game::GameWork::kPlayerObj)), sizeof(gene));
            if (gene >= 0x100000u && gene < PS2_RAM_SIZE - 0x1000u && g_dashArmed)
            {
                if (ram8(rdram, gene + 0x2F5u) == 6u || g_dashFrames > 10u * perFrame)
                    g_dashArmed = false;
                else if (static_cast<int8_t>(ram8(rdram, gene + 0x627u)) < static_cast<int8_t>(2u * perFrame))
                    ram8(rdram, gene + 0x627u) = static_cast<uint8_t>(2u * perFrame);
            }
            const uint8_t x = ram8(rdram, kStickBytes[0]), y = ram8(rdram, kStickBytes[1]);
            const auto centred = [](uint8_t v) { return v >= 0x60u && v <= 0xA0u; };
            if (centred(x) && centred(y))
            {
                injectStick(rdram, 1, 0x00u);
                injectStick(rdram, 3, 0x00u);
            }
        }

        // The roulette's time: one count every g_roulettePeriod game frames. 1 at 30 frames/s, 2 at 60 (the PS2's
        // time); [Game] RouletteTimer=double doubles it (2 at 30, 4 at 60), for the timer and the time to choose.
        std::atomic<uint32_t> g_roulettePeriod{1};

        void applyRoulettePeriod()
        {
            const uint32_t period = (g_fps60 ? 2u : 1u) * (g_rouletteSlow.load() ? 2u : 1u);
            g_roulettePeriod.store(period);
            ps2xSetPatchFlag("roulette_timer", period > 1u);
        }

        // The 60 FPS mode on or off, between two game frames: the patch bits (every code change of the mode is a
        // runtime flag the recompiled code tests at each patched instruction), the VU1 timing, the roulette period
        // and the stick-hold state. The frame divider byte follows at every frame start (below), in both modes.
        void setFps60(bool on, const char *why = nullptr)
        {
            if (on == g_fps60)
                return;
            g_fps60 = on;
            ps2xSetPatchFlag("fps60", on);
            ps2x::asyncvif::setVu1TimingScale(on ? 0.5 : 1.0);
            applyRoulettePeriod();
            for (int axis = 0; axis < 4; ++axis)
            {
                g_stickHeld[axis] = 0u;
                g_stickLast[axis] = 0u;
            }
            KM_INFO(Game, "frame rate switched: " << (on ? "60 FPS mode" : "30 FPS, as on the PS2") << (why ? why : ""));
        }

        // Random rolls made every frame (claude/fps60-notes.md family 8). The game draws from one Mersenne twister
        // (func_30F348) through irand 0x31CFE0 (u32), frand 0x31D000 (0..1) and 0x31D020 (frand - 0.5). Hundreds of AI
        // tests roll a chance once a game frame while they wait (`irand() & 15` == 0 -> attack now, `frand() < p`), so at
        // 60 they fired twice as often per second. The rule, exact for both kinds of roll and blind to how the caller
        // reads the number: a roll at the same call site for the same object on two game frames in a row takes the first
        // frame's number again (its outcome repeats), then the next frame draws a fresh one. A roll made every frame thus
        // gets 30 fresh numbers a second as at 30; a one-off roll (a random wait, a choice on entering a state) never has
        // a roll on the frame before and stays fresh. Scope: rolls from Gene's states, the objects and the common enemy
        // code (randSite); the object is the one MoveObj is updating (0 elsewhere: the k-th roll of the site that frame).
        // The table goes into save states ("kamite.rand60"), so test cases replay it exactly; 60 FPS only (flag fps60).
        struct RandSlot
        {
            uint32_t frame = 0, count = 0, fresh = 0, prevFrame = 0, prevCount = 0, prevFresh = 0;
            uint32_t value[4] = {}, prevValue[4] = {};
        };
        constexpr size_t kRandSlotWords = 14;
        std::unordered_map<uint64_t, RandSlot> g_rand;
        uint32_t g_randObject = 0;
        bool g_randRestored = false;

        bool randSite(uint32_t ra)
        {
            return (ra >= 0x108000u && ra < 0x128000u) || (ra >= 0x160000u && ra < 0x1D0000u) || (ra >= 0x208000u && ra < 0x290000u);
        }

        uint32_t randPaired(uint8_t *rdram, uint32_t site, uint32_t drawn)
        {
            const uint32_t frame = gameFrames(rdram);
            RandSlot &slot = g_rand[(static_cast<uint64_t>(site) << 32) | g_randObject];
            if (slot.frame != frame || slot.count == 0u)
            {
                if (slot.frame != frame)
                {
                    slot.prevFrame = slot.frame;
                    slot.prevCount = slot.count;
                    slot.prevFresh = slot.fresh;
                    std::memcpy(slot.prevValue, slot.value, sizeof(slot.value));
                }
                slot.frame = frame;
                slot.count = 0;
                slot.fresh = 0;
            }
            const uint32_t index = slot.count++;
            if (index >= 4u)
                return drawn;
            uint32_t value = drawn;
            if (slot.prevFrame + 1u == frame && index < slot.prevCount && ((slot.prevFresh >> index) & 1u) != 0u)
                value = slot.prevValue[index];
            else
                slot.fresh |= 1u << index;
            slot.value[index] = value;
            return value;
        }

        uint32_t callerRa(uint8_t *rdram, R5900Context *ctx) // the wrappers keep the caller's ra at 0(sp)
        {
            uint32_t ra = 0;
            std::memcpy(&ra, &ram8(rdram, GPR_U32(ctx, 29)), sizeof(ra));
            return ra;
        }
        // Every roll (the hooks are always on): a case's roll track first (test_mode: a replay at the other frame rate
        // takes the recorded value, a recording keeps the value the game gets), the 60 FPS pairing between.
        uint32_t rollValue(uint8_t *rdram, uint32_t ra, uint32_t drawn)
        {
            uint32_t value = test_mode::rollBefore(ra, g_randObject, drawn);
            if (ram8(rdram, game::GameWork::address(game::GameWork::kVblankDivider)) == 1u && randSite(ra))
                value = randPaired(rdram, ra, value);
            test_mode::rollAfter(value);
            return value;
        }
        void randInt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // before ld ra at 0x31CFF0 (v0 = the number)
        {
            SET_GPR_U32(ctx, 2, rollValue(rdram, callerRa(rdram, ctx), GPR_U32(ctx, 2)));
        }
        void randFloat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // 0x31D010 / 0x31D030 (f0 = frand)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &ctx->f[0], sizeof(bits));
            bits = rollValue(rdram, callerRa(rdram, ctx), bits);
            std::memcpy(&ctx->f[0], &bits, sizeof(bits));
        }
        void randObjectBegin(uint8_t *, R5900Context *ctx, PS2Runtime *) { g_randObject = GPR_U32(ctx, 5); } // MoveObj 0x30F160
        void randObjectEnd(uint8_t *, R5900Context *, PS2Runtime *) { g_randObject = 0; }                     // MoveObj 0x30F16C

        void forgetRandAfterLoad(uint8_t *rdram)
        {
            static uint32_t loads = 0;
            const uint32_t now = ps2x::savestate::loadCount();
            if (now != loads)
            {
                loads = now;
                if (!g_randRestored)
                    g_rand.clear(); // a state from before this table (or without it)
                g_randRestored = false;
                g_randObject = 0;
            }
            const uint32_t frame = gameFrames(rdram);
            if ((frame & 0x3FFu) == 0u)
                for (auto it = g_rand.begin(); it != g_rand.end();)
                    it = (frame - it->second.frame > 2u) ? g_rand.erase(it) : std::next(it);
        }

        void installRandState()
        {
            ps2x::savestate::registerExtension(
                "kamite.rand60",
                [](ps2x::state::Writer &out) {
                    uint32_t version = 1u;
                    std::vector<uint32_t> words;
                    words.reserve(g_rand.size() * (kRandSlotWords + 2u));
                    for (const auto &[key, slot] : g_rand)
                    {
                        words.push_back(static_cast<uint32_t>(key >> 32));
                        words.push_back(static_cast<uint32_t>(key));
                        const uint32_t head[6] = {slot.frame, slot.count, slot.fresh, slot.prevFrame, slot.prevCount, slot.prevFresh};
                        words.insert(words.end(), head, head + 6);
                        words.insert(words.end(), slot.value, slot.value + 4);
                        words.insert(words.end(), slot.prevValue, slot.prevValue + 4);
                    }
                    ps2x::state::fields(out, version, words);
                },
                [](ps2x::state::Reader &in) {
                    uint32_t version = 0u;
                    std::vector<uint32_t> words;
                    ps2x::state::fields(in, version, words);
                    g_rand.clear();
                    for (size_t i = 0; i + kRandSlotWords + 2u <= words.size(); i += kRandSlotWords + 2u)
                    {
                        RandSlot slot;
                        const uint32_t *w = &words[i + 2u];
                        slot.frame = w[0], slot.count = w[1], slot.fresh = w[2], slot.prevFrame = w[3], slot.prevCount = w[4], slot.prevFresh = w[5];
                        std::memcpy(slot.value, w + 6, sizeof(slot.value));
                        std::memcpy(slot.prevValue, w + 10, sizeof(slot.prevValue));
                        g_rand[(static_cast<uint64_t>(words[i]) << 32) | words[i + 1u]] = slot;
                    }
                    g_randRestored = true;
                });
        }

        // A save state was loaded: the stick hold's memory belongs to the frames before it. Without this the hold
        // compared the loaded stick bytes with what it saw before the load (at boot, before the pad was up, the
        // libpad buffer reads 0 = full deflection) and wrote a full push over the first frame after the load: at 60
        // FPS Gene turned or dashed by himself right after a state loaded (the first test-case replays, Oct 29).
        void forgetStickAfterLoad()
        {
            static uint32_t loads = 0;
            const uint32_t now = ps2x::savestate::loadCount();
            if (now == loads)
                return;
            loads = now;
            for (int axis = 0; axis < 4; ++axis)
            {
                g_stickHeld[axis] = 0u;
                g_stickLast[axis] = 0u;
                g_stickInjected[axis] = -1;
                g_stickRaw[axis] = 0x80u;
            }
        }

        // The rate changed under the game: the mode switched (frameStart, before the game's frame) or a save state from
        // the other rate loaded (right after the load: the game resumes before the next frameStart, so the first frame
        // ran with the state's divider and its cEmManage rate, enemy motions x4 at 60 / x1/4 at 30; 66sec and goon
        // cases, Oct 31). Writes the divider and converts cEmManage's rate +0x548 (1.0 at 30, 0.5 at 60: the 0x29059C
        // patch; a slow-motion value halved at 60), the source of every enemy's motion step and speed rate.
        // The enemies' staggered thinking (16 sites in the common enemy code: func_0020FBB8, cEm00's patrol, ...): a test
        // runs only when GameWork +0x210 & 7 is the enemy's slot (+0x17D0, cEm00 +0x2FC), once every 8 frames. At 60
        // that counter counts 60 FPS frames, so every enemy thought twice as often, at other moments than at 30 (66sec
        // case, Oct 31: both goons reacted to Gene 2-4 vblanks early and the fight parted from there). Replaces the
        // counter load: every other frame a value no slot test of this enemy matches, otherwise the 30 FPS frame count,
        // kept across rate changes and in save states ("kamite.frame30") so a state thinks on the same frames at both.
        uint32_t g_frame30Base = 0, g_counterBase = 0, g_frame30Saved = 0, g_frame30Half = 0;
        bool g_frame30Loaded = false;
        uint8_t *g_rdram = nullptr;
        bool thinkSkipped(uint8_t *rdram) { return ((gameFrames(rdram) - g_counterBase) & 1u) != 0u; }
        uint32_t frame30(uint8_t *rdram) { return g_frame30Base + ((gameFrames(rdram) - g_counterBase) >> 1); }
        template <int Dst, int Slot>
        void thinkFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, Dst, thinkSkipped(rdram) ? GPR_U32(ctx, Slot) + 1u : frame30(rdram));
        }
        // An enemy's state change (mode / phase / step, +0x2F4 word) takes effect the next game frame: at 30 that is the
        // next 30 FPS frame, at 60 the next vblank, so every change made in the first half of a 30 FPS frame ran the new
        // step a vblank early and the enemy's sequences drifted ahead (66sec case, Oct 31: +1 vblank per change, up to 6
        // ahead between hits). On both ways into the mode-0 phase handler's jalr (func_0020F3C0 0x20FAE8 / 0x20FAEC;
        // s1 = the enemy; a hook on the jalr itself recompiles to a jump to itself): in the second half of a 30 FPS
        // frame, after a change seen since the first half, the handler is held for that vblank (a2 = NoOp_100318) as
        // the 30 FPS game would.
        struct EnemyStep
        {
            uint32_t state = 0, counter = 0, motion = 0;
        };
        std::unordered_map<uint32_t, EnemyStep> g_enemyStep;
        // A guest function run to its end on a copy of the registers (game_options.cpp callGuest).
        void callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address, uint32_t a0, float f12)
        {
            if (!runtime || !runtime->hasFunction(address))
                return;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return;
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 4, a0);
            ctx->f[12] = f12;
            ctx->pc = address; // a hooked function finds its hook by the pc (game_options.cpp callGuest)
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            *ctx = saved;
        }
        // The held vblank is the second half of the 30 FPS frame in which the old step ran its last frame: the current
        // motion (the old one, or the one the change set) goes on (moveMotion 0x157C18, cObjBase_addNullSpeed_Rotation
        // 0x14F9F0 and cObjBase_addNullSpeed 0x14F928 at 1.0, as the walking and idle steps do every frame). Without it each change lost half a 30 FPS frame of root motion (66sec case: the grab
        // lunge ended 0.1 m short, the grab put Gene 0.19 m off, and the other goon chose otherwise from there).
        // The same hold on the hit reactions' dispatchers (Nov 1; the juggle case: the launch ran a vblank ahead of
        // the 30 FPS run): mode 1 func_0024C0F0 (jalr a2 at 0x24C370: 0x24C368 / 0x24C36C) and mode 2 func_0025E600
        // (jalr a2 at 0x25E770: 0x25E768 / 0x25E76C), the enemy in s0. g_enemyStep is shared, so a mode change (the
        // hit itself, mode 0 -> 1) counts as a change wherever the next frame's dispatcher runs.
        // Gene's state dispatcher func_0010B6B0 (jalr a3 at 0x10B748: 0x10B740 / 0x10B744, Gene in a2), Nov 1: each move
        // of a mashed string starts with a one-frame step 0, 2 vblanks at 30 and 1 at 60, so his strings ran ~10%
        // faster at 60 (statue case 2026-11-01_15-41-58: 42 vblanks ahead after 26 moves) and every change came a
        // vblank early. Same hold; and his handlers read the pad's pressed edge (GameWork +0x08: the action chooser
        // func_00123938 from 62 states, the mash states), which exists on one vblank only, so a press on a held vblank
        // is carried into the next one instead of being lost.
        uint32_t g_genePressCarry = 0, g_genePressAt = 0;
        // Gene's slow-motion states call cEmManage_SetSpeedRate every frame (the roulette moves 0.1 / 0.05, the dodge's
        // slow-mo; the manager's +0x548 and the effect / object rates fall back to normal every frame). The held vblank
        // skips his handler, so the world ran one vblank at full speed after each such state change (Nov 1, case
        // shockwave30: the enemies' rate 0.05, 0.5, 0.05 as Shockwave began). The hold repeats the last call.
        uint32_t g_slowRateAt = 0, g_slowRateManager = 0;
        float g_slowRateValue = 1.0f;
        // What the tick's handler moved (Nov 2, 66sec: Gene 0.05 m off the 30 FPS run from his first attack on). The
        // held vblank repeats the motion calls the handler made on the tick, with their f12, and only those: handlers
        // differ (moveMotion alone at 0x10DAC0, addNullSpeed at 1.5 at 0x10DBCC). At 30 the one-frame step 0 of
        // DB2FA0 steps its motion but applies no root motion; the hold's unconditional addNullSpeed moved Gene back
        // 0.048 m. Recorded by hooks on the three functions' entries while a dispatcher's handler runs on a tick
        // (handlerBegin in enemyStepHold, handlerEnd after the jalr returns); a held vblank with no record (the first
        // after a load) keeps the old moveMotion + both at 1.0. For k: the same calls on each of the k-1 held frames.
        // The same for the button-action requests a handler makes every frame (Nov 2, K: the pickup prompt flickers
        // as Gene walks up to an object, 60 only): func_1F7858 / func_1F7900 put an entry in the list at 0x568288
        // (button, message, the callback run on the press: the "B :Carry" prompt from the action chooser 0x124308 ->
        // func_2DBD40 / func_2DBFB0), and func_1F75D0 releases every entry each frame. On a held vblank the handler
        // didn't run, so the prompt (and a press on that vblank) was gone for a vblank after every state change. The
        // hold makes the tick's requests again, with the same a0..t2. Enemy handlers' requests (0x237940.., the
        // grab prompts) go the same way. And the body switches (Nov 3, T1's lunge cases): cCollisionSolidManage_SetActive
        // (0x1351D8) from a handler turns its owner's body spheres off for the frame (lunges, grabs: 0x239D84 ..), after
        // the update turned them on (Gene 0x1092D8, enemies 0x20952C); the body push now runs on the second half
        // (bodyPush below), which a held handler left with the body on.
        struct ActionRequest
        {
            uint32_t fn = 0;
            uint32_t args[7] = {}; // a0..a3, t0..t2
        };
        struct HandlerMoves
        {
            uint32_t counter = 0;
            uint8_t move = 0, rot = 0, null = 0, requests = 0;
            float rotF12 = 1.0f, nullF12 = 1.0f;
            ActionRequest request[6];
            bool stepped = false;   // handlerEnd saw the tick's handler return: motion / step / rate below
            uint32_t motion = 0;    // +0x428 as the handler left it
            float step = 1.0f;      // +0x444 (SetMotionStep)
            float rate = 1.0f;      // +0x5A8
        };
        std::unordered_map<uint32_t, HandlerMoves> g_handlerMoves;
        uint32_t g_handlerStack[8] = {};
        int g_handlerDepth = 0;
        bool g_holdCalling = false;
        HandlerMoves *tickMoves(uint8_t *rdram, uint32_t obj)
        {
            if (g_holdCalling || g_handlerDepth <= 0 || obj == 0u || g_handlerStack[g_handlerDepth - 1] != obj)
                return nullptr;
            auto it = g_handlerMoves.find(obj);
            return it != g_handlerMoves.end() && it->second.counter == gameFrames(rdram) ? &it->second : nullptr;
        }
        void moveMotionCalled(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (HandlerMoves *m = tickMoves(rdram, GPR_U32(ctx, 4)))
                m->move = static_cast<uint8_t>(std::min(m->move + 1, 3));
        }
        void nullRotationCalled(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (HandlerMoves *m = tickMoves(rdram, GPR_U32(ctx, 4)))
                m->rot = static_cast<uint8_t>(std::min(m->rot + 1, 3)), m->rotF12 = ctx->f[12];
        }
        void nullSpeedCalled(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (HandlerMoves *m = tickMoves(rdram, GPR_U32(ctx, 4)))
                m->null = static_cast<uint8_t>(std::min(m->null + 1, 3)), m->nullF12 = ctx->f[12];
        }
        template <uint32_t Fn>
        void actionRequested(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_holdCalling || g_handlerDepth <= 0 || g_handlerStack[g_handlerDepth - 1] == 0u)
                return;
            auto it = g_handlerMoves.find(g_handlerStack[g_handlerDepth - 1]);
            if (it == g_handlerMoves.end() || it->second.counter != gameFrames(rdram) || it->second.requests >= 6u)
                return;
            ActionRequest &r = it->second.request[it->second.requests++];
            r.fn = Fn;
            for (int i = 0; i < 7; ++i)
                r.args[i] = GPR_U32(ctx, 4 + i);
        }
        void callRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const ActionRequest &r)
        {
            if (!runtime || !runtime->hasFunction(r.fn))
                return;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(r.fn);
            if (!fn)
                return;
            const R5900Context saved = *ctx;
            for (int i = 0; i < 7; ++i)
                SET_GPR_U32(ctx, 4 + i, r.args[i]);
            ctx->pc = r.fn; // a hooked function finds its hook by the pc (game_options.cpp callGuest)
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            *ctx = saved;
        }
        // Gene's per-frame flags +0x15F4 (cleared every frame before his handler; the action chooser sets 0x400 and 0x100
        // at 0x124108 / 0x1242BC, "an action is available"): the action button display (the "B :Carry" prompt) needs
        // them, so on a held vblank the prompt vanished (Nov 2, K's video: off / on / off at each state change next to a
        // pickup since d834476; case 2026-11-02_11-38-52 frames 145 and 147: 0x520 -> 0x20). The hold sets the bits the
        // tick's handler added.
        uint32_t g_geneFlagsObj = 0, g_geneFlagsAt = 0, g_geneFlagsBefore = 0, g_geneFlagsAdded = 0;
        constexpr uint32_t kGeneFrameFlags = 0x15F4u;
        // The enemies' per-frame flags (Nov 3, T1's "Female enemy grab (bitchslap attack)"): their update clears these
        // bits of +0x16D0 / +0x16D4 every frame before the handler (func_00208BE0 0x209038..0x209068, masks 0x77288B82
        // / 0xFFB1DBBB), and handlers set the ones they want for that frame. The slap grab (phase 88 func_00237F98)
        // lowers her by (scale - 1) x 1.6 in step 0 and sets +0x16D0 bit 0x10000 every frame ("no floor snap", read at
        // 0x209CA0 before func_001329C0): a held vblank after step 0 left the bit clear, the floor check put her back
        // up 0.24 m and her hand closed above Gene's head for the whole grab. The hold sets the bits the tick's
        // handler added, within the per-frame masks only.
        struct EnemyFlags
        {
            uint32_t counter = 0, before[2] = {}, added[2] = {};
        };
        std::unordered_map<uint32_t, EnemyFlags> g_enemyFlags;
        constexpr uint32_t kEnemyFrameFlags[2] = {0x16D0u, 0x16D4u};
        constexpr uint32_t kEnemyFrameMask[2] = {~0x77288B82u, ~0xFFB1DBBBu}; // the bits the update clears every frame
        // After a dispatcher's jalr returns (0x10B750, 0x20FAF8, 0x24C37C, 0x25E778).
        void handlerEnd(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            if (g_handlerDepth <= 0)
                return;
            const uint32_t obj = g_handlerStack[--g_handlerDepth];
            if (obj != 0u)
            {
                auto mv = g_handlerMoves.find(obj);
                if (mv != g_handlerMoves.end() && mv->second.counter == gameFrames(rdram))
                {
                    HandlerMoves &m = mv->second;
                    m.stepped = true;
                    std::memcpy(&m.motion, &ram8(rdram, obj + 0x428u), sizeof(m.motion));
                    std::memcpy(&m.step, &ram8(rdram, obj + 0x444u), sizeof(m.step));
                    std::memcpy(&m.rate, &ram8(rdram, obj + 0x5A8u), sizeof(m.rate));
                }
            }
            if (obj != 0u && obj != g_geneFlagsObj)
            {
                auto it = g_enemyFlags.find(obj);
                if (it != g_enemyFlags.end() && it->second.counter == gameFrames(rdram))
                    for (int i = 0; i < 2; ++i)
                    {
                        uint32_t flags = 0;
                        std::memcpy(&flags, &ram8(rdram, obj + kEnemyFrameFlags[i]), sizeof(flags));
                        it->second.added[i] |= flags & ~it->second.before[i] & kEnemyFrameMask[i];
                    }
            }
            if (obj != 0u && obj == g_geneFlagsObj && g_geneFlagsAt == gameFrames(rdram))
            {
                uint32_t flags = 0;
                std::memcpy(&flags, &ram8(rdram, obj + kGeneFrameFlags), sizeof(flags));
                g_geneFlagsAdded = flags & ~g_geneFlagsBefore;
            }
        }
        template <int ObjReg, int FnReg = 6, bool CarryPress = false>
        void enemyStepHold(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, ObjReg);
            const uint32_t pressedAt = game::GameWork::address(game::GameWork::kPadBtn);
            if (CarryPress && g_genePressCarry != 0u && g_genePressAt + 1u == gameFrames(rdram))
            {
                uint32_t pressed = 0;
                std::memcpy(&pressed, &ram8(rdram, pressedAt), sizeof(pressed));
                pressed |= g_genePressCarry;
                std::memcpy(&ram8(rdram, pressedAt), &pressed, sizeof(pressed));
            }
            if (CarryPress)
                g_genePressCarry = 0u;
            uint32_t state = 0, motion = 0;
            std::memcpy(&state, &ram8(rdram, obj + 0x2F4u), sizeof(state));
            std::memcpy(&motion, &ram8(rdram, obj + 0x428u), sizeof(motion));
            const uint32_t counter = gameFrames(rdram);
            EnemyStep &last = g_enemyStep[obj];
            bool changed = last.counter + 1u == counter && last.state != state;
            // A hit that landed in this update (mode 1 / 2 at step 0 with its damage take, +0x698): at 30 the
            // reaction's step 0 runs in the hit's own frame and reads the blow's direction from the take, which the
            // enemy's update frees at its end (func_00208858). Held to the tick, step 0 ran without it (Nov 1, K's
            // demon counters: launched sideways). Not held: the reaction starts in the frame of the hit, as at 30.
            // Any mode (Nov 3, T1's "Azel sweep broken"): a blow on a guard starts a mode 0 reaction (Azel's phase 112
            // func_0023E6B0, step 0 at 0x23ED24: the attacker's position into +0x5D0 and +0x5F0 = 10 frames of turning
            // toward it before the counter). Held, step 0 found no take: no turn, the sweep came out facing the old way.
            if (!CarryPress && changed)
            {
                const uint8_t step = ram8(rdram, obj + 0x2F6u);
                uint32_t take = 0;
                std::memcpy(&take, &ram8(rdram, obj + 0x698u), sizeof(take));
                if (step == 0u && take != 0u)
                    changed = false;
            }
            last = EnemyStep{state, counter, motion};
            // The handler about to run (handlerEnd pops it); on a tick its motion calls are recorded for the hold.
            if (g_handlerDepth >= 8)
                g_handlerDepth = 0;
            g_handlerStack[g_handlerDepth++] = thinkSkipped(rdram) ? 0u : obj;
            if (CarryPress && !thinkSkipped(rdram))
            {
                g_geneFlagsObj = obj;
                g_geneFlagsAt = counter;
                std::memcpy(&g_geneFlagsBefore, &ram8(rdram, obj + kGeneFrameFlags), sizeof(g_geneFlagsBefore));
                g_geneFlagsAdded = 0u;
            }
            if (!CarryPress && !thinkSkipped(rdram))
            {
                EnemyFlags &f = g_enemyFlags[obj];
                if (f.counter != counter) // a second dispatch in the same update adds to the first's record
                {
                    f = EnemyFlags{counter};
                    for (int i = 0; i < 2; ++i)
                        std::memcpy(&f.before[i], &ram8(rdram, obj + kEnemyFrameFlags[i]), sizeof(f.before[i]));
                }
            }
            if (!thinkSkipped(rdram))
            {
                HandlerMoves &moves = g_handlerMoves[obj];
                if (moves.counter != counter) // a second dispatch in the same update adds to the first's record
                    moves = HandlerMoves{counter};
            }
            if (changed && thinkSkipped(rdram))
            {
                // Also when the change set a new motion (Nov 1): at 30 the frame that sets a motion steps it a whole
                // frame (set before moveMotion), so the held half steps it too; skipping it started every such motion
                // half a frame behind (Gene and enemies, all cases; the case csv's mframe is +0x41C, copied by
                // Motion_seq_ctr before the step, so it shows each vblank's step one row later). Only the calls the
                // tick's handler made (HandlerMoves, Nov 2).
                auto rec = g_handlerMoves.find(obj);
                g_holdCalling = true;
                if (rec != g_handlerMoves.end() && rec->second.counter + 1u == counter)
                {
                    const HandlerMoves m = rec->second;
                    // Gene's motion step and rate as the tick's handler left them (Nov 4, case 2026-11-03_14-53-38,
                    // the phase 59 mash): his update resets +0x5A8 and the step to 1.0 every frame before the
                    // dispatcher (func_00123460 0x1236AC, heat level 0) and each handler sets its own again. The held
                    // vblank skips the handler, so the repeated moveMotion ran at 1.0 and the second-half snap cut
                    // the frame (0.35 + 0.5 -> 0): his mash loop restarted and stayed a 30 FPS frame behind the held
                    // enemy's for the whole mash. Every rated state change did the same in a smaller way (the run's
                    // start at 1.3 / 1.6, the evade 1.3, the slow-mo 0.029). Same motion only. Enemies too (Nov 4,
                    // same case after 5e99f09): cEmManage's update hands every enemy its step again before the hold,
                    // so the held enemy's first frame in the mash loop ran at 1.0 instead of Gene's 0.7 and snapped.
                    if (m.stepped && m.motion == motion)
                    {
                        setRamF32(rdram, obj + 0x444u, m.step);
                        setRamF32(rdram, obj + 0x5A8u, m.rate);
                    }
                    for (int i = 0; i < m.move; ++i)
                        callGuest(rdram, ctx, runtime, 0x157C18u, obj, 0.0f);
                    for (int i = 0; i < m.rot; ++i)
                        callGuest(rdram, ctx, runtime, 0x14F9F0u, obj, m.rotF12);
                    for (int i = 0; i < m.null; ++i)
                        callGuest(rdram, ctx, runtime, 0x14F928u, obj, m.nullF12);
                    for (int i = 0; i < m.requests; ++i)
                        callRequest(rdram, ctx, runtime, m.request[i]);
                }
                else
                {
                    callGuest(rdram, ctx, runtime, 0x157C18u, obj, 0.0f);
                    callGuest(rdram, ctx, runtime, 0x14F9F0u, obj, 1.0f);
                    callGuest(rdram, ctx, runtime, 0x14F928u, obj, 1.0f);
                }
                g_holdCalling = false;
                if (!CarryPress)
                {
                    auto f = g_enemyFlags.find(obj);
                    if (f != g_enemyFlags.end() && f->second.counter + 1u == counter)
                        for (int i = 0; i < 2; ++i)
                        {
                            uint32_t flags = 0;
                            std::memcpy(&flags, &ram8(rdram, obj + kEnemyFrameFlags[i]), sizeof(flags));
                            flags |= f->second.added[i];
                            std::memcpy(&ram8(rdram, obj + kEnemyFrameFlags[i]), &flags, sizeof(flags));
                        }
                }
                if (CarryPress && obj == g_geneFlagsObj && g_geneFlagsAt + 1u == counter && g_geneFlagsAdded != 0u)
                {
                    uint32_t flags = 0;
                    std::memcpy(&flags, &ram8(rdram, obj + kGeneFrameFlags), sizeof(flags));
                    flags |= g_geneFlagsAdded;
                    std::memcpy(&ram8(rdram, obj + kGeneFrameFlags), &flags, sizeof(flags));
                }
                SET_GPR_U32(ctx, FnReg, 0x100318u);
                if (CarryPress && g_slowRateAt + 1u == counter && g_slowRateManager != 0u)
                    callGuest(rdram, ctx, runtime, 0x292F38u, g_slowRateManager, g_slowRateValue);
                if (CarryPress)
                {
                    std::memcpy(&g_genePressCarry, &ram8(rdram, pressedAt), sizeof(g_genePressCarry));
                    g_genePressAt = counter;
                }
            }
            if ((counter & 0x3FFu) == 0u)
                for (auto it = g_enemyStep.begin(); it != g_enemyStep.end();)
                    it = counter - it->second.counter > 2u ? g_enemyStep.erase(it) : std::next(it);
            if ((counter & 0x3FFu) == 0u)
                for (auto it = g_handlerMoves.begin(); it != g_handlerMoves.end();)
                    it = counter - it->second.counter > 2u ? g_handlerMoves.erase(it) : std::next(it);
            if ((counter & 0x3FFu) == 0u)
                for (auto it = g_enemyFlags.begin(); it != g_enemyFlags.end();)
                    it = counter - it->second.counter > 2u ? g_enemyFlags.erase(it) : std::next(it);
        }

        // moveMotion (0x157C18), the game's own 60 Hz code, at 60 (family 5b, Nov 1; case shockwave30: Shockwave's slow
        // window ended 2 frames early, the enemies' long motions 2-3 frames short in every Shockwave case).
        // 1. Speed keys: func_2C7FD0 sets +0x440 = 1.0 or the speed of the motion's key span holding frame +0x430 (u16
        //    start, u16 end, float speed; 0x2C8080..0x2C8110), before every step. At 30 a frame steps at the speed
        //    looked up at its start; at 60 a span entered on the first half took over on the second (Gene's E3D8A0:
        //    13.999 + 0.1 = 14.099, then 1.0 from 14.099 where the PS2 still stepped 0.2 to 14.199). The second half
        //    keeps the first half's speed (same object, same motion). Replaces lui v0,0x74 after the call (0x157C70).
        // 2. The snap: a step of exactly 1.0 truncates the frame to a whole number (0x157F4C c.eq.s f21, 1.0, then
        //    trunc(frame + 0.0001)), so at 30 every normal-speed frame wipes the fraction a slow or fast span left. At
        //    60 the step is 0.5 and the fraction lived on: motions ran up to a frame ahead after every speed span or
        //    slow motion (Gene and enemies). Now the second half of a 30 FPS frame stepping 0.5 snaps, the first half
        //    never does, and a 1.0 step (speed 2 at 60) doesn't. Replaces add.s f2,f0,f21 at 0x157F48 (f1 is 1.0 there).
        //    For k: snap on the k-th sub-step when the step is 1/k; hold the speed over the k-1 sub-steps after the tick.
        struct MotionSpeed
        {
            uint32_t counter;
            uint32_t motion;
            float speed;
        };
        std::unordered_map<uint32_t, MotionSpeed> g_motionSpeed;
        // moveMotion's own test for its 60 Hz mode (GameWork +0x569 == 1: the advance is halved); cutscenes run at 30.
        bool motion60(uint8_t *rdram) { return ram8(rdram, 0x7474A0u + 0x569u) == 1u; }

        // The motion's seq words seen a frame later (family 15, Nov 1; case shockwave30: Gene's Shockwave slowed the
        // world 1-2 vblanks early, so the whole slow window and every enemy motion in it ran early). Motion_seq_ctr
        // (0x2C7FD0, the top of moveMotion) rewrites +0x3AC (the seq level: bits of the key spans holding the frame;
        // 869 reads in 255 functions, 79 of Gene's states, 164 enemy functions) and the one-shot events +0x3A0 /
        // +0x3A2 / +0x3A4 every call. Handlers mostly read them before their moveMotion, i.e. what the previous frame's
        // step left: at 30 a span entered on frame k is acted on at frame k + 1. At 60 the tick's step sets it and the
        // second half's handler acted on it, half a frame early (Shockwave: +0x3AC & 0x100 -> SetSpeedRate 0.03 at
        // 0x114788). Now the second half sees the words as the tick's handler saw them (the tick's moveMotion changes
        // undone at its start, frameStart), and the next tick sees what the tick's step left (the second half's
        // moveMotion changes undone: a span ending on the tick's frame still shows, as at 30). Code after moveMotion
        // in the same frame sees the fresh words, as at 30. For k: undo on the k-1 frames after the tick, restore at the
        // next tick.
        struct SeqWords
        {
            uint16_t w[4] = {}; // +0x3AC, +0x3A0, +0x3A2, +0x3A4
            bool operator==(const SeqWords &o) const { return std::memcmp(w, o.w, sizeof(w)) == 0; }
        };
        constexpr uint32_t kSeqAt[4] = {0x3ACu, 0x3A0u, 0x3A2u, 0x3A4u};
        SeqWords readSeq(uint8_t *rdram, uint32_t obj)
        {
            SeqWords s;
            for (int i = 0; i < 4; ++i)
                std::memcpy(&s.w[i], &ram8(rdram, obj + kSeqAt[i]), sizeof(uint16_t));
            return s;
        }
        void writeSeq(uint8_t *rdram, uint32_t obj, const SeqWords &s)
        {
            for (int i = 0; i < 4; ++i)
                std::memcpy(&ram8(rdram, obj + kSeqAt[i]), &s.w[i], sizeof(uint16_t));
        }
        struct SeqTick
        {
            uint32_t counter = 0, motion = 0;
            SeqWords pre, post, post2; // before / after the tick's moveMotion; after the second half's
            bool hidden = false, stepped2 = false;
        };
        std::unordered_map<uint32_t, SeqTick> g_seqTick;
        uint32_t g_seqPreObj = 0;
        SeqWords g_seqPre;
        // 0x157C64 lw a1,0x42C(s1), before moveMotion's Motion_seq_ctr call (s1 = the object).
        void seqBefore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t obj = GPR_U32(ctx, 17);
            if (obj < 0x100000u || obj >= 0x2000000u - 0x500u)
                return;
            g_seqPreObj = obj;
            g_seqPre = readSeq(rdram, obj);
        }
        // After the call (motionSpeedHold).
        void seqAfter(uint8_t *rdram, uint32_t obj)
        {
            if (g_seqPreObj != obj)
                return;
            g_seqPreObj = 0;
            const uint32_t counter = gameFrames(rdram);
            uint32_t motion = 0;
            std::memcpy(&motion, &ram8(rdram, obj + 0x428u), sizeof(motion));
            const SeqWords now = readSeq(rdram, obj);
            if (!thinkSkipped(rdram))
            {
                SeqTick &t = g_seqTick[obj];
                if (t.counter != counter || t.motion != motion) // the tick's first step of this motion
                    t = SeqTick{counter, motion, g_seqPre, now, now, false, false};
                else
                    t.post = now;
                if (t.pre == t.post)
                    g_seqTick.erase(obj);
                return;
            }
            auto it = g_seqTick.find(obj);
            if (it != g_seqTick.end() && it->second.counter + 1u == counter && it->second.motion == motion)
            {
                it->second.post2 = now;
                it->second.stepped2 = true;
            }
        }
        // frameStart, before the game counts the frame up (the frame about to run sees counter + 1).
        void seqFrameStart(uint8_t *rdram)
        {
            if (g_seqTick.empty())
                return;
            const uint32_t next = gameFrames(rdram) + 1u;
            const bool second = ((next - g_counterBase) & 1u) != 0u;
            for (auto it = g_seqTick.begin(); it != g_seqTick.end();)
            {
                const uint32_t obj = it->first;
                SeqTick &t = it->second;
                uint32_t motion = 0;
                std::memcpy(&motion, &ram8(rdram, obj + 0x428u), sizeof(motion));
                SeqWords cur = readSeq(rdram, obj);
                // +0x3AC: flip back the bits a moveMotion changed, when the handler has not touched them since (it may
                // set its own latch bits after the step, e.g. 0x800 at 0x1149C4: kept). Events: put back when untouched.
                const auto undo = [&](const SeqWords &from, const SeqWords &to, bool keepNewerEvents) {
                    const uint16_t d = static_cast<uint16_t>(from.w[0] ^ to.w[0]);
                    if ((cur.w[0] & d) == (from.w[0] & d))
                        cur.w[0] = static_cast<uint16_t>(cur.w[0] ^ d);
                    for (int i = 1; i < 4; ++i)
                        if (cur.w[i] == from.w[i] && !(keepNewerEvents && from.w[i] != 0u))
                            cur.w[i] = to.w[i];
                    writeSeq(rdram, obj, cur);
                };
                if (second && t.counter + 1u == next && !t.hidden && motion == t.motion)
                {
                    undo(t.post, t.pre, false); // the second half sees what the tick's handler saw
                    t.hidden = true;
                    ++it;
                    continue;
                }
                if (!second && t.counter + 2u == next && t.hidden && motion == t.motion)
                {
                    if (t.stepped2)
                        undo(t.post2, t.post, true); // the tick's step's words; an event of the second half's own stands
                    else
                        undo(t.pre, t.post, false);
                }
                it = g_seqTick.erase(it);
            }
        }

        void motionSpeedHold(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 2, 0x740000u);
            const uint32_t obj = GPR_U32(ctx, 17);
            if (obj < 0x100000u || obj >= 0x2000000u - 0x500u || !motion60(rdram))
                return;
            seqAfter(rdram, obj);
            uint32_t motion = 0;
            std::memcpy(&motion, &ram8(rdram, obj + 0x428u), sizeof(motion));
            const uint32_t counter = gameFrames(rdram);
            auto it = g_motionSpeed.find(obj);
            if (thinkSkipped(rdram) && it != g_motionSpeed.end() && it->second.counter + 1u == counter &&
                it->second.motion == motion)
            {
                setRamF32(rdram, obj + 0x440u, it->second.speed);
                return;
            }
            float speed = 1.0f;
            std::memcpy(&speed, &ram8(rdram, obj + 0x440u), sizeof(speed));
            g_motionSpeed[obj] = MotionSpeed{counter, motion, speed};
            if ((counter & 0x3FFu) == 0u)
                for (auto e = g_motionSpeed.begin(); e != g_motionSpeed.end();)
                    e = counter - e->second.counter > 2u ? g_motionSpeed.erase(e) : std::next(e);
        }
        void motionSnap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const float step = ctx->f[21];
            ctx->f[2] = ctx->f[0] + step;
            if (!motion60(rdram))
                return;
            if (step == 0.5f && thinkSkipped(rdram))
                ctx->f[1] = step; // c.eq.s f21, f1: snap
            else if (step == 1.0f)
                ctx->f[1] = -1.0f; // a 2.0 step at 30: no snap
        }

        // The launched enemy's flight (mode 1 phase 9 step 1, func_002557F8 0x255AEC / 0x255AF8): y += vy, vy -= 0.026
        // a frame, without the speed rate the other airborne states multiply in, so at 60 the arc ran in half the time
        // (66sec case, Oct 31: 18 vblanks in the air instead of 36, the horizontal speed right). At 60 half steps that
        // meet the 30 FPS arc on every 30 FPS frame: y += vy / 2 + g / 8, vy -= g / 2 (f2 = g).
        // Texture scrolls in the effect code below the forced slow paths (0x2E0000..0x2F5000), stepped once a game frame
        // with no rate: UvScroll_step (0x2EAA0C / 0x2EAA14: +0x2B8 / +0x2BC += +0x2C0 / +0x2C4, a vtable method) and
        // ScrollObj_advanceScroll (0x2EC34C: +0x2D8 += +0x2DC; trails and others). At 60 they scrolled twice as fast
        // (Nov 1, K: the first room's sky clouds and the dust drifting over the ground, the demon's summoning clouds).
        // Visual only: half the step each vblank (k: 1/k).
        template <int Dst, int A, int B>
        void scrollHalf(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->f[Dst] = ctx->f[A] + ctx->f[B] * 0.5f; }
        // cModel_ScrollTexture (0x14E4D0: the model's texture scroll += its speed x f12, every call) is given the
        // enemies' rate +0x5A8 by enemy code (already half at 60) but a plain 1.0 by cObjSimple_Update (0x2B6974) and
        // func_001B76D8 (0x1B7740), once a game frame: those objects' textures scrolled twice as fast at 60 (Nov 1, K:
        // the first room's sky clouds still fast after the effect scrolls). Replaces mtc1 at,f12 (1.0): 0.5.
        void modelScrollHalf(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->f[12] = 0.5f; }
        // The map's scroll objects (vtable 0x450CE8: the sky domes and other texture-scrolled map pieces, every room;
        // Nov 2, K: the sky clouds twice as fast at 60 everywhere; found in the standing case's RAM snapshots: the dome's
        // texture-matrix V offset fell 0.06 a second at 30 and 0.12 at 60). Their time step is GameWork +0x574 x the
        // object rate 0x754C48, 1.0 a game frame at either rate (moveMotion halves its own copy, these do not), and
        // some of their phases come from the frame counter GameWork +0x210 directly. At 60: the step halved, the
        // counter read as half the vblanks. Visual only (k: 1/k).
        // func_00319970 (+0x38C / +0x390 += speed x step): mul.s f1,f0,f20 at 0x319A80 and 0x319B54.
        void mapScrollStep(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->f[1] = ctx->f[0] * ctx->f[20] * 0.5f; }
        // The stage 5 boat ride's scrolling town (r501, func at 0x79A7C0: K, Nov 7, case boat_too_fast: the buildings
        // slid by 0.11 a game frame at both rates, 3.3 m/s at 30 and 6.6 at 60): its step is GameWork +0x574 x the
        // object rate 0x754C48 (1.0 at either rate, as the map scroll objects above), which then moves the scenery
        // (+0x394 -= step x 0.11, wrapped at 550) and its texture phase (+0x2E4 += step x 0.002). The only room-DLL read
        // of 0x754C48 (census of all 79 room DLLs, Nov 7). Replaces mul.s f4,f4,f0 at 0x79A7FC: half while the game
        // runs at 60.
        // Dropped items (func_001C0678, the item object: heals, roulette reels, magazines; T1's case "Item drop speed",
        // Nov 7): substep 1 pops the item up and lets it fall, `pos += vel (+0x610); vel.y -= g (+0x600, 0.02)` once a
        // frame with no rate (spawn func_001C0128: vel.y 0.2), and the settled state pulls it down the same way; at 60
        // both ran twice as fast. Half steps that put every tick on the 30 FPS arc (position first, then the pull):
        // pos += vel / 2 + (0, g / 8, 0) at the vadd (0x1C07FC / 0x1C0ACC), vel.y -= g / 2 at the sub.s (0x1C0838 /
        // 0x1C0B08). s2 = the item. Cutscenes (divider 2) keep the full steps.
        void itemDropMove(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // replaces vadd.xyz vf4,vf4,vf5
        {
            alignas(16) float a[4], b[4];
            _mm_store_ps(a, ctx->vu0_vf[4]);
            _mm_store_ps(b, ctx->vu0_vf[5]);
            const bool half = motion60(rdram);
            for (int i = 0; i < 3; ++i)
                a[i] += half ? 0.5f * b[i] : b[i];
            if (half)
            {
                float g = 0.0f;
                std::memcpy(&g, &ram8(rdram, GPR_U32(ctx, 18) + 0x600u), sizeof(g));
                a[1] += g * 0.125f;
            }
            ctx->vu0_vf[4] = _mm_load_ps(a);
        }
        void itemDropPull(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // replaces sub.s f0,f0,f1 (f1 = g)
        {
            ctx->f[0] = ctx->f[0] - ctx->f[1] * (motion60(rdram) ? 0.5f : 1.0f);
        }
        void r501BoatStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[4] = ctx->f[4] * ctx->f[0] * (motion60(rdram) ? 0.5f : 1.0f);
        }

        // The stage 5 cannon minigame at 60 (r501, K Nov 7, case cannon_minigame_too_fast): its tasks sleep(1) and step
        // once a game frame with no rate, so every target, the crosshair and the banner ran at twice the speed. The tasks
        // keep running every vblank (they draw: holding them every other vblank flickered, 68e079a), and each step is
        // made the 60 FPS form of its 30 FPS self, so the values at every even vblank are the 30 FPS ones:
        //   x += c                     -> x += c / 2                     (floatAddHalf / floatSubHalf)
        //   v += a; x += v             -> v += a/2; x += v/2 + a/8       (the cannonballs: r501BallMove)
        //   x += (t - x) k             -> k' = 1 - sqrt(1 - k)           (lerpRoot)
        //   x *= r                     -> r' = sqrt(r)                   (mulRoot)
        //   v += a; x += v; v *= d     -> a/2, x += v / (1 + sqrt d), d' = sqrt d (aim: r501AimDamp; banner: geomAdd)
        //   timers, frame counters     -> every other vblank (the countdown rule)
        template <int Dst, int A, int B>
        void floatSubHalf(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[Dst] = ctx->f[A] - ctx->f[B] * 0.5f;
        }
        template <int Dst, int A, int K>
        void lerpRoot(uint8_t *, R5900Context *ctx, PS2Runtime *) // mul.s Dst,A,K: (t - x) x k
        {
            const float k = ctx->f[K];
            ctx->f[Dst] = ctx->f[A] * (k > 0.0f && k < 1.0f ? 1.0f - std::sqrt(1.0f - k) : k);
        }
        template <int Dst, int A, int R>
        void mulRoot(uint8_t *, R5900Context *ctx, PS2Runtime *) // mul.s Dst,A,R: x x r (a decay)
        {
            const float r = ctx->f[R];
            ctx->f[Dst] = ctx->f[A] * (r > 0.0f ? std::sqrt(r) : r);
        }
        // x += v ahead of v *= r (R holds r): x += v / (1 + sqrt r).
        template <int Dst, int A, int V, int R>
        void geomAdd(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const float r = ctx->f[R];
            ctx->f[Dst] = ctx->f[A] + ctx->f[V] / (1.0f + (r > 0.0f ? std::sqrt(r) : 0.0f));
        }
        template <int Reg>
        void countupReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // addiu Reg,Reg,1
        {
            const int32_t value = GPR_S32(ctx, Reg);
            SET_GPR_S32(ctx, Reg, skipFrame(rdram) ? value : value + 1);
        }
        // CannonGame's balls (0x79E168 vy += -0.02, then position += velocity at 0x79E184): with vy += -0.01 a vblank,
        // position += v/2 + (0, g/8, 0) lands on the 30 FPS arc (and the aim predictor's) at every even vblank.
        void r501BallMove(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            alignas(16) float a[4], b[4];
            _mm_store_ps(a, ctx->vu0_vf[4]);
            _mm_store_ps(b, ctx->vu0_vf[5]);
            for (int i = 0; i < 3; ++i)
                a[i] += 0.5f * b[i];
            a[1] += -0.02f / 8.0f;
            ctx->vu0_vf[4] = _mm_load_ps(a);
        }
        // The hit banner's wobble (0x79EF5C / 0x79EFAC: phase = +0x6C x 0x1C8 / x 0xEA): +0x6C now counts every other
        // vblank (0x79F020, raised at the end of the even vblank), so the odd vblank takes it half a count back.
        template <int Reg>
        void r501BannerPhase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // mult v0,v0,Reg
        {
            const int32_t step = GPR_S32(ctx, Reg);
            int32_t value = GPR_S32(ctx, 2) * step;
            if (skipFrame(rdram))
                value -= step / 2;
            SET_GPR_S32(ctx, 2, value);
            ctx->lo = static_cast<uint64_t>(static_cast<int64_t>(value));
            ctx->hi = 0;
        }
        // StateNeutral's aim (0x79F3A8..0x79F410): v -= stick x a, angle += v, v *= d (d 0.8 / 0.92 per axis), per frame.
        float g_aimMove[2] = {1.0f, 1.0f};
        template <int Axis, int Dst, int A, int D>
        void r501AimDamp(uint8_t *, R5900Context *ctx, PS2Runtime *) // mul.s Dst,A,D
        {
            const float root = ctx->f[D] > 0.0f ? std::sqrt(ctx->f[D]) : 0.0f;
            g_aimMove[Axis] = 1.0f / (1.0f + root);
            ctx->f[Dst] = ctx->f[A] * root;
        }
        template <int Axis, int Dst, int A, int V>
        void r501AimAdd(uint8_t *, R5900Context *ctx, PS2Runtime *) // add.s Dst,A,V
        {
            ctx->f[Dst] = ctx->f[A] + ctx->f[V] * g_aimMove[Axis];
        }
        // func_00318B58's step f23 (counter x f23 at 0x319470, a vector step at 0x3190B8): mul.s f23,f0,f1 at 0x318BB4.
        void mapScrollStep23(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->f[23] = ctx->f[0] * ctx->f[1] * 0.5f; }
        // func_00318B58's camera-locked dome: V += (counter & 0xFF) / 256 (mul.s f0,f2,f0 at 0x319088): half the count.
        void mapScrollCounter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[0] = std::fmod(static_cast<float>(gameFrames(rdram)) * 0.5f, 256.0f) * ctx->f[0];
        }
        // func_00318B58's wobble phase: the counter as a float in f3 (both paths meet at 0x3191A0): half of it.
        void mapScrollPhase(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->f[3] *= 0.5f; }
        void launchRise(uint8_t *, R5900Context *ctx, PS2Runtime *) // add.s f1,f1,f0
        {
            ctx->f[1] = ctx->f[1] + ctx->f[0] * 0.5f + ctx->f[2] * 0.125f;
        }
        void launchFall(uint8_t *, R5900Context *ctx, PS2Runtime *) // sub.s f0,f0,f2
        {
            ctx->f[0] = ctx->f[0] - ctx->f[2] * 0.5f;
        }

        // The other airborne states (family 12b, Nov 1; case "Goon jump out of juggle incorrect speed", the juggle
        // mode 1 phase 8): y += vy x rate, then vy -= g x rate. Two half steps at 60 land g x rate^2 / 4 short of
        // the 30 FPS frame's y += vy (the juggle's peak 0.058 m low over an 18-frame rise). This replaces the vy store
        // `swc1 fF, 0x5C4(base)` that follows the y step: store, then y += (vy before - vy after) x rate / 2 (=
        // g x rate^2 / 2), which puts every 30 FPS frame's y back on the 30 FPS arc. Sites: tools in fps60-notes 12b.
        // Gene's fall (Nov 1, static read; his common update func_00108AB0 0x10954C..0x1095E8): once he is 0.4 m or more
        // above the floor, vy (+0x1608) -= g (+0x1604, 0.02 from 0x1088AC), then y += vy, every game frame and without
        // a rate: at 60 he fell with 4x the acceleration. Exact half steps for this order (speed first): vy -= g / 2,
        // y += vy / 2 - g / 8 (k frames per 30 FPS frame: vy -= g / k, y += vy / k - g (k - 1) / (2 k^2)).
        template <int Dst, int V, int G>
        void geneFallSpeed(uint8_t *, R5900Context *ctx, PS2Runtime *) // sub.s Dst, V, G
        {
            ctx->f[Dst] = ctx->f[V] - ctx->f[G] * 0.5f;
        }
        template <int Dst, int V>
        void geneFallY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // add.s Dst, Dst, V (y + new vy); Gene in s1
        {
            float g = 0.0f;
            std::memcpy(&g, rdram + ((GPR_U32(ctx, 17) + 0x1604u) & 0x01FFFFFFu), 4);
            ctx->f[Dst] = ctx->f[Dst] + ctx->f[V] * 0.5f - g * 0.125f;
        }

        template <int F, int Base>
        void airStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t obj = GPR_U32(ctx, Base);
            const float after = ctx->f[F];
            float before = 0.0f, rate = 0.0f, y = 0.0f;
            uint32_t pos = 0u;
            std::memcpy(&before, rdram + ((obj + 0x5C4u) & 0x01FFFFFFu), 4);
            std::memcpy(&rate, rdram + ((obj + 0x5A8u) & 0x01FFFFFFu), 4);
            std::memcpy(rdram + ((obj + 0x5C4u) & 0x01FFFFFFu), &after, 4);
            std::memcpy(&pos, rdram + ((obj + 0xF0u) & 0x01FFFFFFu), 4);
            if (pos == 0u)
                return;
            std::memcpy(&y, rdram + ((pos + 4u) & 0x01FFFFFFu), 4);
            y += (before - after) * rate * 0.5f;
            std::memcpy(rdram + ((pos + 4u) & 0x01FFFFFFu), &y, 4);
        }

        // cGameObj_SetTgtTurn (0x2A7498) is 60-aware: the turn per call is f12 x GameWork +0x55C (1.0, or 0.5 when the
        // divider is 1: func_0030AC40 0x30AF14). 182 callers pass f12 = the enemy's speed rate x a turn step, and the
        // rate is 0.5 at 60 too (0x29059C), so those turns ran at a quarter speed (66sec case, Oct 31: the grab lunge,
        // phase 75 step 1, turned 0.065 a vblank against 0.26 a 30 FPS frame and went 0.1 m wide). Replaces the
        // mul.s f13,f12,f0 at 0x2A74B0: x2 for those callers (ra - 8) on an object in cEmManage's list. Nov 6 (T1: Elvis'
        // ring of fire homed poorly at 60): + 12 enemy sites the census window missed, and the objects that pass
        // GetSpeedRate x a step (the homing bullets cOmbe: they turned at a quarter, half per 30 FPS frame).
#include "kamite/fps60_turn_sites.inc"
        void tgtTurnRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            float step = ctx->f[12] * ctx->f[0];
            const uint32_t site = GPR_U32(ctx, 31) - 8u;
            if ((std::binary_search(std::begin(kTgtTurnRateSites), std::end(kTgtTurnRateSites), site) &&
                 inEnemyList(rdram, GPR_U32(ctx, 16))) ||
                std::binary_search(std::begin(kTgtTurnObjectSites), std::end(kTgtTurnObjectSites), site))
                step *= 2.0f;
            ctx->f[13] = step;
        }

        // SetMotionStep (0x158288) from enemy handlers with a step made of the enemy's own rate (+0x5A8, 0.5 at 60) or
        // cEmManage_GetSpeedRate (also 0.5): moveMotion already halves every advance at 60, so these motions ran at
        // half the PS2's speed (Nov 3, T1's "Azel pummel logic": phase 90 func_002383D0, SetMotionStep(rate x 2) at
        // 0x2385BC, 2 motion frames per 30 FPS frame at 30, 1 at 60). The manager's own call for every enemy is
        // doubled at 0x290584; these are the handlers' own calls (census of the 57 callers: Gene's pass his rate,
        // which stays the 30 FPS value; the mode 1 grab reactions 0x25C1FC.. copy Gene's; objects not checked).
        constexpr uint32_t kMotionStepRateSites[] = {
            0x22C984u, 0x22D3E0u, 0x2385A4u, 0x2385BCu, 0x2452A4u, 0x278B48u, // the enemy's +0x5A8 x a factor
        };
        // Nov 8 (T1's Demon Shannon case, room 0x602): cEmManage_GetSpeedRate is the halved rate for every caller, so
        // these are doubled whether or not the object is in cEmManage's list. Shannon's familiar (kind 0x263, update
        // func_0027BB78) is not in the list: it takes its owner's (+0x1580) +0x5A8, else GetSpeedRate, and its motion
        // ran at a quarter of the PS2's speed (motion frames 15 a second at 60), so its volley's fire window (seq bit 0,
        // a shot every 6 / rate frames) lasted twice as long: twice the projectiles. 0x280900 / 0x282BD8 (the sync states
        // func_002808D8 / 00282BB0) are gone from the list: they copy Gene's +0x5A8, which is the 30 FPS value.
        constexpr uint32_t kMotionStepGetSpeedRateSites[] = {0x27E0ECu, 0x27F1F0u, 0x2819A8u};
        constexpr uint32_t kMotionStepOwnerRateSite = 0x27BBCCu, kMotionStepOwnerOffset = 0x1580u;
        // Objects (Nov 6, T1's "Conchita and possibly whip ladies broken whip physics"): the weapons cOmWeapon (the whip
        // ladies' whips: object em +0x6F0, state 6) set their motion step from cEmManage_GetSpeedRate (0.5 at 60) or
        // copy their holder's +0x5A8: the whip's motion ran at +0x444 0.5 at 60 (1.0 at 30), so it unfolded and folded
        // back slowly. x2 where the rate is the halved kind: GetSpeedRate always; a holder's rate only when the holder is
        // an enemy (Gene's rate stays the 30 FPS value). Holder offset 0 = GetSpeedRate only.
        struct MotionStepObjectSite
        {
            uint32_t site, holder;
        };
        constexpr MotionStepObjectSite kMotionStepObjectSites[] = {
            {0x1C80C0u, 0x644u}, // func_001C8020: the holder's +0x5A8 (always a holder)
            {0x1CB060u, 0x600u}, // func_001CB030: the holder's (+0x600) +0x5A8, else GetSpeedRate
            {0x1CB430u, 0x600u}, // func_001CB400: same
            {0x1CB84Cu, 0u},     // func_001CB828 / 001CB8C0 / 001CB990 / 001CEF28: GetSpeedRate
            {0x1CB8E4u, 0u},
            {0x1CB9B4u, 0u},
            {0x1CEF58u, 0u},
            // Nov 7 (K's case 2026-11-07_15-41-10, r601: the rolling boulders at half speed, motion frames 15 a second
            // at 60 against 30): the census of every other object caller of SetMotionStep, GetSpeedRate in f12.
            {0x1A5570u, 0u}, // func_001A5540: cOm50 (the boulder) state 0
            {0x1AA1F8u, 0u}, // func_001AA188: GetSpeedRate x GameWork +0x7A14
            {0x1AF0C0u, 0u}, // func_001AF098
            {0x1B8380u, 0u}, // func_001B8338
        };
        void motionStepRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // at SetMotionStep's entry
        {
            const uint32_t site = GPR_U32(ctx, 31) - 8u;
            if (std::binary_search(std::begin(kMotionStepRateSites), std::end(kMotionStepRateSites), site) &&
                inEnemyList(rdram, GPR_U32(ctx, 4)))
            {
                ctx->f[12] *= 2.0f;
                return;
            }
            if (std::find(std::begin(kMotionStepGetSpeedRateSites), std::end(kMotionStepGetSpeedRateSites), site) !=
                std::end(kMotionStepGetSpeedRateSites))
            {
                ctx->f[12] *= 2.0f;
                return;
            }
            if (site == kMotionStepOwnerRateSite)
            {
                uint32_t owner = 0;
                std::memcpy(&owner, &ram8(rdram, GPR_U32(ctx, 4) + kMotionStepOwnerOffset), sizeof(owner));
                if (owner == 0u || inEnemyList(rdram, owner))
                    ctx->f[12] *= 2.0f;
                return;
            }
            for (const MotionStepObjectSite &s : kMotionStepObjectSites)
            {
                if (s.site != site)
                    continue;
                uint32_t holder = 0;
                if (s.holder != 0u)
                    std::memcpy(&holder, &ram8(rdram, GPR_U32(ctx, 4) + s.holder), sizeof(holder));
                if (holder == 0u ? s.site != 0x1C80C0u : inEnemyList(rdram, holder))
                    ctx->f[12] *= 2.0f;
                return;
            }
        }

        // The body push (Nov 3, T1's lunge cases: Gene's running attack no longer went through Azel at 60, the Elite
        // Robot Dude's lunges no longer went through Gene). cCollisionSolidManage_ChkHit (0x1348E8, a1 = the owner)
        // moves its owner out of every other active body sphere by the whole overlap (func_00134CB0). A sphere sits on
        // its owner's matrix +0x80, which Gene's update rebuilds only after the push (cModel_calcParts 0x1097BC after
        // ChkHit 0x109464) and an enemy's before it (0x209AAC / 0x209B54, ChkHit 0x209C2C). So at 30 each enemy is
        // pushed out of where Gene stood at the start of the frame, and Gene out of the enemies' last spheres: a lunge's
        // 0.3-0.5 m steps carry him past the enemy's centre and through (case Square lunge: Azel ends each frame 0.84 m
        // from Gene's start position, frame 27 -> 28 Gene's start is past him). At 60 the same push every vblank on
        // half steps holds him at 0.84 m minus half a step: never through. Here the characters' pushes run on the
        // second half only, Gene's with the matrix he had at the tick (the 30 FPS frame's start): one push per 30 FPS
        // frame from the same positions as the PS2. The tick's overlap of up to half a step is visible for a vblank.
        // Objects (MoveObj's own ChkHit callers) push before they move: left as they are.
        GuestFunction g_origChkHit = nullptr;
        struct BodyMatrix
        {
            uint32_t counter = 0;
            uint8_t m[64] = {};
        };
        std::unordered_map<uint32_t, BodyMatrix> g_bodyMatrix;
        // The callers by return address: the player updates rebuild the matrix after the push (stale = true), the
        // enemy updates before it.
        bool bodyPushSite(uint32_t ra, bool &stale)
        {
            switch (ra)
            {
            case 0x10946Cu: // Gene, func_00108AB0
            case 0x102260u: // func_00102150 (calcParts 0x102350)
                stale = true;
                return true;
            case 0x209C34u: // enemies, func_00208BE0
            case 0x277690u: // func_00277568
            case 0x285AF0u: // func_002856C0
            case 0x28A17Cu: // func_00289E38
                stale = false;
                return true;
            default:
                return false;
            }
        }
        void bodyPush(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t owner = GPR_U32(ctx, 5), ra = GPR_U32(ctx, 31);
            bool stale = false;
            if (!g_fps60 || ctx->pc != 0x1348E8u || owner < 0x100000u || owner >= 0x2000000u - 0x100u || !bodyPushSite(ra, stale))
                return g_origChkHit(rdram, ctx, runtime);
            const uint32_t counter = gameFrames(rdram);
            if (!thinkSkipped(rdram))
            {
                if (stale)
                {
                    BodyMatrix &b = g_bodyMatrix[owner];
                    b.counter = counter;
                    std::memcpy(b.m, &ram8(rdram, owner + 0x80u), sizeof(b.m));
                }
                if ((counter & 0x3FFu) == 0u)
                    for (auto it = g_bodyMatrix.begin(); it != g_bodyMatrix.end();)
                        it = counter - it->second.counter > 2u ? g_bodyMatrix.erase(it) : std::next(it);
                SET_GPR_U32(ctx, 2, 0u); // the callers ignore it
                ctx->pc = ra;
                return;
            }
            const auto it = stale ? g_bodyMatrix.find(owner) : g_bodyMatrix.end();
            if (it == g_bodyMatrix.end() || it->second.counter + 1u != counter) // no tick seen (a load): as it is
                return g_origChkHit(rdram, ctx, runtime);
            // The position (+0xF0) points at the matrix's translation row (Gene: +0xF0 -> +0xB0): the swap moves him to
            // the tick's position too, and the push lands there. Restore the matrix, then add what the push moved
            // (Nov 3, T1's "Square lunge 2.0" on ac80751: restoring wiped Gene's push, so at 60 enemies never pushed
            // Gene at all; his contact steps equalled his free running-attack steps, 0.381 / 0.381 m at motion frame 7,
            // against 0.21 / 0.37 at 30, and his running attack went through a blocking Azel).
            uint32_t pos = 0;
            std::memcpy(&pos, &ram8(rdram, owner + 0xF0u), sizeof(pos));
            const bool posInMatrix = pos >= owner + 0x80u && pos + 12u <= owner + 0xC0u;
            uint8_t now[64];
            std::memcpy(now, &ram8(rdram, owner + 0x80u), sizeof(now));
            std::memcpy(&ram8(rdram, owner + 0x80u), it->second.m, sizeof(now));
            float before[3] = {}, after[3] = {};
            if (posInMatrix)
                std::memcpy(before, &ram8(rdram, pos), sizeof(before));
            g_origChkHit(rdram, ctx, runtime);
            if (posInMatrix)
                std::memcpy(after, &ram8(rdram, pos), sizeof(after));
            std::memcpy(&ram8(rdram, owner + 0x80u), now, sizeof(now));
            if (posInMatrix)
            {
                float p[3];
                std::memcpy(p, &ram8(rdram, pos), sizeof(p));
                for (int i = 0; i < 3; ++i)
                    p[i] += after[i] - before[i];
                std::memcpy(&ram8(rdram, pos), p, sizeof(p));
            }
        }

        void installFrame30State()
        {
            ps2x::savestate::registerExtension(
                "kamite.frame30",
                [](ps2x::state::Writer &out) {
                    // version 2 adds the half: 1 when the saved frame was the first half of its 30 FPS frame (the next
                    // update is that frame's second half), so a load resumes on the same grid as the run that saved it
                    // (the test cases' live runs never reload their state: without it about half of all 60 FPS
                    // recordings replayed every hold and staggered think a vblank off the live run's grid).
                    uint32_t version = 2u, frame = g_rdram ? (g_fps60 ? frame30(g_rdram) : gameFrames(g_rdram)) : 0u;
                    uint32_t half = g_rdram && g_fps60 ? (((gameFrames(g_rdram) - g_counterBase) & 1u) == 0u ? 1u : 0u) : 0u;
                    ps2x::state::fields(out, version, frame, half);
                },
                [](ps2x::state::Reader &in) {
                    uint32_t version = 0u;
                    ps2x::state::fields(in, version, g_frame30Saved);
                    g_frame30Half = 0u;
                    if (version >= 2u)
                        ps2x::state::fields(in, g_frame30Half);
                    g_frame30Loaded = true;
                });
        }
        // After a load (before the game runs): the 30 FPS frame count from the state (a state without it: its counter).
        // At 60 it is the base; at 30 a state saved at 60 gets it as the game's counter.
        void frame30AfterLoad(uint8_t *rdram)
        {
            const uint32_t counter = gameFrames(rdram);
            const uint32_t frame = g_frame30Loaded ? g_frame30Saved : counter;
            // The game counts the frame up before its update: the first update after the load sees counter + 1. A state
            // saved on a first half resumes on that 30 FPS frame's second half (same 30 FPS frame, base one earlier).
            const uint32_t half = g_frame30Loaded ? g_frame30Half : 0u;
            if (g_fps60)
                g_frame30Base = half ? frame : frame + 1u, g_counterBase = counter + 1u - half;
            else if (g_frame30Loaded && ram8(rdram, kFrameDivider) == 1u)
                std::memcpy(&ram8(rdram, game::GameWork::address(game::GameWork::kFrameCount)), &frame, sizeof(frame));
            g_frame30Loaded = false;
        }

        void rateChange(uint8_t *rdram, uint32_t was, uint32_t now)
        {
            if (was == now || (was != 1u && was != 2u))
                return;
            ram8(rdram, kFrameDivider) = static_cast<uint8_t>(now);
            if (now == 1u)
                g_frame30Base = gameFrames(rdram) + 1u, g_counterBase = gameFrames(rdram) + 1u;
            const uint32_t at = 0x5864F0u + 0x548u; // cEmManage D_005864F0
            float rate = 0.0f;
            std::memcpy(&rate, &ram8(rdram, at), sizeof(rate));
            if (!(rate > 0.0f && rate <= 2.0f))
                return;
            rate = now == 1u ? rate * 0.5f : rate * 2.0f;
            std::memcpy(&ram8(rdram, at), &rate, sizeof(rate));
        }

        void frameStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            pcsampler::noteMainContext(ctx);
            ++g_gameFrame;
            g_handlerDepth = 0; // no dispatcher's handler spans a frame (a load or a rate switch left one open)
            forgetStickAfterLoad();
            forgetRandAfterLoad(rdram);
            forgetClothAfterLoad(rdram);
            deadPanFrame(rdram, ctx, runtime);
            rouletteTargetFrame(rdram, ctx, runtime);
            if (const int request = g_fps60Request.exchange(-1); request >= 0)
            {
                g_fps60User = request != 0;
                g_fps60Shown.store(g_fps60User);
            }
            // Cutscenes run at the PS2's 30 FPS whatever the divider: while GameWork flags5E4 has EVENT (a cutscene is
            // running) the main loop waits GameWork +0x570 vblanks a frame (0x30AD70 / 0x30AE14), and the setter
            // 0x30D168 never stores less than 2. The 60 FPS mode's fixes assume one vblank a frame, so in a cutscene
            // they halved 33 ms frames: the cloth took its real step every other frame with a half step between (Gene's
            // coat and Olivia's hair jittered in the hotel cutscene, K Nov 1, overlay "Game 30 FPS"). The mode stands
            // down for the cutscene's frames (as when the player switches it), exactly the PS2's cutscene.
            {
                uint32_t sta = 0;
                std::memcpy(&sta, &ram8(rdram, game::GameWork::address(game::GameWork::kFlags5E4)), sizeof(sta));
                const bool cutscene = (sta & game::Sta::kEvent) != 0u;
                const bool want = g_fps60User && !cutscene;
                if (want != g_fps60)
                    setFps60(want, cutscene ? " (cutscene: the game's own 30 FPS wait)" : (g_fps60User ? " (cutscene over)" : ""));
            }
            if (g_rouletteRequest.exchange(0) != 0)
                applyRoulettePeriod();
            ps2_host_display::noteGameFrame();
            profileWindow();
            counterfinder::onFrame(rdram);
            hud::onFrame(rdram);
            characters::onFrame(rdram);
            npc_poses::onFrame(rdram, ctx, runtime);
            debug_tools::onFrame(rdram, ctx, runtime);
            sprint_traversal::onFrame(rdram, ctx, runtime); // Busta-Chan Jive: a window with a room jump starts it
            npc_tools::onFrame(rdram, ctx, runtime);
            hitbox_overlay::onFrame();
            bone_tweaks::onFrame(rdram, ctx, runtime);
            test_mode::onFrame(rdram, ctx, runtime); // KAMITE_TEST: the scripted run (dev-only)
            // The game's frame divider: 2 (the PS2's, written once at boot by func_0030B298) or 1 for 60 frames a
            // second. Written in both modes: a save state made at 60 carries a 1, and loaded at 30 nothing put the 2
            // back, so the game ran at 60 with none of the mode's fixes ("the old broken 60 FPS", Thinso, Oct 27).
            rateChange(rdram, ram8(rdram, kFrameDivider), g_fps60 ? 1u : 2u);
            ram8(rdram, kFrameDivider) = g_fps60 ? 1u : 2u;
            if (!g_fps60)
            {
                restoreInjected(rdram);
                l3Dash(rdram);
                rouletteDpad(rdram);
                return;
            }
            seqFrameStart(rdram);
            // The pnach writes the held value over the game's and reads it back the next frame as if it came from the
            // pad. The game does not rewrite these bytes every frame here, so a written 0xFF came back as a new full
            // deflection and was held again, forever (stick stuck at full right/left; the pnach's "input loop" with
            // PCSX2 overclock). A byte that still holds what was written last frame is the game's earlier reading:
            // it is put back and used as the reading. A held value is written only when it differs from the reading.
            for (int axis = 0; axis < 4; ++axis)
            {
                uint8_t &value = ram8(rdram, kStickBytes[axis]);
                if (g_stickInjected[axis] >= 0 && value == static_cast<uint8_t>(g_stickInjected[axis]))
                {
                    value = g_stickRaw[axis];
                    ++g_staleRestores;
                }
                g_stickInjected[axis] = -1;
                const uint8_t raw = value;
                g_stickRaw[axis] = raw;
                const bool wasHeld = g_stickHeld[axis] != 0u;
                const uint8_t last = g_stickLast[axis];
                if (raw == 0u || raw == 0xFFu)
                {
                    g_stickHeld[axis] = 1u;
                    g_stickLast[axis] = raw;
                }
                else
                    g_stickHeld[axis] = 0u;
                if (wasHeld && raw != last)
                {
                    value = last;
                    g_stickInjected[axis] = last;
                    ++g_holds;
                }
            }
            if (++g_frames % 3600u == 0u) // once a minute at 60 frames/s
                KM_INFO(Game, "fps60 stick hold: " << g_holds << " held frames, " << g_staleRestores
                                                   << " readings restored in the last 3600 frames; left stick now "
                                                   << int(ram8(rdram, kStickBytes[0])) << "," << int(ram8(rdram, kStickBytes[1])));
            if (g_frames % 3600u == 0u)
                g_holds = g_staleRestores = 0u;
            l3Dash(rdram);
            rouletteDpad(rdram);
        }

        // Replaces c.eq.s f0,f2 at 0x2C81C0: the condition is "same integer frame" (cvt.w.s of both).
        void hitboxCompare(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t last = FPU_CVT_W_S(ctx->f[0]);
            const int32_t current = FPU_CVT_W_S(ctx->f[2]);
            ctx->fcr31 = last == current ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
        }

        // Replaces sw v0,0x15B0(s1) at 0x1147CC (the player's roulette timer, decremented once per game frame): the
        // store happens once every g_roulettePeriod calls.
        void rouletteTimer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (gameFrames(rdram) % g_roulettePeriod.load(std::memory_order_relaxed) != 0u)
                return;
            const uint32_t value = GPR_U32(ctx, 2), address = GPR_U32(ctx, 17) + 0x15B0u;
            std::memcpy(&ram8(rdram, address), &value, sizeof(value));
        }

        // Replaces swc1 f20,0x5A8(s1) at 0x11481C in Gene's roulette state (func_00114608): while the timer counts,
        // the world runs at 0.03 (cEmManage_SetSpeedRate) and Gene's rate and motion step get the same 0.03 (f20; the
        // SetMotionStep call's delay slot passes it on). The cast pose's hold (seq +0x3AC & 0x100) lasts a fixed stretch
        // of motion frames, about 102 game frames at that step: with the double timer it ran out with half the time
        // left, the state ended by its motion (0x114954) and the wheel closed without the hovered move (K, Oct 30).
        // Double timer: Gene's step halves too, so the hold lasts as long as the timer.
        void rouletteCastRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_rouletteSlow.load(std::memory_order_relaxed))
                ctx->f[20] *= 0.5f;
            setRamF32(rdram, GPR_U32(ctx, 17) + 0x5A8u, ctx->f[20]);
        }

        // Replaces lbu v0,0x1C(s1) at 0x2AAA88 in cIDBase_move (0 = the layout's animations do not step): every
        // other frame the animations of all HUD and menu layouts (button prompts, the stick-wiggle prompt, pulses,
        // slides) are not stepped, so they play at the PS2's speed; positions are still worked out every frame.
        void uiAnimStep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint8_t value = skipFrame(rdram) ? 0u : ram8(rdram, GPR_U32(ctx, 17) + 0x1Cu);
            SET_GPR_U32(ctx, 2, value);
        }

        // Before slti v1,v0,1 at 0x3038BC in func_003035D0 (an effect emitter's emission count for this frame): the
        // every-frame emitters (mode 0, interval 0; the beql at 0x303880 lands here with $v1 = 0) emit on even game
        // frames only, so they emit once a PS2 frame like the others now that the children's frames step by 0.5
        // (recomp/kamite.toml, the fps60 effect patches). The slow-motion path arrives with $v1 != 0 and keeps its count.
        // Replaces `and v1,v1,a0` after each effect test of STA_WORLD_SLOW (a0 = 0x20000): at 60 every effect takes its
        // slow-motion path, which steps by the effect rate (0.5 at 60, below) instead of a whole PS2 frame
        // (recomp/kamite.toml "Effects at 60 FPS").
        void effectWorldSlow(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 3, (GPR_U32(ctx, 3) | 0x20000u) & GPR_U32(ctx, 4));
        }
        // The effects' colour fade at 60 (Nov 1; kamite.toml at 0x2FC7F0): RGBA x a per-frame factor each PS2 frame, so
        // the square root of each factor a vblank (two vblanks = one 30 FPS frame). Replaces mul.s f0,f0,f4 (R, with G
        // f6 and B f7 already loaded) and mul.s f3,f3,f5 (A).
        float fadeRoot(float factor) { return factor > 0.0f ? std::sqrt(factor) : factor; }
        void effectFadeRgb(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[4] = fadeRoot(ctx->f[4]);
            ctx->f[6] = fadeRoot(ctx->f[6]);
            ctx->f[7] = fadeRoot(ctx->f[7]);
            ctx->f[0] = ctx->f[0] * ctx->f[4];
        }
        void effectFadeA(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[3] = ctx->f[3] * fadeRoot(ctx->f[5]);
        }
        // Replaces sw zero,0x3BB8(v1) at 0x30F254, at the top of MoveEffect (before the effects step; Nov 1, the
        // Shockwave case shockwave30 / shockwave60). Gene's roulette moves call cEmManage_SetSpeedRate (0.1 / 0.05) to
        // slow the enemies, which also writes the effect rate 0x741DC0; at 30 the effects read that rate only on their
        // slow path, i.e. while GameWork flags5E4 STA_WORLD_SLOW (0x20000) is set. At 60 every effect takes the slow
        // path, so with the flag clear they ran at the enemies' slow rate (Gene's brace pieces in slow motion, the wave
        // late against its sound, the glow's growth stalled while its fade ran). Flag clear: the effect rate is the
        // plain 60 FPS step (1/k of a PS2 frame); flag set: the slowed rate stands (already x0.5, effectSlowRate).
        // The bullets cOmbe (func_001B93E8), Gene's subtypes 0x29..0x2F (Nov 6): `vadd.xyz vf4,vf4,vf5` = position +
        // velocity once a game frame with no rate (the enemies' bullets scale it by GetSpeedRate): half the velocity a
        // vblank at 60. For k: 1/k.
        void bulletVaddHalf(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            alignas(16) float a[4], b[4];
            _mm_store_ps(a, ctx->vu0_vf[4]);
            _mm_store_ps(b, ctx->vu0_vf[5]);
            for (int i = 0; i < 3; ++i)
                a[i] += 0.5f * b[i];
            ctx->vu0_vf[4] = _mm_load_ps(a);
        }

        void effectRateForFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t zero = 0;
            std::memcpy(&ram8(rdram, GPR_U32(ctx, 3) + 0x3BB8u), &zero, sizeof(zero));
            uint32_t flags = 0;
            std::memcpy(&flags, &ram8(rdram, 0x747A84u), sizeof(flags));
            if ((flags & 0x20000u) == 0u)
                setRamF32(rdram, 0x741DC0u, 0.5f);
        }
        // Replaces swc1 f12,0x1DC0(v1) in cEmManage_SetSpeedRate: the effects' slow-motion rate, half at 60.
        void effectSlowRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const float value = ctx->f[12] * 0.5f;
            std::memcpy(&ram8(rdram, GPR_U32(ctx, 3) + 0x1DC0u), &value, sizeof(value));
        }

        // Replaces addiu v0,v1,-1 at 0x1F6F8C in TimedButton_tick (an action button prompt's time, +0x32): counts
        // down every other frame, so timed prompts (grab escapes and the like) last as long as on the PS2.
        void buttonTimer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t value = GPR_S32(ctx, 3);
            SET_GPR_S32(ctx, 2, skipFrame(rdram) ? value : value - 1);
        }

        // Gene's timer block (the player update func_00108AB0, 0x108D08..0x10917C): every countdown he keeps is lowered
        // by 1 each game frame, so at 60 FPS every one of them ran in half the time (upper-body dodge +0x1603 10 frames
        // -> its head/chest hurt volumes back after 1/6 s instead of 1/3; dodge timer +0x1618; combo timer +0x1616;
        // +0x648 10..90; the stick windows +0x621..0x628 / +0x1600..0x1602; +0x5F8, +0x5FC, float +0x15D0). They now
        // count every other frame: the `addiu v0,v1,-1` sites take km_fps60_button_timer's rule (km_fps60_countdown),
        // the three other forms are below. claude/fps60-notes.md "Families".
        void countdownSelf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // addiu v0,v0,-1 (+0x5FC at 0x108DFC)
        {
            const int32_t value = GPR_S32(ctx, 2);
            SET_GPR_S32(ctx, 2, skipFrame(rdram) ? value : value - 1);
        }
        // addiu rN,rN,-1 on another register (the dodge state's +0x15B4 at 0x10D2CC: a0).
        template <int Reg>
        void countdownReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t value = GPR_S32(ctx, Reg);
            SET_GPR_S32(ctx, Reg, skipFrame(rdram) ? value : value - 1);
        }
        // Gene's charged attacks (func_0010D3A8 / func_0010E5B8, case charged_punch): while the button is held the
        // charge +0x15C4 (4.0 or 1.0) drains 0.1 / 0.025 a frame with his rate held at that same value (the slow wind-up),
        // and the power +0x15C0 grows 0.05 a frame (sound 0x11B at 1.1, +1.0 once the charge is out). Per-frame float
        // steps: at 60 the hold lasted half as long (1.33 -> 0.67 s for the charged punch, id 0x55) and the wind-up got
        // two thirds as far. The sub.s / add.s take the full step every other frame (the countdown rule), so the
        // floats run through the same values as at 30 (half steps rounded the charge to -0 one step early: power 4.00
        // against 4.05); the step register keeps its value (it is also the rate stored after the sub.s).
        template <int Dst, int A, int B>
        void floatSubParity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[Dst] = skipFrame(rdram) ? ctx->f[A] : ctx->f[A] - ctx->f[B];
        }
        template <int Dst, int A, int B>
        void floatAddParity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[Dst] = skipFrame(rdram) ? ctx->f[A] : ctx->f[A] + ctx->f[B];
        }
        // A per-frame phase step taken at half size (an oscillation, smooth at 60 where the parity form would hold
        // each value for two frames): Gene's coat wind in the roulette (func_00100CE0, phase 0x3BC740 += 0.8 a frame) and
        // the cloth step's swing phase (func_00104010, work +0x48 += frand x 1.2, and x work +0x70 in the wind).
        template <int Dst, int A, int B>
        void floatAddHalf(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[Dst] = ctx->f[A] + ctx->f[B] * 0.5f;
        }
        // The cloth step func_00104010 (Gene's coat, the enemies' cloth) is a Verlet solver with no time factor: per node
        // vel.y -= gravity, pos += vel + the wind push, spring corrections x k, vel = (pos - old) x damp, every frame. At 60
        // it ran on a doubled clock (the coat swung out twice as fast and harder). The half step that keeps the same pose
        // and the same motion per second: accelerations (gravity, push) x 1/4, damp -> sqrt(damp), k -> 1 - sqrt(1 - k).
        //
        // Those constants alone were not enough (the coat still moved 1.4-1.6x as much per second, Oct 31): the springs
        // are position corrections that become velocity, which no constant change makes equal over two steps. So the
        // cloth runs at the 30 FPS pace (K, Oct 31: look correct, interpolated if it has to be): on even game frames
        // the game's own step (these hooks pass the original values) from the state the last even step left, on odd
        // frames a half step for the picture only (these hooks scale), which the next even step undoes (clothBegin).
        bool g_clothDisplay = false; // this call of the cloth step is an odd frame's half step
        template <int Dst, int A, int B>
        void clothSubQuarter(uint8_t *, R5900Context *ctx, PS2Runtime *) // sub.s Dst,A,B: the gravity step
        {
            ctx->f[Dst] = ctx->f[A] - ctx->f[B] * (g_clothDisplay ? 0.25f : 1.0f);
        }
        template <int Dst, int A, int B>
        void clothAddQuarter(uint8_t *, R5900Context *ctx, PS2Runtime *) // add.s Dst,A,B: a push's swing factor
        {
            ctx->f[Dst] = (ctx->f[A] + ctx->f[B]) * (g_clothDisplay ? 0.25f : 1.0f);
        }
        template <int Dst, int A, int B>
        void clothAddHalf(uint8_t *, R5900Context *ctx, PS2Runtime *) // add.s Dst,A,B: the swing phase steps
        {
            ctx->f[Dst] = ctx->f[A] + ctx->f[B] * (g_clothDisplay ? 0.5f : 1.0f);
        }
        void clothDamp(uint8_t *, R5900Context *ctx, PS2Runtime *) // mfc1 t0,f20 at 0x105F24: the velocity's damping
        {
            const float damp = g_clothDisplay ? std::sqrt(std::max(ctx->f[20], 0.0f)) : ctx->f[20];
            uint32_t bits = 0;
            std::memcpy(&bits, &damp, sizeof(bits));
            SET_GPR_U32(ctx, 8, bits);
        }
        template <int Dst>
        void clothSpring(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // lwc1 Dst,0x4C(s4): the springs' k
        {
            float k = 0.0f;
            std::memcpy(&k, &ram8(rdram, GPR_U32(ctx, 20) + 0x4Cu), sizeof(k));
            ctx->f[Dst] = (!g_clothDisplay || k >= 1.0f) ? k : 1.0f - std::sqrt(std::max(1.0f - k, 0.0f));
        }

        // The state an even step left, kept over the odd frame's half step (per cloth, by its work block): each node's
        // velocity, position and previous position (+0x30..+0x5F of 0xA0; its anchor +0x80 is written by the owner every
        // frame and stays), the swing phase (work +0x48), and the owner's move that frame (Gene's +0x650, which the step
        // adds to every node: the even step must carry both frames' moves, as the 30 FPS step does).
        struct ClothSnap
        {
            uint32_t frame = 0, nodes = 0, count = 0, phase = 0;
            float shift[3] = {};
            std::vector<uint32_t> words; // count x 12
        };
        constexpr uint32_t kClothNode = 0xA0u, kClothKept = 0x30u, kClothKeptWords = 12u;
        std::unordered_map<uint32_t, ClothSnap> g_cloth;
        bool g_clothRestored = false;

        uint32_t clothNodes(uint8_t *rdram, uint32_t work)
        {
            uint32_t count = 0, list = 0, top = 0;
            std::memcpy(&count, &ram8(rdram, work), sizeof(count));
            std::memcpy(&list, &ram8(rdram, work + 4u), sizeof(list));
            for (uint32_t i = 0; i < std::min(count, 256u); ++i)
                top = std::max<uint32_t>(top, ram8(rdram, list + i) + 1u);
            return top;
        }

        // Before sqc2 vf0,0x70(sp) at 0x104064, the cloth step's entry (func_00104010: a0 the cloth object, s4 its work
        // block, a2 its nodes). Odd frame: keep the state. Even frame: put back the state the last even step left (if the
        // odd frame kept it), moved by the owner's odd-frame move.
        void clothBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t cloth = GPR_U32(ctx, 4), work = GPR_U32(ctx, 20), nodes = GPR_U32(ctx, 6);
            const uint32_t frame = gameFrames(rdram);
            g_clothDisplay = skipFrame(rdram);
            if (!g_clothDisplay)
            {
                const auto it = g_cloth.find(work);
                if (it == g_cloth.end())
                    return;
                const ClothSnap &snap = it->second;
                if (snap.frame + 1u == frame && snap.nodes == nodes && clothNodes(rdram, work) == snap.count)
                {
                    for (uint32_t i = 0; i < snap.count; ++i)
                    {
                        const uint32_t node = nodes + i * kClothNode;
                        std::memcpy(&ram8(rdram, node + kClothKept), &snap.words[i * kClothKeptWords], kClothKeptWords * 4u);
                        for (int axis = 0; axis < 3; ++axis)
                            setRamF32(rdram, node + 0x40u + axis * 4u, ramF32(rdram, node + 0x40u + axis * 4u) + snap.shift[axis]);
                    }
                    std::memcpy(&ram8(rdram, work + 0x48u), &snap.phase, sizeof(snap.phase));
                }
                g_cloth.erase(it);
                return;
            }
            ClothSnap &snap = g_cloth[work];
            snap.frame = frame;
            snap.nodes = nodes;
            snap.count = clothNodes(rdram, work);
            std::memcpy(&snap.phase, &ram8(rdram, work + 0x48u), sizeof(snap.phase));
            uint32_t owner = 0;
            std::memcpy(&owner, &ram8(rdram, work + 0x58u), sizeof(owner));
            if (owner == 0u)
                owner = cloth;
            for (int axis = 0; axis < 3; ++axis) // the step adds +0x650 for the player (pl00) only
                snap.shift[axis] = isGene(rdram, owner) ? ramF32(rdram, owner + 0x650u + axis * 4u) : 0.0f;
            snap.words.resize(static_cast<size_t>(snap.count) * kClothKeptWords);
            for (uint32_t i = 0; i < snap.count; ++i)
            {
                const uint32_t node = nodes + i * kClothNode;
                std::memcpy(&snap.words[i * kClothKeptWords], &ram8(rdram, node + kClothKept), kClothKeptWords * 4u);
                // The kept velocity is a whole 30 FPS step's; the half step moves by half of it (pos += vel / 2 +
                // gravity / 4). With the whole velocity the odd frame showed a full step ahead and the even frame
                // pulled the coat back: the jitter K filmed (b971051).
                for (int axis = 0; axis < 3; ++axis)
                    setRamF32(rdram, node + 0x30u + axis * 4u, ramF32(rdram, node + 0x30u + axis * 4u) * 0.5f);
            }
        }

        void forgetClothAfterLoad(uint8_t *rdram)
        {
            static uint32_t loads = 0;
            const uint32_t now = ps2x::savestate::loadCount();
            if (now != loads)
            {
                loads = now;
                if (!g_clothRestored)
                    g_cloth.clear();
                g_clothRestored = false;
            }
            const uint32_t frame = gameFrames(rdram);
            for (auto it = g_cloth.begin(); it != g_cloth.end();)
                it = (frame - it->second.frame > 2u) ? g_cloth.erase(it) : std::next(it);
        }

        void installClothState() // "kamite.cloth60": the kept states go into save states, so test cases replay them
        {
            ps2x::savestate::registerExtension(
                "kamite.cloth60",
                [](ps2x::state::Writer &out) {
                    uint32_t version = 1u;
                    std::vector<uint32_t> words;
                    for (const auto &[work, snap] : g_cloth)
                    {
                        uint32_t shift[3] = {};
                        std::memcpy(shift, snap.shift, sizeof(shift));
                        const uint32_t head[8] = {work, snap.frame, snap.nodes, snap.count, snap.phase, shift[0], shift[1], shift[2]};
                        words.insert(words.end(), head, head + 8);
                        words.insert(words.end(), snap.words.begin(), snap.words.end());
                    }
                    ps2x::state::fields(out, version, words);
                },
                [](ps2x::state::Reader &in) {
                    uint32_t version = 0u;
                    std::vector<uint32_t> words;
                    ps2x::state::fields(in, version, words);
                    g_cloth.clear();
                    for (size_t i = 0; i + 8u <= words.size();)
                    {
                        ClothSnap snap;
                        const uint32_t work = words[i];
                        snap.frame = words[i + 1u], snap.nodes = words[i + 2u], snap.count = words[i + 3u], snap.phase = words[i + 4u];
                        std::memcpy(snap.shift, &words[i + 5u], sizeof(snap.shift));
                        i += 8u;
                        const size_t n = static_cast<size_t>(snap.count) * kClothKeptWords;
                        if (snap.count > 256u || i + n > words.size())
                            break;
                        snap.words.assign(words.begin() + static_cast<std::ptrdiff_t>(i), words.begin() + static_cast<std::ptrdiff_t>(i + n));
                        i += n;
                        g_cloth[work] = std::move(snap);
                    }
                    g_clothRestored = true;
                });
        }

        // A countdown whose load and decrement both sit in delay slots, stored by a plain sh (Gene's grab finishers and
        // mash roulette moves, +0x56E: the 15 / 30 frames Gene follows the held enemy's per-frame move +0x550, phases
        // 57..107; case 2026-10-31_11-18-03): replaces the sh, storing the old value on skipped frames.
        template <int Base, int New, int Old, uint32_t Offset>
        void countdownStore16(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint16_t value = static_cast<uint16_t>(skipFrame(rdram) ? GPR_U32(ctx, Old) : GPR_U32(ctx, New));
            std::memcpy(&ram8(rdram, GPR_U32(ctx, Base) + Offset), &value, sizeof(value));
        }
        // addiu Dst,Src,-1 (the enemy countdowns, generated hooks in kamite.toml).
        template <int Dst, int Src>
        void countdownPair(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t value = GPR_S32(ctx, Src);
            SET_GPR_S32(ctx, Dst, skipFrame(rdram) ? value : value - 1);
        }
        void countdownFloat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // sub.s f0,f1,f0 (+0x15D0 -= 1.0 at 0x108DE8)
        {
            ctx->f[0] = skipFrame(rdram) ? ctx->f[1] : ctx->f[1] - ctx->f[0];
        }
        // Gene's speed rate +0x5A8 (1.0 / 1.3 / 1.5 / 1.6 / 0.1, set by his states; 1.0..2.5 by level in the mash states)
        // is the 30 FPS value at both rates: the game hands it to SetMotionStep, and moveMotion already halves every
        // motion's advance at 60, so it must stay unscaled. Every per-frame use of it is halved at its reader instead
        // (claude/fps60-notes.md family 2). Enemies and objects on cEmManage's rate have 0.5 at 60 already.
        bool isGene(uint8_t *rdram, uint32_t object)
        {
            uint32_t gene = 0;
            std::memcpy(&gene, &ram8(rdram, game::GameWork::address(game::GameWork::kPlayerObj)), sizeof(gene));
            return object != 0u && object == gene;
        }
        // The post-hit guard (FlashDriver_resetFlash 0x1FD4F8 mul.s f1,f1,f0: +0x54C -= +0x5A8 x 0x747A14, a frame; the
        // hit loop func_001FCF20 skips a target while it is > 0). Gene's rate is halved whatever it is; every class
        // whose update never writes +0x5A8 keeps 1.0 and takes 0.5 (Door-kun's second roundhouse contact); objects on
        // the speed rate are 0.5 at 60 already.
        void flashGuardRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            float rate = ctx->f[1];
            if (isGene(rdram, GPR_U32(ctx, 2)) || rate == 1.0f)
                rate *= 0.5f;
            ctx->f[1] = rate * ctx->f[0];
        }
        // The God Hand (tension) drain: pl00 0x123550 calls the gauge helper func_2A9AD0(D_005CB000, 1) with
        // f12 = drain x Gene's speed rate +0x5A8 (loaded at 0x123538, multiplied in the call's delay slot). Hook before
        // the addiu at 0x12354C (plain; f12 still holds the rate): halved at 60. K timed the meter: 12 s at 30, 6 s at
        // 60 before the hook (Oct 29).
        void tensionDrainRate(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[12] *= 0.5f;
        }
        // Gene's rate just loaded into f0 for a per-frame step (the spin mash state func_001162C0: its turn toward the
        // target, rate x 22.5 degrees a frame, and its float timer +0x15C0 -= rate): halved at 60. Hook before the
        // instruction after the lwc1.
        void geneRateHalfF0(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[0] *= 0.5f;
        }
        // Gene's stick turn step (f1) at the heading's load in mode 0 phases 0-3 (kamite.toml): a fixed step a frame,
        // halved at 60.
        void stickTurnHalfF1(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[1] *= 0.5f;
        }
        // An enemy's speed rate +0x5A8 read as a state ("slowed": below 1.0) rather than a step. Enemies on cEmManage's
        // rate are 0.5 at 60 (the 0x29059C patch), so every such test fired all the time: the devil (kind 0x220) only
        // taunted within 3 m (func_002705D8 0x2707F8, case 2026-10-30_19-36-08), func_0020CAC8 / 0020CD00 (the guard /
        // evade checks) always said no, func_00217980 0x218B9C took the slowed branch, func_00256898 never saw its rate
        // back at 1.0, and cSndSeVoice_Play / _Update lowered the owner's voice. Hook after the load: the rate the 30 FPS
        // game would see (x2 for an object in cEmManage's list; Gene and objects off the manager's rate are left alone).
        bool inEnemyList(uint8_t *rdram, uint32_t obj)
        {
            uint32_t slot = 0;
            std::memcpy(&slot, &ram8(rdram, 0x5864F0u + 0x500u), sizeof(slot));
            for (int guard = 0; guard < 128 && slot >= 0x100000u && slot < 0x2000000u - 16u; ++guard)
            {
                uint32_t object = 0, next = 0;
                std::memcpy(&object, &ram8(rdram, slot + 8u), sizeof(object));
                std::memcpy(&next, &ram8(rdram, slot + 4u), sizeof(next));
                if (object == obj)
                    return obj != 0u;
                slot = next;
            }
            return false;
        }
        // cEmManage_SetSpeedRate (0x292F38, called every frame by Gene's slow-motion states: the dodge's slow-mo, his
        // techniques' slow parts): manager +0x548 = f12 (0.1, 0.05, 0.029...), then every enemy's rate = +0x548 x
        // GameWork +0x574 and +0x548 back to 1.0 (0.5 at 60, the 0x29059C patch). The slow value replaced the 0.5, so at
        // 60 slowed enemies (and the manager's +0x544 timer) ran twice as fast as at 30 (case 2026-10-30_21-59-10: 0.029
        // at both rates). Replaces the swc1: half at 60. The effect and object rates it also sets stay 1.0-based.
        void enemySlowRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t caller = GPR_U32(ctx, 31);
            if (caller >= 0x108000u && caller < 0x128000u) // Gene's states (pl00)
            {
                g_slowRateAt = gameFrames(rdram);
                g_slowRateManager = GPR_U32(ctx, 4);
                g_slowRateValue = ctx->f[12];
            }
            const float value = ctx->f[12] * 0.5f;
            std::memcpy(&ram8(rdram, GPR_U32(ctx, 4) + 0x548u), &value, sizeof(value));
        }
        // cEmManage's update (func_00290250) hands every enemy the manager's rate (+0x5A8 = +0x548 x GameWork +0x574,
        // 0x290578) and then resets it (+0x548 = 1.0, 0.5 at 60: 0x2905AC); a slow-motion state keeps calling
        // cEmManage_SetSpeedRate every frame. It runs before Gene, so at 30 a rate he sets in frame k reaches the enemies
        // in frame k + 1. At 60 a rate set on the tick reached them on the second half, a vblank early (shockwave30,
        // Nov 1: the enemies slowed one vblank after Gene instead of one 30 FPS frame). The second half hands out what
        // the tick did and leaves +0x548 alone, so the tick's and the second half's calls reach the next tick, as the
        // 30 FPS frame's call reaches the next frame. For k: hold on the k-1 frames after the tick.
        uint32_t g_managerRateAt = ~0u;
        float g_managerRate = 0.5f;
        void managerRateRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // replaces lwc1 f1,0x548(s0)
        {
            const uint32_t counter = gameFrames(rdram);
            if (thinkSkipped(rdram) && g_managerRateAt + 1u == counter)
            {
                ctx->f[1] = g_managerRate;
                return;
            }
            ctx->f[1] = ramF32(rdram, GPR_U32(ctx, 16) + 0x548u);
            if (!thinkSkipped(rdram))
                g_managerRate = ctx->f[1], g_managerRateAt = counter;
        }
        void managerRateReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) // replaces swc1 f0,0x548(s0)
        {
            if (thinkSkipped(rdram) && g_managerRateAt + 1u == gameFrames(rdram))
                return;
            setRamF32(rdram, GPR_U32(ctx, 16) + 0x548u, ctx->f[0]);
        }
        // The grab reactions Gene's finishers and mash moves hold the enemy in (mode 1 phases 31 / 33 / 34 / 36 / 37 / 38 /
        // 39: func_0025C1D0 .. func_0025E248) copy Gene's +0x5A8 into the enemy's every frame and pass it to
        // SetMotionStep. Gene's rate is the 30 FPS value at 60; an enemy's +0x5A8 is half of it (cEmManage, family 2).
        // SetMotion's hook (0x2A85CC, x2 for enemies) then gave the motion each of these states sets a double step on
        // its first frame (Nov 4, mash case 2026-11-03_14-53-38: the held enemy 0.7 a vblank into the phase 59 loop, a
        // 30 FPS frame ahead of Gene's from there). Replaces the swc1 f0,0x5A8(s0) in SetMotionStep's delay slot (f12
        // already holds the 30 FPS value for the step): the enemy keeps half, as every other enemy.
        void grabRateCopy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            setRamF32(rdram, GPR_U32(ctx, 16) + 0x5A8u, ctx->f[0] * 0.5f);
        }
        // Gene held and pummelled (mode 1 phase 24, func_0011FD10 step 1: Azel's pummel, phase 90): each frame he reads
        // the holder's seq level (+0x3AC & 3, the holder's blow span) and, while it is set, rolls a new reaction motion,
        // sets it and steps it. The span lasts one 30 FPS frame at 30; at 60 Gene saw it on two vblanks, so every
        // reaction was set twice and started a 30 FPS frame late (case "Azel pummel logic": mframe 0, 0, 0, 0.5 where 30
        // shows 0, 1; all 11 reactions, Nov 4). The only pl00 read of another object's seq word. Replaces andi v0,v0,0x3
        // at 0x11FF00: zero on second halves (the holder's word is read once per 30 FPS frame, on the tick).
        void pummelHolderSeq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 2, skipFrame(rdram) ? 0u : (GPR_U32(ctx, 2) & 3u));
        }
        template <int ObjReg, int FReg>
        void enemyRate30(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (inEnemyList(rdram, GPR_U32(ctx, ObjReg)))
                ctx->f[FReg] *= 2.0f;
        }
        // addiu v0,v0,1 on a counter that counts game frames up (the mash states' +0x15B0: frames since the last
        // press): every other frame at 60, the mirror of km_fps60_countdown.
        void countupSelf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t value = GPR_S32(ctx, 2);
            SET_GPR_S32(ctx, 2, skipFrame(rdram) ? value : value + 1);
        }
        // addu Dst,A,B where B is a whole per-frame step (an integer phase): the tick adds B - B/2, the second half B/2,
        // so a 30 FPS frame adds B as on the PS2 and the phase still moves every vblank (the world lights' rotation
        // byte, cWorldLight_Move 0x2D7DF4). For k: B split into k parts.
        template <int Dst, int A, int B>
        void stepSplit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const int32_t step = GPR_S32(ctx, B), half = step / 2;
            SET_GPR_S32(ctx, Dst, GPR_S32(ctx, A) + (skipFrame(rdram) ? half : step - half));
        }
        // `lw v0,X(base)` before a countdown whose `addiu v0,v0,-1` sits in a branch delay slot (no hook there): on the
        // skipped frames the loaded value is raised by one and the decrement stores it back unchanged (the zero test
        // is not affected: a non-zero value stays non-zero). Gene's +0x5F8 (0x108D08), cEmManage +0x510/+0x514/+0x530, the
        // damage manager's hit stop (0x1FDFF8).
        template <int Base, uint32_t Offset>
        void countdownLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + ((GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu), 4);
            SET_GPR_U32(ctx, 2, skipFrame(rdram) && value != 0u ? value + 1u : value);
        }

        // countdownLoad also raising a 0 on the skipped frames, for `if (n > 0) n--; else { action; n = reload }`: the
        // action then waits for the 30 FPS frame too (r505's spark pattern).
        template <int Base, uint32_t Offset>
        void countdownLoadAll(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + ((GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu), 4);
            SET_GPR_U32(ctx, 2, skipFrame(rdram) ? value + 1u : value);
        }

        // countdownLoad for any load: `l[w|hu|bu] Dst,Offset(Base)` (Bytes 4 / 2 / 1) before a decrement in a delay slot.
        template <int Base, uint32_t Offset, int Dst, int Bytes>
        void countdownLoadAny(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + ((GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu), Bytes);
            SET_GPR_U32(ctx, Dst, skipFrame(rdram) && value != 0u ? value + 1u : value);
        }

        // The 1 -> 0 step (Nov 7 census of the 472 countdown hooks, claude/countdown-step.md): a hooked
        // countdown steps on the tick, so on the second half it already reads 0 where the 30 FPS frame still had 1.
        // `if (n > 0) n--; else { act }` then acted a vblank early, and in a chain of states (an enemy's wait -> attack
        // -> wait) every wait after the first came out a whole 30 FPS frame short (r505's sparks: 5 steps ahead in
        // 17 s). The tick notes each such counter it reads at the threshold (1, or 0 for `if (n < 0)` guards); on
        // the next vblank the counter one below it reads as the threshold again, so the second half runs the
        // counting path as the 30 FPS frame did. Only for sites with no one-shot test of the new value after the
        // decrement (those would fire twice): the census sorted them.
        struct CountdownNotes
        {
            uint32_t frame = 0xFFFFFFFFu, count = 0;
            uint32_t addr[128];
        };
        CountdownNotes g_cdNotes;
        void cdNote(uint8_t *rdram, uint32_t addr)
        {
            const uint32_t now = gameFrames(rdram);
            if (g_cdNotes.frame != now)
            {
                g_cdNotes.frame = now;
                g_cdNotes.count = 0;
            }
            for (uint32_t i = 0; i < g_cdNotes.count; ++i)
                if (g_cdNotes.addr[i] == addr)
                    return;
            if (g_cdNotes.count < 128u)
                g_cdNotes.addr[g_cdNotes.count++] = addr;
        }
        bool cdNoted(uint8_t *rdram, uint32_t addr)
        {
            if (g_cdNotes.frame + 1u != gameFrames(rdram))
                return false;
            for (uint32_t i = 0; i < g_cdNotes.count; ++i)
                if (g_cdNotes.addr[i] == addr)
                    return true;
            return false;
        }
        // countdownLoadAny (unsigned loads) plus the 1 -> 0 step: a 0 the tick left behind is raised too.
        template <int Base, uint32_t Offset, int Dst, int Bytes>
        void countdownLoadExact(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t addr = (GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu;
            uint32_t value = 0;
            std::memcpy(&value, rdram + addr, Bytes);
            if (!skipFrame(rdram))
            {
                if (value == 1u)
                    cdNote(rdram, addr);
                SET_GPR_U32(ctx, Dst, value);
                return;
            }
            SET_GPR_U32(ctx, Dst, value != 0u || cdNoted(rdram, addr) ? value + 1u : value);
        }
        // A signed load (lh / lb) that only tests a countdown whose decrement is hooked separately (`lh v0; beqz v0;
        // lhu v1; addiu v0,v1,-1`): Threshold 1 for beqz / blez / bgtz guards, 0 for bgez (`if (n < 0) act; n--`).
        // On the second half after the tick stepped it from the threshold, the guard sees the threshold again.
        template <int Base, uint32_t Offset, int Dst, int Bytes, int Threshold>
        void countdownGuard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t addr = (GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu;
            int32_t value = 0;
            if constexpr (Bytes == 2)
            {
                int16_t v = 0;
                std::memcpy(&v, rdram + addr, 2);
                value = v;
            }
            else
            {
                int8_t v = 0;
                std::memcpy(&v, rdram + addr, 1);
                value = v;
            }
            if (!skipFrame(rdram))
            {
                if (value == Threshold)
                    cdNote(rdram, addr);
            }
            else if (value == Threshold - 1 && cdNoted(rdram, addr))
                value = Threshold;
            SET_GPR_S32(ctx, Dst, value);
        }

        // A toggle flipped every game frame while a SEQ window is open (Nov 8, T1's "Elvis grab munch munch" case, room
        // 0x504: the demon's bite in Gene's grabbed state, mode 1 phase 28): `if (seq & 1) { if (n) { n = 0; act } else
        // n = 1; } else n = 1` acts every other game frame while the window lasts (bites on motion frames 7 and 9 at
        // 30). At 60 it flipped every vblank, so it acted every 30 FPS frame (7, 8 and 9: three bites for two). Replaces
        // the `lw Dst,Offset(Base)` of n inside the window: on a second half the code sees 0 (it stores 1 and does not
        // act) and the value n had is noted; the next tick sees the noted value, so n flips once a 30 FPS frame. A tick
        // with the window closed never reaches the load and stores 1 as the PS2 does. Gene's grabbed states 21, 24, 25
        // and 28 (claude/fps60-notes.md family 20).
        struct ToggleNotes
        {
            uint32_t frame = 0xFFFFFFFFu, count = 0;
            uint32_t addr[16], value[16];
        };
        ToggleNotes g_toggleNotes;
        template <int Base, uint32_t Offset, int Dst>
        void toggleLoad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t addr = (GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu;
            uint32_t value = 0;
            std::memcpy(&value, rdram + addr, 4);
            const uint32_t now = gameFrames(rdram);
            if (!skipFrame(rdram))
            {
                if (g_toggleNotes.frame + 1u == now)
                    for (uint32_t i = 0; i < g_toggleNotes.count; ++i)
                        if (g_toggleNotes.addr[i] == addr)
                        {
                            value = g_toggleNotes.value[i];
                            break;
                        }
                SET_GPR_U32(ctx, Dst, value);
                return;
            }
            if (g_toggleNotes.frame != now)
            {
                g_toggleNotes.frame = now;
                g_toggleNotes.count = 0;
            }
            bool noted = false;
            for (uint32_t i = 0; i < g_toggleNotes.count && !noted; ++i)
                noted = g_toggleNotes.addr[i] == addr;
            if (!noted && g_toggleNotes.count < 16u)
            {
                g_toggleNotes.addr[g_toggleNotes.count] = addr;
                g_toggleNotes.value[g_toggleNotes.count++] = value;
            }
            SET_GPR_U32(ctx, Dst, 0u);
        }

        // Scripted waits (task family, Nov 2): cTaskWork_sleep(task, frames) (0x2D5AA0) stores the game frames a script
        // task sleeps (task +0xE, `sh a1,0xE(a0)` at 0x2D5AAC); cTaskWork_step (0x2D5838) counts it down once a game
        // frame and wakes the task on the frame it reaches 0. Room scripts, soft events and the scenario wait this way
        // (r301's lever: sleep 25 + 55 before the cannon game may stop the world; at 60 it ran ahead of the event
        // camera, the camera froze, waitCam never returned and the soft event's key stop locked the game). A duration
        // (2 or more) becomes 2N vblanks = N 30 FPS frames (k: N x k). sleep(1) stays one game frame: the scripts use it
        // as "poll again next frame" (r301: all 13 are wait-until loops), so a condition or a press is seen on the
        // vblank it happens; a counted sleep(1) loop would run at 2x.
        // Scroll-layer fades in room scripts (Nov 7 census of the 79 room DLLs: r101 x5, r103, r104, r403, r604, r607 x2,
        // r701 x4, r803, r804, r805): `alpha -/+= step; setAlphaScrollLayer(layer, alpha); sleep(1); while (alpha <
        // end)` - a counted loop, so the fade ran in half the time at 60. Its shape: jal setAlphaScrollLayer (0x2DB318)
        // 0x18 before the return address, a float compare (c.cond.s) at it. Wait-until polls after a fade are not this
        // shape (r104 / r403: jal 0x295CA8 after the sleep).
        bool fadeLoopSleep(uint8_t *rdram, uint32_t ra)
        {
            if (ra < 0x100018u || ra >= 0x2000000u)
                return false;
            uint32_t before = 0, after = 0;
            std::memcpy(&before, rdram + ra - 0x18u, 4);
            std::memcpy(&after, rdram + ra, 4);
            return before == (0x0C000000u | (0x2DB318u >> 2)) && (after >> 21) == 0x230u && (after & 0x30u) == 0x30u;
        }
        void taskSleep(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t frames = GPR_U32(ctx, 5) & 0xFFFFu;
            if (frames >= 2u)
                SET_GPR_U32(ctx, 5, std::min(frames * 2u, 0xFFFFu));
            else if (frames == 1u && motion60(rdram) && fadeLoopSleep(rdram, GPR_U32(ctx, 31)))
                SET_GPR_U32(ctx, 5, skipFrame(rdram) ? 1u : 2u);
        }

        // Drivers that step a phase and a timer once a game frame and act on timer values (a beep at 30, a sound at 10),
        // so halving the timer would fire those twice (room DLLs, Nov 2): their "running" bit test `lwu v0,0(Base)`
        // reads the bits in Mask clear on second halves, so the whole driver runs once per 30 FPS frame (the ID anims it
        // starts run at their own rate). r301's start countdown (r301CannonGameId::ExecCountDownAnim, K: the 3-2-1 ran
        // twice as fast), r040's FIGHT / YOU WIN / TIME UP banners (PetitDevilId::ExecFight / ExecYouWin / ExecTimeUp).
        template <int Base, uint32_t Mask>
        void tickOnlyGate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + (GPR_U32(ctx, Base) & 0x01FFFFFFu), 4);
            SET_GPR_U32(ctx, 2, skipFrame(rdram) ? (value & ~Mask) : value);
        }

        // A countdown whose load and decrement both sit in delay slots: its store `sw Src,Offset(Base)` (Src = old - 1 on
        // every path to it) keeps the old value on second halves (r501 +0x14 at 0x79F354).
        template <int Base, uint32_t Offset, int Src>
        void countdownStore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t value = GPR_U32(ctx, Src) + (skipFrame(rdram) ? 1u : 0u);
            std::memcpy(rdram + ((GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu), &value, 4);
        }

        // The stick-wiggle button prompt (cCockActButton move, type 4, 0x13BF08..0x13C020) swings its stick icon a
        // fixed 30 degrees and 5 units per frame (a 4-frame cycle). At 60 frames/s the swing of every other frame is
        // undone: the icon's angle (+0x48) and x (+0x38) of the entry at +0xD8 and the swing direction (0x3BD6E0).
        constexpr uint32_t kWiggleDirection = 0x3BD6E0u;
        uint32_t g_wiggleEntry = 0, g_wiggleAngle = 0, g_wiggleX = 0;
        uint8_t g_wiggleDirection = 0;

        uint32_t ram32(uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, &ram8(rdram, address), sizeof(value));
            return value;
        }

        void wiggleBegin(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_wiggleEntry = ram32(rdram, GPR_U32(ctx, 6) + 0xD8u);
            g_wiggleAngle = ram32(rdram, g_wiggleEntry + 0x48u);
            g_wiggleX = ram32(rdram, g_wiggleEntry + 0x38u);
            g_wiggleDirection = ram8(rdram, kWiggleDirection);
        }

        void wiggleEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!skipFrame(rdram) || g_wiggleEntry == 0u || ram32(rdram, GPR_U32(ctx, 6) + 0xD8u) != g_wiggleEntry)
                return;
            std::memcpy(&ram8(rdram, g_wiggleEntry + 0x48u), &g_wiggleAngle, sizeof(g_wiggleAngle));
            std::memcpy(&ram8(rdram, g_wiggleEntry + 0x38u), &g_wiggleX, sizeof(g_wiggleX));
            ram8(rdram, kWiggleDirection) = g_wiggleDirection;
        }

        // Replaces addiu v0,v0,-1 at 0x14530C (the roulette TIME dial, cCockRoulette +0x4D0): every other frame.
        void rouletteDial(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (gameFrames(rdram) % g_roulettePeriod.load(std::memory_order_relaxed) == 0u)
                SET_GPR_S32(ctx, 2, GPR_S32(ctx, 2) - 1);
        }
    }

    void installFramePatches(PS2Runtime &runtime)
    {
        counterfinder::install(runtime);
        g_fps60 = envOn("KAMITE_FPS60");
        g_fps60User = g_fps60;
        g_fps60Shown.store(g_fps60);
        // 60 FPS: the emulated VU1 gets half the time per program (ps2_async_vif.h setVu1TimingScale), so a view the
        // PS2 itself draws in two fields still fits one.
        if (g_fps60)
            ps2x::asyncvif::setVu1TimingScale(0.5);
        g_l3Dash = envOn("KAMITE_L3_DASH");
        ps2xRegisterHostHook("km_frame_start", &frameStart);
        characters::installProbe();
        characters::installLoadWatch();
        ps2xRegisterHostHook("km_fps60_hitbox_compare", &hitboxCompare);
        ps2xRegisterHostHook("km_fps60_roulette_timer", &rouletteTimer);
        ps2xRegisterHostHook("km_fps60_roulette_dial", &rouletteDial);
        ps2xRegisterHostHook("km_roulette_cast_rate", &rouletteCastRate);
        ps2xRegisterHostHook("km_roulette_seen", &rouletteSeen);
        ps2xRegisterHostHook("km_roulette_step", &rouletteStep);
        ps2xRegisterHostHook("km_roulette_slide_begin", &rouletteSlideBegin);
        ps2xRegisterHostHook("km_roulette_slide_end", &rouletteSlideEnd);
        ps2xRegisterHostHook("km_fps60_ui_anim", &uiAnimStep);
        ps2xRegisterHostHook("km_fps60_button_timer", &buttonTimer);
        ps2xRegisterHostHook("km_fps60_countdown", &buttonTimer);
        ps2xRegisterHostHook("km_fps60_countdown_self", &countdownSelf);
        ps2xRegisterHostHook("km_fps60_countdown_float", &countdownFloat);
        ps2xRegisterHostHook("km_fps60_task_sleep", &taskSleep);
        // r301 (the cannon game, recomp/modules/r301.toml): ExecStart's wait before the explanation, ExecGame's
        // cannonball interval, ChkMastCannonBallHit's +0x70.
        ps2xRegisterHostHook("km_fps60_r301_cd_s1_a0", &countdownLoadExact<17, 0xA0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_r301_cd_s1_78", &countdownLoadExact<17, 0x78u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_r301_cd_s2_70", &countdownLoadExact<18, 0x70u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_r301_countdown_tick", &tickOnlyGate<17, 1u>);
        // The other room DLLs' countdowns (recomp/modules/r*.toml, census Nov 2).
        ps2xRegisterHostHook("km_fps60_countdown_a0v0", &countdownPair<4, 2>); // addiu a0,v0,-1
        ps2xRegisterHostHook("km_fps60_r040_fight_tick", &tickOnlyGate<19, 2u>);
        ps2xRegisterHostHook("km_fps60_r040_youwin_tick", &tickOnlyGate<16, 4u>);
        ps2xRegisterHostHook("km_fps60_r040_timeup_tick", &tickOnlyGate<16, 8u>);
        ps2xRegisterHostHook("km_fps60_r305_cd_s0_c", &countdownLoadExact<16, 0xCu, 2, 4>);
        // The casino's Chihuahua race (main ELF): each dog's start delay and the race's waits.
        ps2xRegisterHostHook("km_fps60_dograce_cd_s0_c", &countdownLoadExact<16, 0xCu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_dograce_cd_s1_10", &countdownLoadExact<17, 0x10u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_r505_cd_s2_0", &countdownLoadAll<18, 0x0u>); // raised at 0 too
        ps2xRegisterHostHook("km_fps60_r501_cd_s1_18", &countdownLoadExact<17, 0x18u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_r501_cd_s0_70", &countdownLoadExact<16, 0x70u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_r501_store_s1_14", &countdownStore<17, 0x14u, 2>);
        ps2xRegisterHostHook("km_fps60_flash_guard_rate", &flashGuardRate);
        ps2xRegisterHostHook("km_fps60_tension_drain", &tensionDrainRate);
        ps2xRegisterHostHook("km_fps60_enemy_slow_rate", &enemySlowRate);
        ps2xRegisterHostHook("km_fps60_countdown_a0", &countdownReg<4>);
        ps2xRegisterHostHook("km_fps60_charge_sub_f0", &floatSubParity<0, 0, 1>); // sub.s f0,f0,f1
        ps2xRegisterHostHook("km_fps60_charge_sub_f1", &floatSubParity<1, 1, 0>); // sub.s f1,f1,f0
        ps2xRegisterHostHook("km_fps60_charge_add_f2f1", &floatAddParity<2, 2, 1>); // add.s f2,f2,f1
        ps2xRegisterHostHook("km_fps60_charge_add_f2f0", &floatAddParity<2, 2, 0>); // add.s f2,f2,f0
        ps2xRegisterHostHook("km_fps60_add_f2f1f0", &floatAddParity<2, 1, 0>);      // add.s f2,f1,f0
        ps2xRegisterHostHook("km_fps60_r501_boat_step", &r501BoatStep);
        ps2xRegisterHostHook("km_fps60_item_drop_move", &itemDropMove);
        ps2xRegisterHostHook("km_fps60_item_drop_pull", &itemDropPull);
        // The cannon minigame (r501.toml, Nov 7).
        ps2xRegisterHostHook("km_fps60_add_half_f0f0f1", &floatAddHalf<0, 0, 1>);
        ps2xRegisterHostHook("km_fps60_add_half_f0f0f2", &floatAddHalf<0, 0, 2>);
        ps2xRegisterHostHook("km_fps60_add_half_f0f0f3", &floatAddHalf<0, 0, 3>);
        ps2xRegisterHostHook("km_fps60_add_half_f1f1f0", &floatAddHalf<1, 1, 0>);
        ps2xRegisterHostHook("km_fps60_add_half_f1f1f2", &floatAddHalf<1, 1, 2>);
        ps2xRegisterHostHook("km_fps60_add_half_f2f2f3", &floatAddHalf<2, 2, 3>);
        ps2xRegisterHostHook("km_fps60_sub_half_f1f1f2", &floatSubHalf<1, 1, 2>);
        ps2xRegisterHostHook("km_fps60_sub_half_f2f2f3", &floatSubHalf<2, 2, 3>);
        ps2xRegisterHostHook("km_fps60_sub_half_f2f2f4", &floatSubHalf<2, 2, 4>);
        ps2xRegisterHostHook("km_fps60_sub_parity_f0f0f1", &floatSubParity<0, 0, 1>);
        ps2xRegisterHostHook("km_fps60_sub_parity_f0f0f5", &floatSubParity<0, 0, 5>);
        ps2xRegisterHostHook("km_fps60_sub_parity_f0f0f12", &floatSubParity<0, 0, 12>);
        ps2xRegisterHostHook("km_fps60_lerp_root_f0f0f3", &lerpRoot<0, 0, 3>);
        ps2xRegisterHostHook("km_fps60_lerp_root_f1f1f2", &lerpRoot<1, 1, 2>);
        ps2xRegisterHostHook("km_fps60_lerp_root_f1f1f6", &lerpRoot<1, 1, 6>);
        ps2xRegisterHostHook("km_fps60_mul_root_f0f0f2", &mulRoot<0, 0, 2>);
        ps2xRegisterHostHook("km_fps60_mul_root_f0f0f4", &mulRoot<0, 0, 4>);
        ps2xRegisterHostHook("km_fps60_geom_add_f1f1f2f4", &geomAdd<1, 1, 2, 4>);
        ps2xRegisterHostHook("km_fps60_countup_a0", &countupReg<4>);
        ps2xRegisterHostHook("km_fps60_r501_ball_move", &r501BallMove);
        ps2xRegisterHostHook("km_fps60_r501_banner_phase_a1", &r501BannerPhase<5>);
        ps2xRegisterHostHook("km_fps60_r501_banner_phase_a0", &r501BannerPhase<4>);
        ps2xRegisterHostHook("km_fps60_r501_aim_damp_x", &r501AimDamp<0, 5, 1, 5>); // mul.s f5,f1,f5
        ps2xRegisterHostHook("km_fps60_r501_aim_damp_z", &r501AimDamp<1, 4, 0, 4>); // mul.s f4,f0,f4
        ps2xRegisterHostHook("km_fps60_r501_aim_add_x", &r501AimAdd<0, 2, 2, 1>);   // add.s f2,f2,f1
        ps2xRegisterHostHook("km_fps60_r501_aim_add_z", &r501AimAdd<1, 3, 3, 0>);   // add.s f3,f3,f0
        // The objects' countdowns (kamite.toml, generated Oct 30).
        ps2xRegisterHostHook("km_fps60_om_cd_a0_9e0", &countdownLoadAny<4, 0x9E0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_600_2", &countdownLoadAny<16, 0x600u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_604_2", &countdownLoadAny<16, 0x604u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_60a_2", &countdownLoadExact<16, 0x60Au, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_900", &countdownLoadExact<16, 0x900u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_9d4", &countdownLoadExact<16, 0x9D4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_a90_2", &countdownLoadAny<16, 0xA90u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_af4", &countdownLoadAny<16, 0xAF4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_f15_1", &countdownLoadAny<16, 0xF15u, 2, 1>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_600_2", &countdownLoadAny<17, 0x600u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_604_2", &countdownLoadAny<17, 0x604u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_608_2", &countdownLoadAny<17, 0x608u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_628_2", &countdownLoadAny<17, 0x628u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_650_2", &countdownLoadAny<17, 0x650u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_654_2", &countdownLoadAny<17, 0x654u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_850", &countdownLoadAny<17, 0x850u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_900", &countdownLoadAny<17, 0x900u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_960_2", &countdownLoadAny<17, 0x960u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_60c", &countdownLoadExact<18, 0x60Cu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_60e_2", &countdownLoadAny<18, 0x60Eu, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_62c_2", &countdownLoadAny<18, 0x62Cu, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_9d4_2", &countdownLoadAny<18, 0x9D4u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_cd0_2", &countdownLoadAny<18, 0xCD0u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_d34_2", &countdownLoadAny<18, 0xD34u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_d94_2", &countdownLoadAny<18, 0xD94u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_d98_2", &countdownLoadAny<18, 0xD98u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_f84_2", &countdownLoadAny<18, 0xF84u, 2, 2>);
        // The common enemy code's countdowns (kamite.toml, generated Oct 30).
        ps2xRegisterHostHook("km_fps60_countdown_v0a0", &countdownPair<2, 4>);
        ps2xRegisterHostHook("km_fps60_countdown_v1", &countdownPair<3, 3>);
        ps2xRegisterHostHook("km_fps60_em_cd_s0_5f0", &countdownLoadExact<16, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s0_5f0_post", &countdownLoadAny<16, 0x5F0u, 2, 4>); // a test of the new value follows
        ps2xRegisterHostHook("km_fps60_em_cd_s0_5f8", &countdownLoadExact<16, 0x5F8u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s1_5f0", &countdownLoadExact<17, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s1_5f0_post", &countdownLoadAny<17, 0x5F0u, 2, 4>); // a test of the new value follows
        ps2xRegisterHostHook("km_fps60_em_cd_s1_5f4", &countdownLoadExact<17, 0x5F4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16dc", &countdownLoadExact<18, 0x16DCu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16e0", &countdownLoadExact<18, 0x16E0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16ec", &countdownLoadExact<18, 0x16ECu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16f8_v1", &countdownLoadExact<18, 0x16F8u, 3, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_17b0_1", &countdownLoadExact<18, 0x17B0u, 2, 1>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5f0", &countdownLoadExact<18, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5f4", &countdownLoadExact<18, 0x5F4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5f8", &countdownLoadAny<18, 0x5F8u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5fc", &countdownLoadExact<18, 0x5FCu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s3_5f0", &countdownLoadExact<19, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_countdown_5f8", &countdownLoad<17, 0x5F8u>);
        ps2xRegisterHostHook("km_fps60_countdown_em510", &countdownLoad<16, 0x510u>);
        ps2xRegisterHostHook("km_fps60_countdown_em514", &countdownLoad<16, 0x514u>);
        ps2xRegisterHostHook("km_fps60_countdown_em530", &countdownLoad<16, 0x530u>);
        ps2xRegisterHostHook("km_fps60_countdown_hitstop", &countdownLoadExact<3, 0x2CD0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_countdown_pl1658", &countdownLoad<16, 0x1658u>); // Gene's 15-frame cooldown, 0x126840
        ps2xRegisterHostHook("km_fps60_countdown_pl15b8", &countdownLoad<17, 0x15B8u>);    // the mash states' decay (s1)
        ps2xRegisterHostHook("km_fps60_countdown_pl15b0", &countdownLoadExact<17, 0x15B0u, 2, 4>);    // Dead Pan's wait (s1)
        ps2xRegisterHostHook("km_fps60_float_add_f0f1", &floatAddParity<0, 0, 1>);         // add.s f0,f0,f1
        ps2xRegisterHostHook("km_fps60_coat_wind", &floatAddHalf<0, 0, 3>);                // add.s f0,f0,f3
        ps2xRegisterHostHook("km_fps60_cloth_begin", &clothBegin);
        ps2xRegisterHostHook("km_fps60_cloth_wind", &clothAddHalf<2, 2, 0>);               // add.s f2,f2,f0 (cloth step)
        ps2xRegisterHostHook("km_fps60_cloth_sway", &clothAddHalf<1, 1, 0>);               // add.s f1,f1,f0 (cloth step)
        ps2xRegisterHostHook("km_fps60_cloth_global_wind", &clothAddQuarter<22, 0, 21>);
        ps2xRegisterHostHook("km_fps60_cloth_gravity_table", &clothSubQuarter<1, 1, 0>);
        ps2xRegisterHostHook("km_fps60_cloth_gravity", &clothSubQuarter<0, 0, 1>);
        ps2xRegisterHostHook("km_fps60_cloth_push", &clothAddQuarter<22, 0, 21>);
        ps2xRegisterHostHook("km_fps60_cloth_flare", &clothAddQuarter<22, 0, 1>);
        ps2xRegisterHostHook("km_fps60_cloth_damp", &clothDamp);
        ps2xRegisterHostHook("km_fps60_cloth_spring_f2", &clothSpring<2>);
        ps2xRegisterHostHook("km_fps60_cloth_spring_f1", &clothSpring<1>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15b8_s0", &countdownLoad<16, 0x15B8u>); // the same in func_0011FD10 (s0)
        // Gene's grabbed states: the bite toggles (family 20) and state 17's bite interval (+0x15B4, 5 frames).
        ps2xRegisterHostHook("km_fps60_grab_toggle_s0", &toggleLoad<16, 0x15B4u, 2>);
        ps2xRegisterHostHook("km_fps60_grab_toggle_s1", &toggleLoad<17, 0x15B4u, 2>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15b4_s0", &countdownLoad<16, 0x15B4u>);
        // Gene's state timers the Oct 31 census left (pl00 states; kamite.toml "Gene's state timers").
        ps2xRegisterHostHook("km_fps60_countdown_pl15b4_s1", &countdownLoad<17, 0x15B4u>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15b0_s1_once", &countdownLoad<17, 0x15B0u>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15b0_s0_once", &countdownLoad<16, 0x15B0u>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15b0_s1_a0", &countdownLoadAny<17, 0x15B0u, 4, 4>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15b0_s3", &countdownLoadExact<19, 0x15B0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15b0_s0_v1", &countdownLoadExact<16, 0x15B0u, 3, 4>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15bc", &countdownLoad<17, 0x15BCu>);
        ps2xRegisterHostHook("km_fps60_countdown_pl15bc_exact", &countdownLoadExact<17, 0x15BCu, 2, 4>);
        // The world lights (cWorldLight_Move; kamite.toml "World lights").
        ps2xRegisterHostHook("km_fps60_light_flicker_cd", &countdownLoadAny<16, 0x12u, 2, 1>);
        ps2xRegisterHostHook("km_fps60_light_phase_split", &stepSplit<3, 3, 2>);
        ps2xRegisterHostHook("km_fps60_countup_self", &countupSelf);
        // The 1 -> 0 step's guards (countdownGuard): the signed test loads in front of hooked decrements.
        ps2xRegisterHostHook("km_fps60_cd_guard_s1_568", &countdownGuard<17, 0x568u, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s0_568", &countdownGuard<16, 0x568u, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s1_56a", &countdownGuard<17, 0x56Au, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s0_56a", &countdownGuard<16, 0x56Au, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s2_186c_b", &countdownGuard<18, 0x186Cu, 2, 1, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s1_616", &countdownGuard<17, 0x616u, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s0_984_v1", &countdownGuard<16, 0x984u, 3, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s1_622", &countdownGuard<17, 0x622u, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s1_628", &countdownGuard<17, 0x628u, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_s2_602", &countdownGuard<18, 0x602u, 2, 2, 1>);
        ps2xRegisterHostHook("km_fps60_cd_guard_neg_s1_604", &countdownGuard<17, 0x604u, 2, 2, 0>);
        ps2xRegisterHostHook("km_fps60_cd_guard_neg_s1_602", &countdownGuard<17, 0x602u, 2, 2, 0>);
        ps2xRegisterHostHook("km_fps60_cd_guard_neg_s1_79c", &countdownGuard<17, 0x79Cu, 2, 2, 0>);
        ps2xRegisterHostHook("km_fps60_gene_rate_half", &geneRateHalfF0);
        ps2xRegisterHostHook("km_fps60_stick_turn_half", &stickTurnHalfF1);
        ps2xRegisterHostHook("km_fps60_em_rate_f1_s0", &enemyRate30<16, 1>);
        ps2xRegisterHostHook("km_fps60_em_rate_f1_s1", &enemyRate30<17, 1>);
        ps2xRegisterHostHook("km_fps60_em_rate_f0_s2", &enemyRate30<18, 0>);
        ps2xRegisterHostHook("km_fps60_em_rate_f1_v0", &enemyRate30<2, 1>);
        ps2xRegisterHostHook("km_fps60_effect_world_slow", &effectWorldSlow);
        ps2xRegisterHostHook("km_fps60_effect_slow_rate", &effectSlowRate);
        ps2xRegisterHostHook("km_fps60_effect_rate_for_flag", &effectRateForFlag);
        ps2xRegisterHostHook("km_fps60_bullet_vadd_half", &bulletVaddHalf);
        ps2xRegisterHostHook("km_fps60_wiggle_begin", &wiggleBegin);
        ps2xRegisterHostHook("km_fps60_wiggle_end", &wiggleEnd);
        ps2xRegisterHostHook("km_fps60_pl56e_store_s0", &countdownStore16<16, 2, 3, 0x56Eu>);
        ps2xRegisterHostHook("km_fps60_pl56e_store_s1", &countdownStore16<17, 2, 3, 0x56Eu>);
        ps2xRegisterHostHook("km_fps60_rand_int", &randInt);
        ps2xRegisterHostHook("km_fps60_rand_float", &randFloat);
        ps2xRegisterHostHook("km_fps60_enemy_step_hold", &enemyStepHold<17>);
        ps2xRegisterHostHook("km_fps60_enemy_step_hold_s0", &enemyStepHold<16>);
        ps2xRegisterHostHook("km_fps60_gene_step_hold", &enemyStepHold<6, 7, true>);
        ps2xRegisterHostHook("km_fps60_motion_speed_hold", &motionSpeedHold);
        ps2xRegisterHostHook("km_fps60_hold_move_motion", &moveMotionCalled);
        ps2xRegisterHostHook("km_fps60_hold_null_rotation", &nullRotationCalled);
        ps2xRegisterHostHook("km_fps60_hold_null_speed", &nullSpeedCalled);
        ps2xRegisterHostHook("km_fps60_hold_handler_end", &handlerEnd);
        ps2xRegisterHostHook("km_fps60_hold_action_request", &actionRequested<0x1F7858u>);
        ps2xRegisterHostHook("km_fps60_hold_action_request2", &actionRequested<0x1F7900u>);
        ps2xRegisterHostHook("km_fps60_hold_body_switch", &actionRequested<0x1351D8u>);
        ps2xRegisterHostHook("km_fps60_motion_step_rate", &motionStepRate);
        ps2xRegisterHostHook("km_fps60_seq_before", &seqBefore);
        ps2xRegisterHostHook("km_fps60_map_scroll_step", &mapScrollStep);
        ps2xRegisterHostHook("km_fps60_map_scroll_step23", &mapScrollStep23);
        ps2xRegisterHostHook("km_fps60_map_scroll_counter", &mapScrollCounter);
        ps2xRegisterHostHook("km_fps60_map_scroll_phase", &mapScrollPhase);
        ps2xRegisterHostHook("km_fps60_model_scroll", &modelScrollHalf);
        ps2xRegisterHostHook("km_fps60_uvscroll_u", &scrollHalf<3, 3, 2>);
        ps2xRegisterHostHook("km_fps60_uvscroll_v", &scrollHalf<1, 1, 0>);
        ps2xRegisterHostHook("km_fps60_scrollobj", &scrollHalf<0, 0, 1>);
        ps2xRegisterHostHook("km_fps60_em_rate_read", &managerRateRead);
        ps2xRegisterHostHook("km_fps60_em_rate_reset", &managerRateReset);
        ps2xRegisterHostHook("km_fps60_motion_snap", &motionSnap);
        ps2xRegisterHostHook("km_fps60_tgt_turn_rate", &tgtTurnRate);
        ps2xRegisterHostHook("km_fps60_launch_rise", &launchRise);
        ps2xRegisterHostHook("km_fps60_launch_fall", &launchFall);
        ps2xRegisterHostHook("km_fps60_air_step_f0_s1", &airStep<0, 17>);
        ps2xRegisterHostHook("km_fps60_effect_fade_rgb", &effectFadeRgb);
        ps2xRegisterHostHook("km_fps60_effect_fade_a", &effectFadeA);
        ps2xRegisterHostHook("km_fps60_gene_fall_v_f1", &geneFallSpeed<1, 2, 1>);
        ps2xRegisterHostHook("km_fps60_gene_fall_y_f0", &geneFallY<0, 1>);
        ps2xRegisterHostHook("km_fps60_gene_fall_v_f0", &geneFallSpeed<0, 2, 0>);
        ps2xRegisterHostHook("km_fps60_gene_fall_y_f1", &geneFallY<1, 0>);
        ps2xRegisterHostHook("km_fps60_air_step_f0_s2", &airStep<0, 18>);
        ps2xRegisterHostHook("km_fps60_air_step_f2_s2", &airStep<2, 18>);
        ps2xRegisterHostHook("km_fps60_em_rate_f12_a0", &enemyRate30<4, 12>);
        ps2xRegisterHostHook("km_fps60_grab_rate_copy", &grabRateCopy);
        ps2xRegisterHostHook("km_fps60_pummel_holder_seq", &pummelHolderSeq);
        ps2xRegisterHostHook("km_fps60_think_a0_v0", &thinkFrame<4, 2>);
        ps2xRegisterHostHook("km_fps60_think_v1_a0", &thinkFrame<3, 4>);
        ps2xRegisterHostHook("km_fps60_think_v0_v1", &thinkFrame<2, 3>);
        ps2xRegisterHostHook("km_fps60_think_v0_a0", &thinkFrame<2, 4>);
        ps2xRegisterHostHook("km_fps60_rand_object_begin", &randObjectBegin);
        ps2xRegisterHostHook("km_fps60_rand_object_end", &randObjectEnd);
        installRandState();
        installClothState();
        // The body push on the 30 FPS grid (bodyPush): a replacement, so it follows the F2 60 FPS switch.
        if (!runtime.hasFunction(0x1348E8u) || !(g_origChkHit = runtime.lookupFunction(0x1348E8u)) ||
            !FunctionHooks::instance().replace(0x1348E8u, "cCollisionSolidManage_ChkHit (60 FPS body push)", &bodyPush,
                                               ReplacementStatus::Native, "60 FPS: one body push per 30 FPS frame"))
        {
            g_origChkHit = nullptr;
            KM_WARN(Game, "fps60: cCollisionSolidManage_ChkHit is not hookable");
        }
        g_rdram = runtime.memory().getRDRAM();
        installFrame30State();
        ps2x::savestate::onLoaded([rdram = runtime.memory().getRDRAM()] {
            frame30AfterLoad(rdram);
            rateChange(rdram, ram8(rdram, kFrameDivider), g_fps60 ? 1u : 2u);
        });
        ps2xSetPatchFlag("frame_hook", true); // always: it also counts game frames for the F3 overlay
        ps2xSetPatchFlag("fps60", g_fps60);
        {
            const char *timer = std::getenv("KAMITE_ROULETTE_TIMER");
            const std::string value = timer ? timer : "";
            const bool slow = value == "double" || value == "Double" || value == "2";
            g_rouletteSlow.store(slow);
            applyRoulettePeriod();
            KM_INFO(Game, "roulette timer: " << (slow ? "double" : "normal") << " (one count every " << g_roulettePeriod.load() << " frames)");
        }
        KM_INFO(Game, "frame rate: " << (g_fps60 ? "60 FPS mode (Fps60=1)" : "30 FPS, as on the PS2") << "; L3 dash: "
                                     << (g_l3Dash.load() ? "on" : "off"));
    }

    bool fps60()
    {
        return g_fps60Shown.load(std::memory_order_relaxed);
    }

    void requestFps60(bool on)
    {
        g_fps60Request.store(on ? 1 : 0, std::memory_order_relaxed);
    }

    bool l3Dash()
    {
        return g_l3Dash.load(std::memory_order_relaxed);
    }

    void setL3Dash(bool on)
    {
        g_l3Dash.store(on, std::memory_order_relaxed);
    }

    bool rouletteUp()
    {
        return g_rouletteSeen != 0u && g_gameFrame - g_rouletteSeen <= 2u;
    }

    bool rouletteTimerDouble()
    {
        return g_rouletteSlow.load(std::memory_order_relaxed);
    }

    void setRouletteTimerDouble(bool on)
    {
        g_rouletteSlow.store(on, std::memory_order_relaxed);
        g_rouletteRequest.store(1, std::memory_order_relaxed);
    }
}
