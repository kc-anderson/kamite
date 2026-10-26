// Style presets, stage 1. See style_presets.h.
#include "kamite/style_presets.h"

#include "kamite/button_prompt_sheets.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/menu_buttons.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
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
            uint8_t active; // the style loaded or saved last (shown as loaded while the equipped moves still match it)
            uint8_t reserved;
            Combo styles[kStyles][kSets];
            uint32_t checksum;
        };
        static_assert(sizeof(Store) == 8u + kStyles * kComboBytes + 4u, "Store layout");
        static_assert(kStoreOffset >= game::CoreSaveData::kUsedEnd, "Store clear of the game's fields");
        static_assert(kStoreOffset + sizeof(Store) <= kSaveSize, "Store fits in cCoreSaveData");

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
            if (validPointer(checkpoint, kSaveSize) && checkpoint != data &&
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
            return subScreenOpen && static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) == kTechniquesKind &&
                   g_ram[(wazaScreen(subScr) + kWazaState) & 0x1FFFFFFu] == 0u;
        }

        // Gene in a stage under the player's control (research of Oct 25, SLUS_215.03; addresses in the game work block
        // 0x7474A0): a player (Getplayer's 0x747A94), no sub screen open, Gene not ignoring input or the game frozen
        // (0x747A78 bits 31/30: sub screens, movies, events), no scripted "soft event" (0x747A84 bit 0x40: the casino
        // slots, the dog race), Gene not suspended (+0x250 bit 0x8000) and in his normal mode (+0x2F4 == 0; 5 is the
        // sub-screen mode: state table 0x3BD0B8 entry 5, 0x10B780, the decomp's UpdateBossPhase_10B780, plays the
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
                // No corner notice (Oct 26): the HUD indicator and the screen's list show the result; the one case
                // neither can, a save with no save data in memory, still gets a word.
                if (!data)
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
        // The info panel's "Already Set" text (shown by the game only while the move picker is open, to say the
        // highlighted move is in another slot) carries the list while the slots are browsed, moved to the empty space
        // right of the big TECHNIQUES title (x ~225..365, y ~10..110 of the 512 x 448 menu screen; K's pick, Oct 25):
        // four lines, "[Y] + arrow  Style S" down to "Style Y" (reads SEXY; LIFE as Xavier). parts[0x23] is the text
        // (align 3: the block centred on its draw position, scale 0.65), parts[0x24] its shadow (+1,+1, colour set 1:
        // black, icons skipped); the rest of its group (parts[0x25] the move name, [0x26] the "down+" art, [0x27] the
        // slot's button, the name box: record #57, no id) is hidden. The text is message 0x30C4 (an unused "<7d>"
        // placeholder) whose offset word points at a string of ours; the shadow is 0x30FE (no colour codes, which
        // would colour the shadow too). The bottom bar's spare line (ids 57/58: "Release" on the chain's last rows,
        // otherwise blank 0x30D0) says "[Y] Hold D-Pad: Save Style" (0x30FF). Entry fields: +0x2C flags (0x08000000 =
        // not drawn), +0x50/+0x54 draw position, +0x90 message id.
        //
        // The arrows and the plus are the icon sheet's style pieces (button_prompt_sheets.h: a down arrow, a plus and a
        // right arrow shaped like the game's "+down" art, in the sheet's empty cells; up and left are the arrows
        // flipped), drawn in place of icon codes 4..8 (L1 R1 L2 R2 L3, never in this text) while the list's entry is
        // drawn, at the size the game draws its own "+down" art next to a button, the plus against the arrow as in
        // that art.
        constexpr uint32_t kNoDraw = game::kIdentFlagNoDraw;
        constexpr uint32_t kMessageBankPointer = 0x3C23A4u; // *(*(u32 *)0x3C23A4 + 0xC) = the message bank
        constexpr uint16_t kListMessage = 0x30C4u, kShadowMessage = 0x30FEu, kHintMessage = 0x30FFu, kBlankMessage = 0x30D0u;
        constexpr uint32_t kListText = 0x1000u, kShadowText = 0x1100u, kHintText = 0x1200u; // in the save record's unused tail
        constexpr uint32_t kHudText = 0x1300u, kHudShadowText = 0x1400u;                  // the in-play indicator's strings
        constexpr uint16_t kHudMessage = 0x3041u, kHudShadowMessage = 0x3042u;            // two more "<7d>" placeholders of bank 3
        constexpr float kListX = 311.0f, kListY = 56.5f; // the block is centred here (align 3): the names' right end and the top line stay put
        // Line pitch: a new line moves down by the line's height (the tallest code: a letter, 41 x the scale = 26.8 at
        // 0.65; icons are 32 x the scale) + state+0x30 x state+0x48. The game's +0x30 at this scale gives ~22 (K: right).
        constexpr float kLinePitch = 22.0f, kLineHeight = 26.8f;
        constexpr uint16_t kArrowCode[kStyles] = {4u, 5u, 6u, 7u}; // up, right, down, left
        constexpr uint16_t kPlusCode = 8u;                         // the plus before the arrow
        constexpr int kPlusPiece = kStyles;                        // g_pendingArrow: the plus
        constexpr uint32_t kFontState = game::MessDrawFont::kInstance; // the font's draw state (D_005E7510)
        constexpr float kArrowTextGap = 5.0f;                      // texels between the arrow and the style's name
        constexpr uint16_t kTriangleIcon = 1u, kPlus = 0x0Eu, kSpace = 0x10u, kNewLine = 0xA001u, kEnd = 0x8001u;
        constexpr uint16_t kColour = 0xB203u, kColourRestore = 0xB301u; // 0xB203 hi lo: A B G R (0x80 = full)
        constexpr uint32_t kGold = 0x80106880u, kDim = 0x80484848u;

        std::atomic<int> g_holding{-1}; // the pad filter: the direction held with Y, before it saves
        uint32_t g_listEntry = 0u;      // the entry the arrows are drawn for (EE thread)
        uint32_t g_shadowEntry = 0u;    // its shadow entry (same text, icons not drawn): gets the same line pitch
        uint32_t g_drawingEntry = 0u;   // the cIDBase entry being drawn (hud_layout's hook)
        bool g_listShown = false;       // this frame's list is on (the screen's draw places it)
        uint32_t g_hidName = 0u, g_hidBox = 0u; // entries hidden by us, shown again when the picker opens
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

        // The equipped moves are this style's (as loading it gave them): ids and lengths, levels aside.
        bool equipped(uint32_t data, const Combo (&style)[kSets])
        {
            Combo now[kSets];
            read(data, now);
            const int8_t *skill = reinterpret_cast<const int8_t *>(g_ram + data + kSkills);
            for (int s = 0; s < kSets; ++s)
            {
                const int n = style[s].max < 0 ? 0 : style[s].max > kSlots ? kSlots : style[s].max;
                int out = 0;
                for (int i = 0; i < n; ++i)
                {
                    const int32_t id = style[s].id[i];
                    if (id < 0 || id >= kMoveCount || skill[id] < 0)
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
        // The style letters in the HUD (K, Oct 26): "S E X Y" above the roulette charges, right of the level meter,
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
        constexpr float kHudX = 97.0f, kHudY = 348.0f, kHudScale = 0.65f;
        // The cream of the Techniques page's lines, in the GS's colour scale (0x80 = full: the glyphs are white, so
        // the colour is what shows). It was 0xDC 0xC8 0xA0, the on-screen value, which the GS doubles past full:
        // white on screen (K, Oct 26).
        constexpr uint32_t kBeige = 0x8050646Eu; // A B G R
        uint32_t g_hudText = 0u, g_hudShadow = 0u; // the two entries (EE thread); 0 = not found
        bool g_hudShown = false;

        std::vector<uint16_t> hudText(uint32_t data, const Store &store, bool colours)
        {
            const bool blinkOn = (g_vblanks / 8u) % 2u == 0u;
            const bool flash = g_vblanks - g_lastAction.load(std::memory_order_relaxed) < 40u;
            std::vector<uint16_t> out;
            for (int slot = 0; slot < kStyles; ++slot)
            {
                if (slot > 0)
                    out.push_back(kSpace);
                const bool used = (store.used & (1u << slot)) != 0u;
                uint32_t colour = kBeige;
                // In play a d-pad tap only loads a style; saving is the Techniques screen's alone (padFilter).
                if (used && store.active == slot && equipped(data, store.styles[slot]))
                    colour = (flash && !blinkOn) ? kBeige : kGold;
                else if (!used)
                    colour = kDim;
                if (colours)
                    out.insert(out.end(), {kColour, static_cast<uint16_t>(colour >> 16), static_cast<uint16_t>(colour & 0xFFFFu)});
                out.push_back(glyph(g_letters[slot]));
            }
            out.push_back(kEnd);
            return out;
        }

        // Four lines: [Y] + arrow  Style S. The style's words take its colour: gold when loaded (blinking for a
        // moment after a load or a save), blinking gold while Y + its direction is held, dim when empty.
        std::vector<uint16_t> listText(uint32_t data, const Store &store, bool colours)
        {
            const int holding = g_holding.load(std::memory_order_relaxed);
            const bool blinkOn = (g_vblanks / 8u) % 2u == 0u;
            const bool flash = g_vblanks - g_lastAction.load(std::memory_order_relaxed) < 40u;
            std::vector<uint16_t> out;
            for (int slot = 0; slot < kStyles; ++slot)
            {
                if (slot > 0)
                    out.push_back(kNewLine);
                out.push_back(kTriangleIcon);
                out.push_back(kPlusCode);
                out.push_back(kArrowCode[slot]);
                const bool used = (store.used & (1u << slot)) != 0u;
                uint32_t colour = 0u; // 0: the line's own colour
                if (slot == holding)
                    colour = blinkOn ? kGold : 0u; // held with Y: blinks until it saves
                else if (used && store.active == slot && equipped(data, store.styles[slot]))
                    colour = (flash && !blinkOn) ? 0u : kGold; // the loaded style (a few blinks right after)
                else if (!used)
                    colour = kDim;
                if (colours && colour)
                    out.insert(out.end(), {kColour, static_cast<uint16_t>(colour >> 16), static_cast<uint16_t>(colour & 0xFFFFu)});
                append(out, "Style ");
                out.push_back(glyph(g_letters[slot]));
                if (colours && colour)
                    out.push_back(kColourRestore);
            }
            out.push_back(kEnd);
            return out;
        }

        std::vector<uint16_t> hintText()
        {
            std::vector<uint16_t> out{kTriangleIcon};
            append(out, "Hold D-Pad: Save Style");
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
        bool isHudLayout(uint32_t layout, uint32_t &text, uint32_t &shadow)
        {
            if (!validPointer(layout, 0x10u) || rd32(layout + game::IDBase::kEntNum) != 6u)
                return false;
            const uint32_t entries = rd32(layout + game::IDBase::kEnt);
            if (!validPointer(entries, 6u * ent::kSize))
                return false;
            text = entries + 4u * ent::kSize;
            shadow = entries + 5u * ent::kSize;
            for (const uint32_t e : {text, shadow})
            {
                const uint16_t msg = rd16(e + ent::kMsg);
                if (g_ram[(e + ent::kKind) & 0x1FFFFFFu] != game::kIdentKindText || (msg != 0u && msg != kHudMessage && msg != kHudShadowMessage))
                    return false;
            }
            return true;
        }

        void hudIndicator(uint32_t layout)
        {
            uint32_t text, shadow;
            if (!isHudLayout(layout, text, shadow))
                return;
            const uint32_t data = dataPointer();
            Store store{};
            const bool show = data && readStore(data, store) && store.used != 0u;
            g_hudText = text;
            g_hudShadow = shadow;
            if (!show)
            {
                g_hudShown = false;
                setNoDraw(text, true);
                setNoDraw(shadow, true);
                return;
            }
            writeText(data + kHudText, hudText(data, store, true));
            writeText(data + kHudShadowText, hudText(data, store, false));
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
            const bool show = data && menu_buttons::modern() && static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) == kTechniquesKind &&
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
                g_listEntry = g_shadowEntry = 0u;
                for (const uint32_t entry : {hintEntry, hintShadow})
                    if (validPointer(entry, ent::kSize) && rd16(entry + ent::kMsg) == kHintMessage)
                        wr16(entry + ent::kMsg, kBlankMessage);
                return;
            }
            if (!validPointer(text, ent::kSize) || !validPointer(shadow, ent::kSize))
                return;
            Store store{};
            readStore(data, store);
            writeText(data + kListText, listText(data, store, true));
            writeText(data + kShadowText, listText(data, store, false));
            writeText(data + kHintText, hintText());
            if (!pointMessage(kListMessage, data + kListText) || !pointMessage(kShadowMessage, data + kShadowText) ||
                !pointMessage(kHintMessage, data + kHintText))
                return;
            setNoDraw(text, false);
            wr16(text + ent::kMsg, kListMessage);
            wr16(shadow + ent::kMsg, kShadowMessage);
            const uint32_t name = part(waza, 0x25), entries = rd32(waza + 0x4u);
            const uint32_t box = validPointer(entries, 0x4000u) ? entries + 57u * ent::kSize : 0u;
            setNoDraw(name, true);
            setNoDraw(box, true);
            g_hidName = validPointer(name, ent::kSize) ? name : 0u;
            g_hidBox = validPointer(box, ent::kSize) ? box : 0u;
            setNoDraw(part(waza, 0x26), true);
            setNoDraw(part(waza, 0x27), true);
            for (const uint32_t entry : {hintEntry, hintShadow})
                if (validPointer(entry, ent::kSize) && rd16(entry + ent::kMsg) == kBlankMessage)
                    wr16(entry + ent::kMsg, kHintMessage);
            g_listEntry = text & 0x1FFFFFFu;
            g_shadowEntry = shadow & 0x1FFFFFFu;
            g_listShown = true;
        }

        // The Techniques screen's draw (0x16DC18, $a0 = the screen; after its layout was placed): the list's place.
        void onWazaDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram || !g_listShown)
                return;
            const uint32_t waza = GPR_U32(ctx, 4);
            const uint32_t text = part(waza, 0x23), shadow = part(waza, 0x24);
            if (!validPointer(text, ent::kSize) || !validPointer(shadow, ent::kSize))
                return;
            wrFloat(text + ent::kDrawX, kListX);
            wrFloat(text + ent::kDrawY, kListY);
            wrFloat(shadow + ent::kDrawX, kListX + 1.0f);
            wrFloat(shadow + ent::kDrawY, kListY + 1.0f);
        }

        // cIDBase_transText at 0x2AB1E0 ($s1 the entry, $s0 the font's draw state D_005E7510, its line spacing just
        // set): the list's lines kLinePitch apart, the shadow's too (cMessDrawFont_measureLineHeight measures the icons
        // of a line whether or not they are drawn, so the same value gives the shadow the same pitch). A new line
        // moves down by the line's height (the tallest code on it: 32 x scale for an icon) + state+0x30 x state+0x48
        // (the vertical scale).
        void onTextSetup(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram || !g_listEntry)
                return;
            const uint32_t entry = GPR_U32(ctx, 17) & 0x1FFFFFFu;
            if (entry != g_listEntry && entry != g_shadowEntry)
                return;
            const uint32_t state = GPR_U32(ctx, 16);
            const float zoom = rdFloat(state + game::MessDrawFont::kZoomY);
            if (zoom > 0.05f && zoom < 4.0f)
                wrFloat(state + game::MessDrawFont::kCharGapY, (kLinePitch - kLineHeight * (zoom / 0.65f)) / zoom);
        }

        prompt_sheets::ArrowRect pieceRect(int piece)
        {
            return prompt_sheets::arrowRect(piece == kPlusPiece ? prompt_sheets::ArrowPlus
                                            : piece == 0 || piece == 2 ? prompt_sheets::ArrowDown
                                                                        : prompt_sheets::ArrowRight);
        }

        // The icon drawer func_002B1300 at 0x2B1584 (every icon's UVs stored at sp+0x14..0x20, $s0 -> the code, $a3 the
        // icon sheet; Gp_draw_polyFT4 follows): inside the list, the four arrow codes and the plus code take the style
        // pieces' UVs.
        void onIconDraw(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            g_pendingArrow = -1;
            if (!g_ram || !g_listEntry || g_drawingEntry != g_listEntry)
                return;
            const uint16_t code = rd16(GPR_U32(ctx, 16));
            int arrow = code == kPlusCode ? kPlusPiece : -1;
            for (int i = 0; i < kStyles; ++i)
                if (code == kArrowCode[i])
                    arrow = i;
            if (arrow < 0)
                return;
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
        // Every arrow advances by the widest arrow, so the names start in one column (K, Oct 25).
        float pieceAdvance(int piece)
        {
            float texels = 0.0f;
            if (piece == kPlusPiece)
                texels = pieceRect(piece).x1 - pieceRect(piece).x0;
            else
            {
                for (int i = 0; i < kStyles; ++i)
                    texels = std::max(texels, pieceRect(i).x1 - pieceRect(i).x0);
                texels += kArrowTextGap;
            }
            return texels * (26.0f / 28.0f);
        }

        // The list's text entry or its shadow is being drawn (the shadow draws no icons but measures and advances
        // over them: it gets the same widths, so its letters sit under the text's).
        bool drawingList()
        {
            return g_listEntry && (g_drawingEntry == g_listEntry || g_drawingEntry == g_shadowEntry);
        }

        int pieceOfCode(uint16_t code)
        {
            int piece = code == kPlusCode ? kPlusPiece : -1;
            for (int i = 0; i < kStyles; ++i)
                if (code == kArrowCode[i])
                    piece = i;
            return piece;
        }

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

    bool onHudEntry(uint32_t entry)
    {
        entry &= 0x1FFFFFFu;
        if (!g_ram || !g_hudShown || (entry != g_hudText && entry != g_hudShadow))
            return false;
        // Placed here, after the layout's move put the entry where its data says (the top left corner).
        const float d = entry == g_hudShadow ? 1.0f : 0.0f;
        wrFloat(entry + ent::kDrawX, kHudX + d);
        wrFloat(entry + ent::kDrawY, kHudY + d);
        return true;
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
        ps2xRegisterHostHook("km_styles_icon_quad", &onIconQuad);
        ps2xRegisterHostHook("km_styles_icon_width", &onIconWidth);
        ps2xRegisterHostHook("km_styles_icon_advance", &onIconAdvance);
        ps2xRegisterHostHook("km_styles_text", &onTextSetup);
        ps2xSetPatchFlag("styles", true);
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("Styles", &drawPanel);
#endif
    }
}
