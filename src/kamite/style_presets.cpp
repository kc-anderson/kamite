// Style presets, stage 1. See style_presets.h.
#include "kamite/style_presets.h"
#include "kamite/style_pose.h"
#include "kamite/style_sfx.h"
#include "kamite/hud_layout.h"

#include "kamite/buck_breaker.h"
#include "kamite/button_prompt_sheets.h"
#include "kamite/debug_tools.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

namespace kamite::style_presets
{
    namespace
    {
        constexpr uint32_t kCoreSave = 0x569B70u;   // cCoreSave D_00569B70: { cCoreSaveData *data; }
        namespace ent = game::IDBaseEnt;
        namespace work = game::GameWork;
        constexpr uint32_t kSkills = game::CoreSaveData::kSkill;       // char skill[0x80]: -1 = not owned
        constexpr uint32_t kCombos = game::CoreSaveData::kCombo;       // cCoreSaveCombo combo[6]
        constexpr uint32_t kSetBytes = game::CoreSaveData::kComboSize; // int id[6]; char lv[6]; char pad[2]; int max;
        constexpr uint32_t kSaveSize = game::CoreSaveData::kSize;
        constexpr int kSets = 6, kSlots = 6, kStyles = 4;
        constexpr uint32_t kComboBytes = kSetBytes * kSets;
        // The Techniques screen (research of Oct 25, SLUS_215.03): sub screen kind 1, cSubScrWaza, registered at
        // cSubScr+0x310 (D_003BD930 -> 0x566E50, so 0x567160). +0x5A its state: 0 browsing the slots, 1 the move picker
        // open, 2 leaving. +0x25C the cursor row (s8, -1 = RETURN), +0x284 the column (0 the square chain). Its input
        // (0x16DF38) never tests triangle; it re-reads combo[] every frame (0x16C730), so a load shows at once.
        constexpr uint32_t kSubScrPointer = game::SubScr::kPointer;
        constexpr int32_t kTechniquesKind = 1;
        constexpr uint32_t kWazaOffset = 0x310u, kWazaState = 0x5Au, kWazaCursor = 0x25Cu, kWazaColumn = 0x284u;

        const char *const kSetNames[kSets] = {"Square chain", "Triangle", "Cross", "Down+Square", "Down+Triangle", "Down+Cross"};
        // The game's move names by id (the Cheat Engine table's movelist.txt, as the Techniques screen names them).
        const char *const kMoveNames[] = {
            "Left Jab 1",
            "Left Jab 2",
            "Left Jab 3",
            "Mach Speed Jab 1",
            "Straight 1",
            "Straight 2",
            "Straight 3",
            "Long Straight 1",
            "Long Straight 2",
            "Long Straight 3",
            "Left Hook 1",
            "Left Hook 2",
            "Left Hook 3",
            "Uppercut 1",
            "Uppercut 2",
            "Uppercut 3",
            "Chop",
            "Elbow Spin 1",
            "Elbow Spin 2",
            "Elbow Spin 3",
            "Pay Up",
            "Really Pay Up",
            "Pay Up NOW!",
            "Pimp Hand",
            "Pimp Smack",
            "Godly Smack",
            "Low Kick",
            "Low Kick 2",
            "Godly Low Kick",
            "Right Roundhouse 1",
            "Right Roundhouse 2",
            "Right Roundhouse 3",
            "Left Roundhouse",
            "Spinning Backfist",
            "Backfist Strike 1",
            "Double Spin Kick",
            "Mule Kick",
            "High Side Kick 1",
            "Rolling Sobat",
            "Hayamaker 1",
            "High Kick",
            "Knee Strike",
            "High Snap Kick",
            "Triple Side Kick",
            "Flying Triple",
            "Step Back Kick",
            "Fist of Justice",
            "Reverse Sweep",
            "Back Roundhouse",
            "Spinning Sobat",
            "Somersault",
            "One-Two Punch",
            "Barrel Roll Kick",
            "Expert Sobat",
            "Dashing Sobat",
            "Hand Plant Kick",
            "High Cross Kick",
            "Flying Knee",
            "Elbow Vortex",
            "Forearm Smash 1",
            "Palm Smash",
            "Reverse Hell Kick",
            "Double Snap Kick",
            "Sugar Gene Combo",
            "Floating Butterfly",
            "Stinging Bee",
            "Punch Rush",
            "Chin Music",
            "Invincible Fist",
            "Right High Knee",
            "Drunken Twist",
            "Drunken Fist 1",
            "Drunken Sweep",
            "Toe Touch Kick",
            "Drunken Fall",
            "Heel Drop",
            "Side Swipe",
            "Chin Rocker",
            "Rocket Uppercut",
            "Overhead Blow",
            "Stomping Fist",
            "Right Twister",
            "Left Twister",
            "Half Moon Kick",
            "Guard Breaker 1",
            "Charged Punch 1",
            "Short Uppercut 1",
            "Short Uppercut 2",
            "Short Uppercut 3",
            "Right Hook 1",
            "Right Hook 2",
            "Right Hook 3",
            "Granny Smacker",
            "Yes Man Kablaam",
            "Jab of God",
            "Godly Straight",
            "God Hook",
            "God Uppercut",
            "Godly Chop",
            "Fist of God",
            "High Side Kick 2",
            "High Side Kick 3",
            "Mach Speed Jab 2",
            "Forearm Smash 2",
            "Hayamaker 2",
            "Hayamaker of God",
            "Guard Break 2",
            "God Breaker",
            "Backfist Strike 2",
            "Drunken Fist 2",
            "Charged Punch 2",
            "Charged Punch 3",
            "Charged Punch 4",
            "God Charged Punch"
        };
        constexpr int kMoveCount = static_cast<int>(sizeof(kMoveNames) / sizeof(kMoveNames[0]));

        struct Combo
        {
            int32_t id[kSlots];
            int8_t lv[kSlots];
            uint8_t pad[2];
            int32_t max;
        };
        static_assert(sizeof(Combo) == kSetBytes, "cCoreSaveCombo is 0x24 bytes");

        // ---- the styles in the save data ----
        // cCoreSaveData is 0x14A0 bytes; no game code reads or writes past +0xBC3 (the highest field, Olivia's costume,
        // oliviaCostumeNo, is the word at +0xBC0): only the new game's memset (cCoreSave_systemInit 0x1F9848, so a new game starts with no styles)
        // and the whole-record copies (the checkpoint slot, the memory card) touch the rest. The styles live at +0xC00,
        // with a magic and a checksum so a cleared record or one from another tool reads as "no styles".
        // The checkpoint slot (*(u32 *)0x3C264C, a whole copy of the record taken at every room change and put back on
        // Continue) gets every change too, so a Continue does not take the styles back to the last checkpoint.
        // (writeStore: also after a move was gained since the checkpoint.)
        constexpr uint32_t kStoreOffset = 0xC00u;
        constexpr uint32_t kCheckpointPointer = 0x3C264Cu;
        constexpr uint32_t kStoreMagic = 0x54534D4Bu; // "KMST"
        constexpr uint8_t kStoreVersion = 2u;
        constexpr uint8_t kNoStyle = 0xFFu;
        constexpr uint8_t kNoneKept = 1u; // Store::flags: `none` holds the None moves (kept while a style is on)
        // Version 2 (Nov 7, K): Style None = the game's own moves, the default; L2 / R2 on the Techniques screen cycle
        // None, S, E, X, Y; edits save to the style on show; a style first visited copies the equipped moves. While a
        // style is on, the None moves wait in `none`.
        struct Store
        {
            uint32_t magic;
            uint8_t version;
            uint8_t used;   // bit n: style n filled
            uint8_t active; // the style on (its moves are the equipped ones), kNoStyle = None (styles off)
            uint8_t flags;  // kNoneKept
            Combo styles[kStyles][kSets];
            Combo none[kSets];
            uint32_t checksum;
        };
        static_assert(sizeof(Store) == 8u + (kStyles + 1) * kComboBytes + 4u, "Store layout");
        // Version 1 (Oct 25 .. Nov 7): no None; `active` = the style loaded or saved last.
        struct StoreV1
        {
            uint32_t magic;
            uint8_t version;
            uint8_t used;
            uint8_t active;
            uint8_t reserved;
            Combo styles[kStyles][kSets];
            uint32_t checksum;
        };
        static_assert(kStoreOffset >= game::CoreSaveData::kUsedEnd, "Store clear of the game's fields");
        static_assert(kStoreOffset + sizeof(Store) <= kSaveSize, "Store fits in cCoreSaveData");

        template <typename T> uint32_t checksumOf(const T &store)
        {
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&store);
            uint32_t sum = 0x9E3779B9u;
            for (size_t i = 0; i < offsetof(T, checksum); ++i)
                sum = (sum ^ bytes[i]) * 0x01000193u;
            return sum;
        }

        // ---- the screen and the moment (EE thread, each vblank) -> the pad filter (window thread) ----
        enum Moment : int
        {
            Elsewhere,  // title, menus, cutscenes, minigames...
            Play,       // Gene in a stage under the player's control: a d-pad tap loads a style
            Techniques, // the Techniques screen, browsing the slots: L2 / R2 cycle the styles
        };
        std::atomic<int> g_moment{Elsewhere};

        struct Snapshot
        {
            bool valid = false;
            uint32_t data = 0u;
            Combo combos[kSets] = {};
            Store store = {};
            bool storeValid = false;
        };

        uint8_t *g_ram = nullptr;
        char g_letters[kStyles + 1] = "SEXY";
        std::mutex g_mutex; // the snapshot and the status (EE thread writes, the F2 tab reads)
        Snapshot g_snapshot;
        std::string g_status;
        // Pad filter / F2 -> EE thread: 1..5 pick None, S..Y (F2); 11..14 in play, the d-pad's style; 21 / 22 the
        // Techniques screen's R2 / L2 (next / previous, wrapping).
        std::atomic<int> g_request{0};
        std::atomic<bool> g_cycleSound{false}; // a cycle on the Techniques screen: it plays its cursor sound
        // The HUD's blink after a swap in play: which style and when. Host milliseconds, not frame-hook
        // ticks: the hook runs once a game frame, so a tick count made the blinking last twice as long at 30 fps
        // (Thinso, Oct 27: "saving a style takes twice as long on 30fps").
        std::atomic<int> g_lastStyle{-1};
        bool g_signalOn = false;        // [Game] StyleSignal (Signal Style Switch)
        std::atomic<int> g_signal{0};   // a switch in play to signal: 1..4 = S..Y (onVblank -> onGeneUpdate)
        std::atomic<bool> g_switchedInPlay{false}; // a d-pad switch in play (onVblank -> onGeneUpdate: the chain window)
        std::atomic<int64_t> g_lastActionMs{-100000};
        uint32_t g_vblanks = 0u; // frame-hook ticks (the snapshot refresh)
        constexpr int64_t kFlashMs = 667;  // 40 ticks at 60
        constexpr int64_t kBlinkMs = 133;  // 8 ticks at 60

        int64_t steadyMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        uint32_t rd32(uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }

        bool validPointer(uint32_t p, uint32_t room = 0x100u)
        {
            return p >= 0x100000u && p < 0x2000000u - room;
        }

        uint32_t dataPointer()
        {
            const uint32_t data = rd32(kCoreSave);
            return validPointer(data, kSaveSize) ? data : 0u;
        }

        std::string moveName(int32_t id)
        {
            if (id >= 0 && id < kMoveCount)
                return kMoveNames[id];
            char text[16];
            std::snprintf(text, sizeof(text), "#%d", id);
            return text;
        }

        std::string describe(const Combo &combo)
        {
            const int n = combo.max < 0 ? 0 : combo.max > kSlots ? kSlots : combo.max;
            if (n == 0)
                return "(empty)";
            std::string text;
            for (int i = 0; i < n; ++i)
                text += (i ? ", " : "") + moveName(combo.id[i]);
            return text;
        }

        void read(uint32_t data, Combo (&out)[kSets])
        {
            std::memcpy(out, g_ram + data + kCombos, kComboBytes);
        }

        bool equipped(uint32_t data, const Combo (&style)[kSets]);

        // The record's styles: false (store cleared: None, no styles) when there are none yet or the bytes are not
        // ours. A version 1 record is converted (*converted set: write it back): None = the moves equipped now (K, Nov
        // 7), the last style loaded stays on when the equipped moves are still its own, else None.
        bool readStore(uint32_t data, Store &store, bool *converted = nullptr)
        {
            std::memcpy(&store, g_ram + data + kStoreOffset, sizeof(Store));
            if (store.magic == kStoreMagic && store.version == kStoreVersion && store.checksum == checksumOf(store))
                return true;
            StoreV1 old{};
            std::memcpy(&old, g_ram + data + kStoreOffset, sizeof(StoreV1));
            const bool wasV1 = old.magic == kStoreMagic && old.version == 1u && old.checksum == checksumOf(old);
            store = Store{};
            store.magic = kStoreMagic;
            store.version = kStoreVersion;
            store.active = kNoStyle;
            if (!wasV1)
                return false;
            store.used = static_cast<uint8_t>(old.used & 0x0Fu);
            std::memcpy(store.styles, old.styles, sizeof(store.styles));
            read(data, store.none);
            store.flags = kNoneKept;
            if (old.active < kStyles && (store.used & (1u << old.active)) && equipped(data, store.styles[old.active]))
                store.active = old.active;
            if (converted)
                *converted = true;
            return true;
        }

        void writeStore(uint32_t data, Store &store)
        {
            store.checksum = checksumOf(store);
            std::memcpy(g_ram + data + kStoreOffset, &store, sizeof(Store));
            // The checkpoint slot: the same record (its save pointer is the live one's copy), refreshed at every room
            // change and put back on Continue. Only when it holds this game's record: every move it owns is owned now
            // (moves are only ever gained), so a stale slot from the title is left. It was "the same skills": a move
            // bought or won since the room change left the styles out of the slot and a death took them back to the
            // room's start (T2, Nov 4). After such a Continue the styles keep the new move; a load leaves it out while
            // the save does not own it (as for any unowned move), the rest of the style stays.
            const uint32_t checkpoint = rd32(kCheckpointPointer);
            if (validPointer(checkpoint, kSaveSize) && checkpoint != data)
            {
                const int8_t *was = reinterpret_cast<const int8_t *>(g_ram + checkpoint + kSkills);
                const int8_t *now = reinterpret_cast<const int8_t *>(g_ram + data + kSkills);
                bool sameGame = true;
                for (int id = 0; id < 0x80 && sameGame; ++id)
                    sameGame = was[id] < 0 || now[id] >= 0;
                if (sameGame)
                    std::memcpy(g_ram + checkpoint + kStoreOffset, &store, sizeof(Store));
            }
        }

        // A move the save can equip: owned (skill[id] >= 0), or any move while F2 > Debug "Every technique owned"
        // (DBG_WAZA_FULL_OPEN) is on. Read from the F2 switch itself, not from the GameWork DBG word: debug_tools sets
        // the word at the frame start, and in play it reads clear at this vblank hook (K's log, Oct 31: every save of a
        // style with debug moves was followed by "every technique off" on leaving the Techniques screen, and loads left
        // the moves out). Ids past the game's move table (0..0x71) never.
        bool everyTechnique() { return debug_tools::wazaFullOpen(); }

        bool equippable(const int8_t *skill, int32_t id, bool everyMove)
        {
            return id >= 0 && id < kMoveCount && (everyMove || skill[id] >= 0);
        }

        // The moves of `from` the save can equip, in order; a single slot without one stays empty, the chain closes up.
        // Returns how many were left out.
        int keepEquippable(const Combo (&from)[kSets], Combo (&to)[kSets], const int8_t *skill, bool everyMove)
        {
            int skipped = 0;
            for (int s = 0; s < kSets; ++s)
            {
                const Combo &f = from[s];
                Combo t = {};
                const int n = f.max < 0 ? 0 : f.max > kSlots ? kSlots : f.max;
                int out = 0;
                for (int i = 0; i < n; ++i)
                {
                    const int32_t id = f.id[i];
                    if (!equippable(skill, id, everyMove))
                    {
                        ++skipped;
                        continue;
                    }
                    t.id[out] = id;
                    t.lv[out] = f.lv[i];
                    ++out;
                }
                t.max = out;
                to[s] = t;
            }
            return skipped;
        }

        const int8_t *skillsOf(uint32_t data) { return reinterpret_cast<const int8_t *>(g_ram + data + kSkills); }

        // Load: the style's moves the save can equip.
        int load(uint32_t data, const Combo (&style)[kSets])
        {
            Combo combos[kSets] = {};
            const int skipped = keepEquippable(style, combos, skillsOf(data), everyTechnique());
            std::memcpy(g_ram + data + kCombos, combos, kComboBytes);
            return skipped;
        }

        // "Every technique owned" off: the saved styles lose the moves the save does not own (K, Oct 31: debug moves
        // count as owned only while the switch is on). Returns how many came out.
        int stripStyles(uint32_t data, Store &store)
        {
            int removed = 0;
            for (int slot = 0; slot < kStyles; ++slot)
                if (store.used & (1u << slot))
                    removed += keepEquippable(store.styles[slot], store.styles[slot], skillsOf(data), false);
            if (store.flags & kNoneKept)
                removed += keepEquippable(store.none, store.none, skillsOf(data), false);
            return removed;
        }

        uint32_t wazaScreen(uint32_t subScr) { return subScr + kWazaOffset; }

        bool techniquesBrowsing(uint32_t subScr, bool subScreenOpen)
        {
            return subScreenOpen && static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) == kTechniquesKind &&
                   g_ram[(wazaScreen(subScr) + kWazaState) & 0x1FFFFFFu] == 0u;
        }

        // Gene in a stage under the player's control (research of Oct 25, SLUS_215.03; addresses in the game work block
        // 0x7474A0): a player (Getplayer's 0x747A94), no sub screen open, Gene not ignoring input or the game frozen
        // (0x747A78 bits 31/30: sub screens, movies, events), no scripted "soft event" (0x747A84 bit 0x40: the casino
        // slots, the dog race), Gene not suspended (+0x250 bit 0x8000) and in his normal mode (+0x2F4 == 0; 5 is the
        // sub-screen mode: state table 0x3BD0B8 entry 5, 0x10B780, the decomp's StepPlayerSubscreenMode_10B780, plays the
        // Techniques screen's move demo; 6 the cannon), no tutorial pop-up. Mid-attack counts: the next move of a chain comes from
        // the new set (ComboObj_startNext 0x126918 reads combo[] only when a move starts; a chain shorter than the hit
        // reached starts over).
        bool inPlay(uint32_t subScr, bool subScreenOpen)
        {
            if (subScreenOpen || !validPointer(subScr) || static_cast<int16_t>(rd32(subScr + game::SubScr::kOpen)) != 0)
                return false;
            const uint32_t player = rd32(work::address(work::kPlayerObj));
            if (!validPointer(player, 0x700u))
                return false;
            if ((rd32(work::address(work::kFlags5D8)) & game::kFlags5D8PlayerOff) != 0u || (rd32(work::address(work::kFlags5E4)) & 0x40u) != 0u)
                return false;
            if ((rd32(player + 0x250u) & 0x8000u) != 0u || g_ram[player + 0x2F4u] != 0u)
                return false;
            if (g_ram[player + 0x2F5u] == 0x2Bu)
                return false; // the roulette: the d-pad steps the wheel and (Cycle Roulette Target) its target
            const uint32_t game = rd32(0x3BD6E8u);
            if (validPointer(game, 0x2000u) && g_ram[game + 0x1AD8u] != 0u)
                return false; // a tutorial pop-up
            return true;
        }

        void onVblank()
        {
            ++g_vblanks;
            const uint32_t data = dataPointer();
            const uint32_t subScr = rd32(kSubScrPointer);
            const bool subScreenOpen = validPointer(subScr, 0x1000u) && static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) >= 0;
            int moment = Elsewhere;
            if (data)
            {
                // L2 / R2 on the Techniques screen: no menu uses the triggers, so they work with every menu style.
                if (techniquesBrowsing(subScr, subScreenOpen))
                    moment = Techniques;
                else if (inPlay(subScr, subScreenOpen))
                    moment = Play;
            }
            g_moment.store(moment, std::memory_order_relaxed);

            // "Every technique owned" switched off: the equipped moves the save does not own come off, as a load
            // would leave them (K, Oct 30).
            static bool everyBefore = false;
            const bool everyNow = everyTechnique();
            if (data && everyBefore && !everyNow)
            {
                Combo now[kSets];
                read(data, now);
                if (const int removed = load(data, now))
                    KM_INFO(Game, "styles: every technique off: " << removed << " equipped move(s) the save does not own removed");
            }
            everyBefore = everyNow;

            const int request = g_request.exchange(0);
            Store store{};
            bool converted = false;
            if (data)
                readStore(data, store, &converted);
            if (converted)
            {
                writeStore(data, store);
                KM_INFO(Game, "styles: record converted to version 2 (None = the equipped moves)");
            }
            // While the switch is off the saved styles hold only owned moves (also a record saved with the switch on
            // and loaded later with it off).
            if (data && store.used && !everyNow)
                if (const int removed = stripStyles(data, store))
                {
                    writeStore(data, store);
                    KM_INFO(Game, "styles: every technique off: " << removed << " move(s) the save does not own removed from the styles");
                }

            // A style on is the equipped moves: the Techniques screen saves every change to it (each vblank the screen
            // is open; the picker writes combo[] when a move is set); opening the screen or being in play with other
            // moves equipped (a Continue puts the checkpoint's moves back) puts the style's back.
            const bool wazaOpen = data && subScreenOpen && static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) == kTechniquesKind;
            static bool wazaWas = false;
            const int on = store.active < kStyles && (store.used & (1u << store.active)) ? store.active : -1;
            if (on >= 0 && !equipped(data, store.styles[on]))
            {
                if (wazaOpen && wazaWas)
                {
                    read(data, store.styles[on]);
                    writeStore(data, store);
                }
                else if (wazaOpen || moment == Play)
                    load(data, store.styles[on]);
            }
            wazaWas = wazaOpen;

            if (request != 0)
            {
                // The position to go to: 0 None, 1..4 S..Y.
                const int at = on + 1;
                int to = -1;
                if (request >= 1 && request <= 5)
                    to = request - 1;
                else if (request >= 11 && request <= 14)
                    to = on >= 0 && (store.used & (1u << (request - 11))) ? request - 10 : -1; // in play: styles on, filled
                else if (request == 21 || request == 22)
                    to = moment == Techniques ? (at + (request == 21 ? 1 : kStyles)) % (kStyles + 1) : -1;
                char text[160] = "";
                if (!data)
                    std::snprintf(text, sizeof(text), "No save data in memory yet.");
                else if (to >= 0 && to != at)
                {
                    if (on < 0)
                    {
                        read(data, store.none); // None's moves wait while a style is on
                        store.flags = static_cast<uint8_t>(store.flags | kNoneKept);
                    }
                    int skipped = 0;
                    bool copied = false;
                    if (to == 0)
                    {
                        if (store.flags & kNoneKept)
                            skipped = load(data, store.none);
                        store.active = kNoStyle;
                    }
                    else
                    {
                        const int slot = to - 1;
                        if (store.used & (1u << slot))
                            skipped = load(data, store.styles[slot]);
                        else
                        {
                            read(data, store.styles[slot]); // first visit: the equipped moves (K, Nov 7)
                            store.used = static_cast<uint8_t>(store.used | (1u << slot));
                            copied = true;
                            buck_breaker::styleCreated(at, to); // and the punish shown (K, Nov 7)
                        }
                        store.active = static_cast<uint8_t>(slot);
                    }
                    writeStore(data, store);
                    if (moment == Techniques)
                    {
                        // A shorter chain: the cursor no further than the chain's first empty row.
                        Combo now[kSets];
                        read(data, now);
                        const uint32_t waza = wazaScreen(subScr);
                        const int32_t chain = now[0].max < 0 ? 0 : now[0].max;
                        int8_t &cursor = reinterpret_cast<int8_t &>(g_ram[waza + kWazaCursor]);
                        if (g_ram[waza + kWazaColumn] == 0u && cursor > chain)
                            cursor = static_cast<int8_t>(chain);
                        g_cycleSound = true;
                    }
                    else
                    {
                        g_lastStyle = to - 1;
                        g_lastActionMs = steadyMs();
                        if (request >= 11 && request <= 14)
                        {
                            g_switchedInPlay.store(true, std::memory_order_relaxed); // the chain window (onGeneUpdate)
                            if (g_signalOn)
                            {
                                g_signal.store(to, std::memory_order_relaxed); // Signal Style Switch (onGeneUpdate)
                                style_pose::trigger(); // the arm swing (only if Gene is free)
                                // The switch sound goes with the flash: none while the God Hand is unleashed (K).
                                const uint32_t gene = rd32(0x7474A0u + 0x5F4u);
                                if (validPointer(gene, 0x1700u) && (rd32(gene + 0x15F4u) & 0x10u) == 0u)
                                    style_sfx::play();
                            }
                        }
                    }
                    if (to == 0)
                        std::snprintf(text, sizeof(text), "Style None (styles off).");
                    else
                        std::snprintf(text, sizeof(text), "Style %c%s%s.", g_letters[to - 1],
                                      copied ? " (new: the equipped moves)" : "",
                                      skipped ? " (moves this save does not own left out)" : "");
                }
                if (text[0])
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_status = text;
                    KM_INFO(Game, "styles: " << text << " (save data 0x" << std::hex << data << std::dec << ")");
                }
            }
            if (request != 0 || g_vblanks % 15u == 0u)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_snapshot.valid = data != 0u;
                g_snapshot.data = data;
                if (data)
                {
                    read(data, g_snapshot.combos);
                    g_snapshot.storeValid = readStore(data, g_snapshot.store);
                }
            }
        }

        // ---- input (ps2_host_input's pad filter, window thread, player 1) ----
        int64_t nowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        constexpr uint16_t kDpad[kStyles] = {ps2x::iop::PadButton::Up, ps2x::iop::PadButton::Right,
                                             ps2x::iop::PadButton::Down, ps2x::iop::PadButton::Left};
        constexpr uint16_t kAllDpad = ps2x::iop::PadButton::Up | ps2x::iop::PadButton::Right | ps2x::iop::PadButton::Down |
                                      ps2x::iop::PadButton::Left;
        constexpr uint16_t kTriggers = ps2x::iop::PadButton::L2 | ps2x::iop::PadButton::R2;

        uint16_t g_dpadWas = 0u, g_triggersWas = 0u;

        int firstPressed(uint16_t pressed)
        {
            for (int i = 0; i < kStyles; ++i)
                if (pressed & kDpad[i])
                    return i;
            return -1;
        }

        // The Techniques screen: L2 / R2 are the styles' (no menu uses the triggers; K, Nov 7).
        void hideTriggers(ps2x::iop::PadInputState &state)
        {
            state.buttons = static_cast<uint16_t>(state.buttons & ~kTriggers);
            state.pressure[static_cast<size_t>(ps2x::iop::PadPressure::L2)] = 0u;
            state.pressure[static_cast<size_t>(ps2x::iop::PadPressure::R2)] = 0u;
        }

        // ---- the Techniques screen (Nov 7, K) ----
        // The style row "Style: None / S / E / X / Y" under the big TECHNIQUES title (the space above the slots'
        // TECHNIQUES bar), the one on gold, filled beige, empty dim, with its shadow: the "Already Set" text and shadow
        // (parts[0x23] / [0x24], shown by the game only while the move picker is open, to say the highlighted move is in
        // another slot) carry it while the slots are browsed. The hints "Style S: [up]" .. "Style Y: [left]", two by two
        // right of the title (x ~220..365, y ~18..66 of the 512 x 448 menu screen; K, Nov 10: four lines until the Punish
        // slot moved to the column's top) go in that group's move name (parts[0x25], a text entry, no shadow).
        // The rest of the group ([0x26] the "down+" art, [0x27] the slot's button, the name box: record #57, no id) is
        // hidden. The texts are placeholder messages (unused "<7d>": 0x30C4 the row, 0x30FE its shadow (no colour codes,
        // which would colour the shadow too), 0x3043 the lines) pointed at strings of ours. The bottom bar's spare line
        // (ids 57/58: "Release" on the chain's last rows, otherwise blank 0x30D0) says "[L2] [R2] Cycle Style" (0x30FF).
        // Entry fields: +0x2C flags (0x08000000 = not drawn), +0x50/+0x54 draw position, +0x90 message id.
        //
        // The arrows are the icon sheet's style pieces (button_prompt_sheets.h: a down arrow and a right arrow shaped
        // like the game's "+down" art, in the sheet's empty cells; up and left are the arrows flipped), drawn in place of
        // icon codes 4..7 (L1 R1 L2 R2, never in the lines' text) while the lines' entry is drawn, at the size the game
        // draws its own "+down" art next to a button.
        constexpr uint32_t kNoDraw = game::kIdentFlagNoDraw;
        constexpr uint32_t kMessageBankPointer = 0x3C23A4u; // *(*(u32 *)0x3C23A4 + 0xC) = the message bank
        constexpr uint16_t kRowMessage = 0x30C4u, kRowShadowMessage = 0x30FEu, kHintMessage = 0x30FFu, kBlankMessage = 0x30D0u;
        constexpr uint16_t kListMessage = 0x3043u; // one more "<7d>" placeholder of bank 3
        // Strings in the save record's unused tail, past the Store (0xC00..0x1044), rewritten every frame (outside the
        // checksum); each with its room (writeText cuts a longer one).
        constexpr uint32_t kListText = 0x1050u, kRowText = 0x1150u, kRowShadowText = 0x1250u, kHintText = 0x1300u;
        constexpr uint32_t kHudText = 0x1340u, kHudShadowText = 0x13C0u, kTextEnd = 0x1440u;
        static_assert(kStoreOffset + sizeof(Store) <= kListText, "the strings clear of the Store");
        static_assert(kTextEnd <= kSaveSize, "the strings in cCoreSaveData");
        constexpr uint16_t kHudMessage = 0x3041u, kHudShadowMessage = 0x3042u;            // two more "<7d>" placeholders of bank 3
        constexpr float kListX = 290.5f, kListY = 42.0f; // the hints' block centre (align 0): right of the big title, level with it, above the Punish slot (K, Nov 10)
        constexpr float kRowX = 100.0f, kRowY = 78.0f;   // the row's centre: under the title, above the TECHNIQUES bar
        constexpr float kTextScale = 0.65f;
        constexpr float kListScale = 0.55f; // the hints, smaller and tighter beside the title (K, Nov 10)
        // Line pitch: a new line moves down by the line's height (the tallest code: a letter, 41 x the scale = 26.8 at
        // 0.65; icons are 32 x the scale) + state+0x30 x state+0x48. The game's +0x30 at this scale gives ~22 (K: right).
        constexpr float kLinePitch = 19.0f, kLineHeight = 26.8f; // the hints' pitch (22 at 0.65 before Nov 10)
        constexpr uint16_t kArrowCode[kStyles] = {4u, 5u, 6u, 7u}; // up, right, down, left
        constexpr uint16_t kL2Icon = 6u, kR2Icon = 7u;             // the game's own (prompt set) icons in the hint
        constexpr uint32_t kFontState = game::MessDrawFont::kInstance; // the font's draw state (D_005E7510)
        constexpr float kArrowTextGap = 5.0f;                      // texels after the arrow
        // The pads: icon codes 8 / 9 (L3 / R3, never in the hints' text), drawn as nothing, advancing by what X is wider
        // than S and Y than E, in font units (K's Nov 10 screenshot at 2.46 px a font unit: 8 px and 7 px). Only for
        // Gene's SEXY: Xavier's LIFE gets no pads.
        constexpr uint16_t kPadCode[2] = {8u, 9u}; // after the first line's letters: S, E
        constexpr float kLetterPad[2] = {3.25f, 2.85f};
        constexpr int kPadPending = 100; // g_pendingArrow: a pad's quad collapses (100, 101: which pad)
        constexpr uint16_t kPlus = 0x0Eu, kSlash = 0x2Du, kSpace = 0x10u, kNewLine = 0xA001u, kEnd = 0x8001u;
        constexpr uint16_t kColour = 0xB203u, kColourRestore = 0xB301u; // 0xB203 hi lo: A B G R (0x80 = full)
        constexpr uint32_t kGold = 0x80106880u, kDim = 0x80343434u; // dim: darker than the cream (K, Oct 26)

        uint32_t g_listEntry = 0u;      // the lines' entry, the arrows are drawn for it (EE thread)
        uint32_t g_drawingEntry = 0u;   // the cIDBase entry being drawn (hud_layout's hook)
        bool g_listShown = false;       // this frame's row and lines are on (the screen's draw places them)
        uint32_t g_hidBox = 0u;         // the name box, hidden by us, shown again when the picker opens
        uint32_t g_nameEntry = 0u;      // parts[0x25] while it carries the lines: its message given back after
        uint16_t g_nameMsg = 0xFFFFu;
        uint8_t g_nameColourSet = 0u;
        int g_pendingArrow = -1;        // the icon drawer is drawing this arrow (its quad is shaped next)

        uint16_t glyph(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return static_cast<uint16_t>(0x40 + (c - 'A'));
            if (c >= 'a' && c <= 'z')
                return static_cast<uint16_t>(0x5A + (c - 'a'));
            if (c >= '0' && c <= '9')
                return static_cast<uint16_t>(0x36 + (c - '0'));
            if (c == ':')
                return 0x31u;
            if (c == '-')
                return 0x1Fu;
            if (c == '+')
                return kPlus;
            if (c == '/')
                return kSlash;
            return kSpace;
        }

        void append(std::vector<uint16_t> &out, const char *text)
        {
            for (const char *c = text; *c; ++c)
                out.push_back(glyph(*c));
        }

        void wr16(uint32_t address, uint16_t value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        void wr32(uint32_t address, uint32_t value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        uint16_t rd16(uint32_t address)
        {
            uint16_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        float rdFloat(uint32_t address)
        {
            float v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        void wrFloat(uint32_t address, float value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }

        void writeText(uint32_t address, std::vector<uint16_t> text, uint32_t room)
        {
            const size_t most = room / sizeof(uint16_t);
            if (text.size() > most)
            {
                text.resize(most);
                text.back() = kEnd;
            }
            std::memcpy(g_ram + (address & 0x1FFFFFFu), text.data(), text.size() * sizeof(uint16_t));
        }

        // Points a message id at a string of ours (the bank adds its offsets as u32, so a string below it is fine).
        bool pointMessage(uint16_t id, uint32_t text)
        {
            const uint32_t holder = rd32(kMessageBankPointer);
            if (!validPointer(holder))
                return false;
            const uint32_t bank = rd32(holder + 0xCu);
            if (!validPointer(bank, 0x1000u))
                return false;
            const uint32_t word = bank + (static_cast<uint32_t>(id) - 0x3000u) * 8u + 4u;
            if (rd32(word) != text - bank)
                wr32(word, text - bank);
            return true;
        }

        // The equipped moves are this style's (as loading it gave them): ids and lengths, levels aside.
        bool equipped(uint32_t data, const Combo (&style)[kSets])
        {
            Combo now[kSets];
            read(data, now);
            const int8_t *skill = reinterpret_cast<const int8_t *>(g_ram + data + kSkills);
            const bool everyMove = everyTechnique();
            for (int s = 0; s < kSets; ++s)
            {
                const int n = style[s].max < 0 ? 0 : style[s].max > kSlots ? kSlots : style[s].max;
                int out = 0;
                for (int i = 0; i < n; ++i)
                {
                    const int32_t id = style[s].id[i];
                    if (!equippable(skill, id, everyMove))
                        continue;
                    if (out >= now[s].max || now[s].id[out] != id)
                        return false;
                    ++out;
                }
                if (out != now[s].max)
                    return false;
            }
            return true;
        }

        // ---- the in-play indicator ----
        // The style letters in the HUD (K, Oct 26): "S E X Y" above the level meter (Oct 31; first right of it),
        // the loaded style gold, the others cream, empty ones dim grey; blinks like the list right after a swap. The
        // text is carried by the money display's two spare text entries (cCockMoney's layout: 6 entries, the last
        // two text entries (kind 2) with message 0, at 16,14, never drawn: the text and its shadow), pointed at two
        // more "<7d>" placeholder messages of bank 3. The layout is found by that shape every frame (cIDBase_move);
        // hud_layout's cIDBase_trans hook places the two entries right before they are drawn and anchors them to the
        // left edge with the level meter (onHudEntry), not with the money at the right.
        // Centred 97: the first letter starts about 11 units left of the first charge slot (86.5; the six slots span
        // 86.5..196.5), just clear of the meter's ring at that height, so the letters, the charges and the bars step
        // round the ring (K, Oct 26: 142 over the middle of the six slots was far from the ring; 108, over the
        // first slot, wanted a little further left). The text is about 44 units wide at 0.65.
        // Oct 31 (K: nothing uses the space above the level meter): centred over the meter's ring (its quads span x
        // 4.5..79.5 / 8..88, top at y 351 in the F6 dump), the letters' bottom just above its top.
        constexpr float kHudX = 44.0f, kHudY = 332.0f, kHudScale = 0.65f; // 339 overlapped the ring (K's F6: glyphs 331..356)
        // The cream of the Techniques page's lines, in the GS's colour scale (0x80 = full: the glyphs are white, so
        // the colour is what shows). It was 0xDC 0xC8 0xA0, the on-screen value, which the GS doubles past full:
        // white on screen (K, Oct 26).
        constexpr uint32_t kBeige = 0x8050646Eu; // A B G R
        uint32_t g_hudText = 0u, g_hudShadow = 0u; // the two entries (EE thread); 0 = not found
        bool g_hudShown = false;

        constexpr int kOutlineCopies = 8;
        std::vector<uint16_t> hudText(const Store &store, bool colours)
        {
            const int64_t now = steadyMs();
            const bool blinkOn = (now / kBlinkMs) % 2 == 0;
            const bool flash = now - g_lastActionMs.load(std::memory_order_relaxed) < kFlashMs;
            // K, Nov 10: the letters sit round the minimap (hud_layout moves each glyph there: S top, E right, X bottom,
            // Y left). The text is the four letters; the shadow carries each letter kOutlineCopies times, spread round it
            // by hud_layout, as a full outline (readable on both the PS2 and the texture pack's minimap).
            std::vector<uint16_t> out;
            if (!colours)
            {
                for (int slot = 0; slot < kStyles; ++slot)
                    out.insert(out.end(), kOutlineCopies, glyph(g_letters[slot]));
                out.push_back(kEnd);
                return out;
            }
            for (int slot = 0; slot < kStyles; ++slot)
            {
                const bool used = (store.used & (1u << slot)) != 0u;
                uint32_t colour = kBeige;
                if (used && store.active == slot)
                    colour = (flash && !blinkOn && g_lastStyle.load(std::memory_order_relaxed) == slot) ? kBeige : kGold;
                else if (!used)
                    colour = kDim;
                if (colours)
                    out.insert(out.end(), {kColour, static_cast<uint16_t>(colour >> 16), static_cast<uint16_t>(colour & 0xFFFFu)});
                out.push_back(glyph(g_letters[slot]));
            }
            out.push_back(kEnd);
            return out;
        }

        void colourOn(std::vector<uint16_t> &out, uint32_t colour)
        {
            out.insert(out.end(), {kColour, static_cast<uint16_t>(colour >> 16), static_cast<uint16_t>(colour & 0xFFFFu)});
        }

        // The hints, two by two (K, Nov 10: the Punish slot took the column's top): "Style S: [up] Style E: [right]" /
        // "Style X: [down] Style Y: [left]", the style on gold, empty ones dim. The first column's arrows are the narrow
        // ones, the second's the wide ones; S and E are followed by an invisible pad (kPadCode) as wide as the X / Y
        // below them are wider, so the colons, the arrows and the second column's "Style" line up.
        constexpr int kHintGrid[2][2] = {{0, 1}, {2, 3}}; // S E / X Y
        constexpr int kHintColumnGap = 1;                  // spaces between the two columns
        std::vector<uint16_t> listText(const Store &store)
        {
            std::vector<uint16_t> out;
            for (int at = 0; at < kStyles; ++at)
            {
                const int slot = kHintGrid[at / 2][at % 2];
                if (at == 2)
                    out.push_back(kNewLine);
                else if (at % 2 == 1)
                    out.insert(out.end(), kHintColumnGap, kSpace);
                const bool used = (store.used & (1u << slot)) != 0u;
                const uint32_t colour = used && store.active == slot ? kGold : used ? 0u : kDim; // 0: the line's own
                if (colour)
                    colourOn(out, colour);
                append(out, "Style ");
                out.push_back(glyph(g_letters[slot]));
                if ((slot == 0 || slot == 1) && std::memcmp(g_letters, "SEXY", kStyles) == 0)
                    out.push_back(kPadCode[slot]); // S / E as wide as X / Y below them: the columns line up
                append(out, ": ");
                out.push_back(kArrowCode[slot]);
                if (colour)
                    out.push_back(kColourRestore);
            }
            out.push_back(kEnd);
            return out;
        }

        // "Style: None / S / E / X / Y": the one on gold, filled cream (the line's own), empty dim. The shadow has no
        // colour codes.
        std::vector<uint16_t> rowText(const Store &store, bool colours)
        {
            std::vector<uint16_t> out;
            append(out, "Style: ");
            for (int at = 0; at <= kStyles; ++at)
            {
                if (at > 0)
                    append(out, " / ");
                const int slot = at - 1;
                const bool on = slot < 0 ? store.active >= kStyles : store.active == slot;
                const bool used = slot < 0 || (store.used & (1u << slot)) != 0u;
                const uint32_t colour = on ? kGold : used ? 0u : kDim;
                if (colours && colour)
                    colourOn(out, colour);
                if (slot < 0)
                    append(out, "None");
                else
                    out.push_back(glyph(g_letters[slot]));
                if (colours && colour)
                    out.push_back(kColourRestore);
            }
            out.push_back(kEnd);
            return out;
        }

        std::vector<uint16_t> hintText()
        {
            // "[L2] [R2] Cycle Style" (K, Nov 7): the game's trigger icons, which follow the prompt set.
            std::vector<uint16_t> out{kL2Icon, kSpace, kR2Icon};
            append(out, "Cycle Style");
            out.push_back(kEnd);
            return out;
        }

        uint32_t part(uint32_t waza, int id) { return rd32(waza + 0x80u + 4u * static_cast<uint32_t>(id)); }
        void setNoDraw(uint32_t entry, bool hidden)
        {
            if (!validPointer(entry, ent::kSize))
                return;
            const uint32_t flags = rd32(entry + ent::kFlags);
            wr32(entry + ent::kFlags, hidden ? (flags | kNoDraw) : (flags & ~kNoDraw));
        }

        // The money display's layout (cCockMoney): the indicator goes into its two spare text entries.
        // The two entries are not always spare: cCockMoney's update (func_00143670, 0x143924..0x14394C) shows the
        // game's action prompt in them ("A :Throw": message 0x3010 in both, the text entry's no-draw bit cleared) and
        // afterwards only sets no-draw again, leaving 0x3010 in place. So: 0x3010 with no-draw set is free (Nov 1, K:
        // after the first prompt the letters never came back), 0x3010 shown is the game's, and then the entries get
        // back the alignment and scale the indicator changed (the prompt was drawn small and off its place).
        constexpr uint16_t kGamePromptMessage = 0x3010u;
        struct HudEntryWas
        {
            uint32_t entry = 0u, noDraw = 0u;
            float sclX = 1.0f, sclY = 1.0f;
            uint8_t align = 0u;
        };
        HudEntryWas g_hudWas[2];
        // 0: not that layout; 1: free or ours; 2: the game's prompt is up.
        int hudLayoutState(uint32_t layout, uint32_t &text, uint32_t &shadow)
        {
            if (!validPointer(layout, 0x10u) || rd32(layout + game::IDBase::kEntNum) != 6u)
                return 0;
            const uint32_t entries = rd32(layout + game::IDBase::kEnt);
            if (!validPointer(entries, 6u * ent::kSize))
                return 0;
            text = entries + 4u * ent::kSize;
            shadow = entries + 5u * ent::kSize;
            for (const uint32_t e : {text, shadow})
            {
                const uint16_t msg = rd16(e + ent::kMsg);
                if (g_ram[(e + ent::kKind) & 0x1FFFFFFu] != game::kIdentKindText ||
                    (msg != 0u && msg != kHudMessage && msg != kHudShadowMessage && msg != kGamePromptMessage))
                    return 0;
            }
            if (rd16(text + ent::kMsg) == kGamePromptMessage && (rd32(text + ent::kFlags) & kNoDraw) == 0u)
                return 2;
            return 1;
        }
        void claimHudEntry(int i, uint32_t entry)
        {
            HudEntryWas &was = g_hudWas[i];
            if (was.entry == entry)
                return;
            was.entry = entry;
            was.noDraw = rd32(entry + ent::kFlags) & kNoDraw;
            was.sclX = rdFloat(entry + ent::kDrawSclX);
            was.sclY = rdFloat(entry + ent::kDrawSclY);
            was.align = g_ram[(entry + ent::kAlign) & 0x1FFFFFFu];
        }
        // Scale and alignment from the entry's source record (ent +0x1C; cIDBase_initEntry: scale +0x28 / +0x2C,
        // alignment +0x54 = 1, left), whatever the entry holds: a save state taken while the indicator showed carries
        // its values (centred), and the prompt came back centred on x 16 with its button icon off the screen's left
        // edge (K's F6, Nov 4).
        void restoreFromSource(uint32_t entry)
        {
            const uint32_t src = rd32(entry + 0x1Cu);
            if (!validPointer(src, 0x60u))
                return;
            wrFloat(entry + ent::kDrawSclX, rdFloat(src + 0x28u));
            wrFloat(entry + ent::kDrawSclY, rdFloat(src + 0x2Cu));
            g_ram[(entry + ent::kAlign) & 0x1FFFFFFu] = g_ram[(src + 0x54u) & 0x1FFFFFFu];
        }

        // The game's prompt is in them: its scale and alignment back; the text's no-draw is the game's (clear), the
        // shadow's as it was.
        void releaseHudEntries()
        {
            for (int i = 0; i < 2; ++i)
            {
                HudEntryWas &was = g_hudWas[i];
                if (was.entry == 0u || !validPointer(was.entry, ent::kSize))
                {
                    was.entry = 0u;
                    continue;
                }
                if (i == 1)
                    wr32(was.entry + ent::kFlags, (rd32(was.entry + ent::kFlags) & ~kNoDraw) | was.noDraw);
                wrFloat(was.entry + ent::kDrawSclX, was.sclX);
                wrFloat(was.entry + ent::kDrawSclY, was.sclY);
                g_ram[(was.entry + ent::kAlign) & 0x1FFFFFFu] = was.align;
                restoreFromSource(was.entry);
                was.entry = 0u;
            }
            g_hudShown = false;
        }

        void hudIndicator(uint32_t layout)
        {
            uint32_t text, shadow;
            const int state = hudLayoutState(layout, text, shadow);
            if (state == 0)
                return;
            if (state == 2)
            {
                releaseHudEntries();
                restoreFromSource(text);
                restoreFromSource(shadow);
                return;
            }
            claimHudEntry(0, text);
            claimHudEntry(1, shadow);
            const uint32_t data = dataPointer();
            Store store{};
            const bool show = data && readStore(data, store) && store.active < kStyles; // None: styles off, no letters
            g_hudText = text;
            g_hudShadow = shadow;
            if (!show)
            {
                g_hudShown = false;
                setNoDraw(text, true);
                setNoDraw(shadow, true);
                return;
            }
            writeText(data + kHudText, hudText(store, true), kHudShadowText - kHudText);
            writeText(data + kHudShadowText, hudText(store, false), kTextEnd - kHudShadowText);
            if (!pointMessage(kHudMessage, data + kHudText) || !pointMessage(kHudShadowMessage, data + kHudShadowText))
                return;
            for (const uint32_t e : {text, shadow})
            {
                wr16(e + ent::kMsg, e == text ? kHudMessage : kHudShadowMessage);
                g_ram[(e + ent::kAlign) & 0x1FFFFFFu] = 0u; // align: the block centred on its position
                wrFloat(e + ent::kDrawSclX, kHudScale);
                wrFloat(e + ent::kDrawSclY, kHudScale);
                setNoDraw(e, false);
            }
            g_hudShown = true;
        }

        // cIDBase_move entry (every layout, every frame; EE thread): for the Techniques screen's layout, after its
        // update set this frame's entries and before they are placed, the list goes in (or the group is given back).
        void onLayoutMove(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram)
                return;
            hudIndicator(GPR_U32(ctx, 4));
            const uint32_t subScr = rd32(kSubScrPointer);
            if (!validPointer(subScr, 0x1000u) || GPR_U32(ctx, 4) != wazaScreen(subScr))
                return;
            const uint32_t waza = wazaScreen(subScr);
            const uint32_t data = dataPointer();
            const bool show = data && static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) == kTechniquesKind &&
                              g_ram[waza + kWazaState] == 0u;
            const uint32_t text = part(waza, 0x23), shadow = part(waza, 0x24), name = part(waza, 0x25),
                           hintEntry = part(waza, 57), hintShadow = part(waza, 58);
            g_listShown = false;
            if (!show)
            {
                // The game's own use of the group (the picker's "Already Set"): what we changed comes back.
                if (g_hidBox)
                    setNoDraw(g_hidBox, false);
                if (g_nameEntry && validPointer(g_nameEntry, ent::kSize))
                {
                    wr16(g_nameEntry + ent::kMsg, g_nameMsg);
                    g_ram[(g_nameEntry + ent::kColourSet) & 0x1FFFFFFu] = g_nameColourSet;
                    restoreFromSource(g_nameEntry);
                    setNoDraw(g_nameEntry, false);
                }
                g_hidBox = g_nameEntry = 0u;
                g_listEntry = 0u;
                for (const uint32_t entry : {hintEntry, hintShadow})
                    if (validPointer(entry, ent::kSize) && rd16(entry + ent::kMsg) == kHintMessage)
                        wr16(entry + ent::kMsg, kBlankMessage);
                return;
            }
            if (!validPointer(text, ent::kSize) || !validPointer(shadow, ent::kSize))
                return;
            Store store{};
            readStore(data, store);
            writeText(data + kRowText, rowText(store, true), kRowShadowText - kRowText);
            writeText(data + kRowShadowText, rowText(store, false), kHintText - kRowShadowText);
            writeText(data + kListText, listText(store), kRowText - kListText);
            writeText(data + kHintText, hintText(), kHudText - kHintText);
            if (!pointMessage(kRowMessage, data + kRowText) || !pointMessage(kRowShadowMessage, data + kRowShadowText) ||
                !pointMessage(kListMessage, data + kListText) || !pointMessage(kHintMessage, data + kHintText))
                return;
            setNoDraw(text, false);
            wr16(text + ent::kMsg, kRowMessage);
            wr16(shadow + ent::kMsg, kRowShadowMessage);
            // The lines in the move name (a text entry: anything else is left alone, and the lines go unshown).
            if (validPointer(name, ent::kSize) && g_ram[(name + ent::kKind) & 0x1FFFFFFu] == game::kIdentKindText)
            {
                if (g_nameEntry != (name & 0x1FFFFFFu))
                {
                    g_nameEntry = name & 0x1FFFFFFu;
                    g_nameMsg = rd16(name + ent::kMsg);
                    g_nameColourSet = g_ram[(name + ent::kColourSet) & 0x1FFFFFFu];
                }
                wr16(name + ent::kMsg, kListMessage);
                g_ram[(name + ent::kAlign) & 0x1FFFFFFu] = 0u; // the block centred on its position
                g_ram[(name + ent::kColourSet) & 0x1FFFFFFu] = g_ram[(text + ent::kColourSet) & 0x1FFFFFFu];
                setNoDraw(name, false);
                g_listEntry = g_nameEntry;
            }
            for (const uint32_t e : {text, shadow, name})
                if (validPointer(e, ent::kSize))
                {
                    wrFloat(e + ent::kDrawSclX, e == name ? kListScale : kTextScale);
                    wrFloat(e + ent::kDrawSclY, e == name ? kListScale : kTextScale);
                }
            const uint32_t entries = rd32(waza + 0x4u);
            const uint32_t box = validPointer(entries, 0x4000u) ? entries + 57u * ent::kSize : 0u;
            setNoDraw(box, true);
            g_hidBox = validPointer(box, ent::kSize) ? box : 0u;
            setNoDraw(part(waza, 0x26), true);
            setNoDraw(part(waza, 0x27), true);
            for (const uint32_t entry : {hintEntry, hintShadow})
                if (validPointer(entry, ent::kSize) && rd16(entry + ent::kMsg) == kBlankMessage)
                    wr16(entry + ent::kMsg, kHintMessage);
            g_listShown = true;
        }

        // The Techniques screen's draw (0x16DC18, $a0 = the screen; after its layout was placed): the list's place.
        // The Techniques screen's cursor sound (its input, 0x16DFC4: func_002CB8A0(0x5FEE00, 0, 0x15F, -1, -1, 0, 0)),
        // run to its end on a copy of the registers (game_options.cpp callGuest).
        void playCursorSound(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            constexpr uint32_t kSeCall = 0x2CB8A0u;
            if (!runtime || !runtime->hasFunction(kSeCall))
                return;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(kSeCall);
            if (!fn)
                return;
            const R5900Context saved = *ctx;
            const uint32_t args[] = {0x5FEE00u, 0u, 0x15Fu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u};
            for (int i = 0; i < 7; ++i)
                SET_GPR_U32(ctx, 4 + i, args[i]);
            ctx->pc = kSeCall; // a hooked function finds its hook by the pc (game_options.cpp callGuest)
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            *ctx = saved;
        }

        // ---- Signal Style Switch (K, Nov 10): a voice line and the roulette's arm flash on a switch in play ----
        // A guest function run to its end on a copy of the registers; its $v0.
        uint32_t callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address,
                           std::initializer_list<uint32_t> args)
        {
            if (!runtime || !runtime->hasFunction(address))
                return 0u;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return 0u;
            const R5900Context saved = *ctx;
            int reg = 4;
            for (uint32_t a : args)
            {
                if (reg > 11)
                    break;
                SET_GPR_U32(ctx, reg, a); // a macro: it reads its register argument more than once (no reg++ in it)
                ++reg;
            }
            ctx->pc = address; // a hooked function finds its hook by the pc (game_options.cpp callGuest)
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            const uint32_t v0 = GPR_U32(ctx, 2);
            *ctx = saved;
            return v0;
        }

        // Gene's 48 kHz lines are streams of se/pl00.afs, listed in se/pl00.afd (entry n = stream n; the .afd is also
        // the .ses's section at +0x18). A sound record (0x30 bytes; +6 type, +0x0E & 0x8000 empty, +0x21 random count,
        // +0x28 afd entry) of type 0x30 / 0x31 is a stream sound; pl00.ses has two, 48 and 49 (entry 14, "Shit!").
        // pl00.ses is cSnd slot / Sde bank 3 (cSnd_LoadStartupBanks: 0 "core", 3 "pl00"). Gene's sounds are called as
        // bank 0 with fixed ids that cSndSeVoice_Setup moves to bank 3 (0x2CE040: 0xC8..0xE8 -> 0..0x20, 0xEA, 0xEE/F,
        // 0xF2/3, 0xF8/9 -> 0x30/1, 0x109, 0x10B); calling bank 3 directly gives no voice (K's log, Nov 10: handle 0).
        // So record 48 (id 0xF8) is played the game's way with its +0x28 pointed at the style's line for a while (the
        // driver reads the record when the sound starts, SdeIssTblGet 0x374CB8: bank table 0x76DF10, 0x20 a bank, +0
        // count, +4 records), then put back.
        constexpr uint32_t kSnd = 0x5FEE00u;                       // cSnd
        constexpr uint32_t kSeCallObj = 0x2CBA48u;                 // cSnd_SeCall(snd, bank, id, obj, 0, 0, 0, 0)
        constexpr uint32_t kSeStop = 0x2CBBA0u;                    // cSnd_SeStop(snd, handle)
        constexpr uint32_t kSdeBanks = 0x76DF10u;
        constexpr uint32_t kGeneBank = 3u;                         // pl00.ses (checked by its records, else searched)
        constexpr uint32_t kStreamRecord = 48u, kStreamCallId = 0xF8u; // record 48 as the game calls it (bank 0)
        constexpr uint16_t kStreamLine = 14u;                      // record 48's own line ("Shit!")
        constexpr int64_t kRestoreMs = 2000;                       // record 48 back to its own line after this (K)
        constexpr uint16_t kStyleLines[kStyles] = {8u, 2u, 12u, 6u}; // afd entries: S Cool, E Nice, X Great, Y Awesome
        // The flash (K, Nov 10: the roulette's; the arm glow's on, effect 0x2E, was too strong): func_00114608 step 1,
        // SetEffect(0, 0xE, gene, 0, -1, -1) for costumes 0..7 even (its jump table 0x41C210); costume = save data +0xAE
        // (cCoreSave_getCostumeNo). None while the God Hand is unleashed (+0x15F4 & 0x10; K liked that).
        constexpr uint32_t kSetEffect = 0x306528u;
        void flash(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t gene)
        {
            const uint32_t data = dataPointer();
            const uint32_t costume = data ? g_ram[data + 0xAEu] : 0u;
            if (costume < 8u && (costume & 1u) == 0u && (rd32(gene + 0x15F4u) & 0x10u) == 0u)
                callGuest(rdram, ctx, runtime, kSetEffect, {0u, 0xEu, gene, 0u, 0xFFFFFFFFu, 0xFFFFFFFFu});
        }
        uint32_t g_signalHandle = 0u;  // the line playing (cut by the next switch's)
        uint32_t g_patchedRecord = 0u; // record 48 while it points at a style's line
        int64_t g_restoreAt = 0;

        uint32_t records(uint32_t bank, int16_t &count)
        {
            const uint32_t at = kSdeBanks + bank * 0x20u;
            count = static_cast<int16_t>(rd32(at) & 0xFFFFu);
            const uint32_t list = rd32(at + 4u);
            return count > 0 && validPointer(list, static_cast<uint32_t>(count) * 0x30u) ? list : 0u;
        }
        uint32_t streamRecord(uint32_t bank)
        {
            int16_t count = 0;
            const uint32_t list = records(bank, count);
            if (!list || count <= static_cast<int16_t>(kStreamRecord))
                return 0u;
            const uint32_t r = list + kStreamRecord * 0x30u;
            return (g_ram[r + 6u] == 0x31u || r == g_patchedRecord) && (g_ram[r + 0xFu] & 0x80u) == 0u ? r : 0u;
        }
        // Type 0x31 looks like "one of the lines up to +0x28" (the pickup's random positive line, 0x1C198C: 0xF8 or
        // 0xF9 at random, both entry 14), type 0x30 "the line +0x28" (r000's narration, core's jingles).
        void writeLine(uint32_t record, uint8_t type, uint16_t line)
        {
            g_ram[record + 6u] = type;
            g_ram[record + 0x28u] = static_cast<uint8_t>(line);
            g_ram[record + 0x29u] = static_cast<uint8_t>(line >> 8);
        }
        void restoreRecord()
        {
            if (g_patchedRecord && validPointer(g_patchedRecord, 0x30u) && g_ram[g_patchedRecord + 6u] == 0x30u)
                writeLine(g_patchedRecord, 0x31u, kStreamLine);
            g_patchedRecord = 0u;
        }

        void playStyleLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t gene, int slot)
        {
            uint32_t record = streamRecord(kGeneBank);
            if (!record || ((rd32(record + 0x28u) & 0xFFFFu) != kStreamLine && record != g_patchedRecord))
            {
                KM_INFO(Game, "styles: signal: Gene's sound bank (3) not as expected, no voice line");
                return;
            }
            if (g_signalHandle)
                callGuest(rdram, ctx, runtime, kSeStop, {kSnd, g_signalHandle}); // a new switch cuts the last line
            writeLine(record, 0x30u, kStyleLines[slot]);
            g_patchedRecord = record;
            g_restoreAt = steadyMs() + kRestoreMs;
            g_signalHandle = callGuest(rdram, ctx, runtime, kSeCallObj, {kSnd, 0u, kStreamCallId, gene, 0u, 0u, 0u, 0u});
        }

        // The Square chain's window (K, Nov 11: a switch mid-chain gets 2/3 s for the next attack). The chain attack
        // state func_0010D3A8 sets Gene's +0x5F0 to 10 when each move starts; it counts down (one per 30 FPS frame, also
        // at 60) while the move's cancel window is open (+0x3AC & 0x10), and at 0 the chain resets (+0x5E0 index and
        // +0x5E2 set to 0, 0x10DC0C): the next press starts at move 1. A press counts for 3 frames (+0x621..0x625,
        // 0x108F38). A switch while the chain is on (+0x5E0 > 0, +0x5E2 == 0 the Square chain, +0x5F0 > 0) raises the
        // count to kChainAfterSwitch; the next move sets it back to 10. (func_0010E5B8, Triangle's attack state, keeps
        // a copy of this counter but has no chain.)
        constexpr uint16_t kChainAfterSwitch = 20; // 30 FPS frames
        void widenChainWindow(uint32_t gene)
        {
            uint16_t index = 0, set = 0, count = 0;
            std::memcpy(&index, g_ram + gene + 0x5E0u, 2);
            std::memcpy(&set, g_ram + gene + 0x5E2u, 2);
            std::memcpy(&count, g_ram + gene + 0x5F0u, 2);
            if (index == 0u || set != 0u || count == 0u || count >= kChainAfterSwitch)
                return;
            std::memcpy(g_ram + gene + 0x5F0u, &kChainAfterSwitch, 2);
        }

        // Run inside Gene's own update (cObjBase_SetSeqEffect, called by it every frame, 0x1095F0), where the game plays
        // his sounds and effects: from the frame start (the top of the main loop) the sound call gave no voice.
        void onGeneUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_ram || GPR_U32(ctx, 4) != rd32(0x7474A0u + 0x5F4u))
                return;
            if (g_switchedInPlay.exchange(false, std::memory_order_relaxed) && validPointer(GPR_U32(ctx, 4), 0x1700u))
                widenChainWindow(GPR_U32(ctx, 4));
            const int64_t now = steadyMs();
            if (g_patchedRecord && now >= g_restoreAt)
                restoreRecord();
            const int to = g_signal.exchange(0, std::memory_order_relaxed);
            if (to < 1 || to > kStyles)
                return;
            const uint32_t gene = GPR_U32(ctx, 4);
            if (!validPointer(gene, 0x1700u))
                return;
            playStyleLine(rdram, ctx, runtime, gene, to - 1);
            flash(rdram, ctx, runtime, gene);
        }

        void onWazaDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (g_cycleSound.exchange(false))
                playCursorSound(rdram, ctx, runtime); // L2 / R2 cycled the style
            if (!g_ram || !g_listShown)
                return;
            const uint32_t waza = GPR_U32(ctx, 4);
            const uint32_t text = part(waza, 0x23), shadow = part(waza, 0x24);
            if (!validPointer(text, ent::kSize) || !validPointer(shadow, ent::kSize))
                return;
            wrFloat(text + ent::kDrawX, kRowX);
            wrFloat(text + ent::kDrawY, kRowY);
            wrFloat(shadow + ent::kDrawX, kRowX + 1.0f);
            wrFloat(shadow + ent::kDrawY, kRowY + 1.0f);
            if (g_listEntry)
            {
                wrFloat(g_listEntry + ent::kDrawX, kListX);
                wrFloat(g_listEntry + ent::kDrawY, kListY);
            }
        }

        // cIDBase_transText at 0x2AB1E0 ($s1 the entry, $s0 the font's draw state D_005E7510, its line spacing just
        // set): the list's lines kLinePitch apart, the shadow's too (cMessDrawFont_measureLineHeight measures the icons
        // of a line whether or not they are drawn, so the same value gives the shadow the same pitch). A new line
        // moves down by the line's height (the tallest code on it: 32 x scale for an icon) + state+0x30 x state+0x48
        // (the vertical scale).
        // The text's draw slot (font state +0x58 list, +0x5C position, just set to the entry's list and position 5):
        // the indicator round the minimap goes to the minimap's list, one position nearer than its nearest sprite, so
        // the minimap does not cover it (text and sprites sit in per-list ordering tables: a lower position is drawn
        // later; equal positions draw the last queued first). K's Nov 10 build: drawn under the minimap's rim.
        void hudTextSlot(uint32_t entry, uint32_t state)
        {
            int list, pos;
            if (!g_hudShown || (entry != g_hudText && entry != g_hudShadow) || !hud::radarSlot(list, pos))
                return;
            wr32(state + 0x58u, static_cast<uint32_t>(list));
            wr32(state + 0x5Cu, static_cast<uint32_t>(std::max(0, pos - 1)));
        }

        void onTextSetup(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram)
                return;
            const uint32_t entry = GPR_U32(ctx, 17) & 0x1FFFFFFu;
            hudTextSlot(entry, GPR_U32(ctx, 16) & 0x1FFFFFFu);
            if (!g_listEntry || entry != g_listEntry)
                return;
            const uint32_t state = GPR_U32(ctx, 16);
            const float zoom = rdFloat(state + game::MessDrawFont::kZoomY);
            if (zoom > 0.05f && zoom < 4.0f)
                wrFloat(state + game::MessDrawFont::kCharGapY, (kLinePitch - kLineHeight * (zoom / 0.65f)) / zoom);
        }

        prompt_sheets::ArrowRect pieceRect(int piece)
        {
            return prompt_sheets::arrowRect(piece == 0 || piece == 2 ? prompt_sheets::ArrowDown : prompt_sheets::ArrowRight);
        }

        int pieceOfCode(uint16_t code)
        {
            for (int i = 0; i < 2; ++i)
                if (code == kPadCode[i])
                    return kPadPending + i;
            for (int i = 0; i < kStyles; ++i)
                if (code == kArrowCode[i])
                    return i;
            return -1;
        }

        // The icon drawer func_002B1300 at 0x2B1584 (every icon's UVs stored at sp+0x14..0x20, $s0 -> the code, $a3 the
        // icon sheet; Gp_draw_polyFT4 follows): inside the list, the four arrow codes and the plus code take the style
        // pieces' UVs.
        void onIconDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            g_pendingArrow = -1;
            if (!g_ram || !g_listEntry || g_drawingEntry != g_listEntry)
                return;
            const int arrow = pieceOfCode(rd16(GPR_U32(ctx, 16)));
            if (arrow < 0)
                return;
            if (arrow >= kPadPending)
            {
                g_pendingArrow = arrow;
                return;
            }
            // Up = the down arrow flipped vertically, left = the right arrow flipped horizontally.
            const prompt_sheets::ArrowRect r = pieceRect(arrow);
            const float u0 = r.x0 / 128.0f, v0 = r.y0 / 128.0f, u1 = r.x1 / 128.0f, v1 = r.y1 / 128.0f;
            const float uv[4] = {arrow == 3 ? u1 : u0, arrow == 0 ? v1 : v0, arrow == 3 ? u0 : u1, arrow == 0 ? v0 : v1};
            const uint32_t sp = GPR_U32(ctx, 29);
            for (int i = 0; i < 4; ++i)
                wrFloat(sp + 0x14u + 4u * static_cast<uint32_t>(i), uv[i]);
            g_pendingArrow = arrow;
        }

        // A piece's width in the text, in the units cMessDrawFont_measureCode gets from func_002B12D8 for an icon (32 =
        // a cell, drawn as a 26-unit square; a piece is drawn at side / 28 a texel, so w texels = w x 26 / 28 units).
        // Each arrow advances by its own shape's width (up / down narrow, left / right wide); the pad by kLetterPad.
        float pieceAdvance(int piece)
        {
            if (piece >= kPadPending)
                return kLetterPad[piece - kPadPending];
            const prompt_sheets::ArrowRect r = pieceRect(piece);
            return (r.x1 - r.x0 + kArrowTextGap) * (26.0f / 28.0f);
        }

        // The lines' entry is being drawn.
        bool drawingList() { return g_listEntry && g_drawingEntry == g_listEntry; }

        // cMessDrawFont_measureCode at 0x2B0A24, right after func_002B12D8 gave an icon's width in $f0 (32; before
        // lwc1 f1,($s2); $s1 -> the code): the measured width of the plus and the arrows is the width they are drawn
        // (this measure centres the block; the draw advances on its own, below).
        void onIconWidth(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram || !drawingList())
                return;
            const int piece = pieceOfCode(rd16(GPR_U32(ctx, 17)));
            if (piece >= 0)
                ctx->f[0] = pieceAdvance(piece);
        }

        // cMessDrawFont_draw at 0x2AFB30 (before lwc1 f1,0x88(s2); $s2 the font state, its +0x64 -> the code): the
        // draw's own advance for an icon, $f2 = 32 (26 when GameWork.language is 0, Japanese), just loaded; the plus and the arrows
        // advance by their width, so the arrow starts where the plus ends and the name follows the arrow by
        // kArrowTextGap.
        void onIconAdvance(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram || !drawingList())
                return;
            const uint32_t code = rd32(GPR_U32(ctx, 18) + game::MessDrawFont::kCursor);
            if (!validPointer(code))
                return;
            const int piece = pieceOfCode(rd16(code));
            if (piece >= 0)
                ctx->f[2] = pieceAdvance(piece);
        }

        // The same function at 0x2B16C4, its quad's corners at sp+0x60/0x70/0x80/0x90 (x, y each; a square of 26 x
        // scale): a piece's quad gets the piece's own shape at the texel size the game gives its "+down" art next to a
        // button (a 32-texel cell over about 28 of the square), centred on the square's middle line, starting at the
        // left of its square. With the advances above the arrow's square starts where the plus ends, so the two touch
        // as in the game's art, and the name follows the arrow by kArrowTextGap.
        void onIconQuad(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (g_pendingArrow < 0)
                return;
            const int arrow = g_pendingArrow;
            g_pendingArrow = -1;
            const uint32_t sp = GPR_U32(ctx, 29);
            const float x0 = rdFloat(sp + 0x60u), y0 = rdFloat(sp + 0x64u), x1 = rdFloat(sp + 0x70u), y1 = rdFloat(sp + 0x84u);
            const float side = x1 - x0;
            if (!(side > 0.0f && side < 200.0f))
                return;
            if (arrow >= kPadPending) // nothing: the quad shrinks to a point
            {
                for (uint32_t o : {0x60u, 0x70u, 0x80u, 0x90u})
                {
                    wrFloat(sp + o, x0);
                    wrFloat(sp + o + 4u, y0);
                }
                return;
            }
            const prompt_sheets::ArrowRect r = pieceRect(arrow);
            const float k = side / 28.0f;
            const float w = (r.x1 - r.x0) * k, h = (r.y1 - r.y0) * k;
            const float cy = 0.5f * (y0 + y1);
            const float left = x0;
            const float right = left + w, top = cy - 0.5f * h, bottom = cy + 0.5f * h;
            wrFloat(sp + 0x60u, left);
            wrFloat(sp + 0x64u, top);
            wrFloat(sp + 0x70u, right);
            wrFloat(sp + 0x74u, top);
            wrFloat(sp + 0x80u, left);
            wrFloat(sp + 0x84u, bottom);
            wrFloat(sp + 0x90u, right);
            wrFloat(sp + 0x94u, bottom);
        }

#if defined(KAMITE_HAS_GS_MENU)
        void drawPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version >= 4u && !ui->collapsing_header("Styles", 0))
                return; // a fold-out section of the Gameplay tab (K, Oct 27)
            std::lock_guard<std::mutex> lock(g_mutex);
            ui->text_wrapped("Styles are kept in the save (they go to the memory card with it). On the Techniques screen "
                             "L2 / R2 cycle None, S, E, X, Y; changes save to the style shown; None = styles off. In play "
                             "a d-pad tap switches style (up, right, down, left) while one is on.");
            if (g_signalOn)
            {
                style_pose::drawTuning(ui);
            }
            ui->separator_text("Equipped now");
            if (!g_snapshot.valid)
                ui->text_disabled("No save data in memory yet (start or load a game).");
            else
                for (int s = 0; s < kSets; ++s)
                {
                    const std::string line = std::string(kSetNames[s]) + ": " + describe(g_snapshot.combos[s]);
                    ui->text_wrapped(line.c_str());
                }
            ui->separator_text("Styles");
            static const char *const kDirections[kStyles] = {"up", "right", "down", "left"};
            for (int at = 0; at <= kStyles; ++at)
            {
                const int slot = at - 1;
                const Store &st = g_snapshot.store;
                const bool used = g_snapshot.valid && (slot < 0 || (st.used & (1u << slot)));
                const bool on = g_snapshot.valid && (slot < 0 ? st.active >= kStyles : st.active == slot);
                ui->push_id(at);
                char label[48];
                if (slot < 0)
                    std::snprintf(label, sizeof(label), "Style None (styles off)%s", on ? " - on" : "");
                else
                    std::snprintf(label, sizeof(label), "Style %c (%s)%s%s", g_letters[slot], kDirections[slot],
                                  used ? "" : " - empty", on ? " - on" : "");
                ui->text(label);
                ui->same_line();
                ui->begin_disabled(!g_snapshot.valid || on);
                if (ui->small_button("Select"))
                    g_request = at + 1;
                ui->end_disabled();
                if (slot >= 0 && used && !on)
                {
                    const std::string line = describe(st.styles[slot][0]);
                    ui->text_disabled(line.c_str());
                }
                ui->pop_id();
            }
            if (!g_status.empty())
            {
                ui->separator();
                ui->text(g_status.c_str());
            }
        }
#endif
    }

    void padFilter(uint32_t player, ps2x::iop::PadInputState &state)
    {
        if (player != 0u || !g_ram)
            return;
        const uint16_t dpad = static_cast<uint16_t>(state.buttons & kAllDpad);
        const uint16_t pressed = static_cast<uint16_t>(dpad & ~g_dpadWas);
        g_dpadWas = dpad;
        const uint16_t triggers = static_cast<uint16_t>(state.buttons & kTriggers);
        const uint16_t pulled = static_cast<uint16_t>(triggers & ~g_triggersWas);
        g_triggersWas = triggers;
        const int moment = g_moment.load(std::memory_order_relaxed);
        if (moment == Techniques)
        {
            // L2 / R2: the previous / next style (None, S, E, X, Y, wrapping), kept from the game.
            if (pulled & ps2x::iop::PadButton::R2)
                g_request = 21;
            else if (pulled & ps2x::iop::PadButton::L2)
                g_request = 22;
            hideTriggers(state);
        }
        else if (moment == Play)
        {
            // In play a tap switches at once while a style is on (Gene's controls never read the d-pad; the next move
            // of a chain already comes from the new style).
            const int direction = firstPressed(pressed);
            if (direction >= 0)
                g_request = 11 + direction;
        }
    }

    void noteEntry(uint32_t entry)
    {
        g_drawingEntry = entry & 0x1FFFFFFu;
    }

    int activePosition()
    {
        if (!g_ram)
            return 0;
        const uint32_t data = dataPointer();
        Store store{};
        if (!data || !readStore(data, store))
            return 0;
        return store.active < kStyles && (store.used & (1u << store.active)) ? store.active + 1 : 0;
    }

    bool lettersOnRadar() { return g_ram != nullptr; }

    int hudGlyph(uint32_t entry, int index, float &dx, float &dy)
    {
        entry &= 0x1FFFFFFu;
        dx = dy = 0.0f;
        if (!g_ram || !g_hudShown || (entry != g_hudText && entry != g_hudShadow) || index < 0)
            return -1;
        if (entry == g_hudText)
            return index < kStyles ? index : -1;
        if (index >= kStyles * kOutlineCopies)
            return -1;
        static constexpr float kDir[kOutlineCopies][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
        dx = kDir[index % kOutlineCopies][0];
        dy = kDir[index % kOutlineCopies][1];
        return index / kOutlineCopies;
    }

    bool onHudEntry(uint32_t entry)
    {
        entry &= 0x1FFFFFFu;
        if (!g_ram || !g_hudShown || (entry != g_hudText && entry != g_hudShadow))
            return false;
        // Placed here, after the layout's move put the entry where its data says (the top left corner).
        const float d = 0.0f; // the outline is spread per glyph (hudGlyph): the shadow sits under the text
        wrFloat(entry + ent::kDrawX, kHudX + d);
        wrFloat(entry + ent::kDrawY, kHudY + d);
        return true;
    }

    void install(PS2Runtime &runtime)
    {
        // [Game] StyleSwitching ("Style switching" in the launcher, off by default): without it nothing here is
        // installed and g_ram stays empty, which the entry points the other modules call check first.
        if (const char *on = std::getenv("KAMITE_STYLE_SWITCHING"); !on || !*on || *on == '0')
        {
            KM_INFO(Game, "style switching: off ([Game] StyleSwitching=0)");
            return;
        }
        g_ram = runtime.memory().getRDRAM();
        if (!g_ram)
            return;
        // Xavier's styles read LIFE down the list (Gene's read SEXY).
        if (const char *character = std::getenv("KAMITE_CHARACTER"); character && std::tolower(static_cast<unsigned char>(*character)) == 'x')
            std::memcpy(g_letters, "LIFE", kStyles + 1);
        // [Game] StyleSignal ("Signal Style Switch", under Moveset Presets): a voice line and the arm flash per switch.
        if (const char *signal = std::getenv("KAMITE_STYLE_SIGNAL"); signal && *signal && *signal != '0')
            g_signalOn = true;
        KM_INFO(Game, "style switching: on" << (g_signalOn ? ", signal style switch" : ""));
        ps2x::pnach::addFrameHook(&onVblank);
        FunctionHooks::instance().observeEntry(0x14FB48u, "cObjBase_SetSeqEffect (styles: chain window, signal)", &onGeneUpdate);
        if (g_signalOn)
            style_pose::install(runtime);
        FunctionHooks::instance().observeEntry(0x2AAA40u, "cIDBase_move (style list)", &onLayoutMove);
        FunctionHooks::instance().observeEntry(0x16DC18u, "cSubScrWaza draw (style list)", &onWazaDraw);
        ps2xRegisterHostHook("km_styles_icon", &onIconDraw);
        ps2xRegisterHostHook("km_styles_icon_quad", &onIconQuad);
        ps2xRegisterHostHook("km_styles_icon_width", &onIconWidth);
        ps2xRegisterHostHook("km_styles_icon_advance", &onIconAdvance);
        ps2xRegisterHostHook("km_styles_text", &onTextSetup);
        ps2xSetPatchFlag("styles", true);
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("Gameplay", &drawPanel); // a section of the Gameplay tab
#endif
    }
}
