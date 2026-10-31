// 60 FPS mode and L3 dash: a port of the PCSX2 patch SLUS-21503_6FB69282.pnach ("60 Fps", by Puggsy + Claude Ai +
// Gemini; K's copy, 2026-10-22). PCSX2 writes the patch into the game's code; here the game code is recompiled C++,
// so the code changes are compiled in as switchable patches (recomp/kamite.toml [[patches.conditional]] and
// [[patches.hooks]], flag "fps60") and this file holds the parts that are C++:
//
//   pnach                                         here
//   00747A09 byte 01 (every vblank)               frameStart: the game's frame divider byte = 1 (60 frames/s)
//   302D04/10D694 lui at,0x3F00                   conditional patches (an effect emitter's frame, the Unleash gauge
//                                                  drain: 0.5 instead of 1.0); the pnach's third one, 10B7B8 (the
//                                                  sub-screen demo's motion step), is left out: moveMotion halves it
//                                                  already, the demo ran at half speed
//   304500/303910/2FCA8C lui at,0x3F00 (ours)     the other emitter's frame, the emission countdown, the effect
//                                                  children's frames: the whole effect system counts PS2 frames
//                                                  (emitEveryFrame holds the every-frame emitters to even frames)
//   2C81BC hook -> F0000 (hitbox sync)            hitboxCompare replaces c.eq.s f0,f2 at 0x2C81C0: the hitbox logic
//                                                  is skipped when the integer frame (cvt.w.s) of the last and the
//                                                  current position are equal, not only the exact floats
//   15765C nop (dodge sync)                       conditional patch: the add.s in the beql delay slot is not run
//   108FF8 / 108FE8 (ours, stick dash)            conditional patches: the double-tap window 10 -> 20 frames and the
//                                                  "dash requested" flag 2 -> 4 (counted in game frames)
//   10FCE8.. / 120204.. (ours, mash meter)        conditional patches: the mash meter's per-level limits and decay
//                                                  doubled (both copies; recomp/kamite.toml lists them)
//   1147CC sw zero,0(at) (roulette time)          conditional patch: nop (the timer store is dropped; the pnach's
//                                                  store to whatever $at holds is left out)
//   29059C lui at,0x3F00 (enemy physics x0.5)     conditional patch
//   290584 hook -> F0038 (enemy animation x2)     conditional patch: mov.s f12,f1 -> add.s f12,f1,f1 (f1 * 2)
//   30ADF4 hook -> D0020 (left stick)             frameStart: a stick axis at full deflection last frame is held
//                                                  there for one more frame (state in g_stick* instead of 0xD0200;
//                                                  without the pnach's input loop, see frameStart)
//   D05E8242 == 2: 005E8147 = 0, 005E81C7 = 0     frameStart with L3Dash: L3 held pushes the left stick fully
//   (L3 dash, every vblank)                        forward
//
// The frame hook runs before the instruction at 0x30ADF4 (lw v0,0x3D84(s3)), right after the main loop's wait for the
// next frame (0x30ADD8..0x30ADF0), where the pnach's stick hook ran; the per-vblank writes run there too, once per
// game frame.
#include "kamite/frame_patches.h"
#include "kamite/character_mods.h"
#include "kamite/hud_layout.h"
#include "kamite/npc_poses.h"
#include "kamite/debug_tools.h"
#include "kamite/npc_tools.h"
#include "kamite/hitbox_overlay.h"
#include "kamite/bone_tweaks.h"
#include "kamite/call_profile.h"
#include "kamite/counter_finder.h"
#include "kamite/test_mode.h"
#include "kamite/game_options.h"
#include "kamite/pc_sampler.h"
#include "kamite/log.h"
#include "kamite/game_layout.h"
#include "ps2_runtime_macros.h" // FPU_CVT_W_S (the recompiled CVT.W.S)
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_async_vif.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_save_state.h"

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
        bool g_fps60 = false;
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
        bool skipFrame(uint8_t *rdram) { return (gameFrames(rdram) & 1u) != 0u; }

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

        // L3 alone (pad buttons == 2): a dash, as a quick double tap of the left stick forward (from centre) and then
        // forward held while L3 is. The pnach this comes from wrote "stick up" every frame from PCSX2's patch engine, between the
        // game's own pad reads, which the game saw as tapping; here the game reads the byte once and kept seeing a
        // plain "up" (Gene walked). Each step lasts 2 frames at 30 frames/s (4 in the 60 FPS mode). Written after the
        // stick hold, and marked as injected so the next frame puts the reading back.
        uint32_t g_dashFrames = 0; // frames since L3 went down (0 = not held)

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
            if (buttons != 2u)
            {
                g_dashFrames = 0;
                return;
            }
            const uint32_t step = g_fps60 ? 4u : 2u;
            const uint32_t phase = g_dashFrames++ / step; // 0 neutral, 1 forward, 2 neutral, 3.. forward
            // While the tap is being made the stick is centred sideways too (a diagonal is not a forward tap, which is
            // why a dash started while walking did nothing); once dashing, sideways is the player's again.
            const bool tapping = phase < 3u;
            const uint8_t forward = (phase == 1u || phase >= 3u) ? 0x00u : 0x80u;
            for (int axis = 0; axis < 4; ++axis)
            {
                const bool vertical = (axis & 1) != 0;
                if (!vertical && !tapping)
                    continue;
                injectStick(rdram, axis, vertical ? forward : 0x80u);
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
        void setFps60(bool on)
        {
            if (on == g_fps60)
                return;
            g_fps60 = on;
            g_fps60Shown.store(on);
            ps2xSetPatchFlag("fps60", on);
            ps2x::asyncvif::setVu1TimingScale(on ? 0.5 : 1.0);
            applyRoulettePeriod();
            for (int axis = 0; axis < 4; ++axis)
            {
                g_stickHeld[axis] = 0u;
                g_stickLast[axis] = 0u;
            }
            KM_INFO(Game, "frame rate switched: " << (on ? "60 FPS mode" : "30 FPS, as on the PS2"));
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
        uint32_t g_frame30Base = 0, g_counterBase = 0, g_frame30Saved = 0;
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
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            *ctx = saved;
        }
        // The held vblank is the second half of the 30 FPS frame in which the old step ran its last frame: the old
        // motion goes on as that step moved it (moveMotion 0x157C18, cObjBase_addNullSpeed_Rotation 0x14F9F0 and
        // cObjBase_addNullSpeed 0x14F928 at 1.0, as the walking and idle steps do every frame), unless the change set
        // a new motion already. Without it each change lost half a 30 FPS frame of root motion (66sec case: the grab
        // lunge ended 0.1 m short, the grab put Gene 0.19 m off, and the other goon chose otherwise from there).
        void enemyStepHold(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 17);
            uint32_t state = 0, motion = 0;
            std::memcpy(&state, &ram8(rdram, obj + 0x2F4u), sizeof(state));
            std::memcpy(&motion, &ram8(rdram, obj + 0x428u), sizeof(motion));
            const uint32_t counter = gameFrames(rdram);
            EnemyStep &last = g_enemyStep[obj];
            const bool changed = last.counter + 1u == counter && last.state != state;
            const bool sameMotion = last.motion == motion;
            last = EnemyStep{state, counter, motion};
            if (changed && thinkSkipped(rdram))
            {
                if (sameMotion)
                {
                    callGuest(rdram, ctx, runtime, 0x157C18u, obj, 0.0f);
                    callGuest(rdram, ctx, runtime, 0x14F9F0u, obj, 1.0f);
                    callGuest(rdram, ctx, runtime, 0x14F928u, obj, 1.0f);
                }
                SET_GPR_U32(ctx, 6, 0x100318u);
            }
            if ((counter & 0x3FFu) == 0u)
                for (auto it = g_enemyStep.begin(); it != g_enemyStep.end();)
                    it = counter - it->second.counter > 2u ? g_enemyStep.erase(it) : std::next(it);
        }

        // The launched enemy's flight (mode 1 phase 9 step 1, func_002557F8 0x255AEC / 0x255AF8): y += vy, vy -= 0.026
        // a frame, without the speed rate the other airborne states multiply in, so at 60 the arc ran in half the time
        // (66sec case, Oct 31: 18 vblanks in the air instead of 36, the horizontal speed right). At 60 half steps that
        // meet the 30 FPS arc on every 30 FPS frame: y += vy / 2 + g / 8, vy -= g / 2 (f2 = g).
        void launchRise(uint8_t *, R5900Context *ctx, PS2Runtime *) // add.s f1,f1,f0
        {
            ctx->f[1] = ctx->f[1] + ctx->f[0] * 0.5f + ctx->f[2] * 0.125f;
        }
        void launchFall(uint8_t *, R5900Context *ctx, PS2Runtime *) // sub.s f0,f0,f2
        {
            ctx->f[0] = ctx->f[0] - ctx->f[2] * 0.5f;
        }

        void installFrame30State()
        {
            ps2x::savestate::registerExtension(
                "kamite.frame30",
                [](ps2x::state::Writer &out) {
                    uint32_t version = 1u, frame = g_rdram ? (g_fps60 ? frame30(g_rdram) : gameFrames(g_rdram)) : 0u;
                    ps2x::state::fields(out, version, frame);
                },
                [](ps2x::state::Reader &in) {
                    uint32_t version = 0u;
                    ps2x::state::fields(in, version, g_frame30Saved);
                    g_frame30Loaded = true;
                });
        }
        // After a load (before the game runs): the 30 FPS frame count from the state (a state without it: its counter).
        // At 60 it is the base; at 30 a state saved at 60 gets it as the game's counter.
        void frame30AfterLoad(uint8_t *rdram)
        {
            const uint32_t counter = gameFrames(rdram);
            const uint32_t frame = g_frame30Loaded ? g_frame30Saved : counter;
            if (g_fps60) // the game counts the frame up before its update: the first update after the load sees counter + 1
                g_frame30Base = frame + 1u, g_counterBase = counter + 1u;
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
            forgetStickAfterLoad();
            forgetRandAfterLoad(rdram);
            forgetClothAfterLoad(rdram);
            deadPanFrame(rdram, ctx, runtime);
            if (const int request = g_fps60Request.exchange(-1); request >= 0)
                setFps60(request != 0);
            if (g_rouletteRequest.exchange(0) != 0)
                applyRoulettePeriod();
            ps2_host_display::noteGameFrame();
            profileWindow();
            counterfinder::onFrame(rdram);
            hud::onFrame(rdram);
            characters::onFrame(rdram);
            npc_poses::onFrame(rdram, ctx, runtime);
            debug_tools::onFrame(rdram, ctx, runtime);
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
        void emitEveryFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 3) == 0u && skipFrame(rdram))
                SET_GPR_S32(ctx, 2, 0);
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
            const float value = ctx->f[12] * 0.5f;
            std::memcpy(&ram8(rdram, GPR_U32(ctx, 4) + 0x548u), &value, sizeof(value));
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

        // countdownLoad for any load: `l[w|hu|bu] Dst,Offset(Base)` (Bytes 4 / 2 / 1) before a decrement in a delay slot.
        template <int Base, uint32_t Offset, int Dst, int Bytes>
        void countdownLoadAny(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + ((GPR_U32(ctx, Base) + Offset) & 0x01FFFFFFu), Bytes);
            SET_GPR_U32(ctx, Dst, skipFrame(rdram) && value != 0u ? value + 1u : value);
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
        ps2xRegisterHostHook("km_fps60_flash_guard_rate", &flashGuardRate);
        ps2xRegisterHostHook("km_fps60_tension_drain", &tensionDrainRate);
        ps2xRegisterHostHook("km_fps60_enemy_slow_rate", &enemySlowRate);
        ps2xRegisterHostHook("km_fps60_countdown_a0", &countdownReg<4>);
        ps2xRegisterHostHook("km_fps60_charge_sub_f0", &floatSubParity<0, 0, 1>); // sub.s f0,f0,f1
        ps2xRegisterHostHook("km_fps60_charge_sub_f1", &floatSubParity<1, 1, 0>); // sub.s f1,f1,f0
        ps2xRegisterHostHook("km_fps60_charge_add_f2f1", &floatAddParity<2, 2, 1>); // add.s f2,f2,f1
        ps2xRegisterHostHook("km_fps60_charge_add_f2f0", &floatAddParity<2, 2, 0>); // add.s f2,f2,f0
        // The objects' countdowns (kamite.toml, generated Oct 30).
        ps2xRegisterHostHook("km_fps60_om_cd_a0_9e0", &countdownLoadAny<4, 0x9E0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_600_2", &countdownLoadAny<16, 0x600u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_604_2", &countdownLoadAny<16, 0x604u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_60a_2", &countdownLoadAny<16, 0x60Au, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_900", &countdownLoadAny<16, 0x900u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_9d4", &countdownLoadAny<16, 0x9D4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_a90_2", &countdownLoadAny<16, 0xA90u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_af4", &countdownLoadAny<16, 0xAF4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s0_f15_1", &countdownLoadAny<16, 0xF15u, 2, 1>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_600_2", &countdownLoadAny<17, 0x600u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_604_2", &countdownLoadAny<17, 0x604u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_608_2", &countdownLoadAny<17, 0x608u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_61c_v1", &countdownLoadAny<17, 0x61Cu, 3, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_628_2", &countdownLoadAny<17, 0x628u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_650_2", &countdownLoadAny<17, 0x650u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_654_2", &countdownLoadAny<17, 0x654u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_850", &countdownLoadAny<17, 0x850u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_900", &countdownLoadAny<17, 0x900u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_om_cd_s1_960_2", &countdownLoadAny<17, 0x960u, 2, 2>);
        ps2xRegisterHostHook("km_fps60_om_cd_s2_60c", &countdownLoadAny<18, 0x60Cu, 2, 4>);
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
        ps2xRegisterHostHook("km_fps60_em_cd_s0_5f0", &countdownLoadAny<16, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s0_5f8", &countdownLoadAny<16, 0x5F8u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s1_5f0", &countdownLoadAny<17, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s1_5f4", &countdownLoadAny<17, 0x5F4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16dc", &countdownLoadAny<18, 0x16DCu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16e0", &countdownLoadAny<18, 0x16E0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16ec", &countdownLoadAny<18, 0x16ECu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_16f8_v1", &countdownLoadAny<18, 0x16F8u, 3, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_17b0_1", &countdownLoadAny<18, 0x17B0u, 2, 1>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5f0", &countdownLoadAny<18, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5f4", &countdownLoadAny<18, 0x5F4u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5f8", &countdownLoadAny<18, 0x5F8u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s2_5fc", &countdownLoadAny<18, 0x5FCu, 2, 4>);
        ps2xRegisterHostHook("km_fps60_em_cd_s3_5f0", &countdownLoadAny<19, 0x5F0u, 2, 4>);
        ps2xRegisterHostHook("km_fps60_countdown_5f8", &countdownLoad<17, 0x5F8u>);
        ps2xRegisterHostHook("km_fps60_countdown_em510", &countdownLoad<16, 0x510u>);
        ps2xRegisterHostHook("km_fps60_countdown_em514", &countdownLoad<16, 0x514u>);
        ps2xRegisterHostHook("km_fps60_countdown_em530", &countdownLoad<16, 0x530u>);
        ps2xRegisterHostHook("km_fps60_countdown_hitstop", &countdownLoad<3, 0x2CD0u>);
        ps2xRegisterHostHook("km_fps60_countdown_pl1658", &countdownLoad<16, 0x1658u>); // Gene's 15-frame cooldown, 0x126840
        ps2xRegisterHostHook("km_fps60_countdown_pl15b8", &countdownLoad<17, 0x15B8u>);    // the mash states' decay (s1)
        ps2xRegisterHostHook("km_fps60_countdown_pl15b0", &countdownLoad<17, 0x15B0u>);    // Dead Pan's wait (s1)
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
        ps2xRegisterHostHook("km_fps60_countup_self", &countupSelf);
        ps2xRegisterHostHook("km_fps60_gene_rate_half", &geneRateHalfF0);
        ps2xRegisterHostHook("km_fps60_em_rate_f1_s0", &enemyRate30<16, 1>);
        ps2xRegisterHostHook("km_fps60_em_rate_f1_s1", &enemyRate30<17, 1>);
        ps2xRegisterHostHook("km_fps60_em_rate_f0_s2", &enemyRate30<18, 0>);
        ps2xRegisterHostHook("km_fps60_em_rate_f1_v0", &enemyRate30<2, 1>);
        ps2xRegisterHostHook("km_fps60_emit_every_frame", &emitEveryFrame);
        ps2xRegisterHostHook("km_fps60_wiggle_begin", &wiggleBegin);
        ps2xRegisterHostHook("km_fps60_wiggle_end", &wiggleEnd);
        ps2xRegisterHostHook("km_fps60_pl56e_store_s0", &countdownStore16<16, 2, 3, 0x56Eu>);
        ps2xRegisterHostHook("km_fps60_pl56e_store_s1", &countdownStore16<17, 2, 3, 0x56Eu>);
        ps2xRegisterHostHook("km_fps60_rand_int", &randInt);
        ps2xRegisterHostHook("km_fps60_rand_float", &randFloat);
        ps2xRegisterHostHook("km_fps60_enemy_step_hold", &enemyStepHold);
        ps2xRegisterHostHook("km_fps60_launch_rise", &launchRise);
        ps2xRegisterHostHook("km_fps60_launch_fall", &launchFall);
        ps2xRegisterHostHook("km_fps60_em_rate_f12_a0", &enemyRate30<4, 12>);
        ps2xRegisterHostHook("km_fps60_think_a0_v0", &thinkFrame<4, 2>);
        ps2xRegisterHostHook("km_fps60_think_v1_a0", &thinkFrame<3, 4>);
        ps2xRegisterHostHook("km_fps60_think_v0_v1", &thinkFrame<2, 3>);
        ps2xRegisterHostHook("km_fps60_think_v0_a0", &thinkFrame<2, 4>);
        ps2xRegisterHostHook("km_fps60_rand_object_begin", &randObjectBegin);
        ps2xRegisterHostHook("km_fps60_rand_object_end", &randObjectEnd);
        installRandState();
        installClothState();
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
