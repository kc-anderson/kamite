// Style presets, stage 1. See style_presets.h.
#include "kamite/style_presets.h"

#include "kamite/log.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_pnach.h"

#include <array>
#include <atomic>
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

        struct Snapshot
        {
            bool valid = false;
            uint32_t data = 0u;
            Combo combos[kSets] = {};
        };

        uint8_t *g_ram = nullptr;
        char g_letters[kStyles + 1] = "SEXY";
        std::mutex g_mutex; // the styles and the snapshot (EE thread writes, the F2 tab reads)
        std::array<bool, kStyles> g_used{};
        std::array<std::array<Combo, kSets>, kStyles> g_styles{};
        Snapshot g_snapshot;
        std::string g_status;
        // F2 -> EE thread: 1..4 save that style, 11..14 load it.
        std::atomic<int> g_request{0};

        uint32_t rd32(uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }

        uint32_t dataPointer()
        {
            const uint32_t data = rd32(kCoreSave);
            return data >= 0x100000u && data < 0x2000000u - 0x14A0u ? data : 0u;
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

        // Load: the style's moves the save owns, in order; a single slot without one stays empty, the chain closes up.
        int load(uint32_t data, const std::array<Combo, kSets> &style)
        {
            const int8_t *skill = reinterpret_cast<const int8_t *>(g_ram + data + kSkills);
            int skipped = 0;
            Combo combos[kSets] = {};
            for (int s = 0; s < kSets; ++s)
            {
                const Combo &from = style[static_cast<size_t>(s)];
                Combo &to = combos[s];
                const int n = from.max < 0 ? 0 : from.max > kSlots ? kSlots : from.max;
                int out = 0;
                for (int i = 0; i < n; ++i)
                {
                    const int32_t id = from.id[i];
                    if (id < 0 || id >= 0x80 || skill[id] < 0)
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

        void onVblank()
        {
            static uint32_t vblanks = 0u;
            ++vblanks;
            const int request = g_request.exchange(0);
            const uint32_t data = dataPointer();
            if (request != 0)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const int slot = (request % 10) - 1;
                const bool save = request < 10;
                char text[160];
                if (!data || slot < 0 || slot >= kStyles)
                    std::snprintf(text, sizeof(text), "No save data in memory yet.");
                else if (save)
                {
                    Combo combos[kSets];
                    read(data, combos);
                    std::memcpy(g_styles[static_cast<size_t>(slot)].data(), combos, kComboBytes);
                    g_used[static_cast<size_t>(slot)] = true;
                    std::snprintf(text, sizeof(text), "Style %c saved.", g_letters[slot]);
                }
                else if (!g_used[static_cast<size_t>(slot)])
                    std::snprintf(text, sizeof(text), "Style %c is empty.", g_letters[slot]);
                else
                {
                    const int skipped = load(data, g_styles[static_cast<size_t>(slot)]);
                    std::snprintf(text, sizeof(text), "Style %c loaded%s.", g_letters[slot],
                                  skipped ? " (moves this save does not own left out)" : "");
                }
                g_status = text;
                KM_INFO(Game, "styles: " << text << " (save data 0x" << std::hex << data << std::dec << ")");
            }
            if (request != 0 || vblanks % 15u == 0u)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_snapshot.valid = data != 0u;
                g_snapshot.data = data;
                if (data)
                    read(data, g_snapshot.combos);
            }
        }

#if defined(KAMITE_HAS_GS_MENU)
        void drawPanel(const Pcsx2GsUi *ui)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            ui->text_wrapped("Testing the style presets (stage 1): save the moves Gene has equipped to a style and load them "
                             "back, here or with the Techniques screen open. The styles are kept until the game closes.");
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
            for (int slot = 0; slot < kStyles; ++slot)
            {
                ui->push_id(slot);
                char label[32];
                std::snprintf(label, sizeof(label), "Style %c", g_letters[slot]);
                ui->text(label);
                ui->same_line();
                if (ui->small_button("Save"))
                    g_request = slot + 1;
                ui->same_line();
                ui->begin_disabled(!g_used[static_cast<size_t>(slot)]);
                if (ui->small_button("Load"))
                    g_request = slot + 11;
                ui->end_disabled();
                if (g_used[static_cast<size_t>(slot)])
                {
                    const std::string line = describe(g_styles[static_cast<size_t>(slot)][0]);
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
