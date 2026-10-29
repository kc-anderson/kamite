// The F2 menu's NPCs tab, above Pose NPC (dev-only: claude/strip-list.md; notes in claude/dev-menu.md "NPCs").
//
// Practice tools for the enemies the level placed: pathfinding off (the Debug switch), AI off, Kill all, and the
// Target enemy fold-out (can't die, a motion played over and over). Spawning enemies (with a test room, 002) was
// tried on Oct 29 and cut the same day: the game's loading calls made from here never all came back (claude/
// dev-menu.md LESSONS keeps what was learned).
//
// Calls into the game (Kill all) run from the game's frame update, Move, as guest code (see queueCall).
//
// AI off (every enemy, kinds 0x200..0x27F) and the target: func_00262AA8 (the "attack now?" every common enemy state
// asks, from the idle, the walks and the hit reactions) answers no; the idle choosers (sub-step 0 func_00211C08, the
// common one, and 111 cEm00_idleChooseNext, the bosses' and special kinds') run, then any sub-step change they made is
// undone and the enemy is put back where it stood. Hits, falls and getting up are other sub-steps: they play as ever.
//
// Repeat a motion (the target): in its idle chooser the game's start motion func_002A8578(enemy, motion, events, 3,
// flags, 0, blend 0) with a motion and its SEQ events from the enemy's data (+0x304: a dat, n entries, offsets at
// +4, four-character types after them; a MOT followed by its SEQ), then moveMotion every frame (it runs Motion_seq_ctr:
// the SEQ's attack windows, sounds and effects) until it says the motion ended; then the idle again for the pause, a
// turn to face Gene, and again. A hit cuts it (the enemy's motion is no longer ours): the loop restarts after the pause.
#include "kamite/npc_tools.h"
#include "kamite/debug_tools.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h" // GPR_U32 / SET_GPR_U32
#include "runtime/ps2_memory.h"

#include "runtime/ps2_game_menu.h"
#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace kamite::npc_tools
{
    namespace
    {
        using namespace kamite::game;
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kEmManage = 0x005864F0u, kEmList = kEmManage + 0x500u;
        constexpr uint32_t kDefeatAll = 0x001EF978u; // ColiseumBattle_DefeatAllEnemies (no args)
        constexpr uint32_t kAttackNow = 0x00262AA8u, kIdleCommon = 0x00211C08u, kIdleBoss = 0x0023D7D0u;
        constexpr uint32_t kStartMotion = 0x002A8578u, kMoveMotion = 0x00157C18u;

        // cGameObj / cEm00 fields used here.
        constexpr uint32_t kObjPos = 0x0F0u, kObjHeading = 0x104u, kObjFlags = 0x250u, kObjKind = 0x2FEu;
        constexpr uint32_t kSubStep = 0x2F5u, kSubStep2 = 0x2F6u, kSubStep3 = 0x2F7u, kObjData = 0x304u;
        constexpr uint32_t kObjMotion = 0x428u, kObjLoops = 0x436u, kObjHpMax = 0x548u, kObjHp = 0x54Au, kObjMotionFlag = 0x17C3u;

        // ---- UI thread -> game thread
        std::atomic<bool> g_aiOff{false};
        std::atomic<bool> g_cantDie{false};
        std::atomic<float> g_pause{0.5f};
        std::atomic<bool> g_killRequest{false};
        std::atomic<bool> g_lockRequest{false}, g_releaseRequest{false};
        std::atomic<int> g_moveRequest{-2}; // a motion slot to repeat; -1 = stop; -2 = nothing new
        std::atomic<bool> g_onceRequest{false};

        // ---- game thread -> UI thread
        std::atomic<int> g_alive{0};
        struct View
        {
            uint32_t target = 0, nearest = 0;
            uint16_t kind = 0;
            float distance = 0.0f;
            std::vector<std::string> labels; // the target's (or the nearest enemy's) motions
            std::vector<int> slots;
            int playing = -1;                // the slot repeating, -1 none
            bool once = false;
            bool driven = true;              // the target came through an idle chooser lately
        };
        std::mutex g_viewMutex;
        View g_view;
        std::mutex g_statusMutex;
        std::string g_status;

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        // ---- game thread state
        uint16_t g_room = 0xFFFFu;
        uint32_t g_target = 0;
        int g_motionSlot = -1;           // repeating (or once)
        bool g_once = false;
        enum class Phase { Wait, Play } g_phase = Phase::Wait;
        Clock::time_point g_nextStart{};
        uint32_t g_ourMotion = 0;
        Clock::time_point g_lastDriven{};
        std::map<uint32_t, uint8_t> g_steady; // per enemy: the chooser's own resting step (+0x2F6)
        uint32_t g_listFor = 0;          // whose motions the view lists

        PS2Runtime *g_runtime = nullptr;
        GuestFunction g_origAttack = nullptr, g_origIdleCommon = nullptr, g_origIdleBoss = nullptr;

        uint32_t rd32(const uint8_t *r, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, r + (a & 0x01FFFFFFu), 4);
            return v;
        }
        uint16_t rd16(const uint8_t *r, uint32_t a)
        {
            uint16_t v = 0;
            std::memcpy(&v, r + (a & 0x01FFFFFFu), 2);
            return v;
        }
        int16_t rds16(const uint8_t *r, uint32_t a) { return static_cast<int16_t>(rd16(r, a)); }
        uint8_t rd8(const uint8_t *r, uint32_t a) { return r[a & 0x01FFFFFFu]; }
        float rdf(const uint8_t *r, uint32_t a)
        {
            float v = 0.0f;
            std::memcpy(&v, r + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wr32(uint8_t *r, uint32_t a, uint32_t v) { std::memcpy(r + (a & 0x01FFFFFFu), &v, 4); }
        void wr16(uint8_t *r, uint32_t a, uint16_t v) { std::memcpy(r + (a & 0x01FFFFFFu), &v, 2); }
        void wr8(uint8_t *r, uint32_t a, uint8_t v) { r[a & 0x01FFFFFFu] = v; }
        void wrf(uint8_t *r, uint32_t a, float v) { std::memcpy(r + (a & 0x01FFFFFFu), &v, 4); }
        bool inRam(uint32_t a, uint32_t size)
        {
            const uint32_t x = a & 0x1FFFFFFFu;
            return x >= 0x100000u && x + size <= PS2_RAM_SIZE;
        }

        GuestFunction resolve(uint32_t address)
        {
            if (GuestFunction o = FunctionHooks::instance().original(address))
                return o;
            return g_runtime && g_runtime->hasFunction(address) ? g_runtime->lookupFunction(address) : nullptr;
        }

        // Calls into the game's code go through the game's own frame update (Move 0x30EEE8, called once a frame by
        // cGame_gameLoop): at its entry, a queued call is tail-called with ra = Move's entry, so it runs as guest code
        // on the game's thread; when it returns (Move entered with ra still Move), the next one goes, then Move itself
        // with its caller's ra. Run from host code instead (up to f58e712), a call that switches threads part-way
        // (the disc read: cDvd_ReadAlloc -> the CRI file system's svm lock, ChangeThreadPriority/SuspendThread) was
        // cut off and left the sound threads locked: the game hung. `done` gets $v0 and the registers on return.
        struct Call
        {
            uint32_t address = 0;
            uint32_t args[8] = {};
            int count = 0;
            bool hasF12 = false;
            float f12 = 0.0f;
            std::function<void(uint32_t, R5900Context &)> done;
        };
        std::deque<Call> g_calls;     // game thread only (onFrame and Move)
        Call g_running;
        bool g_callActive = false;
        uint32_t g_moveRa = 0;        // Move's caller, given back before Move runs
        int g_moveQuiet = 0;          // frames with calls waiting and no Move seen
        constexpr uint32_t kMove = 0x0030EEE8u;
        GuestFunction g_origMove = nullptr;

        void queueCall(const R5900Context &, uint32_t address, std::initializer_list<uint32_t> args,
                       std::function<void(uint32_t, R5900Context &)> done = {}, const float *f12 = nullptr)
        {
            Call c;
            c.address = address;
            for (uint32_t a : args)
                if (c.count < 8)
                    c.args[c.count++] = a;
            if (f12)
            {
                c.hasF12 = true;
                c.f12 = *f12;
            }
            c.done = std::move(done);
            g_calls.push_back(std::move(c));
        }

        void startCall(uint8_t *rdram, R5900Context *ctx)
        {
            g_running = std::move(g_calls.front());
            g_calls.pop_front();
            GuestFunction f = resolve(g_running.address);
            if (!f)
            {
                KM_ERROR(Game, "npc tools: no function at 0x" << std::hex << g_running.address << std::dec);
                return;
            }
            for (int i = 0; i < g_running.count; ++i)
            {
                const int r = 4 + i; // SET_GPR_U32 evaluates its index twice: never pass it reg++ (every argument
                SET_GPR_U32(ctx, r, g_running.args[i]); // landed two registers on up to e6f98f4)
            }
            if (g_running.hasF12)
                ctx->f[12] = g_running.f12;
            SET_GPR_U32(ctx, 31, kMove);
            g_callActive = true;
            ctx->pc = g_running.address;
            f(rdram, ctx, g_runtime); // an unwind inside resumes from ctx->pc, as in any guest code
        }

        void moveHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kMove)
                return g_origMove(rdram, ctx, runtime); // a resume inside Move
            g_moveQuiet = 0;
            const bool back = g_callActive && GPR_U32(ctx, 31) == kMove;
            if (g_callActive && !back)
            {
                // Move called by the game while ours was out: a save state loaded, or the room changed, mid-call.
                KM_WARN(Game, "npc tools: a call to 0x" << std::hex << g_running.address << std::dec << " never came back");
                g_callActive = false;
                g_running = Call{};
                g_calls.clear();
            }
            if (back)
            {
                g_callActive = false;
                Call done = std::move(g_running);
                g_running = Call{};
                if (done.done)
                {
                    R5900Context after = *ctx;
                    done.done(GPR_U32(ctx, 2), after); // may queue more
                }
            }
            else
                g_moveRa = GPR_U32(ctx, 31);
            if (!g_calls.empty())
                return startCall(rdram, ctx);
            SET_GPR_U32(ctx, 31, g_moveRa);
            g_origMove(rdram, ctx, runtime);
        }

        // Inside a replaced guest function: hand over to another function as if it had been jumped to (its return goes
        // where ours would). A scheduler unwind inside it resumes properly, as in any guest code.
        void tailCall(uint8_t *rdram, R5900Context *ctx, GuestFunction f, uint32_t address)
        {
            ctx->pc = address;
            f(rdram, ctx, g_runtime);
        }

        void returnNow(R5900Context *ctx, uint32_t v0)
        {
            SET_GPR_U32(ctx, 2, v0);
            ctx->pc = GPR_U32(ctx, 31);
        }

        uint32_t player(const uint8_t *rdram) { return rd32(rdram, GameWork::address(GameWork::kPlayerObj)); }
        bool position(const uint8_t *rdram, uint32_t obj, float out[3])
        {
            if (!inRam(obj, 0x600u))
                return false;
            const uint32_t p = rd32(rdram, obj + kObjPos);
            if (!inRam(p, 12u))
                return false;
            for (int i = 0; i < 3; ++i)
                out[i] = rdf(rdram, p + 4u * i);
            return true;
        }
        bool isEnemy(const uint8_t *rdram, uint32_t obj)
        {
            const uint16_t kind = rd16(rdram, obj + kObjKind);
            return kind >= 0x200u && kind < 0x280u;
        }
        bool alive(const uint8_t *rdram, uint32_t obj)
        {
            return inRam(obj, 0x1800u) && isEnemy(rdram, obj) && rds16(rdram, obj + kObjHp) > 0 &&
                   static_cast<int32_t>(rd32(rdram, obj + kObjFlags)) >= 0;
        }
        template <typename F>
        void forEachEnemy(const uint8_t *rdram, F f)
        {
            uint32_t slot = rd32(rdram, kEmList);
            for (int guard = 0; guard < 64 && inRam(slot, 16u); ++guard)
            {
                const uint32_t em = rd32(rdram, slot + 8u);
                slot = rd32(rdram, slot + 4u);
                if (inRam(em, 0x1800u))
                    f(em);
            }
        }
        // ---- the target and its motion (game thread)
        void listMotions(const uint8_t *rdram, uint32_t em)
        {
            std::vector<std::string> labels;
            std::vector<int> slots;
            const uint32_t base = inRam(em, 0x1800u) ? rd32(rdram, em + kObjData) : 0u;
            const uint32_t n = inRam(base, 8u) ? rd32(rdram, base) : 0u;
            if (n > 0 && n < 8192u && inRam(base, 8u + 8u * n))
            {
                auto type = [&](uint32_t i) { return rd32(rdram, base + 4u + 4u * n + 4u * i); };
                constexpr uint32_t kMot = 0x00544F4Du, kSeq = 0x00514553u; // "MOT\0", "SEQ\0"
                for (uint32_t i = 0; i + 1 < n; ++i)
                {
                    if (type(i) != kMot || type(i + 1) != kSeq)
                        continue;
                    const uint32_t mot = base + rd32(rdram, base + 4u + 4u * i);
                    char label[64];
                    std::snprintf(label, sizeof(label), "Motion %u  (%u frames)", i, inRam(mot, 8u) ? rd16(rdram, mot + 4u) : 0u);
                    labels.push_back(label);
                    slots.push_back(static_cast<int>(i));
                }
            }
            std::lock_guard<std::mutex> lock(g_viewMutex);
            g_view.labels.swap(labels);
            g_view.slots.swap(slots);
        }

        void faceGene(uint8_t *rdram, uint32_t em)
        {
            float e[3], g[3];
            if (position(rdram, em, e) && position(rdram, player(rdram), g))
                wrf(rdram, em + kObjHeading, std::atan2(g[0] - e[0], g[2] - e[2]));
        }

        GuestFunction g_startMotion = nullptr, g_moveMotion = nullptr;
        bool g_started = false;   // the motion's first frame seen (its pointer and loop count noted)
        uint8_t g_loops = 0;      // +0x436 when it started: moveMotion counts it up at the motion's end

        // In the target's idle chooser (at its entry): true when the frame is ours (the motion started or moved on by a
        // tail call), false to let the chooser run.
        bool driveTarget(uint8_t *rdram, R5900Context *ctx, uint32_t em)
        {
            g_lastDriven = Clock::now();
            if (g_motionSlot < 0 || !g_startMotion || !g_moveMotion)
                return false;
            const uint32_t base = rd32(rdram, em + kObjData);
            const uint32_t n = inRam(base, 8u) ? rd32(rdram, base) : 0u;
            if (static_cast<uint32_t>(g_motionSlot) + 1u >= n || n >= 8192u)
            {
                g_motionSlot = -1;
                return false;
            }
            const auto pause = std::chrono::milliseconds(static_cast<int>(g_pause.load() * 1000.0f));
            if (g_phase == Phase::Play)
            {
                if (!g_started)
                {
                    g_ourMotion = rd32(rdram, em + kObjMotion);
                    g_loops = rd8(rdram, em + kObjLoops);
                    g_started = true;
                }
                const bool ended = rd8(rdram, em + kObjLoops) != g_loops;
                if (ended || rd32(rdram, em + kObjMotion) != g_ourMotion)
                {
                    // Over (or cut off by a hit): the idle for the pause, then again.
                    g_phase = Phase::Wait;
                    g_nextStart = Clock::now() + pause;
                    wr8(rdram, em + kSubStep2, 0); // the idle starts over (its own motion again)
                    if (ended && g_once)
                    {
                        g_motionSlot = -1;
                        g_once = false;
                    }
                    return false;
                }
                SET_GPR_U32(ctx, 4, em);
                tailCall(rdram, ctx, g_moveMotion, kMoveMotion); // runs the SEQ (attack windows, sounds, effects)
                return true;
            }
            if (Clock::now() < g_nextStart)
                return false; // the idle, for the pause
            const uint32_t mot = base + rd32(rdram, base + 4u + 4u * static_cast<uint32_t>(g_motionSlot));
            const uint32_t seq = base + rd32(rdram, base + 8u + 4u * static_cast<uint32_t>(g_motionSlot));
            if (!inRam(mot, 8u) || !inRam(seq, 8u))
            {
                g_motionSlot = -1;
                return false;
            }
            faceGene(rdram, em);
            SET_GPR_U32(ctx, 4, em);
            SET_GPR_U32(ctx, 5, mot);
            SET_GPR_U32(ctx, 6, seq);
            SET_GPR_U32(ctx, 7, 3u);
            SET_GPR_U32(ctx, 8, rd8(rdram, em + kObjMotionFlag) ? 2u : 0u);
            SET_GPR_U32(ctx, 9, 0u);
            ctx->f[12] = 0.0f; // no blend
            g_phase = Phase::Play;
            g_started = false;
            tailCall(rdram, ctx, g_startMotion, kStartMotion);
            return true;
        }

        bool dummy(const uint8_t *rdram, uint32_t em)
        {
            return (g_aiOff.load(std::memory_order_relaxed) && isEnemy(rdram, em)) || (em != 0u && em == g_target);
        }

        // ---- the hooks (game thread, inside the enemies' update). A call that does not come in at the entry is the
        // scheduler resuming the function after an unwind: straight to the original.
        void attackNowHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kAttackNow && dummy(rdram, GPR_U32(ctx, 4)))
                return returnNow(ctx, 0u);
            g_origAttack(rdram, ctx, runtime);
        }

        template <uint32_t Entry, GuestFunction *Original>
        void idleHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t em = GPR_U32(ctx, 4);
            if (ctx->pc != Entry || !inRam(em, 0x1800u) || !dummy(rdram, em))
                return (*Original)(rdram, ctx, runtime);
            if (em == g_target && driveTarget(rdram, ctx, em))
                return;
            const uint32_t ra = GPR_U32(ctx, 31);
            const uint8_t step = rd8(rdram, em + kSubStep), step2 = rd8(rdram, em + kSubStep2), step3 = rd8(rdram, em + kSubStep3);
            float pos[3];
            const bool hasPos = position(rdram, em, pos);
            ctx->pc = Entry;
            (*Original)(rdram, ctx, runtime);
            if (ctx->pc != ra)
                return; // unwound part-way (it resumes on its own): no undo this frame
            if (rd8(rdram, em + kSubStep) != step)
            {
                // A decision (a walk, a taunt, a move): undone. Back to the idle's resting step.
                const auto it = g_steady.find(em);
                wr8(rdram, em + kSubStep, step);
                wr8(rdram, em + kSubStep2, step2 != 0 ? step2 : (it != g_steady.end() ? it->second : 1));
                wr8(rdram, em + kSubStep3, step3);
            }
            else if (step2 != 0 && rd8(rdram, em + kSubStep2) == step2)
                g_steady[em] = step2;
            if (hasPos)
            {
                const uint32_t p = rd32(rdram, em + kObjPos);
                wrf(rdram, p, pos[0]); // where it stood (the height is the game's: it may land or fall)
                wrf(rdram, p + 8u, pos[2]);
            }
        }

        uint32_t nearestEnemy(const uint8_t *rdram, float &distance)
        {
            float g[3];
            distance = 0.0f;
            if (!position(rdram, player(rdram), g))
                return 0;
            uint32_t best = 0;
            float bestD = 1e9f;
            forEachEnemy(rdram, [&](uint32_t em) {
                float e[3];
                if (!alive(rdram, em) || !position(rdram, em, e))
                    return;
                const float d = std::hypot(e[0] - g[0], e[2] - g[2]);
                if (d < bestD)
                {
                    bestD = d;
                    best = em;
                }
            });
            distance = best ? bestD : 0.0f;
            return best;
        }

#if defined(KAMITE_HAS_GS_MENU)
        int g_movePick = 0; // UI thread

        void drawPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 4u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again.");
                return;
            }
            ui->separator_text("Enemies");
            int path = debug_tools::pathfindingOff() ? 1 : 0;
            if (ui->checkbox("Enemy pathfinding off (PATHFINDING OFF, the same switch as in Debug)##npcpath", &path))
                debug_tools::setPathfindingOff(path != 0);
            int ai = g_aiOff.load() ? 1 : 0;
            if (ui->checkbox("AI off: practice dummies (they stand idle and take hits, but never attack or walk)##aioff", &ai))
                g_aiOff = ai != 0;
            if (ui->button("Kill all enemies##killall"))
                g_killRequest = true;
            ui->same_line();
            char alive[48];
            std::snprintf(alive, sizeof(alive), "%d live enemies.", g_alive.load());
            ui->text_disabled(alive);
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                    ui->text(g_status.c_str());
            }

            ui->spacing();

            if (ui->collapsing_header("Target enemy", 0) == 0)
                return;
            View v;
            {
                std::lock_guard<std::mutex> lock(g_viewMutex);
                v.target = g_view.target;
                v.nearest = g_view.nearest;
                v.kind = g_view.kind;
                v.distance = g_view.distance;
                v.labels = g_view.labels;
                v.slots = g_view.slots;
                v.playing = g_view.playing;
                v.once = g_view.once;
                v.driven = g_view.driven;
            }
            char line[128];
            if (v.target)
            {
                std::snprintf(line, sizeof(line), "Target: em%02x, %.1f m away.", v.kind & 0xFFu, v.distance);
                ui->text(line);
                ui->same_line();
                if (ui->button("Release##release"))
                    g_releaseRequest = true;
            }
            else if (v.nearest)
            {
                std::snprintf(line, sizeof(line), "Nearest: em%02x, %.1f m away (picking a motion locks onto it).", v.kind & 0xFFu, v.distance);
                ui->text(line);
                ui->same_line();
                if (ui->button("Lock##lock"))
                    g_lockRequest = true;
            }
            else
            {
                ui->text_disabled("No enemy near.");
                return;
            }
            int cant = g_cantDie.load() ? 1 : 0;
            if (ui->checkbox("Can't die: its HP never falls below 1 and is refilled at a quarter##targetnodeath", &cant))
                g_cantDie = cant != 0;

            std::vector<const char *> items{"(none: back to its own behaviour)"};
            for (const std::string &l : v.labels)
                items.push_back(l.c_str());
            int pick = 0;
            for (size_t i = 0; i < v.slots.size(); ++i)
                if (v.slots[i] == v.playing)
                    pick = static_cast<int>(i) + 1;
            g_movePick = std::min(g_movePick, static_cast<int>(v.slots.size()));
            ui->set_next_width(300.0f);
            if (ui->combo("Repeat##repeatmove", &pick, items.data(), static_cast<int>(items.size())))
            {
                g_movePick = pick;
                g_moveRequest = pick == 0 ? -1 : v.slots[static_cast<size_t>(pick - 1)];
                if (!v.target)
                    g_lockRequest = true;
            }
            if (pick > 0)
                g_movePick = pick;
            float pause = g_pause.load();
            ui->set_next_width(200.0f);
            if (ui->slider_float("Pause between repeats##repeatpause", &pause, 0.0f, 5.0f, "%.1f s"))
                g_pause = pause;
            ui->same_line();
            if (ui->button("Play once##playonce") && g_movePick > 0 && g_movePick <= static_cast<int>(v.slots.size()))
            {
                g_moveRequest = v.slots[static_cast<size_t>(g_movePick - 1)];
                g_onceRequest = true;
                if (!v.target)
                    g_lockRequest = true;
            }
            if (v.playing >= 0 && !v.driven)
                ui->text_wrapped("It isn't playing: this enemy's idle is not one Kamite drives yet (the common one and the "
                                 "bosses' are). Tell Claude which enemy.");
            ui->text_disabled("The motion's own attack windows, sounds and effects play; lunges, grabs and thrown things "
                              "that the attack's code adds may not.");
        }
#endif
    }

    bool cantDie(uint32_t enemy) { return enemy != 0u && enemy == g_target && g_cantDie.load(std::memory_order_relaxed); }

    void install(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        auto replace = [&](uint32_t address, const char *name, GuestFunction hook, GuestFunction &original) {
            if (!runtime.hasFunction(address) || !(original = runtime.lookupFunction(address)) ||
                !FunctionHooks::instance().replace(address, name, hook, ReplacementStatus::Native, "F2 NPCs tab (AI off, target)"))
            {
                original = nullptr;
                KM_WARN(Game, "npc tools: 0x" << std::hex << address << " (" << name << ") is not hookable");
                return false;
            }
            return true;
        };
        bool ok = replace(kAttackNow, "enemy attack-now check (AI off)", &attackNowHook, g_origAttack);
        ok = replace(kIdleCommon, "enemy idle chooser (AI off)", &idleHook<kIdleCommon, &g_origIdleCommon>, g_origIdleCommon) && ok;
        ok = replace(kIdleBoss, "cEm00_idleChooseNext (AI off)", &idleHook<kIdleBoss, &g_origIdleBoss>, g_origIdleBoss) && ok;
        ok = replace(kMove, "Move (the NPCs tab's calls)", &moveHook, g_origMove) && ok;
        g_startMotion = resolve(kStartMotion);
        g_moveMotion = resolve(kMoveMotion);
        if (!ok)
            KM_WARN(Game, "npc tools: some hooks missing");
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("NPCs", &drawPanel);
#endif
    }

    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!ctx || !runtime || !g_origAttack || !g_origMove)
            return;
        g_runtime = runtime;
        const uint16_t room = rd16(rdram, GameWork::address(GameWork::kRoom));
        if (room != g_room)
        {
            g_room = room;
            g_calls.clear(); // not started: for the old room
            g_steady.clear();
            g_target = 0;
            g_motionSlot = -1;
        }
        if (!g_calls.empty() && ++g_moveQuiet == 300)
            KM_WARN(Game, "npc tools: " << g_calls.size() << " calls waiting and no Move (the game's frame update) for 300 frames");
        if (g_killRequest.exchange(false))
        {
            queueCall(*ctx, kDefeatAll, {});
            setStatus("Every enemy defeated.");
        }


        // The target.
        float nearestD = 0.0f;
        const uint32_t nearest = nearestEnemy(rdram, nearestD);
        if (g_releaseRequest.exchange(false))
        {
            g_target = 0;
            g_motionSlot = -1;
        }
        if (g_lockRequest.exchange(false) && nearest)
        {
            g_target = nearest;
            g_phase = Phase::Wait;
            g_nextStart = Clock::now();
        }
        if (g_target && !alive(rdram, g_target))
        {
            g_target = 0; // it died or went
            g_motionSlot = -1;
        }
        if (const int move = g_moveRequest.exchange(-2); move != -2)
        {
            g_motionSlot = move;
            g_once = g_onceRequest.exchange(false);
            g_phase = Phase::Wait;
            g_nextStart = Clock::now();
            g_lastDriven = Clock::now();
        }
        if (g_target && g_cantDie.load(std::memory_order_relaxed))
        {
            const int16_t hp = rds16(rdram, g_target + kObjHp), max = rds16(rdram, g_target + kObjHpMax);
            if (max > 0 && hp > 0 && hp <= max / 4)
                wr16(rdram, g_target + kObjHp, static_cast<uint16_t>(max));
        }
        const uint32_t shown = g_target ? g_target : nearest;
        if (shown != g_listFor)
        {
            g_listFor = shown;
            listMotions(rdram, shown);
        }
        int count = 0;
        forEachEnemy(rdram, [&](uint32_t em) { count += alive(rdram, em) ? 1 : 0; });
        g_alive = count;
        float distance = nearestD;
        if (g_target)
        {
            float e[3], g[3];
            if (position(rdram, g_target, e) && position(rdram, player(rdram), g))
                distance = std::hypot(e[0] - g[0], e[2] - g[2]);
        }
        std::lock_guard<std::mutex> lock(g_viewMutex);
        g_view.target = g_target;
        g_view.nearest = nearest;
        g_view.kind = shown ? rd16(rdram, shown + kObjKind) : 0;
        g_view.distance = distance;
        g_view.playing = g_target ? g_motionSlot : -1;
        g_view.once = g_once;
        g_view.driven = Clock::now() - g_lastDriven < std::chrono::seconds(2);
    }
}
