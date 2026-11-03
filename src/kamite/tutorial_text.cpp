// Tutorial popups reworded for the launcher's options. See tutorial_text.h.
//
// The tutorial popups' text is in id/local/us/idtutorial_us.idd (loaded into RAM with the tutorial; K, Nov 3): a
// "MSD"/"MSA" container whose MSA bank header is u32 count (13) followed by one u32 offset per entry, each from the
// header, then 4 bytes (the game's 8-byte entries start at header + 4: offset, 0). Entry 0 (offset 0x6C) is
// Tutorial 1: "Use the right analog stick<NL>to evade enemy attacks!<A301><END>" beside the layout's stick sprite.
// The text is 16-bit codes in the menu font: A..Z 0x40.., a..z 0x5A.., space 0x35, '.' 0x21, '!' 0x23, ':' 0x31,
// ',' 0x7E, newline 0xA001, end 0x8001, A301 before the end; B002 n = the text scale in percent from there on (the
// other tutorials draw their button icons at B002 0x96 = 150 and go back to 0x64 = 100); button icons are codes
// 0..13 (R3 9, Select 13).
//
// With Modern on, entry 0's offset is pointed (u32, wrapping) at our text in guest memory below the ELF (0xFE000,
// past npc_poses' slots and before debug_tools' landing scratch; unused by the game); with Modern off it is put back.
// A scanner thread finds the bank by entry 0's original text every 250 ms (the file comes and goes with rooms); the
// game thread re-checks and points it each vblank. The new lines are drawn at 75%, each no wider than ~35 letters
// (the block is centred on the original's place; wider lines ran over the stick sprite, K Nov 3).
#include "kamite/tutorial_text.h"

#include "kamite/log.h"
#include "kamite/modern_controls.h"
#include "runtime/ps2_pnach.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace kamite::tutorial_text
{
    namespace
    {
        constexpr uint32_t kRamBytes = 0x2000000u;
        constexpr uint32_t kText = 0x000FE000u;     // our text (4 KB up to 0xFF000)
        constexpr uint32_t kTextBytes = 0x1000u;
        constexpr uint32_t kEntryCount = 13u;
        constexpr uint32_t kEntry0Offset = 0x6Cu;

        constexpr uint16_t kSpace = 0x35u, kNewline = 0xA001u, kEnd = 0x8001u, kClose = 0xA301u, kScale = 0xB002u;
        constexpr uint16_t kSwitchIcon = 9u; // R3 (Modern's switch)
        // The popup centres the whole block on the original's place, so a line much wider than the original's
        // (26 letters at 100%) pushes the block over the stick sprite on the left (K's screenshot, Nov 3): lines are
        // kept to ~35 letters at 75%. Icons at 1.5x the text, as the other tutorials (150 / 100), or they sit high.
        constexpr uint16_t kTextScale = 75u, kIconScale = 112u, kGapScale = 30u;

        uint8_t *g_ram = nullptr;
        std::atomic<uint32_t> g_bank{0}; // the bank header the scanner found (0 = none)

        uint16_t glyph(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return static_cast<uint16_t>(0x40 + (c - 'A'));
            if (c >= 'a' && c <= 'z')
                return static_cast<uint16_t>(0x5A + (c - 'a'));
            if (c >= '0' && c <= '9')
                return static_cast<uint16_t>(0x36 + (c - '0'));
            switch (c)
            {
            case '.': return 0x21u;
            case '!': return 0x23u;
            case ':': return 0x31u;
            case ',': return 0x7Eu;
            case '\'': return 0x27u;
            case '\n': return kNewline;
            default: return kSpace;
            }
        }
        void add(std::vector<uint16_t> &out, const char *text)
        {
            for (const char *c = text; *c; ++c)
                out.push_back(glyph(*c));
        }

        // Entry 0 as on the disc (the scan's pattern; the first two lines are enough to be sure).
        std::vector<uint16_t> original()
        {
            std::vector<uint16_t> out;
            add(out, "Use the right analog stick\nto evade enemy attacks!");
            return out;
        }

        // A gap between blocks: a line holding one space at kGapScale. A line is as tall as its tallest glyph (every
        // code below 0x1000 shares one height, the space included) plus the font's fixed line spacing, so a small
        // space makes a short line (cMessDrawFont_measurePage / measureText; an empty line would add only the spacing).
        void gap(std::vector<uint16_t> &out)
        {
            out.insert(out.end(), {kScale, kGapScale, kSpace, kNewline, kScale, kTextScale});
        }

        // K's wording (Nov 3), the Auto block only with Auto on.
        std::vector<uint16_t> modernText(bool autoOn)
        {
            std::vector<uint16_t> out{kScale, kTextScale};
            add(out, "Use the right analog stick to evade\nenemy attacks in brawling mode.\n");
            gap(out);
            out.insert(out.end(), {kScale, kIconScale, kSwitchIcon, kScale, kTextScale});
            add(out, "Switch between brawling mode\nand 3rd person free camera.");
            if (autoOn)
            {
                add(out, "\n");
                gap(out);
                add(out, "AUTO: Brawling mode starts when\nyou deal or take damage.");
            }
            out.insert(out.end(), {kClose, kEnd});
            return out;
        }

        uint32_t rd32(uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, g_ram + (address & (kRamBytes - 1u)), sizeof(value));
            return value;
        }
        void wr32(uint32_t address, uint32_t value) { std::memcpy(g_ram + (address & (kRamBytes - 1u)), &value, sizeof(value)); }

        bool isBank(uint32_t header)
        {
            if (header < 0x100000u || header + kEntry0Offset + 0x40u >= kRamBytes || rd32(header) != kEntryCount)
                return false;
            const std::vector<uint16_t> pattern = original();
            return std::memcmp(g_ram + header + kEntry0Offset, pattern.data(), pattern.size() * 2u) == 0;
        }

        void scanLoop()
        {
            const std::vector<uint16_t> pattern = original();
            const uint8_t *pat = reinterpret_cast<const uint8_t *>(pattern.data());
            const size_t bytes = pattern.size() * 2u;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (!modern::enabled())
                    continue;
                if (isBank(g_bank.load(std::memory_order_relaxed)))
                    continue; // still there
                const uint8_t *begin = g_ram + 0x100000u, *end = g_ram + kRamBytes;
                uint32_t found = 0;
                for (const uint8_t *p = begin; p + bytes <= end;)
                {
                    p = static_cast<const uint8_t *>(std::memchr(p, pat[0], static_cast<size_t>(end - bytes - p) + 1u));
                    if (!p)
                        break;
                    if (std::memcmp(p, pat, bytes) == 0)
                    {
                        const uint32_t header = static_cast<uint32_t>(p - g_ram) - kEntry0Offset;
                        if (isBank(header))
                        {
                            found = header;
                            break;
                        }
                    }
                    ++p;
                }
                g_bank.store(found, std::memory_order_relaxed);
            }
        }

        // EE thread, every vblank.
        void onVblank()
        {
            const uint32_t header = g_bank.load(std::memory_order_relaxed);
            if (!header || !isBank(header))
                return;
            const bool modern = modern::enabled();
            const uint32_t ours = kText - header; // u32: wraps to reach below the bank
            if (modern)
            {
                const std::vector<uint16_t> text = modernText(modern::autoEnabled());
                if (text.size() * 2u <= kTextBytes && std::memcmp(g_ram + kText, text.data(), text.size() * 2u) != 0)
                    std::memcpy(g_ram + kText, text.data(), text.size() * 2u);
                if (rd32(header + 4u) != ours)
                {
                    wr32(header + 4u, ours);
                    KM_INFO(Game, "tutorial text: Tutorial 1 explains Modern's Select" << (modern::autoEnabled() ? " and Auto" : "")
                                                                                      << " (bank at 0x" << std::hex << header << std::dec << ")");
                }
            }
            else if (rd32(header + 4u) == ours)
                wr32(header + 4u, kEntry0Offset);
        }
    }

    void install(uint8_t *rdram)
    {
        g_ram = rdram;
        if (!g_ram)
            return;
        std::thread(scanLoop).detach();
        ps2x::pnach::addFrameHook(&onVblank);
    }
}
