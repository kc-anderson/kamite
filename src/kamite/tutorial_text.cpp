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
// Entry 1 (Tutorial 2, the roulette) always points at ours (0xFE800): the cycle line names R1, the left stick and the
// d-pad, the OK line follows the menu buttons (rouletteText). Entry 2 (Tutorial 3, the unleash) always points at ours
// (0xFF000; unleashText: its first line moved to the title, the R2-again line with Tension is Placable on). Titles
// (K, Nov 4): entries 7 / 8 ("TUTORIAL 2" / "TUTORIAL 3") point at "TUTORIAL 2: GODHAND ROULETTE REEL" (0xFF800) /
// "TUTORIAL 3: UNLEASH THE GODHAND" (0xFF900), the first lines of entries 1 / 2 on the disc, at kTitleScale so they
// fit the box; the other titles are as on the disc (their popups have no heading line).
// Entry 4 (the dash, "TUTORIAL 5" in play) always points at ours (0xFFA00; dashText): the stick icon in the text in
// place of the layout's LEFT stick picture, and the L3 icon with L3 dash on.
// A scanner thread finds the bank by entry 0's original text every 250 ms (the file comes and goes with rooms); the
// game thread re-checks and points it each vblank. The new lines are drawn at 75%, each no wider than ~35 letters
// (the block is centred on the original's place; wider lines ran over the stick sprite, K Nov 3).
#include "kamite/tutorial_text.h"

#include "kamite/frame_patches.h"
#include "kamite/game_options.h"
#include "kamite/log.h"
#include "kamite/menu_buttons.h"
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
        constexpr uint32_t kText = 0x000FE000u;     // our text (6 KB up to 0xFF800): entries 0, 1, 2, then the
                                                    // titles (0x100 each, up to 0xFFA00), the dash (entry 4, up to
                                                    // 0xFFC00); buck_breaker's slot text at 0xFFC00
                                                    // (0x100); debug_tools' landing scratch is at
                                                    // 0xFFF00, game_options' 0xFFFF0
        constexpr uint32_t kTitleText = 0x000FF800u, kTitleBytes = 0x100u;
        constexpr uint32_t kTextBytes = 0x800u;     // per entry
        constexpr uint32_t kEntryCount = 13u;
        constexpr uint32_t kEntry0Offset = 0x6Cu, kEntry1Offset = 0xD4u, kEntry2Offset = 0x1B4u; // entry n's u32 at header + 4 + 8n
        constexpr uint32_t kEntry7Offset = 0x4C0u, kEntry8Offset = 0x4D8u; // "TUTORIAL 2" / "TUTORIAL 3"
        constexpr uint32_t kEntry4Offset = 0x2ECu;                          // the dash ("TUTORIAL 5" in play)
        constexpr uint32_t kDashText = 0x000FFA00u, kDashBytes = 0x200u;
        // The longer titles at this scale: "TUTORIAL 3" spans ~405 px of K's 1441 px screenshot from x 175, the box's
        // inside ends near 1400, the title letters are ~1.16x the popup text: "TUTORIAL 2: GODHAND ROULETTE REEL" is
        // ~1385 px at 100%.
        constexpr uint16_t kTitleScale = 85u;

        constexpr uint16_t kSpace = 0x35u, kNewline = 0xA001u, kEnd = 0x8001u, kClose = 0xA301u, kScale = 0xB002u;
        constexpr uint16_t kStickIcon = 11u, kSwitchIcon = 9u; // RS (the right stick), R3 (Modern's switch)
        constexpr uint16_t kGapCode = 0xA802u, kGapNever = 0xFFFFu; // see indent()
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
            case '-': return 0x1Fu; // "One-Two Punch" in the game's text
            case '/': return 0x2Du; // "reset/switch off the console"
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

        // A line's start where the text after an icon starts (the hanging indent), no taller than a text line. Widths
        // from the menu font (func_002A4B20 on the font at *0x3C2380, read from a state): icons 9 / 11 are 31 units, the
        // space 6; a plain code also gets the font's letter spacing, a control code does not (measureText; the draw
        // loop advances every code by measureCode too). The icon at 112% = 34.72 units + spacing; one space at 72%
        // (4.32 + spacing) and the 0xA8 gap code at 76% (40 units x scale = 30.4) make the same, and neither is taller
        // than the text (space 32 x 0.72, gap 26 x 0.76, text 32 x 0.75). The gap code draws the page marker when its
        // parameter equals the font's +0x54 (func_002B00A0, read as a signed short): 0xFFFF, read unsigned, never does.
        void indent(std::vector<uint16_t> &out)
        {
            static_assert(kIconScale == 112u && kTextScale == 75u, "indent() is worked out for these scales");
            out.insert(out.end(), {kScale, 72u, kSpace, kScale, 76u, kGapCode, kGapNever, kScale, kTextScale});
        }
        void iconLine(std::vector<uint16_t> &out, uint16_t icon, const char *text)
        {
            out.insert(out.end(), {kScale, kIconScale, icon, kScale, kTextScale});
            add(out, text);
            add(out, "\n");
        }
        void indentedLine(std::vector<uint16_t> &out, const char *text, bool last = false)
        {
            indent(out);
            add(out, text);
            if (!last)
                add(out, "\n");
        }

        // K's wording (Nov 3; the switch line "Press [R3] to switch between / brawling and freecam." Nov 4), the Auto
        // block only with Auto on. Each block: an icon (or none) and the text beside it,
        // both lines starting where the text after the icon does. The stick icons are the prompt set's side views
        // (prompt_hd: RS without the click arrow, R3 with it); the layout's big stick picture is hidden with Modern
        // (button_prompts updateSticks). Lines up to ~36 letters: without the picture the centred block may be wider.
        std::vector<uint16_t> modernText(bool autoOn)
        {
            std::vector<uint16_t> out{kScale, kTextScale};
            iconLine(out, kStickIcon, "Use the right analog stick to evade");
            indentedLine(out, "enemy attacks while brawling.");
            gap(out);
            indent(out); // K (Nov 4): the switch's icon in the text, so a remapped switch reads right
            add(out, "Press");
            out.insert(out.end(), {kScale, kIconScale, kSwitchIcon, kScale, kTextScale});
            add(out, "to switch between\n");
            indentedLine(out, "brawling and freecam.", !autoOn);
            if (autoOn)
            {
                gap(out);
                indentedLine(out, "AUTO: Brawling starts automatically");
                indentedLine(out, "when you deal or take damage.", true);
            }
            out.insert(out.end(), {kClose, kEnd});
            return out;
        }

        // Tutorial 2 (entry 1, the roulette), every control style (K, Nov 4). The disc's: "GODHAND ROULETTE WHEEL / Press
        // [R1] to open the wheel / Press [R1] to cycle choices / Press [cross][circle] to select technique" (icons at
        // B002 0x96, text back at 0x64). The wheel steps on R1 and the left stick up / down (command bits 40 / 48 / 49,
        // func_00145810) and the d-pad (frame_patches rouletteDpad, always on); its OK follows the menu buttons
        // (menu_buttons.h: Western cross, Japanese circle; Classic the game's own cross and circle, so that line stays). K's
        // wording (Nov 4): "Use [R1]/[LS]/ D-pad to cycle", "to execute technique" (Classic: both of the disc's OK buttons).
        // '-' and '/' are the font's own (0x1F, 0x2D). The heading line is in the title (as "...ROULETTE REEL") since Nov 4.
        std::vector<uint16_t> rouletteText(menu_buttons::Style style)
        {
            constexpr uint16_t kBig = 0x96u, kNormal = 0x64u, kR1 = 5u, kLeftStick = 10u, kCross = 3u, kCircle = 0u;
            std::vector<uint16_t> out;
            const auto icon = [&](uint16_t code) { out.insert(out.end(), {kScale, kBig, code, kScale, kNormal}); };
            add(out, "Press");
            icon(kR1);
            add(out, "to open the wheel\nUse");
            icon(kR1);
            add(out, "/");
            icon(kLeftStick);
            add(out, "/ D-pad to cycle\nPress");
            // The OK buttons by their place on the pad (menu_buttons.h "Pinned menus").
            if (style == menu_buttons::Style::Classic)
            {
                out.insert(out.end(), {kScale, kBig, menu_buttons::placeIcon(kCross), menu_buttons::placeIcon(kCircle), kScale, kNormal});
                add(out, "to execute technique");
            }
            else
            {
                icon(menu_buttons::placeIcon(style == menu_buttons::Style::Japanese ? kCircle : kCross));
                add(out, "to execute technique");
            }
            out.insert(out.end(), {kClose, kEnd});
            return out;
        }

        // Tutorial 3 (entry 2, the unleash; K, Nov 4). The disc's: "UNLEASH THE GODHAND / Once your tension gauge is /
        // full, press [R2] to unleash / the GODHAND." (R2 = icon 7 at 150%). The first line is in the title now; Tension
        // is Placable adds "Press [R2] again to stop."; Off the Cuff swaps the first lines for "With any tension in your
        // / gauge, press [R2] ...". All at the disc's sizes.
        std::vector<uint16_t> unleashText(bool placable, bool offTheCuff)
        {
            constexpr uint16_t kBig = 0x96u, kNormal = 0x64u, kR2 = 7u;
            std::vector<uint16_t> out;
            const auto r2 = [&] { out.insert(out.end(), {kScale, kBig, kR2, kScale, kNormal}); };
            add(out, placable && offTheCuff ? "With any tension in your\ngauge, press" : "Once your tension gauge is\nfull, press");
            r2();
            add(out, "to unleash\nthe GODHAND.");
            if (placable)
            {
                add(out, "\nPress");
                r2();
                add(out, "again to stop.");
            }
            out.insert(out.end(), {kClose, kEnd});
            return out;
        }

        // The dash tutorial (entry 4; K, Nov 4). The disc's: "Pressing the left analog stick / forward twice quickly will /
        // cause you to DASH!" beside the layout's LEFT stick picture (that cell is now empty: button_prompts
        // updateSticks). The stick icon (LS 10) goes in the text at the other tutorials' 150%; with L3 dash on, the L3
        // icon (8: whatever L3 is bound to) as the other way.
        std::vector<uint16_t> dashText(bool l3)
        {
            constexpr uint16_t kBig = 0x96u, kNormal = 0x64u, kLeftStick = 10u, kL3 = 8u;
            std::vector<uint16_t> out;
            const auto icon = [&](uint16_t code) { out.insert(out.end(), {kScale, kBig, code, kScale, kNormal}); };
            add(out, "Pressing the");
            icon(kLeftStick);
            add(out, "left stick\nforward twice");
            if (l3)
            {
                add(out, " or");
                icon(kL3);
                add(out, "will\n");
            }
            else
                add(out, " quickly will\n");
            add(out, "cause you to DASH!");
            out.insert(out.end(), {kClose, kEnd});
            return out;
        }

        // A popup's title (entries 6-11), smaller to fit.
        std::vector<uint16_t> titleText(const char *text)
        {
            std::vector<uint16_t> out{kScale, kTitleScale};
            add(out, text);
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

        // One entry: our text at `at` while `wanted`, else the disc's offset back.
        void pointEntry(uint32_t header, int entry, uint32_t discOffset, uint32_t at, bool wanted, const std::vector<uint16_t> &text,
                        const char *what, uint32_t capacity = kTextBytes)
        {
            const uint32_t slot = header + 4u + 8u * static_cast<uint32_t>(entry);
            const uint32_t ours = at - header; // u32: wraps to reach below the bank
            if (wanted && text.size() * 2u <= capacity)
            {
                if (std::memcmp(g_ram + at, text.data(), text.size() * 2u) != 0)
                    std::memcpy(g_ram + at, text.data(), text.size() * 2u);
                if (rd32(slot) != ours)
                {
                    wr32(slot, ours);
                    KM_INFO(Game, "tutorial text: " << what << " (bank at 0x" << std::hex << header << std::dec << ")");
                }
            }
            else if (rd32(slot) == ours)
                wr32(slot, discOffset);
        }

        // EE thread, every vblank.
        void onVblank()
        {
            const uint32_t header = g_bank.load(std::memory_order_relaxed);
            if (!header || !isBank(header))
                return;
            const bool modern = modern::enabled();
            pointEntry(header, 0, kEntry0Offset, kText, modern, modern ? modernText(modern::autoEnabled()) : std::vector<uint16_t>{},
                       "Tutorial 1 explains Modern's switch");
            const menu_buttons::Style style = menu_buttons::style();
            pointEntry(header, 1, kEntry1Offset, kText + kTextBytes, true, rouletteText(style),
                       style == menu_buttons::Style::Japanese  ? "Tutorial 2: the roulette's controls (Japanese OK)"
                       : style == menu_buttons::Style::Classic ? "Tutorial 2: the roulette's controls (Classic OK)"
                                                               : "Tutorial 2: the roulette's controls (Western OK)");
            const bool placable = tensionPlacable();
            pointEntry(header, 2, kEntry2Offset, kText + 2u * kTextBytes, true, unleashText(placable, offTheCuff()),
                       placable ? "Tutorial 3 (the title holds its heading; Tension is Placable)" : "Tutorial 3 (the title holds its heading)");
            pointEntry(header, 4, kEntry4Offset, kDashText, true, dashText(l3Dash()),
                       "Tutorial 5 (the dash) with its stick icon in the text", kDashBytes);
            pointEntry(header, 7, kEntry7Offset, kTitleText, true, titleText("TUTORIAL 2: GODHAND ROULETTE REEL"),
                       "Tutorial 2's title", kTitleBytes);
            pointEntry(header, 8, kEntry8Offset, kTitleText + kTitleBytes, true, titleText("TUTORIAL 3: UNLEASH THE GODHAND"),
                       "Tutorial 3's title", kTitleBytes);
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
