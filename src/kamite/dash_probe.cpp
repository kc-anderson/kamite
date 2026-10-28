// Dash logger (temporary diagnostic, claude/strip-list.md): finds the game's double-tap window for the stick dash,
// which counts game frames and so is half as long at 60 frames/s. Off unless F2 > Debug > "Dash logger" is ticked.
//
// At every game frame start (frame_patches.cpp, after the 60 FPS stick hold and the L3 dash wrote the stick) it
// snapshots Gene's object (0x1800 bytes from GameWork.playerObj) and GameWork (0x700 bytes from 0x7474A0) and diffs
// them against the last frame. A word that changes on most frames (positions, animation, timers that always run) is
// noise: each word keeps a count of the frames it changed in over the last 64. When the left stick of pad 1 goes
// from centre to a full push or back (the events the double tap is made of), the next 20 frames log every quiet word
// that changed (u32 old -> new), one line per frame, so the counter that restarts on a push and counts until the
// next one shows up as a run of +1 steps between the two pushes, with the dash state flipping after the second.
#include "kamite/dash_probe.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ps2_memory.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <atomic>
#include <bit>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace kamite::dash_probe
{
    namespace
    {
        constexpr uint32_t kPlayerBytes = 0x1800u;
        constexpr uint32_t kGameWorkBytes = 0x700u;
        constexpr uint32_t kStickX = 0x5E8146u, kStickY = 0x5E8147u; // pad 1's left stick (the libpad buffer)
        constexpr int kFollowFrames = 20;
        constexpr int kNoisyChanges = 16; // changed in this many of the last 64 frames: noise
        constexpr size_t kMaxPerLine = 40;

        struct Region
        {
            const char *name;
            uint32_t base = 0;
            std::vector<uint8_t> last;
            std::vector<uint64_t> history; // per word: bit i = changed i frames ago
        };

        std::atomic<bool> g_enabled{false};
        Region g_player{"pl"}, g_work{"gw"};
        bool g_stickOut = false;   // the stick was pushed (past 3/4) last frame
        int g_follow = 0;          // frames still to log after an event
        uint64_t g_frame = 0;
        uint32_t g_events = 0;

        bool inRam(uint32_t address, uint32_t size)
        {
            const uint32_t a = address & 0x1FFFFFFFu;
            return a >= 0x100000u && a + size <= PS2_RAM_SIZE;
        }

        // Diffs one region against its last snapshot; appends "name+off old->new" for quiet words when logging.
        void diff(const uint8_t *rdram, Region &r, uint32_t base, uint32_t bytes, bool log, std::string &line, size_t &count)
        {
            const uint32_t words = bytes / 4u;
            if (r.base != base || r.last.size() != bytes)
            {
                r.base = base;
                r.last.assign(rdram + (base & 0x01FFFFFFu), rdram + (base & 0x01FFFFFFu) + bytes);
                r.history.assign(words, 0u);
                return;
            }
            const uint8_t *now = rdram + (base & 0x01FFFFFFu);
            for (uint32_t w = 0; w < words; ++w)
            {
                uint32_t a = 0, b = 0;
                std::memcpy(&a, r.last.data() + 4u * w, 4);
                std::memcpy(&b, now + 4u * w, 4);
                const bool changed = a != b;
                const bool noisy = std::popcount(r.history[w]) >= kNoisyChanges;
                r.history[w] = (r.history[w] << 1) | (changed ? 1u : 0u);
                if (changed && log && !noisy && count < kMaxPerLine)
                {
                    char item[48];
                    std::snprintf(item, sizeof(item), " %s+%03X %X->%X", r.name, 4u * w, a, b);
                    line += item;
                    ++count;
                }
            }
            std::memcpy(r.last.data(), now, bytes);
        }
    }

    void onFrame(const uint8_t *rdram)
    {
        if (!g_enabled.load(std::memory_order_relaxed))
        {
            g_player.last.clear();
            g_work.last.clear();
            g_follow = 0;
            return;
        }
        ++g_frame;
        const int x = static_cast<int>(rdram[kStickX]) - 128, y = static_cast<int>(rdram[kStickY]) - 128;
        const int magnitude2 = x * x + y * y;
        const bool out = magnitude2 > 96 * 96;  // past 3/4
        const bool centre = magnitude2 < 32 * 32; // within 1/4
        const char *event = nullptr;
        if (out && !g_stickOut)
            event = "PUSH";
        else if (centre && g_stickOut)
            event = "CENTRE";
        if (out)
            g_stickOut = true;
        else if (centre)
            g_stickOut = false;
        if (event)
        {
            g_follow = kFollowFrames;
            ++g_events;
        }
        const bool log = g_follow > 0 || event;
        std::string line;
        size_t count = 0;
        const uint32_t player = [&] {
            uint32_t p = 0;
            std::memcpy(&p, rdram + game::GameWork::address(game::GameWork::kPlayerObj), 4);
            return p;
        }();
        if (inRam(player, kPlayerBytes))
            diff(rdram, g_player, player, kPlayerBytes, log, line, count);
        diff(rdram, g_work, game::GameWork::kAddress, kGameWorkBytes, log, line, count);
        if (log)
        {
            const bool fps60 = rdram[game::GameWork::address(game::GameWork::kVblankDivider)] == 1u;
            KM_INFO(Game, "dash log f" << g_frame << (fps60 ? " 60" : " 30") << " stick " << x << "," << y
                                        << (event ? std::string(" ") + event : std::string()) << " |" << line);
            if (!event)
                --g_follow;
        }
    }

    void drawPanel(const Pcsx2GsUi *ui)
    {
#if defined(KAMITE_HAS_GS_MENU)
        int on = g_enabled.load() ? 1 : 0;
        if (ui->checkbox("Dash logger (temporary: writes stick taps and what they change to godhand.log)##dashlog", &on))
            g_enabled = on != 0;
        if (g_enabled.load())
        {
            char text[96];
            std::snprintf(text, sizeof(text), "%u stick events logged.", g_events);
            ui->text_disabled(text);
        }
#else
        (void)ui;
#endif
    }
}
