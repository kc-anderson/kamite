// Style presets, stage 1. See style_presets.h.
#include "kamite/style_presets.h"

#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "kamite/menu_buttons.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

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

        // ---- the list on the Techniques screen ----
        // The info panel's "Already Set" group (shown by the game only while the move picker is open, to say the
        // highlighted move is in another slot) carries the list while the slots are browsed: parts[0x23] the text
        // (align 3: the block centred on its draw position, scale 0.65), parts[0x24] its shadow (+1,+1, colour set 1:
        // black, icons skipped), parts[0x25] the move name, [0x26] the "down+" art, [0x27] the slot's button and the
        // name box (record #57, no id) hidden. Two lines, two styles each, labelled like the screen's own "down+"
        // rows: [Y] then the game's "+arrow" art (system texture 20: "+down" top left, "+up" top right, "+ over left"
        // bottom left, "+ over right" bottom right) then the letter. The text is message 0x30C4 (an unused "<7d>"
        // placeholder) whose offset word points at a string of ours; the shadow is 0x30FE (no colour codes, which
        // would colour the shadow too). The bottom bar's spare line (ids 57/58: "Release" on the chain's last rows,
        // otherwise blank 0x30D0) says "[Y] Hold: Save Style" (0x30FF). Entry fields: +0x2C flags (0x08000000 = not
        // drawn), +0x50/+0x54 draw position, +0x90 message id.
        constexpr uint32_t kNoDraw = 0x08000000u;
        constexpr uint32_t kMessageBankPointer = 0x3C23A4u; // *(*(u32 *)0x3C23A4 + 0xC) = the message bank
        constexpr uint16_t kListMessage = 0x30C4u, kShadowMessage = 0x30FEu, kHintMessage = 0x30FFu, kBlankMessage = 0x30D0u;
        constexpr uint32_t kListText = 0x1000u, kShadowText = 0x1100u, kHintText = 0x1200u; // in the save record's unused tail
        constexpr float kListX = 395.0f, kListY = 399.0f; // under "Attack Str" (x 353..437, y 337..369)
        // Icon codes the list never uses as buttons (L3, R3, Start, Select): drawn as the arrows while it is drawn.
        constexpr uint16_t kArrowCode[kStyles] = {8u, 9u, 12u, 13u}; // up, right, down, left
        constexpr float kArrowUv[kStyles][4] = {
            {0.5f, 0.0f, 1.0f, 0.5f}, // "+up"
            {0.5f, 0.5f, 1.0f, 1.0f}, // "+" over "right"
            {0.0f, 0.0f, 0.5f, 0.5f}, // "+down"
            {0.0f, 0.5f, 0.5f, 1.0f}, // "+" over "left"
        };
        constexpr uint16_t kTriangleIcon = 1u, kSpace = 0x10u, kNewLine = 0xA001u, kEnd = 0x8001u;
        constexpr uint16_t kColour = 0xB203u, kColourRestore = 0xB301u; // 0xB203 hi lo: A B G R (0x80 = full)
        constexpr uint32_t kGold = 0x80106880u, kDim = 0x80383838u;

        std::atomic<int> g_holding{-1}; // the pad filter: the direction held with Y, before it saves
        uint32_t g_listEntry = 0u;      // the entry the arrows are drawn for (EE thread)
        uint32_t g_drawingEntry = 0u;   // the cIDBase entry being drawn (hud_layout's hook)
        bool g_listShown = false;       // this frame's list is on (the screen's draw places it)
        uint32_t g_hidName = 0u, g_hidBox = 0u; // entries hidden by us, shown again when the picker opens

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
            return kSpace;
        }

        void wr16(uint32_t address, uint16_t value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        void wr32(uint32_t address, uint32_t value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        uint16_t rd16(uint32_t address)
        {
            uint16_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        void wrFloat(uint32_t address, float value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }

        void writeText(uint32_t address, const std::vector<uint16_t> &text)
        {
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

        std::vector<uint16_t> listText(const Store &store, bool colours)
        {
            const int holding = g_holding.load(std::memory_order_relaxed);
            const bool blinkOn = (g_vblanks / 8u) % 2u == 0u;
            const bool flash = g_vblanks - g_lastAction.load(std::memory_order_relaxed) < 40u;
            std::vector<uint16_t> out;
            for (int slot = 0; slot < kStyles; ++slot)
            {
                if (slot == 2)
                    out.push_back(kNewLine);
                else if (slot == 1 || slot == 3)
                    out.insert(out.end(), {kSpace, kSpace, kSpace});
                out.push_back(kTriangleIcon);
                out.push_back(kArrowCode[slot]);
                out.push_back(kSpace);
                const bool used = (store.used & (1u << slot)) != 0u;
                uint32_t colour = 0u; // 0: the line's own colour
                if (slot == holding)
                    colour = blinkOn ? kGold : 0u; // held with Y: blinks until it saves
                else if (used && store.active == slot)
                    colour = (flash && !blinkOn) ? 0u : kGold; // the loaded style (a few blinks right after)
                else if (!used)
                    colour = kDim;
                if (colours && colour)
                    out.insert(out.end(), {kColour, static_cast<uint16_t>(colour >> 16), static_cast<uint16_t>(colour & 0xFFFFu)});
                out.push_back(glyph(g_letters[slot]));
                if (colours && colour)
                    out.push_back(kColourRestore);
            }
            out.push_back(kEnd);
            return out;
        }

        std::vector<uint16_t> hintText()
        {
            std::vector<uint16_t> out{kTriangleIcon, kSpace};
            for (const char *c = "Hold: Save Style"; *c; ++c)
                out.push_back(glyph(*c));
            out.push_back(kEnd);
            return out;
        }

        uint32_t part(uint32_t waza, int id) { return rd32(waza + 0x80u + 4u * static_cast<uint32_t>(id)); }
        void setNoDraw(uint32_t entry, bool hidden)
        {
            if (!validPointer(entry, 0xACu))
                return;
            const uint32_t flags = rd32(entry + 0x2Cu);
            wr32(entry + 0x2Cu, hidden ? (flags | kNoDraw) : (flags & ~kNoDraw));
        }

        // cIDBase_move entry (every layout, every frame; EE thread): for the Techniques screen's layout, after its
        // update set this frame's entries and before they are placed, the list goes in (or the group is given back).
        void onLayoutMove(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram)
                return;
            const uint32_t subScr = rd32(kSubScrPointer);
            if (!validPointer(subScr, 0x1000u) || GPR_U32(ctx, 4) != wazaScreen(subScr))
                return;
            const uint32_t waza = wazaScreen(subScr);
            const uint32_t data = dataPointer();
            const bool show = data && menu_buttons::modern() && static_cast<int32_t>(rd32(subScr + 0x48u)) == kTechniquesKind &&
                              g_ram[waza + kWazaState] == 0u;
            const uint32_t text = part(waza, 0x23), shadow = part(waza, 0x24), hintEntry = part(waza, 57),
                           hintShadow = part(waza, 58);
            g_listShown = false;
            if (!show)
            {
                // The game's own use of the group (the picker's "Already Set"): what we hid comes back.
                if (g_hidName)
                    setNoDraw(g_hidName, false);
                if (g_hidBox)
                    setNoDraw(g_hidBox, false);
                g_hidName = g_hidBox = 0u;
                g_listEntry = 0u;
                for (const uint32_t entry : {hintEntry, hintShadow})
                    if (validPointer(entry, 0xACu) && rd16(entry + 0x90u) == kHintMessage)
                        wr16(entry + 0x90u, kBlankMessage);
                return;
            }
            if (!validPointer(text, 0xACu) || !validPointer(shadow, 0xACu))
                return;
            Store store{};
            readStore(data, store);
            writeText(data + kListText, listText(store, true));
            writeText(data + kShadowText, listText(store, false));
            writeText(data + kHintText, hintText());
            if (!pointMessage(kListMessage, data + kListText) || !pointMessage(kShadowMessage, data + kShadowText) ||
                !pointMessage(kHintMessage, data + kHintText))
                return;
            setNoDraw(text, false);
            wr16(text + 0x90u, kListMessage);
            wr16(shadow + 0x90u, kShadowMessage);
            const uint32_t name = part(waza, 0x25), entries = rd32(waza + 0x4u);
            const uint32_t box = validPointer(entries, 0x4000u) ? entries + 57u * 0xACu : 0u;
            setNoDraw(name, true);
            setNoDraw(box, true);
            g_hidName = validPointer(name, 0xACu) ? name : 0u;
            g_hidBox = validPointer(box, 0xACu) ? box : 0u;
            setNoDraw(part(waza, 0x26), true);
            setNoDraw(part(waza, 0x27), true);
            for (const uint32_t entry : {hintEntry, hintShadow})
                if (validPointer(entry, 0xACu) && rd16(entry + 0x90u) == kBlankMessage)
                    wr16(entry + 0x90u, kHintMessage);
            g_listEntry = text;
            g_listShown = true;
        }

        // The Techniques screen's draw (0x16DC18, $a0 = the screen; after its layout was placed): the list's place.
        void onWazaDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram || !g_listShown)
                return;
            const uint32_t waza = GPR_U32(ctx, 4);
            const uint32_t text = part(waza, 0x23), shadow = part(waza, 0x24);
            if (!validPointer(text, 0xACu) || !validPointer(shadow, 0xACu))
                return;
            wrFloat(text + 0x50u, kListX);
            wrFloat(text + 0x54u, kListY);
            wrFloat(shadow + 0x50u, kListX + 1.0f);
            wrFloat(shadow + 0x54u, kListY + 1.0f);
        }

        // System texture 20 (the "+arrow" art): getTexAddr(*(u32 *)0x3C2388, 21) as func_002ACDC8 finds it.
        uint32_t arrowTexture()
        {
            const uint32_t manager = rd32(0x3C2388u);
            if (!validPointer(manager))
                return 0u;
            const uint32_t table = rd32(manager + 0xCu);
            if (!validPointer(table, 0x100u) || rd32(table) < 21u)
                return 0u;
            const uint32_t offset = rd32(table + 21u * 4u);
            return offset ? table + offset : 0u;
        }

        // The icon drawer func_002B1300 at 0x2B1584 (every icon's UVs stored at sp+0x14..0x20, $s0 -> the code, $a3 the
        // icon sheet; Gp_draw_polyFT4 follows): inside the list, the four arrow codes become the arrow art.
        void onIconDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram || !g_listEntry || g_drawingEntry != g_listEntry)
                return;
            const uint16_t code = rd16(GPR_U32(ctx, 16));
            int arrow = -1;
            for (int i = 0; i < kStyles; ++i)
                if (code == kArrowCode[i])
                    arrow = i;
            if (arrow < 0)
                return;
            const uint32_t texture = arrowTexture();
            if (!texture)
                return;
            SET_GPR_U32(ctx, 7, texture);
            const uint32_t sp = GPR_U32(ctx, 29);
            for (int i = 0; i < 4; ++i)
                wrFloat(sp + 0x14u + 4u * static_cast<uint32_t>(i), kArrowUv[arrow][i]);
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
            g_holding.store(g_held >= 0 && !g_heldSaved ? g_held : -1, std::memory_order_relaxed);
            if (triangle && dpad != 0u)
                g_swallow = true;
            if (triangle)
                hideDpad(state);
        }
        else
        {
            g_held = -1;
            g_holding.store(-1, std::memory_order_relaxed);
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

    void noteEntry(uint32_t entry)
    {
        g_drawingEntry = entry & 0x1FFFFFFu;
    }

    void install(PS2Runtime &runtime)
    {
        g_ram = runtime.memory().getRDRAM();
        if (!g_ram)
            return;
        // Xavier's styles read LIFE down the list (Gene's read SEXY).
        if (const char *character = std::getenv("KAMITE_CHARACTER"); character && std::tolower(static_cast<unsigned char>(*character)) == 'x')
            std::memcpy(g_letters, "LIFE", kStyles + 1);
        ps2x::pnach::addFrameHook(&onVblank);
        FunctionHooks::instance().observeEntry(0x2AAA40u, "cIDBase_move (style list)", &onLayoutMove);
        FunctionHooks::instance().observeEntry(0x16DC18u, "cSubScrWaza draw (style list)", &onWazaDraw);
        ps2xRegisterHostHook("km_styles_icon", &onIconDraw);
        ps2xSetPatchFlag("styles", true);
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("Styles", &drawPanel);
#endif
    }
}
