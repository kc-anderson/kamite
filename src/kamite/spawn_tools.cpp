// The F2 menu's NPCs tab, above Pose NPC (dev-only: claude/strip-list.md; notes in claude/dev-menu.md "Spawn").
//
// Test room: casino room 0x002 ("Meeting room entrance": the blank room module, 3201 bytes, no script, no enemy or
// object placements on the disc). "Go to test room" door-jumps there (debug_tools::requestJump); in it, SCENARIO
// (Sta::kScenario, "events locked out": cScenario_isEventStartOk) is held set so a cutscene that is due waits for
// the next room, and 30 frames in, the enemy/NPC data no enemy uses is let go (cDataManager_resetKind, every hold:
// left over from earlier rooms, it filled the 32-slot data table). "Leave test room" releases the spawns, lets go of
// their data and jumps to the first RoomJmpTbl point of the room it came from. Spawning is offered only in there.
//
// Spawning, the game's own way in three steps over a few frames (the game thread, at frame start):
//   1. the kind's data (em/emXX.dat): cDataManager 0x5864E0. Already loaded: on to 3. Else a slot (findKind /
//      findFreeSlot) and FileLoadRec_start(slot, kind, heap D 0x754220) (0 = the heap has no room: "not enough memory"),
//      plus the sound banks loadSeWait would queue (queueSounds, a copy of its rules), then
//   2. UpdateStateReady(slot) once a frame until the disc read and the banks are in, then the slot is held
//      (func_001FF090, as cDataManager_loadWait does for cEmManage_EntryEm), then
//      (a full table or heap: the unused data let go, 30 frames, once more; the table is logged), then
//   3. cEmManage_EntryEm(0x5864F0, set data, 0, 0) with set data built like func_002951B0 does from a room's set
//      record (spawn_kinds.inc: the values most rooms use for that kind; +0x31 = 0xFF, so the room save never hears of
//      it). EntryEm finds everything loaded and does not wait. Its waits (cScenario_sleepFrames -> cTaskWork_sleep)
//      would put the main thread to sleep for good (only tasks may sleep), so while it runs sleepFrames pumps the disc
//      and the sound (cDvd_step, cSnd_update) instead, and after 15 s UpdateStateReady says yes (logged).
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
#include "kamite/spawn_tools.h"
#include "kamite/debug_tools.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h" // GPR_U32 / SET_GPR_U32
#include "runtime/ps2_memory.h"

#include "runtime/ee_scheduler.h"
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
#include <memory>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace kamite::spawn_tools
{
    namespace
    {
        using namespace kamite::game;
        using Clock = std::chrono::steady_clock;

        struct Kind
        {
            uint8_t kind;      // em/emXX.dat: kind 0x200 + XX
            uint32_t flags;    // set data +0x24
            uint32_t flags2;   // set data +0x2C (bit 0: the kind's sound bank by loadSeWait's table)
            uint8_t byte30;    // set data +0x30
            uint8_t sound;     // set data +0x28 = 0x200 + it
            bool boss;
            uint32_t kb;       // the file's size
            const char *room;  // the first story room that places it ("040" = only the Fight Club rings)
        };
        constexpr Kind kKinds[] = {
#include "kamite/spawn_kinds.inc"
        };

        constexpr uint32_t kEmManage = 0x005864F0u, kEmList = kEmManage + 0x500u;
        constexpr uint32_t kDataManager = 0x005864E0u, kHeapD = 0x00754220u;
        constexpr uint32_t kSetData = 0x000FFF40u; // 0x40 bytes of guest scratch (past debug_tools' landing at 0xFFF00)
        constexpr uint32_t kSeTable = 0x0042C540u; // loadSeWait's jump table by kind - 0x200 (flags2 bit 0)

        constexpr uint32_t kEntryEm = 0x002905D0u, kReleaseEm = 0x00290770u, kDefeatAll = 0x001EF978u;
        constexpr uint32_t kLoadStart = 0x001FF180u, kReady = 0x001FF238u, kHold = 0x001FF090u, kSlotStride = 0x5Cu;
        constexpr uint32_t kAddQueue = 0x001FF4D0u, kSleepFrames = 0x002D5250u, kDvdStep = 0x00200B20u;
        constexpr uint32_t kSndUpdate = 0x002CA470u, kDvd = 0x00583F20u, kSnd = 0x005FEE00u;
        constexpr uint32_t kAttackNow = 0x00262AA8u, kIdleCommon = 0x00211C08u, kIdleBoss = 0x0023D7D0u;
        constexpr uint32_t kStartMotion = 0x002A8578u, kMoveMotion = 0x00157C18u;

        // cGameObj / cEm00 fields used here.
        constexpr uint32_t kObjPos = 0x0F0u, kObjHeading = 0x104u, kObjFlags = 0x250u, kObjKind = 0x2FEu;
        constexpr uint32_t kSubStep = 0x2F5u, kSubStep2 = 0x2F6u, kSubStep3 = 0x2F7u, kObjData = 0x304u;
        constexpr uint32_t kObjMotion = 0x428u, kObjLoops = 0x436u, kObjHpMax = 0x548u, kObjHp = 0x54Au, kObjMotionFlag = 0x17C3u;

        constexpr uint16_t kTestRoom = 0x002;                      // the casino's "Meeting room entrance"
        constexpr float kTestLanding[3] = {-18.0f, 0.0f, 22.0f};  // if the jump table had no point for it (its open floor)
        constexpr uint32_t kResetKind = 0x001FED78u; // cDataManager_resetKind(dm, kind, all): one hold off (all = every hold)
        constexpr uint32_t kSlotCount = 32u;          // func_0028FF80: 32 slots at 0x585960

        // ---- UI thread -> game thread
        std::atomic<bool> g_aiOff{false};
        std::atomic<bool> g_cantDie{false};
        std::atomic<float> g_pause{0.5f};
        std::atomic<int> g_testRequest{-1};
        std::atomic<bool> g_leaveRequest{false}, g_removeRequest{false}, g_killRequest{false};
        std::atomic<bool> g_lockRequest{false}, g_releaseRequest{false};
        std::atomic<int> g_moveRequest{-2}; // a motion slot to repeat; -1 = stop; -2 = nothing new
        std::atomic<bool> g_onceRequest{false};
        std::mutex g_spawnMutex;
        std::deque<uint8_t> g_spawnQueue;

        // ---- game thread -> UI thread
        std::atomic<bool> g_inTest{false};
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
        std::string g_tableLine; // the data table's use (with the status mutex)

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        // ---- game thread state
        uint16_t g_room = 0xFFFFu;
        uint16_t g_cameFrom = 0xFFFFu;   // the room "Go to test room" was pressed in
        bool g_eventLock = false, g_gameHadLock = false; // Sta::kScenario held set in the test room
        int g_cleanIn = 0;               // frames until the data nobody uses is let go (after entering the test room)
        int g_tableIn = 0;               // frames until the data table summary is refreshed
        Clock::time_point g_jumpAt{};
        int g_jumpPending = 0;           // 1 = into the test room, 2 = out of it
        int g_leavePhase = 0;            // 1 = released, 2 = data given back, then the jump
        std::vector<uint32_t> g_spawned; // enemies spawned here
        std::set<uint32_t> g_loadedKinds; // data kinds loaded by us (held: given back when leaving)
        int g_freeIn = 0;                // frames until the released kinds' data is given back

        uint32_t g_target = 0;
        int g_motionSlot = -1;           // repeating (or once)
        bool g_once = false;
        enum class Phase { Wait, Play } g_phase = Phase::Wait;
        Clock::time_point g_nextStart{};
        uint32_t g_ourMotion = 0;
        Clock::time_point g_lastDriven{};
        std::map<uint32_t, uint8_t> g_steady; // per enemy: the chooser's own resting step (+0x2F6)
        uint32_t g_listFor = 0;          // whose motions the view lists

        // Waits inside EntryEm (see the top).
        std::atomic<bool> g_inEntry{false};
        Clock::time_point g_entryStart{};
        Clock::time_point g_lastSnd{};
        bool g_fakeReady = false;

        PS2Runtime *g_runtime = nullptr;
        GuestFunction g_origAttack = nullptr, g_origIdleCommon = nullptr, g_origIdleBoss = nullptr;
        GuestFunction g_origSleep = nullptr, g_origReady = nullptr;

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

        const Kind *kindInfo(uint8_t k)
        {
            for (const Kind &e : kKinds)
                if (e.kind == k)
                    return &e;
            return nullptr;
        }

        GuestFunction resolve(uint32_t address)
        {
            if (GuestFunction o = FunctionHooks::instance().original(address))
                return o;
            return g_runtime && g_runtime->hasFunction(address) ? g_runtime->lookupFunction(address) : nullptr;
        }

        // A guest function run to its end by the EE scheduler, as a queued invocation (its own context and stack; the
        // way the runtime runs RPC callbacks), at the scheduler's next safe point: right after the frame hook. Calling a
        // recompiled function straight from host code is not safe: at any call or loop the scheduler may unwind it to
        // service an event and resume it later from its pc, which a host caller never does (it got half a function:
        // findFreeSlot said "none" on an empty table). Invocations run in the order queued; `done` gets $v0.
        int g_inFlight = 0; // queued and not finished (game thread)
        void queueCall(const R5900Context &base, uint32_t address, std::initializer_list<uint32_t> args,
                       std::function<void(uint32_t, R5900Context &)> done = {}, const float *f12 = nullptr)
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::HleCall;
            invocation.context = base;
            invocation.context.pc = address;
            int reg = 4;
            for (uint32_t a : args)
                SET_GPR_U32(&invocation.context, reg++, a);
            if (f12)
                invocation.context.f[12] = *f12;
            SET_GPR_U32(&invocation.context, 29, 0u); // the invocation stack
            SET_GPR_U32(&invocation.context, 31, 0u); // returning to 0 ends it
            ++g_inFlight;
            invocation.onComplete = [done = std::move(done)](const R5900Context &result, R5900Context &owner) {
                --g_inFlight;
                if (done)
                    done(getRegU32(&result, 2), owner);
            };
            g_runtime->eeScheduler().queueInvocation(std::move(invocation));
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
        bool listed(const uint8_t *rdram, uint32_t obj)
        {
            bool found = false;
            forEachEnemy(rdram, [&](uint32_t em) { found = found || em == obj; });
            return found;
        }
        // The data table (cDataManager 0x5864E0: count +4, slots *(+8), 0x5C each: +0 state, +4 holds, +8 flags,
        // +0x10 data, +0x20 kind). A slot is free with no holds, no data and flag bit 2 clear (findFreeSlot).
        bool slotUsed(const uint8_t *rdram, uint32_t slot)
        {
            return rd32(rdram, slot + 4u) != 0u || rd32(rdram, slot + 0x10u) != 0u || (rd32(rdram, slot + 8u) & 4u) != 0u;
        }
        bool kindInUse(const uint8_t *rdram, uint32_t kind)
        {
            bool used = false;
            forEachEnemy(rdram, [&](uint32_t em) { used = used || rd16(rdram, em + kObjKind) == kind; });
            return used;
        }
        std::string tableSummary(const uint8_t *rdram, bool full)
        {
            const uint32_t base = rd32(rdram, kDataManager + 8u);
            const uint32_t count = std::min(rd32(rdram, kDataManager + 4u), 64u);
            int used = 0;
            std::string kinds;
            for (uint32_t i = 0; i < count && inRam(base, kSlotStride * count); ++i)
            {
                const uint32_t slot = base + kSlotStride * i;
                if (!slotUsed(rdram, slot))
                    continue;
                ++used;
                char one[64];
                const uint32_t kind = rd32(rdram, slot + 0x20u);
                if (full)
                    std::snprintf(one, sizeof(one), " [%u] kind %03x state %u holds %u flags %x data %08x", i, kind,
                                  rd32(rdram, slot), rd32(rdram, slot + 4u), rd32(rdram, slot + 8u), rd32(rdram, slot + 0x10u));
                else
                    std::snprintf(one, sizeof(one), " %03x", kind);
                kinds += one;
            }
            return "Data table: " + std::to_string(used) + " of " + std::to_string(count) + " slots used:" + kinds;
        }
        // Lets go of the enemy and NPC data no enemy here uses (left over from earlier rooms), as objects let go of
        // theirs (cObj_releaseActorData: resetKind), all holds at once: the game's own clean-up then frees it.
        // The game's heaps (func_0030ECF8: Arena_init; cHeap {+0 first block, +4 size}; block {+0 heap, +4 prev, +8 next,
        // +0xC free after it, +0x10 used}). Enemy data goes into heap D.
        struct Heap
        {
            uint32_t desc;
            const char *name;
        };
        constexpr Heap kHeaps[] = {{0x00754220u, "D"}, {0x00754200u, "A"}, {0x00754210u, "B"}};
        void heapRoom(const uint8_t *rdram, uint32_t desc, uint32_t &largest, uint32_t &total)
        {
            largest = total = 0u;
            uint32_t block = rd32(rdram, desc) & 0x01FFFFFFu;
            for (uint32_t n = 0; block && block + 0x14u <= PS2_RAM_SIZE && n < 8192u; ++n)
            {
                const uint32_t freeBytes = rd32(rdram, block + 0xCu);
                largest = std::max(largest, freeBytes);
                total += freeBytes;
                block = rd32(rdram, block + 8u) & 0x01FFFFFFu;
            }
        }
        std::string heapsLine(const uint8_t *rdram)
        {
            std::string out = "Heaps (largest free block / free):";
            for (const Heap &h : kHeaps)
            {
                uint32_t largest = 0, total = 0;
                heapRoom(rdram, h.desc, largest, total);
                char one[64];
                std::snprintf(one, sizeof(one), " %s %.2f / %.2f MB", h.name, largest / 1048576.0, total / 1048576.0);
                out += one;
            }
            return out;
        }

        uint32_t dataSlot(const uint8_t *rdram, int index) { return rd32(rdram, kDataManager + 8u) + kSlotStride * static_cast<uint32_t>(index); }
        int slotCount(const uint8_t *rdram) { return static_cast<int>(std::min(rd32(rdram, kDataManager + 4u), 64u)); }
        // cDataManager_findKind / findFreeSlot read straight from the table (no side effects in either).
        int findKind(const uint8_t *rdram, uint32_t kind)
        {
            for (int i = slotCount(rdram) - 1; i >= 0; --i)
            {
                const uint32_t slot = dataSlot(rdram, i);
                const bool held = rd32(rdram, slot + 4u) != 0u;
                const bool inUse = held || (!(rd32(rdram, slot + 8u) & 4u) && rd32(rdram, slot + 0x10u) != 0u);
                if (inUse && rd32(rdram, slot + 0x20u) == kind)
                    return i;
            }
            return -1;
        }
        int findFreeSlot(const uint8_t *rdram)
        {
            for (int i = 0; i < slotCount(rdram); ++i)
                if (!slotUsed(rdram, dataSlot(rdram, i)))
                    return i;
            return -1;
        }

        // Lets go of the enemy and NPC data no enemy here uses (left over from earlier rooms), as objects let go of
        // theirs (cObj_releaseActorData: resetKind), all holds at once: the game's own clean-up then frees it.
        int releaseUnused(uint8_t *rdram, const R5900Context &base)
        {
            std::vector<uint32_t> kinds;
            for (int i = 0; i < slotCount(rdram); ++i)
            {
                const uint32_t slot = dataSlot(rdram, i);
                const uint32_t kind = rd32(rdram, slot + 0x20u);
                if (slotUsed(rdram, slot) && kind >= 0x200u && kind < 0x300u && !g_loadedKinds.count(kind) &&
                    !kindInUse(rdram, kind) && std::find(kinds.begin(), kinds.end(), kind) == kinds.end())
                    kinds.push_back(kind);
            }
            for (uint32_t kind : kinds)
                queueCall(base, kResetKind, {kDataManager, kind, 1u});
            if (!kinds.empty())
                KM_INFO(Game, "spawn: let go of " << kinds.size() << " unused enemy/NPC data kinds in the test room");
            return static_cast<int>(kinds.size());
        }

        void logHeapMap(const uint8_t *rdram, uint32_t desc)
        {
            struct Owner
            {
                uint32_t data, kind, manager;
            };
            std::vector<Owner> owners;
            for (uint32_t manager : {kDataManager, 0x00580D20u})
            {
                const uint32_t base = rd32(rdram, manager + 8u);
                const uint32_t count = std::min(rd32(rdram, manager + 4u), 96u);
                for (uint32_t i = 0; i < count && inRam(base, kSlotStride * count); ++i)
                {
                    const uint32_t slot = base + kSlotStride * i;
                    if (rd32(rdram, slot + 0x10u))
                        owners.push_back({rd32(rdram, slot + 0x10u), rd32(rdram, slot + 0x20u), manager});
                }
            }
            KM_WARN(Game, "spawn: heap 0x" << std::hex << desc << " map (block, used, free after, owner):" << std::dec);
            uint32_t block = rd32(rdram, desc) & 0x01FFFFFFu;
            for (uint32_t n = 0; block && block + 0x14u <= PS2_RAM_SIZE && n < 400u; ++n)
            {
                const uint32_t used = rd32(rdram, block + 0x10u), freeBytes = rd32(rdram, block + 0xCu);
                std::string owner;
                for (const Owner &o : owners)
                    if (o.data >= block && o.data < block + 0x20u + used)
                    {
                        char one[48];
                        std::snprintf(one, sizeof(one), " kind %03x (%s)", o.kind, o.manager == kDataManager ? "enemy table" : "object table");
                        owner += one;
                    }
                KM_WARN(Game, "spawn:   0x" << std::hex << block << std::dec << " " << used << " " << freeBytes << owner);
                block = rd32(rdram, block + 8u) & 0x01FFFFFFu;
            }
        }

        // Why cDvd_ReadAlloc 0x200CB0 said no: no free job (cDvd 0x583F20: 32 jobs of 0x88 from +4, busy while +0 is
        // set; cDvd_takeFreeJob), the name not in the file table (FindEntryValue_1FF9C0(0x580D40, name, &size, &x) < 0),
        // or cHeap_allocFirstFit's refusal (DBG word 2 bit 0x80000 makes every allocation fail, or no block fits).
        void diagnoseReadAlloc(uint8_t *rdram, const R5900Context &base, uint32_t kind)
        {
            int busy = 0;
            for (uint32_t i = 0; i < 32u; ++i)
                busy += rd32(rdram, kDvd + 4u + 0x88u * i) != 0u ? 1 : 0;
            const uint32_t dbg2 = rd32(rdram, 0x00747A34u);
            constexpr uint32_t kName = 0x000FFF80u, kSizeOut = 0x000FFFA0u, kOtherOut = 0x000FFFA4u;
            char name[16];
            std::snprintf(name, sizeof(name), "em/em%02x.dat", kind & 0xFFu);
            std::memset(rdram + kName, 0, 0x20);
            std::memcpy(rdram + kName, name, std::strlen(name));
            wr32(rdram, kSizeOut, 0xDEADBEEFu); // FindEntryValue writes 0 here first: still DEADBEEF = it never ran
            wr32(rdram, kOtherOut, 0u);
            KM_WARN(Game, "spawn: ReadAlloc check: " << busy << " of 32 disc jobs busy, DBG word 2 = 0x" << std::hex << dbg2
                                                     << (dbg2 & 0x80000u ? " (bit 0x80000 set: the heap refuses every allocation)" : "")
                                                     << std::dec << "; looking up " << name);
            {
                // The same lookup called straight (the old way), for comparison: pc 0x1 after it = it ran to its end.
                R5900Context *ctx = const_cast<R5900Context *>(&base);
                const R5900Context saved = *ctx;
                SET_GPR_U32(ctx, 4, 0x00580D40u);
                SET_GPR_U32(ctx, 5, kName);
                SET_GPR_U32(ctx, 6, kSizeOut);
                SET_GPR_U32(ctx, 7, kOtherOut);
                SET_GPR_U32(ctx, 31, 0x1u);
                ctx->pc = 0x001FF9C0u;
                uint32_t v0 = 0xFFFFFFFFu, pc = 0u;
                if (GuestFunction f = resolve(0x001FF9C0u))
                {
                    f(rdram, ctx, g_runtime);
                    v0 = GPR_U32(ctx, 2);
                    pc = ctx->pc;
                }
                *ctx = saved;
                KM_WARN(Game, "spawn: file table lookup of " << name << " (direct): " << static_cast<int32_t>(v0) << ", ended at pc 0x"
                                                             << std::hex << pc << ", size 0x" << rd32(rdram, kSizeOut) << std::dec);
                wr32(rdram, kSizeOut, 0xDEADBEEFu);
            }
            KM_WARN(Game, "spawn: name in RAM: '" << reinterpret_cast<const char *>(rdram + kName) << "', table root 0x" << std::hex
                                                  << rd32(rdram, 0x00580D40u) << std::dec);
            queueCall(base, 0x001FF9C0u, {0x00580D40u, kName, kSizeOut, kOtherOut}, [name = std::string(name)](uint32_t found, R5900Context &) {
                const uint8_t *r = g_runtime->memory().getRDRAM();
                KM_WARN(Game, "spawn: file table lookup of " << name << " (queued): " << static_cast<int32_t>(found) << ", size 0x" << std::hex
                                                             << rd32(r, kSizeOut) << ", second value 0x" << rd32(r, kOtherOut) << std::dec);
            });
        }

        // ---- spawning (game thread)
        void queueSounds(const uint8_t *rdram, const R5900Context &base, uint32_t slot, uint32_t kind, const Kind &k)
        {
            uint32_t bank = 0x200u + k.sound;
            if ((k.flags2 & 1u) && kind - 0x200u < 0x4Au)
            {
                // The table's targets each set the bank (cDataManager_loadSeWait 0x1FEB08..0x1FEB58); others keep it.
                struct Target
                {
                    uint32_t pc, bank;
                };
                static constexpr Target kTargets[] = {{0x1FEB08u, 0x227u}, {0x1FEB10u, 0x22Au}, {0x1FEB18u, 0x22Bu},
                                                      {0x1FEB20u, 0x22Cu}, {0x1FEB28u, 0x22Eu}, {0x1FEB30u, 0x22Fu},
                                                      {0x1FEB38u, 0x22Du}, {0x1FEB40u, 0x24Du}, {0x1FEB48u, 0x24Au},
                                                      {0x1FEB50u, 0x243u}, {0x1FEB58u, 0x24Eu}};
                const uint32_t pc = rd32(rdram, kSeTable + 4u * (kind - 0x200u));
                for (const Target &t : kTargets)
                    if (t.pc == pc)
                        bank = t.bank;
            }
            if (rd16(rdram, GameWork::address(GameWork::kRoom)) == 0x504u && bank - 0x250u < 2u)
                return;
            queueCall(base, kAddQueue, {slot, bank});
            if (kind == 0x264u)
                queueCall(base, kAddQueue, {slot, 0x263u});
            else if (kind == 0x211u)
                for (uint32_t b : {0x200u, 0x203u, 0x227u, 0x22Au})
                    queueCall(base, kAddQueue, {slot, b});
            else if (kind == 0x278u)
                queueCall(base, kAddQueue, {slot, 0x20Fu});
        }

        struct Job
        {
            uint8_t kind = 0;
            enum Phase { Start, Ready, Entry, Done } phase = Start;
            uint32_t slot = 0;
            bool fresh = false;    // our load (held when ready, let go of when leaving)
            bool retried = false;
            bool busy = false;     // a queued call not back yet
            bool noMemory = false;
            int heap = 0;          // kHeaps index tried (D first, then one with room)
            int waitFrames = 0;    // before going on (the game's clean-up frees things over a few frames)
            uint32_t enemy = 0;    // EntryEm's answer
            Clock::time_point start{};
        };
        std::deque<std::shared_ptr<Job>> g_jobs;

        bool buildSetData(uint8_t *rdram, const Kind &k)
        {
            const uint32_t gene = player(rdram);
            float here[3];
            if (!position(rdram, gene, here))
                return false;
            const float heading = rdf(rdram, gene + kObjHeading);
            std::memset(rdram + kSetData, 0, 0x40);
            wr32(rdram, kSetData + 0x00u, 0x200u + k.kind);
            wrf(rdram, kSetData + 0x10u, here[0] + 3.0f * std::sin(heading));
            wrf(rdram, kSetData + 0x14u, here[1]);
            wrf(rdram, kSetData + 0x18u, here[2] + 3.0f * std::cos(heading));
            wrf(rdram, kSetData + 0x1Cu, 1.0f);
            wrf(rdram, kSetData + 0x20u, heading + 3.1415927f); // facing Gene
            wr32(rdram, kSetData + 0x24u, k.flags);
            wr32(rdram, kSetData + 0x28u, 0x200u + k.sound);
            wr32(rdram, kSetData + 0x2Cu, k.flags2);
            wr8(rdram, kSetData + 0x30u, k.byte30);
            wr8(rdram, kSetData + 0x31u, 0xFFu); // no room-save record
            return true;
        }

        void stepJobs(uint8_t *rdram, const R5900Context &base)
        {
            if (g_jobs.empty())
                return;
            const std::shared_ptr<Job> job = g_jobs.front();
            const Kind *k = kindInfo(job->kind);
            const uint32_t kind = 0x200u + job->kind;
            char line[192];
            auto fail = [&](const char *why) {
                char message[256];
                std::snprintf(message, sizeof(message), "em%02x: %s", job->kind, why);
                KM_WARN(Game, "spawn: " << message);
                setStatus(message);
                g_jobs.pop_front();
            };
            if (!k)
                return (void)g_jobs.pop_front();
            if (job->phase != Job::Start && Clock::now() - job->start > std::chrono::seconds(30))
                return fail("it did not finish loading in 30 seconds.");
            if (job->busy)
                return;
            if (job->waitFrames > 0)
            {
                --job->waitFrames;
                return;
            }
            if (job->noMemory)
            {
                job->noMemory = false;
                if (job->heap == 0)
                {
                    logHeapMap(rdram, kHeaps[0].desc);
                    KM_WARN(Game, "spawn: " << heapsLine(rdram) << "; " << tableSummary(rdram, true));
                    diagnoseReadAlloc(rdram, base, kind);
                }
                // Another heap with room for it and 1 MB to spare (its data goes back through the same free).
                const uint32_t need = k->kb * 1024u + 0x10000u;
                for (int h = job->heap + 1; h < static_cast<int>(std::size(kHeaps)); ++h)
                {
                    uint32_t largest = 0, total = 0;
                    heapRoom(rdram, kHeaps[h].desc, largest, total);
                    if (largest >= need + 0x100000u)
                    {
                        job->heap = h;
                        job->phase = Job::Start;
                        KM_INFO(Game, "spawn: em" << std::hex << unsigned(job->kind) << std::dec << " goes into heap " << kHeaps[h].name);
                        return;
                    }
                }
                if (job->retried || releaseUnused(rdram, base) == 0)
                {
                    std::snprintf(line, sizeof(line), "not enough memory here for its %.1f MB: remove spawned enemies first.", k->kb / 1024.0f);
                    return fail(line);
                }
                job->retried = true;
                job->phase = Job::Start;
                job->waitFrames = 30;
                setStatus("Making room in memory...");
                return;
            }
            switch (job->phase)
            {
            case Job::Start:
            {
                job->start = Clock::now();
                int index = findKind(rdram, kind);
                if (index >= 0)
                {
                    job->slot = dataSlot(rdram, index);
                    job->fresh = false;
                    job->phase = Job::Ready;
                    return;
                }
                index = findFreeSlot(rdram);
                if (index < 0)
                {
                    KM_WARN(Game, "spawn: " << tableSummary(rdram, true));
                    if (job->retried || releaseUnused(rdram, base) == 0)
                        return fail("the game's data table is full (the 32 slots: see the line below): remove spawned enemies first.");
                    job->retried = true;
                    job->waitFrames = 30;
                    setStatus("Making room in the data table...");
                    return;
                }
                job->slot = dataSlot(rdram, index);
                job->fresh = true;
                job->busy = true;
                const Kind info = *k;
                wr32(rdram, job->slot + 0x14u, 0xDEADBEEFu); // FileLoadRec_start writes the heap here first
                queueCall(base, kLoadStart, {job->slot, kind, kHeaps[job->heap].desc}, [job, kind, info](uint32_t data, R5900Context &owner) {
                    job->busy = false;
                    {
                        const uint8_t *r = g_runtime->memory().getRDRAM();
                        KM_INFO(Game, "spawn: FileLoadRec_start(slot 0x" << std::hex << job->slot << ", kind 0x" << kind << ") = 0x" << data
                                                                          << "; slot +0x10 0x" << rd32(r, job->slot + 0x10u) << " +0x14 0x"
                                                                          << rd32(r, job->slot + 0x14u) << " +0x1C 0x" << rd32(r, job->slot + 0x1Cu)
                                                                          << " +0x20 0x" << rd32(r, job->slot + 0x20u) << "; result sp 0x"
                                                                          << getRegU32(&owner, 29) << std::dec);
                    }
                    if (data == 0u)
                    {
                        job->noMemory = true; // cDvd_ReadAlloc found no room in heap D
                        return;
                    }
                    g_loadedKinds.insert(kind);
                    queueSounds(g_runtime->memory().getRDRAM(), owner, job->slot, kind, info);
                    job->phase = Job::Ready;
                });
                std::snprintf(line, sizeof(line), "Loading em%02x...", job->kind);
                setStatus(line);
                return;
            }
            case Job::Ready:
                job->busy = true;
                queueCall(base, kReady, {job->slot}, [job](uint32_t ready, R5900Context &) {
                    job->busy = false;
                    if (ready)
                        job->phase = Job::Entry;
                });
                return;
            case Job::Entry:
            {
                if (!buildSetData(rdram, *k))
                    return fail("no Gene to spawn in front of.");
                if (job->fresh)
                    queueCall(base, kHold, {job->slot}); // held as cDataManager_loadWait(.., 1) holds it
                job->busy = true;
                g_entryStart = g_lastSnd = Clock::now();
                g_fakeReady = false;
                g_inEntry = true;
                queueCall(base, kEntryEm, {kEmManage, kSetData, 0u, 0u}, [job](uint32_t enemy, R5900Context &) {
                    g_inEntry = false;
                    job->busy = false;
                    job->enemy = enemy;
                    job->phase = Job::Done;
                });
                return;
            }
            case Job::Done:
                if (!inRam(job->enemy, 0x1800u))
                    return fail("the game would not create it.");
                g_spawned.push_back(job->enemy);
                std::snprintf(line, sizeof(line), "Spawned em%02x.%s", job->kind,
                              g_fakeReady ? " (It waited too long for its data: it may misbehave.)" : "");
                KM_INFO(Game, "spawn: em" << std::hex << unsigned(job->kind) << " at 0x" << job->enemy << std::dec
                                          << (g_fakeReady ? " (wait cut short)" : ""));
                setStatus(line);
                g_jobs.pop_front();
                return;
            }
        }

        void releaseSpawned(uint8_t *rdram, const R5900Context &base)
        {
            int n = 0;
            for (uint32_t em : g_spawned)
                if (listed(rdram, em))
                {
                    queueCall(base, kReleaseEm, {kEmManage, em});
                    ++n;
                }
            g_spawned.clear();
            g_target = 0;
            g_motionSlot = -1;
            g_freeIn = 4; // ReleaseObj may finish a frame later: the data waits a few frames
            setStatus("Removed " + std::to_string(n) + " spawned.");
        }
        // Our hold off the kinds we loaded, once no enemy of that kind is left: the game frees them once unheld.
        void giveBackData(uint8_t *rdram, const R5900Context &base)
        {
            for (auto it = g_loadedKinds.begin(); it != g_loadedKinds.end();)
            {
                if (kindInUse(rdram, *it))
                {
                    ++it;
                    continue;
                }
                queueCall(base, kResetKind, {kDataManager, *it, 0u});
                it = g_loadedKinds.erase(it);
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

        GuestFunction g_startMotion = nullptr, g_moveMotion = nullptr, g_dvdStep = nullptr, g_sndUpdate = nullptr;
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

        // The waits of cDataManager_loadWait / loadSeWait (sleepFrames, then UpdateStateReady, in a loop) inside our
        // EntryEm, told apart by where they return to.
        bool ourWait(const R5900Context *ctx, uint32_t a, uint32_t b)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            return g_inEntry.load(std::memory_order_relaxed) && (ra == a || ra == b);
        }
        void sleepFramesHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc != kSleepFrames || !ourWait(ctx, 0x1FE948u, 0x1FEC48u) || !g_dvdStep || !g_sndUpdate)
                return g_origSleep(rdram, ctx, runtime);
            // No sleeping on the main thread: move the disc (or the sound, every 16 ms) along instead, then back to
            // the loop's readiness check.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const auto now = Clock::now();
            if (!g_fakeReady && now - g_entryStart > std::chrono::seconds(15))
            {
                g_fakeReady = true;
                KM_ERROR(Game, "spawn: the new enemy waited 15 s for its data; going on without it");
            }
            if (now - g_lastSnd > std::chrono::milliseconds(16))
            {
                g_lastSnd = now;
                SET_GPR_U32(ctx, 4, kSnd);
                return tailCall(rdram, ctx, g_sndUpdate, kSndUpdate);
            }
            SET_GPR_U32(ctx, 4, kDvd);
            tailCall(rdram, ctx, g_dvdStep, kDvdStep);
        }
        void readyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (ctx->pc == kReady && g_fakeReady && ourWait(ctx, 0x1FE954u, 0x1FEC54u))
                return returnNow(ctx, 1u);
            g_origReady(rdram, ctx, runtime);
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
        std::vector<std::string> g_enemyLabels, g_bossLabels;
        std::vector<uint8_t> g_enemyKinds, g_bossKinds;
        int g_enemyPick = 0, g_bossPick = 0, g_movePick = 0; // UI thread

        void buildLabels()
        {
            for (const Kind &k : kKinds)
            {
                const uint16_t room = static_cast<uint16_t>(std::strtoul(k.room, nullptr, 16));
                char label[128];
                if (!*k.room)
                    std::snprintf(label, sizeof(label), "em%02x  (no placement on the disc: may need its script)  %.1f MB", k.kind, k.kb / 1024.0f);
                else if (room == 0x040)
                    std::snprintf(label, sizeof(label), "em%02x  (Fight Club only)  %.1f MB", k.kind, k.kb / 1024.0f);
                else
                {
                    const char *name = debug_tools::roomName(room);
                    std::snprintf(label, sizeof(label), "em%02x  (first in r%s%s%s)  %.1f MB", k.kind, k.room, *name ? " " : "", name,
                                  k.kb / 1024.0f);
                }
                (k.boss ? g_bossLabels : g_enemyLabels).push_back(label);
                (k.boss ? g_bossKinds : g_enemyKinds).push_back(k.kind);
            }
        }

        void spawnRow(const Pcsx2GsUi *ui, const char *id, const char *button, int &pick, const std::vector<std::string> &labels,
                      const std::vector<uint8_t> &kinds)
        {
            std::vector<const char *> items;
            for (const std::string &l : labels)
                items.push_back(l.c_str());
            ui->set_next_width(380.0f);
            ui->combo(id, &pick, items.data(), static_cast<int>(items.size()));
            ui->same_line();
            if (ui->button(button) && pick >= 0 && pick < static_cast<int>(kinds.size()))
            {
                std::lock_guard<std::mutex> lock(g_spawnMutex);
                g_spawnQueue.push_back(kinds[static_cast<size_t>(pick)]);
            }
        }

        void drawPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 4u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again.");
                return;
            }
            ui->separator_text("Spawn");
            const bool inTest = g_inTest.load();
            if (!inTest)
            {
                if (ui->button("Go to test room##gotest"))
                    g_testRequest = 0;
            }
            else if (ui->button("Leave test room##leavetest"))
                g_leaveRequest = true;
            ui->text_disabled("Room 002, an empty casino room: no script, no enemies, no objects, and no cutscenes while "
                              "you are in it (one that is due waits until you leave). Leaving takes you back to the room "
                              "you came from, at its first jump point (a save state keeps an exact spot).");

            int path = debug_tools::pathfindingOff() ? 1 : 0;
            if (ui->checkbox("Enemy pathfinding off (PATHFINDING OFF, the same switch as in Debug)##npcpath", &path))
                debug_tools::setPathfindingOff(path != 0);
            int ai = g_aiOff.load() ? 1 : 0;
            if (ui->checkbox("AI off: practice dummies (they stand idle and take hits, but never attack or walk)##aioff", &ai))
                g_aiOff = ai != 0;

            ui->begin_disabled(inTest ? 0 : 1);
            spawnRow(ui, "Enemies##spawnem", "Spawn##spawnem", g_enemyPick, g_enemyLabels, g_enemyKinds);
            spawnRow(ui, "Bosses##spawnboss", "Spawn##spawnboss", g_bossPick, g_bossLabels, g_bossKinds);
            if (ui->button("Remove spawned##removespawned"))
                g_removeRequest = true;
            ui->end_disabled();
            if (!inTest)
                ui->text_disabled("Go to the test room to spawn.");
            ui->same_line();
            if (ui->button("Kill all enemies##killall"))
                g_killRequest = true;
            char alive[96];
            std::snprintf(alive, sizeof(alive), "%d live enemies. A type's first spawn reads its data: a short freeze.", g_alive.load());
            ui->text_disabled(alive);
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                    ui->text(g_status.c_str());
                if (!g_tableLine.empty())
                    ui->text_disabled(g_tableLine.c_str());
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
                !FunctionHooks::instance().replace(address, name, hook, ReplacementStatus::Native, "F2 NPCs tab (spawn, AI off, target)"))
            {
                original = nullptr;
                KM_WARN(Game, "spawn tools: 0x" << std::hex << address << " (" << name << ") is not hookable");
                return false;
            }
            return true;
        };
        bool ok = replace(kAttackNow, "enemy attack-now check (AI off)", &attackNowHook, g_origAttack);
        ok = replace(kIdleCommon, "enemy idle chooser (AI off)", &idleHook<kIdleCommon, &g_origIdleCommon>, g_origIdleCommon) && ok;
        ok = replace(kIdleBoss, "cEm00_idleChooseNext (AI off)", &idleHook<kIdleBoss, &g_origIdleBoss>, g_origIdleBoss) && ok;
        ok = replace(kSleepFrames, "cScenario_sleepFrames (spawn)", &sleepFramesHook, g_origSleep) && ok;
        ok = replace(kReady, "cDataManager slot ready (spawn)", &readyHook, g_origReady) && ok;
        g_startMotion = resolve(kStartMotion);
        g_moveMotion = resolve(kMoveMotion);
        g_dvdStep = resolve(kDvdStep);
        g_sndUpdate = resolve(kSndUpdate);
        KM_INFO(Game, "spawn tools: " << std::size(kKinds) << " kinds" << (ok ? "" : " (some hooks missing)"));
#if defined(KAMITE_HAS_GS_MENU)
        buildLabels();
        ps2x::menu::addPanel("NPCs", &drawPanel);
#endif
    }

    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!ctx || !runtime || !g_origAttack || !g_origSleep || !g_origReady)
            return;
        g_runtime = runtime;
        const uint16_t room = rd16(rdram, GameWork::address(GameWork::kRoom));
        if (room != g_room)
        {
            // A new room: whatever was spawned went with the old one (and the room change dealt with its data).
            g_room = room;
            g_spawned.clear();
            g_jobs.clear();
            g_steady.clear();
            g_target = 0;
            g_motionSlot = -1;
            g_leavePhase = 0;
            g_loadedKinds.clear();
            if (room == kTestRoom)
            {
                g_cleanIn = 30; // once the room has settled
                setStatus("In the test room.");
            }
            g_jumpPending = 0;
        }
        if (g_jumpPending && Clock::now() - g_jumpAt > std::chrono::seconds(10))
        {
            g_jumpPending = 0;
            setStatus("The jump didn't happen: a cutscene, a door, a talk or a menu was running?");
        }
        g_inTest = room == kTestRoom;

        // No cutscene starts in the test room: SCENARIO (events locked out: cScenario_isEventStartOk) held set, as
        // the game sets it itself; one that is due waits, and plays where it should once the room is left.
        {
            const uint32_t sta = GameWork::address(GameWork::kFlags5E4);
            const bool lock = room == kTestRoom; // kept on the way out too: the door jump does not ask
            const uint32_t w = rd32(rdram, sta);
            if (lock && !g_eventLock)
            {
                g_gameHadLock = (w & Sta::kScenario) != 0u;
                g_eventLock = true;
            }
            if (g_eventLock && lock)
                wr32(rdram, sta, w | Sta::kScenario);
            else if (g_eventLock)
            {
                if (!g_gameHadLock)
                    wr32(rdram, sta, w & ~Sta::kScenario);
                g_eventLock = false;
            }
        }
        if (g_cleanIn > 0 && --g_cleanIn == 0 && room == kTestRoom)
            releaseUnused(rdram, *ctx);
        if (--g_tableIn <= 0)
        {
            g_tableIn = 30;
            std::string line = room == kTestRoom ? tableSummary(rdram, false) + "\n" + heapsLine(rdram) : std::string();
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_tableLine.swap(line);
        }

        // Requests from the menu.
        if (g_testRequest.exchange(-1) >= 0 && room != kTestRoom)
        {
            float pos[3];
            float heading = 0.0f;
            if (!debug_tools::firstJumpPoint(kTestRoom, pos, heading))
                std::memcpy(pos, kTestLanding, sizeof(pos));
            g_cameFrom = room;
            g_jumpPending = 1;
            g_jumpAt = Clock::now();
            debug_tools::requestJump(kTestRoom, pos, heading, "the test room (r002)");
            setStatus("Jumping...");
        }
        if (g_leaveRequest.exchange(false) && g_inTest.load() && g_leavePhase == 0)
        {
            releaseSpawned(rdram, *ctx);
            g_leavePhase = 1;
        }
        if (g_removeRequest.exchange(false))
            releaseSpawned(rdram, *ctx);
        if (g_killRequest.exchange(false))
        {
            queueCall(*ctx, kDefeatAll, {});
            setStatus("Every enemy defeated.");
        }
        {
            std::lock_guard<std::mutex> lock(g_spawnMutex);
            while (!g_spawnQueue.empty())
            {
                if (g_inTest.load())
                {
                    auto job = std::make_shared<Job>();
                    job->kind = g_spawnQueue.front();
                    g_jobs.push_back(std::move(job));
                }
                g_spawnQueue.pop_front();
            }
        }
        stepJobs(rdram, *ctx);
        if (g_freeIn > 0 && --g_freeIn == 0)
            giveBackData(rdram, *ctx);
        if (g_leavePhase == 1 && g_freeIn == 0)
        {
            giveBackData(rdram, *ctx);
            float pos[3];
            float heading = 0.0f;
            if (g_cameFrom != kTestRoom && g_cameFrom != 0xFFFFu && debug_tools::firstJumpPoint(g_cameFrom, pos, heading))
            {
                char label[64];
                std::snprintf(label, sizeof(label), "back to r%x%02x", (g_cameFrom >> 8) & 0xF, g_cameFrom & 0xFF);
                debug_tools::requestJump(g_cameFrom, pos, heading, label);
                g_jumpPending = 2;
                g_jumpAt = Clock::now();
                setStatus("Leaving...");
            }
            else
            {
                setStatus("No way back known (the room you came from has no jump point, or you came another way): use the "
                          "Debug tab's room jump or a save state.");
            }
            g_leavePhase = 0;
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
