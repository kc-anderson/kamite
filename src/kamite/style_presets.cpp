// Style presets, stage 1. See style_presets.h.
#include "kamite/style_presets.h"

#include "kamite/log.h"
#include "kamite/menu_buttons.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

namespace kamite::style_presets
{
    namespace
    {
        constexpr uint32_t kCoreSave = 0x569B70u;   // cCoreSave D_00569B70: { cCoreSaveData *data; }
        constexpr uint32_t kSkills = 0xB0u;         // char skill[0x80]: -1 = not owned
        constexpr uint32_t kCombos = 0x1B0u;        // cCoreSaveCombo combo[6]
        constexpr uint32_t kSetBytes = 0x24u;       // int id[6]; char lv[6]; char pad[2]; int max;
        constexpr int kSets = 6, kSlots = 6, kStyles = 4;
        constexpr uint32_t kComboBytes = kSetBytes * kSets;
        // The Techniques screen (research of Oct 25, SLUS_215.03): sub screen kind 1, cSubScrWaza, registered at
        // cSubScr+0x310 (D_003BD930 -> 0x566E50, so 0x567160). +0x5A its state: 0 browsing the slots, 1 the move picker
        // open, 2 leaving. +0x25C the cursor row (s8, -1 = RETURN), +0x284 the column (0 the square chain). Its input
        // (0x16DF38) never tests triangle; it re-reads combo[] every frame (0x16C730), so a load shows at once.
        constexpr uint32_t kSubScrPointer = 0x3BD930u;
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
        // cCoreSaveData is 0x14A0 bytes; no game code reads or writes past +0xBBA (the highest field, Olivia's costume,
        // is at +0xBC0): only the new game's memset (cCoreSave_systemInit 0x1F9848, so a new game starts with no styles)
        // and the whole-record copies (the checkpoint slot, the memory card) touch the rest. The styles live at +0xC00,
        // with a magic and a checksum so a cleared record or one from another tool reads as "no styles".
        // The checkpoint slot (*(u32 *)0x3C264C, a whole copy of the record taken at every room change and put back on
        // Continue) gets every change too, so a Continue does not take the styles back to the last checkpoint.
        constexpr uint32_t kStoreOffset = 0xC00u;
        constexpr uint32_t kCheckpointPointer = 0x3C264Cu;
        constexpr uint32_t kStoreMagic = 0x54534D4Bu; // "KMST"
        constexpr uint8_t kStoreVersion = 1u;
        constexpr uint8_t kNoStyle = 0xFFu;
        struct Store
        {
            uint32_t magic;
            uint8_t version;
            uint8_t used;   // bit n: style n saved
            uint8_t active; // the style loaded last (kNoStyle: none, or the moves were changed by hand since)
            uint8_t reserved;
            Combo styles[kStyles][kSets];
            uint32_t checksum;
        };
        static_assert(sizeof(Store) == 8u + kStyles * kComboBytes + 4u, "Store layout");
        static_assert(kStoreOffset + sizeof(Store) <= 0x14A0u, "Store fits in cCoreSaveData");

        uint32_t checksumOf(const Store &store)
        {
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&store);
            uint32_t sum = 0x9E3779B9u;
            for (size_t i = 0; i < offsetof(Store, checksum); ++i)
                sum = (sum ^ bytes[i]) * 0x01000193u;
            return sum;
        }

        // ---- the screen and the moment (EE thread, each vblank) -> the pad filter (window thread) ----
        enum Moment : int
        {
            Elsewhere,  // title, menus, cutscenes, minigames...
            Play,       // Gene in a stage under the player's control: a d-pad tap loads a style
            Techniques, // the Techniques screen, browsing the slots (Modern menu buttons): Y + d-pad
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
        // Pad filter / F2 -> EE thread: 1..4 save that style, 11..14 load it.
        std::atomic<int> g_request{0};
        // Shown (the list, a flash) after a load or a save: which style and when (EE vblank count).
        std::atomic<int> g_lastStyle{-1};
        std::atomic<uint32_t> g_lastAction{0u};
        uint32_t g_vblanks = 0u;

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
            return validPointer(data, 0x14A0u) ? data : 0u;
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

        // The record's styles: false (store cleared) when there are none yet or the bytes are not ours.
        bool readStore(uint32_t data, Store &store)
        {
            std::memcpy(&store, g_ram + data + kStoreOffset, sizeof(Store));
            const bool ok = store.magic == kStoreMagic && store.version == kStoreVersion && store.checksum == checksumOf(store);
            if (!ok)
            {
                store = Store{};
                store.magic = kStoreMagic;
                store.version = kStoreVersion;
                store.active = kNoStyle;
            }
            return ok;
        }

        void writeStore(uint32_t data, Store &store)
        {
            store.checksum = checksumOf(store);
            std::memcpy(g_ram + data + kStoreOffset, &store, sizeof(Store));
            // The checkpoint slot: the same record (its save pointer is the live one's copy), refreshed at every room
            // change. Only when it holds this game's record (the same skills), so a stale slot from the title is left.
            const uint32_t checkpoint = rd32(kCheckpointPointer);
            if (validPointer(checkpoint, 0x14A0u) && checkpoint != data &&
                std::memcmp(g_ram + checkpoint + kSkills, g_ram + data + kSkills, 0x80u) == 0)
                std::memcpy(g_ram + checkpoint + kStoreOffset, &store, sizeof(Store));
        }

        // Load: the style's moves the save owns, in order; a single slot without one stays empty, the chain closes up.
        // Ids past the game's move table (0..0x71) are left out too.
        int load(uint32_t data, const Combo (&style)[kSets])
        {
            const int8_t *skill = reinterpret_cast<const int8_t *>(g_ram + data + kSkills);
            int skipped = 0;
            Combo combos[kSets] = {};
            for (int s = 0; s < kSets; ++s)
            {
                const Combo &from = style[s];
                Combo &to = combos[s];
                const int n = from.max < 0 ? 0 : from.max > kSlots ? kSlots : from.max;
                int out = 0;
                for (int i = 0; i < n; ++i)
                {
                    const int32_t id = from.id[i];
                    if (id < 0 || id >= kMoveCount || skill[id] < 0)
                    {
                        ++skipped;
                        continue;
                    }
                    to.id[out] = id;
                    to.lv[out] = from.lv[i];
                    ++out;
                }
                to.max = out;
            }
            std::memcpy(g_ram + data + kCombos, combos, kComboBytes);
            return skipped;
        }

        uint32_t wazaScreen(uint32_t subScr) { return subScr + kWazaOffset; }

        bool techniquesBrowsing(uint32_t subScr, bool subScreenOpen)
        {
            return subScreenOpen && static_cast<int32_t>(rd32(subScr + 0x48u)) == kTechniquesKind &&
                   g_ram[(wazaScreen(subScr) + kWazaState) & 0x1FFFFFFu] == 0u;
        }

        // Gene in a stage under the player's control (research of Oct 25, SLUS_215.03; addresses in the game work block
        // 0x7474A0): a player (Getplayer's 0x747A94), no sub screen open, Gene not ignoring input or the game frozen
        // (0x747A78 bits 31/30: sub screens, movies, events), no scripted "soft event" (0x747A84 bit 0x40: the casino
        // slots, the dog race), Gene not suspended (+0x250 bit 0x8000) and in his normal mode (+0x2F4 == 0; 5 is the
        // sub-screen mode, 6 the cannon), no tutorial pop-up. Mid-attack counts: the next move of a chain comes from
        // the new set (ComboObj_startNext 0x126918 reads combo[] only when a move starts; a chain shorter than the hit
        // reached starts over).
        bool inPlay(uint32_t subScr, bool subScreenOpen)
        {
            if (subScreenOpen || !validPointer(subScr) || static_cast<int16_t>(rd32(subScr + 0x50u)) != 0)
                return false;
            const uint32_t player = rd32(0x747A94u);
            if (!validPointer(player, 0x700u))
                return false;
            if ((rd32(0x747A78u) & 0xC0000000u) != 0u || (rd32(0x747A84u) & 0x40u) != 0u)
                return false;
            if ((rd32(player + 0x250u) & 0x8000u) != 0u || g_ram[player + 0x2F4u] != 0u)
                return false;
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
            const bool subScreenOpen = validPointer(subScr, 0x1000u) && static_cast<int32_t>(rd32(subScr + 0x48u)) >= 0;
            int moment = Elsewhere;
            if (data)
            {
                // Y is free on the Techniques screen only with the Modern menu buttons (Classic: it is Cancel there).
                if (menu_buttons::modern() && techniquesBrowsing(subScr, subScreenOpen))
                    moment = Techniques;
                else if (inPlay(subScr, subScreenOpen))
                    moment = Play;
            }
            g_moment.store(moment, std::memory_order_relaxed);

            const int request = g_request.exchange(0);
            Store store{};
            if (data)
                readStore(data, store);
            if (request != 0)
            {
                const int slot = (request % 10) - 1;
                const bool save = request < 10;
                char text[160];
                if (!data || slot < 0 || slot >= kStyles)
                    std::snprintf(text, sizeof(text), "No save data in memory yet.");
                else if (save)
                {
                    read(data, store.styles[slot]);
                    store.used = static_cast<uint8_t>(store.used | (1u << slot));
                    store.active = static_cast<uint8_t>(slot);
                    writeStore(data, store);
                    g_lastStyle = slot;
                    g_lastAction = g_vblanks;
                    std::snprintf(text, sizeof(text), "Style %c saved.", g_letters[slot]);
                }
                else if (!(store.used & (1u << slot)))
                    std::snprintf(text, sizeof(text), "Style %c is empty.", g_letters[slot]);
                else
                {
                    const int skipped = load(data, store.styles[slot]);
                    if (moment == Techniques)
                    {
                        // A shorter chain: the cursor no further than the chain's first empty row.
                        const uint32_t waza = wazaScreen(subScr);
                        const int32_t chain = store.styles[slot][0].max < 0 ? 0 : store.styles[slot][0].max;
                        int8_t &cursor = reinterpret_cast<int8_t &>(g_ram[waza + kWazaCursor]);
                        if (g_ram[waza + kWazaColumn] == 0u && cursor > chain)
                            cursor = static_cast<int8_t>(chain);
                    }
                    store.active = static_cast<uint8_t>(slot);
                    writeStore(data, store);
                    g_lastStyle = slot;
                    g_lastAction = g_vblanks;
                    std::snprintf(text, sizeof(text), "Style %c loaded%s.", g_letters[slot],
                                  skipped ? " (moves this save does not own left out)" : "");
                }
                // The corner notice (the screen's list shows it too; in play it is the only sign for now).
                if (data && slot >= 0 && slot < kStyles && request >= 10 && (store.used & (1u << slot)))
                {
                    char notice[32];
                    std::snprintf(notice, sizeof(notice), "Style %c", g_letters[slot]);
                    ps2x::menu::notify(notice, 1.2f);
                }
                else if (data)
                    ps2x::menu::notify(std::string(text, std::strlen(text) - 1u), 1.5f);
                std::lock_guard<std::mutex> lock(g_mutex);
                g_status = text;
                KM_INFO(Game, "styles: " << text << " (save data 0x" << std::hex << data << std::dec << ")");
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
        constexpr ps2x::iop::PadPressure kDpadPressure[kStyles] = {ps2x::iop::PadPressure::Up, ps2x::iop::PadPressure::Right,
                                                                   ps2x::iop::PadPressure::Down, ps2x::iop::PadPressure::Left};
        constexpr uint16_t kAllDpad = ps2x::iop::PadButton::Up | ps2x::iop::PadButton::Right | ps2x::iop::PadButton::Down |
                                      ps2x::iop::PadButton::Left;
        constexpr int64_t kSaveHoldMs = 1500;

        uint16_t g_dpadWas = 0u;
        int g_held = -1;         // Techniques: the direction pressed with Y, waiting for its release or 1.5 s
        int64_t g_heldSince = 0;
        bool g_heldSaved = false;
        bool g_swallow = false;  // the d-pad is kept from the game until it is let go (it was pressed with Y)

        int firstPressed(uint16_t pressed)
        {
            for (int i = 0; i < kStyles; ++i)
                if (pressed & kDpad[i])
                    return i;
            return -1;
        }

        void hideDpad(ps2x::iop::PadInputState &state)
        {
            state.buttons = static_cast<uint16_t>(state.buttons & ~kAllDpad);
            for (const ps2x::iop::PadPressure pressure : kDpadPressure)
                state.pressure[static_cast<size_t>(pressure)] = 0u;
        }
#if defined(KAMITE_HAS_GS_MENU)
        void drawPanel(const Pcsx2GsUi *ui)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ui->text_wrapped("Styles are kept in the save (they go to the memory card with it). In play, a d-pad tap loads "
                             "one (up, right, down, left). On the Techniques screen (Modern menu buttons): Y + d-pad loads, "
                             "held 1.5 s saves.");
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
            for (int slot = 0; slot < kStyles; ++slot)
            {
                const bool used = g_snapshot.valid && (g_snapshot.store.used & (1u << slot));
                ui->push_id(slot);
                char label[48];
                std::snprintf(label, sizeof(label), "Style %c (%s)%s", g_letters[slot], kDirections[slot],
                              used && g_snapshot.store.active == slot ? " - loaded" : "");
                ui->text(label);
                ui->same_line();
                ui->begin_disabled(!g_snapshot.valid);
                if (ui->small_button("Save"))
                    g_request = slot + 1;
                ui->end_disabled();
                ui->same_line();
                ui->begin_disabled(!used);
                if (ui->small_button("Load"))
                    g_request = slot + 11;
                ui->end_disabled();
                if (used)
                {
                    const std::string line = describe(g_snapshot.store.styles[slot][0]);
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
        const int moment = g_moment.load(std::memory_order_relaxed);
        const bool triangle = (state.buttons & ps2x::iop::PadButton::Triangle) != 0u;
        if (moment == Techniques)
        {
            // Y + a d-pad direction: let go before 1.5 s loads that style, held 1.5 s saves to it (at once, so the
            // list's flash says when to let go).
            const int64_t now = nowMs();
            if (g_held >= 0)
            {
                if (!(dpad & kDpad[g_held]))
                {
                    if (!g_heldSaved)
                        g_request = 11 + g_held;
                    g_held = -1;
                }
                else if (!g_heldSaved && now - g_heldSince >= kSaveHoldMs)
                {
                    g_request = 1 + g_held;
                    g_heldSaved = true;
                }
            }
            if (triangle && g_held < 0)
            {
                const int direction = firstPressed(pressed);
                if (direction >= 0)
                {
                    g_held = direction;
                    g_heldSince = now;
                    g_heldSaved = false;
                }
            }
            if (triangle && dpad != 0u)
                g_swallow = true;
            if (triangle)
                hideDpad(state);
        }
        else
        {
            g_held = -1;
            // In play a tap loads at once (Gene's controls never read the d-pad; the next move of a chain already
            // comes from the new style).
            if (moment == Play)
            {
                const int direction = firstPressed(pressed);
                if (direction >= 0)
                    g_request = 11 + direction;
            }
        }
        // A direction pressed with Y stays hidden from the game until it is let go (Y let go first: no cursor jump).
        if (g_swallow)
        {
            if (dpad == 0u)
                g_swallow = false;
            else
                hideDpad(state);
        }
    }

    void install(uint8_t *rdram)
    {
        g_ram = rdram;
        if (!g_ram)
            return;
        // Xavier's styles read LIFE down the list (Gene's read SEXY).
        if (const char *character = std::getenv("KAMITE_CHARACTER"); character && std::tolower(static_cast<unsigned char>(*character)) == 'x')
            std::memcpy(g_letters, "LIFE", kStyles + 1);
        ps2x::pnach::addFrameHook(&onVblank);
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("Styles", &drawPanel);
#endif
    }
}
