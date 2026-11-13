// Menu buttons: Classic (PS2), Western (south confirms, east cancels) or Japanese (east confirms, south cancels);
// menus and the roulette only. See menu_buttons.h.
//
// The first version (aa44ccf) traded circle and triangle at the pad, which also traded two attack slots in play;
// the game's own split between the raw word (play) and the command word (menus) is what keeps this one to the menus.
#include "kamite/menu_buttons.h"
#include "kamite/disc_files.h"
#include "kamite/log.h"
#include "kamite/game_layout.h"
#include "ps2_host_gamepads.h"
#include "ps2_runtime.h" // ps2xSetPatchFlag
#include "ps2_runtime_macros.h" // GPR_U32
#include "runtime/ps2_cd_image.h"
#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kamite::menu_buttons
{
    namespace
    {
        std::atomic<int> g_style{static_cast<int>(Style::Western)};
        Style current() { return static_cast<Style>(g_style.load(std::memory_order_relaxed)); }
        uint8_t *g_ram = nullptr;
        // Set at start when every patch of the kind went onto the disc image (the file as the game reads it): the
        // run-time rewrites below are then not needed (no RAM scan thread, no core check every 30 vblanks).
        bool g_coreOnDisc = false, g_menuFilesOnDisc = false;

        // The command tables (SLUS_215.03 .data): 64 raw masks, entry i = the raw bits that set command bit i. The raw
        // word is the pad's two button bytes high-to-low: 0x1000 up, 0x2000 right, 0x4000 down, 0x8000 left (+ select
        // 0x100, L3, R3, start 0x800), 0x01 L2, 0x02 R2, 0x04 L1, 0x08 R1, 0x10 triangle, 0x20 circle, 0x40 cross,
        // 0x80 square. Command bit 28 is the menus' "OK", bit 29 their "Cancel" (the decomp's matched menus test
        // +0x1A0 & 0x10000000 with the confirm sound 0x15E and & 0x20000000 with the cancel sound 0x161). The table the
        // game uses is 0x3C3E50 when GameWork.language (0x747A0C) is not 0 or debugFlags has 0x800 (the US release: OK =
        // circle|cross, Cancel = triangle), 0x3C3D50 otherwise (OK = circle, Cancel = cross, the Japanese way).
        constexpr uint32_t kTables[2] = {0x3C3D50u, 0x3C3E50u};
        constexpr uint32_t kOkEntry = 28u, kCancelEntry = 29u;
        constexpr uint32_t kDiscOk[2] = {0x20u, 0x60u}, kDiscCancel[2] = {0x40u, 0x10u};
        constexpr uint32_t kRawCircle = 0x20u, kRawCross = 0x40u;
        constexpr uint32_t kCorePointer = game::GameWork::address(game::GameWork::kLangData); // GameWork.langData: where core_usa.bin was loaded
        constexpr uint32_t kRamBytes = 0x2000000u;
        constexpr uint32_t kDpadEntry = 24u; // entries 24..27: up, down, right, left

        // ---- places (menu_buttons.h "Pinned menus") ----
        namespace bindings = ps2_host_input::prompts;
        using bindings::Input;
        constexpr uint8_t kButtons = 16u; // Input values 0..15: the buttons (the stick directions follow)
        // Per Input 0..15 (Cross Circle Square Triangle L1 R1 L2 R2 L3 R3 Start Select Up Down Left Right): its bit in the
        // raw word, its text icon (0xFFFF: none), its ps2x::iop::PadButton bit, and its controller binding on the default
        // layout (ps2_host_input.cpp defaultBindings: type, ps2_host_gamepads code, axis sign).
        constexpr uint32_t kRawBit[kButtons] = {0x40u, 0x20u, 0x80u, 0x10u, 0x04u, 0x08u, 0x01u, 0x02u,
                                                0x200u, 0x400u, 0x800u, 0x100u, 0x1000u, 0x4000u, 0x8000u, 0x2000u};
        constexpr uint16_t kIcon[kButtons] = {3u, 0u, 2u, 1u, 4u, 5u, 6u, 7u, 8u, 9u, 12u, 13u, 0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu};
        constexpr uint16_t kPadBit[kButtons] = {1u << 14, 1u << 13, 1u << 15, 1u << 12, 1u << 10, 1u << 11, 1u << 8, 1u << 9,
                                                1u << 1,  1u << 2,  1u << 3,  1u << 0,  1u << 4,  1u << 6,  1u << 7,  1u << 5};
        struct Place
        {
            bindings::Type type;
            int16_t code;
            int8_t sign;
        };
        constexpr Place kDefaultPlace[kButtons] = {
            {bindings::Type::PadButton, ps2_host_gamepads::South, 0},         {bindings::Type::PadButton, ps2_host_gamepads::East, 0},
            {bindings::Type::PadButton, ps2_host_gamepads::West, 0},          {bindings::Type::PadButton, ps2_host_gamepads::North, 0},
            {bindings::Type::PadButton, ps2_host_gamepads::LeftShoulder, 0},  {bindings::Type::PadButton, ps2_host_gamepads::RightShoulder, 0},
            {bindings::Type::PadAxis, ps2_host_gamepads::LeftTrigger, 1},     {bindings::Type::PadAxis, ps2_host_gamepads::RightTrigger, 1},
            {bindings::Type::PadButton, ps2_host_gamepads::LeftStick, 0},     {bindings::Type::PadButton, ps2_host_gamepads::RightStick, 0},
            {bindings::Type::PadButton, ps2_host_gamepads::Start, 0},         {bindings::Type::PadButton, ps2_host_gamepads::Back, 0},
            {bindings::Type::PadButton, ps2_host_gamepads::DPadUp, 0},        {bindings::Type::PadButton, ps2_host_gamepads::DPadDown, 0},
            {bindings::Type::PadButton, ps2_host_gamepads::DPadLeft, 0},      {bindings::Type::PadButton, ps2_host_gamepads::DPadRight, 0},
        };
        uint32_t rawBit(uint8_t input) { return input < kButtons ? kRawBit[input] : 0u; }

        // The PS2 button whose first controller binding is the physical button `of` has on the default layout.
        uint8_t placeOf(const bindings::Snapshot &snapshot, uint8_t of)
        {
            const Place &want = kDefaultPlace[of];
            for (uint8_t i = 0; i < kButtons; ++i)
            {
                const bindings::Binding &b = snapshot.controller[i];
                if (b.type == want.type && b.code == want.code && (want.type != bindings::Type::PadAxis || b.sign == want.sign))
                    return i;
            }
            return of;
        }

        bool isFace(uint16_t icon) { return icon <= 3u; }
        uint16_t faceColour(uint16_t icon) { return icon == 0u ? 1u : icon == 1u ? 2u : icon == 3u ? 3u : 4u; } // kColourCode's index

        // Glyph codes of the game's text (core_usa.bin, 16-bit).
        constexpr uint16_t kSpace = 0x10u, kSpace2 = 0x35u, kColon = 0x31u;
        constexpr uint16_t kCircle = 0u, kTriangle = 1u, kCross = 3u;
        constexpr uint16_t kColourCode = 0xB602u; // + a colour index: the icons' own colours (circle 1, triangle 2, cross 3, square 4)
        constexpr uint16_t kWhite = 0xB301u;      // back to white (no width, no argument)

        // Japanese: the Western text with cross and circle traded (a colour code before an icon follows it).
        std::vector<uint16_t> japaneseOf(const std::vector<uint16_t> &western)
        {
            std::vector<uint16_t> out = western;
            for (size_t i = 0; i < out.size(); ++i)
            {
                if (out[i] == kColourCode && i + 2 < out.size() && (out[i + 2] == kCross || out[i + 2] == kCircle))
                {
                    out[i + 1] = out[i + 2] == kCross ? 1u : 3u; // the traded icon's colour
                    out[i + 2] = out[i + 2] == kCross ? kCircle : kCross;
                    i += 2;
                }
                else if (out[i] == kCross)
                    out[i] = kCircle;
                else if (out[i] == kCircle)
                    out[i] = kCross;
                else if (out[i] == kColourCode)
                    ++i; // a colour index is not a glyph
            }
            return out;
        }
        std::vector<uint16_t> styleText(const std::vector<uint16_t> &original, const std::vector<uint16_t> &western, Style style)
        {
            return style == Style::Classic ? original : style == Style::Japanese ? japaneseOf(western) : western;
        }
        // The dialog choices' patches are the [A804 n] cursor codes' arguments, not icons: never pinned.
        bool cursorArguments(const std::vector<uint16_t> &text)
        {
            return text.size() == 3u && text[1] == 0xFFFFu && text[2] == 0xFFFFu;
        }
        // A style's text names the menus' buttons by their place (cross south, circle east, triangle north, square west:
        // Western's cross confirms, Japanese's circle, Classic's both); each face icon becomes the icon of the PS2 button
        // on that place now, with that icon's own colour where the text colours it (a shoulder or stick icon: white).
        std::vector<uint16_t> pinned(std::vector<uint16_t> t)
        {
            if (cursorArguments(t))
                return t;
            for (size_t i = 0; i < t.size(); ++i)
            {
                if (t[i] == kColourCode && i + 2 < t.size() && isFace(t[i + 2]))
                {
                    const uint16_t icon = placeIcon(t[i + 2]);
                    if (isFace(icon))
                        t[i + 1] = faceColour(icon);
                    else
                        t[i] = t[i + 1] = kWhite;
                    t[i + 2] = icon;
                    i += 2;
                }
                else if (t[i] == kColourCode)
                    ++i; // a colour index is not a glyph
                else if (isFace(t[i]))
                    t[i] = placeIcon(t[i]);
            }
            return t;
        }
        std::vector<uint16_t> wanted(const std::vector<uint16_t> &original, const std::vector<uint16_t> &western, Style style)
        {
            return pinned(styleText(original, western, style));
        }
        // Whether text in RAM is one of ours: a style's text with any icons in the places (pinned with any mapping).
        bool sameShape(const uint8_t *guest, const std::vector<uint16_t> &t)
        {
            if (cursorArguments(t))
                return std::memcmp(guest, t.data(), t.size() * 2u) == 0;
            for (size_t i = 0; i < t.size(); ++i)
            {
                uint16_t g;
                std::memcpy(&g, guest + 2u * i, 2u);
                if (t[i] == kColourCode && i + 2 < t.size() && isFace(t[i + 2]))
                {
                    if (g != kColourCode && g != kWhite)
                        return false;
                    ++i; // the colour (or a second white)
                }
                else if (isFace(t[i]))
                {
                    if (g > 13u)
                        return false;
                }
                else if (g != t[i])
                    return false;
            }
            return true;
        }
        uint16_t glyph(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return static_cast<uint16_t>(0x40 + (c - 'A'));
            if (c >= 'a' && c <= 'z')
                return static_cast<uint16_t>(0x5A + (c - 'a'));
            if (c >= '0' && c <= '9')
                return static_cast<uint16_t>(0x36 + (c - '0')); // 0x36..0x3F (0x20 is "=")
            return kSpace;
        }
        std::vector<uint16_t> text(std::initializer_list<uint16_t> icons, const char *word = nullptr)
        {
            std::vector<uint16_t> out(icons);
            for (const char *c = word; c && *c; ++c)
                out.push_back(glyph(*c));
            return out;
        }
        std::vector<uint16_t> cat(std::initializer_list<std::vector<uint16_t>> parts)
        {
            std::vector<uint16_t> out;
            for (const auto &p : parts)
                out.insert(out.end(), p.begin(), p.end());
            return out;
        }

        uint32_t rd32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_ram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }
        void wr32(uint32_t address, uint32_t value)
        {
            std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value));
        }

        // EE thread, every vblank: the OK, Cancel and d-pad entries, on their places (menu_buttons.h "Pinned menus"; with
        // the default layout the disc's values for Classic). An entry that is empty or holds more than button bits (a
        // mod's table) is left alone.
        void writeEntry(uint32_t table, uint32_t entry, uint32_t want)
        {
            const uint32_t at = table + entry * 4u, have = rd32(at);
            if (have == want || have == 0u || (have & ~0xFFFFu) != 0u || want == 0u)
                return;
            wr32(at, want);
        }
        void applyTables()
        {
            const Style style = current();
            const Places p = places();
            const uint32_t south = rawBit(p.south), east = rawBit(p.east), north = rawBit(p.north);
            const uint32_t dpad[4] = {rawBit(p.up), rawBit(p.down), rawBit(p.right), rawBit(p.left)};
            for (size_t i = 0; i < 2u; ++i)
            {
                const uint32_t table = kTables[i];
                const bool us = i == 1u; // Classic: the US release's OK = circle | cross, Cancel = triangle; the other circle / cross
                writeEntry(table, kOkEntry, style == Style::Western ? south : style == Style::Japanese ? east : us ? (south | east) : east);
                writeEntry(table, kCancelEntry, style == Style::Western ? east : style == Style::Japanese ? south : us ? north : south);
                for (uint32_t k = 0; k < 4u; ++k)
                    writeEntry(table, kDpadEntry + k, dpad[k]);
            }
        }

        // EE thread, every 30 vblanks: the hint strings in the core block follow the setting both ways (it can change
        // while playing, F2 > Controls), checked against the disc's words or ours first (another language's core or a
        // changed one is left alone, once with a warning).
        void applyCoreText()
        {
            const uint32_t core = rd32(kCorePointer);
            if (core < 0x100000u || core >= kRamBytes - 0x100000u || (core & 0xFu) != 0u)
                return;
            const Style style = current();
            static bool warned = false;
            for (const TextPatch &patch : textPatches())
            {
                uint8_t *guest = g_ram + core + patch.offset;
                const size_t bytes = patch.original.size() * 2u;
                if (core + patch.offset + bytes > kRamBytes)
                    continue;
                const std::vector<uint16_t> want = wanted(patch.original, patch.modern, style);
                if (std::memcmp(guest, want.data(), bytes) == 0)
                    continue;
                bool known = false;
                for (const Style s : {Style::Classic, Style::Western, Style::Japanese})
                    known = known || sameShape(guest, styleText(patch.original, patch.modern, s));
                if (!known)
                {
                    if (!warned)
                    {
                        warned = true;
                        KM_WARN(Game, "menu buttons: the hint text at core+0x" << std::hex << patch.offset << std::dec
                                                                               << " is not the US disc's; its wording is left alone");
                    }
                    continue;
                }
                std::memcpy(guest, want.data(), bytes);
            }
        }

        // A fix to the US core text, in every style (a data bug of the release, not a button choice), in RAM so a save
        // state made before it is fixed too. The petit devil's "Up for my challenge?" (message 0x3004): its options
        // are cursor codes [A804 n] (cMessDrawFont draws the arrow at the code whose n is the selection, 0x2B00A0; each
        // raises the option count to n + 1, 0x2B3B40; [A602 0] before them starts at 0, left / right clamped,
        // 0x2B3EC8). The disc labels "Of course!!" 3 and "Leave me alone!" 0, but its handler fights on 0
        // (func_0028E858 0x28E918): the arrow sat on "Leave" while A accepted, and 1 / 2 were blank slots (K, Nov 2).
        // Labelled 0 and 1 here. Stage 4's "Use the switch?" (message 0x3006, read by r404's switches: 0 = use it) has
        // the same [A804 3] Yes [A804 0] No labels: the arrow sat on "No" while A used the switch (K, Nov 6).
        void applyCoreFixes()
        {
            const uint32_t core = rd32(kCorePointer);
            if (core < 0x100000u || core >= kRamBytes - 0x100000u || (core & 0xFu) != 0u)
                return;
            struct Choice
            {
                uint32_t offset;             // core_usa.bin offset of the first option's [A804 n]
                std::vector<uint16_t> text;  // from it up to the second option's [A804 n]
            };
            static const Choice kChoices[] = {
                // "[A804 3] Of course!! [A804 0]"
                {0x577A6u, {0xA804u, 0x0003u, 0xFFFFu, 0xFFFFu, 0x004Eu, 0x005Fu, 0x0035u, 0x005Cu, 0x0068u, 0x006Eu, 0x006Bu, 0x006Cu,
                            0x005Eu, 0x0023u, 0x0023u, 0x0010u, 0xA804u, 0x0000u}},
                // "[A804 3] Yes [A804 0]" of "Use the switch?"
                {0x57864u, {0xA804u, 0x0003u, 0xFFFFu, 0xFFFFu, 0x0058u, 0x005Eu, 0x006Cu, 0x0010u, 0xA804u, 0x0000u}},
            };
            for (const Choice &choice : kChoices)
            {
                uint8_t *guest = g_ram + core + choice.offset;
                const size_t bytes = choice.text.size() * 2u;
                if (std::memcmp(guest, choice.text.data(), bytes) != 0)
                    continue; // fixed already, or not the US disc's text
                const uint16_t first = 0u, second = 1u;
                std::memcpy(guest + 2u, &first, 2u);
                std::memcpy(guest + bytes - 2u, &second, 2u);
            }
        }

        // Menu-file strings: the scanner thread lists where each rewrite's original text is, the EE thread rewrites.
        std::mutex g_foundMutex;
        std::vector<std::pair<size_t, uint32_t>> g_found; // rewrite index, guest address

        void scanLoop()
        {
            const auto &rewrites = textRewrites();
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                std::vector<std::pair<size_t, uint32_t>> found;
                const uint8_t *begin = g_ram + 0x100000u, *end = g_ram + kRamBytes;
                for (size_t i = 0; i < rewrites.size(); ++i)
                {
                    const uint8_t *pat = reinterpret_cast<const uint8_t *>(rewrites[i].original.data());
                    const size_t bytes = rewrites[i].original.size() * 2u;
                    for (const uint8_t *p = begin; p + bytes <= end;)
                    {
                        p = static_cast<const uint8_t *>(std::memchr(p, pat[0], static_cast<size_t>(end - bytes - p) + 1u));
                        if (!p)
                            break;
                        if (std::memcmp(p, pat, bytes) == 0)
                            found.emplace_back(i, static_cast<uint32_t>(p - g_ram));
                        ++p;
                    }
                }
                if (found.empty())
                    continue;
                std::lock_guard<std::mutex> lock(g_foundMutex);
                g_found.insert(g_found.end(), found.begin(), found.end());
            }
        }

        void applyFoundText()
        {
            std::vector<std::pair<size_t, uint32_t>> found;
            {
                std::unique_lock<std::mutex> lock(g_foundMutex, std::try_to_lock);
                if (!lock.owns_lock())
                    return;
                found.swap(g_found);
            }
            const auto &rewrites = textRewrites();
            for (const auto &[index, address] : found)
            {
                const TextRewrite &rw = rewrites[index];
                const size_t bytes = rw.original.size() * 2u;
                if (address + bytes > kRamBytes)
                    continue;
                uint8_t *guest = g_ram + address;
                if (std::memcmp(guest, rw.original.data(), bytes) != 0)
                    continue; // gone since the scan
                const std::vector<uint16_t> want = wanted(rw.original, rw.modern, current());
                if (want == rw.original)
                    continue; // Classic on the default layout: the text as it is
                std::memcpy(guest, want.data(), bytes);
                KM_INFO(Game, "menu buttons: " << rw.where << " reworded at 0x" << std::hex << address << std::dec);
            }
        }

        // The text patches as disc patches: the files are plain files in the archive, loaded as they are, and every
        // patch keeps its length, so writing them over the file in the image is the same as writing them over the
        // file in RAM once it is loaded, without watching RAM for it. Each original is checked in the disc's file
        // first; anything not found as expected is left to the run-time path.
        void installDiscText()
        {
            if (!disc_files::open())
            {
                KM_INFO(Game, "menu buttons: no file archive on the disc image; the hint text is reworded in RAM");
                return;
            }
            std::vector<PS2DiscOverlay::Patch> patches;
            auto bytesOf = [](const std::vector<uint16_t> &codes) {
                std::vector<uint8_t> out(codes.size() * 2u);
                for (size_t i = 0; i < codes.size(); ++i)
                {
                    out[2 * i] = static_cast<uint8_t>(codes[i] & 0xFFu);
                    out[2 * i + 1] = static_cast<uint8_t>(codes[i] >> 8);
                }
                return out;
            };

            // core_usa.bin: the patches sit at fixed offsets (the core block in RAM is the file).
            std::vector<uint8_t> file;
            if (const auto core = disc_files::find("core_usa.bin"); core && disc_files::read(*core, file))
            {
                size_t matched = 0;
                for (const TextPatch &patch : textPatches())
                {
                    const std::vector<uint8_t> original = bytesOf(patch.original);
                    if (patch.offset + original.size() > file.size() ||
                        std::memcmp(file.data() + patch.offset, original.data(), original.size()) != 0)
                        continue;
                    patches.emplace_back(core->imageOffset + patch.offset, bytesOf(wanted(patch.original, patch.modern, current())));
                    ++matched;
                }
                g_coreOnDisc = matched == textPatches().size();
                KM_INFO(Game, "menu buttons: " << matched << " of " << textPatches().size() << " hint texts reworded in core_usa.bin on the disc"
                                               << (g_coreOnDisc ? "" : "; the rest is reworded in RAM"));
            }

            // The menu files: each original found by searching the file (every place it occurs).
            size_t foundAll = 0;
            const auto &rewrites = textRewrites();
            std::vector<bool> found(rewrites.size(), false);
            for (const char *path : {"id/local/us/idsavecore_us.idd", "id/local/us/idsubscrmenu_us.idd", "id/local/us/idsubscrmap_us.idd",
                                     "id/local/us/idsubscrwaza_us.idd", "id/local/us/idsubscroption_us.idd",
                                     "id/local/us/idsubscroptiont_us.idd", "id/local/us/idsubscrbonus_us.idd",
                                     "id/local/us/idsubscrgodreel_us.idd", "id/local/us/idtitlecore_us.idd"})
            {
                const auto menu = disc_files::find(path);
                if (!menu || !disc_files::read(*menu, file))
                    continue;
                for (size_t i = 0; i < rewrites.size(); ++i)
                {
                    const std::vector<uint8_t> original = bytesOf(rewrites[i].original);
                    for (size_t at = 0; at + original.size() <= file.size(); ++at)
                    {
                        if (std::memcmp(file.data() + at, original.data(), original.size()) != 0)
                            continue;
                        patches.emplace_back(menu->imageOffset + at, bytesOf(wanted(rewrites[i].original, rewrites[i].modern, current())));
                        KM_INFO(Game, "menu buttons: " << rewrites[i].where << " reworded in " << path << " at 0x" << std::hex << at << std::dec);
                        found[i] = true;
                    }
                }
            }
            for (bool f : found)
                foundAll += f ? 1u : 0u;
            g_menuFilesOnDisc = foundAll == rewrites.size();
            PS2DiscOverlay::add(std::move(patches));
        }

        // ---- the Techniques screen's Gene turned by the right stick (K, Nov 7; no option) ----
        // The screen (cSubScr kind 1, cSubScrWaza at cSubScr + 0x310) shows Gene (waza +0x264, the player) playing the
        // hovered move. The stick's x turns his heading (+0x104, radians) like a turntable, up to 180 degrees a second
        // (host time: the same at 30 and 60). The turn stays while the screen is open: when the game puts his heading
        // back to the screen's facing (the facing seen when the stick first moved), the turn goes back on. Closing the
        // screen puts back the heading the game saved at its opening (Screen_closeAndRestorePlayer 0x16C678: waza +0x270
        // -> player +0x100), so play is untouched; the next opening starts unturned.
        std::atomic<int> g_rightX{0x80}; // the pad filter (window thread)
        bool g_turnOpen = false;
        float g_turn = 0.0f, g_turnBase = 0.0f, g_turnWritten = 0.0f;
        bool g_turnHasBase = false;
        std::chrono::steady_clock::time_point g_turnTime;

        float rdF(uint32_t address)
        {
            float value;
            std::memcpy(&value, g_ram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }
        void wrF(uint32_t address, float value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        bool ramPointer(uint32_t p, uint32_t room) { return p >= 0x100000u && p < 0x2000000u - room; }

        float wrapPi(float a)
        {
            constexpr float kPi = 3.14159265f;
            while (a > kPi)
                a -= 2.0f * kPi;
            while (a < -kPi)
                a += 2.0f * kPi;
            return a;
        }

        void turnTechniquesGene()
        {
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(0.1f, std::chrono::duration<float>(now - g_turnTime).count());
            g_turnTime = now;
            const uint32_t subScr = rd32(game::SubScr::kPointer);
            const bool open = ramPointer(subScr, 0x1000u) && static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) == 1;
            const uint32_t player = open ? rd32(subScr + 0x310u + 0x264u) : 0u;
            if (!open || !ramPointer(player, 0x700u) || g_ram[(subScr + 0x310u + 0x5Au) & 0x1FFFFFFu] == 2u)
            {
                g_turnOpen = open && g_turnOpen; // leaving (state 2): keep the turn until it closes
                if (!open)
                {
                    g_turnOpen = false;
                    g_turn = 0.0f;
                    g_turnHasBase = false;
                }
                return;
            }
            g_turnOpen = true;
            const uint32_t heading = player + 0x104u;
            const float now_ = rdF(heading);
            float x = (static_cast<float>(g_rightX.load(std::memory_order_relaxed)) - 128.0f) / 127.0f;
            constexpr float kDead = 0.2f;
            x = std::fabs(x) < kDead ? 0.0f : (x - std::copysign(kDead, x)) / (1.0f - kDead);
            if (x != 0.0f && !g_turnHasBase)
            {
                g_turnBase = now_;
                g_turnWritten = now_;
                g_turnHasBase = true;
            }
            if (!g_turnHasBase)
                return;
            const float step = x * 3.14159265f * dt; // 180 degrees a second at full tilt
            float want = now_;
            if (g_turn != 0.0f && std::fabs(wrapPi(now_ - g_turnBase)) < 1e-4f && std::fabs(wrapPi(now_ - g_turnWritten)) > 1e-4f)
                want = g_turnBase + g_turn; // the game put the screen's facing back (a new move): the turn goes back on
            g_turn = wrapPi(g_turn + step);
            want = wrapPi(want + step);
            if (want != now_)
                wrF(heading, want);
            g_turnWritten = want;
        }

        void onVblank()
        {
            static uint32_t vblanks = 0u;
            ++vblanks;
            applyTables();
            turnTechniquesGene();
            if (vblanks % 30u == 0u)
            {
                applyCoreText();
                applyCoreFixes();
            }
            if (!g_menuFilesOnDisc)
                applyFoundText();
        }

        // The roulette (part of play, so the command tables do not reach it; flag menu_roulette, on in every style since
        // the menus are pinned: with the default layout Classic gets the game's own values back).
        // Its OK (func_00145810: ori v1,zero,0xA000 at 0x145CE8, the command bits 33 | 35 = cross | circle, shifted
        // by 20 next): the OK place(s) of the style, as command bits 32..43 (the buttons one by one, from the table).
        // The menus' OK (bit 28) is tested after it and follows the tables.
        void rouletteOk(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const Style style = current();
            const Places p = places();
            const uint32_t ok = style == Style::Western ? rawBit(p.south) : style == Style::Japanese ? rawBit(p.east)
                                                                                                     : rawBit(p.south) | rawBit(p.east);
            uint32_t bits = 0u;
            for (uint32_t i = 32u; i < 44u; ++i)
            {
                const uint32_t mask = rd32(kTables[1] + i * 4u);
                if (mask != 0u && (mask & (mask - 1u)) == 0u && (ok & mask) != 0u)
                    bits |= 1u << (i - 20u);
            }
            if (bits == 0u)
                bits = style == Style::Japanese ? 0x8000u : style == Style::Western ? 0x2000u : 0xA000u;
            SET_GPR_U32(ctx, 3, bits);
        }
        uint32_t rouletteCancelBit()
        {
            const Places p = places();
            const Style style = current();
            return style == Style::Western ? rawBit(p.east) : style == Style::Japanese ? rawBit(p.south) : rawBit(p.north);
        }
        // Its cancel: Gene's +0x626 window, filled from the pressed word (andi v0,v0,0x10 at 0x108FA0: the triangle).
        // The cancel place's raw bit (Western the circle's place, Japanese the cross's, Classic the triangle's).
        void rouletteCancelSource(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            SET_GPR_U32(ctx, 2, GPR_U32(ctx, 2) & rouletteCancelBit());
        }
        // Before sb zero,0x621(s1) at 0x114904 (the roulette taking the cancel): the button that cancelled must not also
        // start its attack (Gene's 3-frame windows, 0x108F2C..0x108F88: +0x621 square, +0x622 triangle, +0x623 cross,
        // +0x624 R1, +0x625 circle). Classic as the PS2 (nothing cleared).
        void rouletteCancelWindow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (current() == Style::Classic)
                return;
            const uint32_t bit = rouletteCancelBit();
            const uint32_t window = bit == 0x80u ? 0x621u : bit == 0x10u ? 0x622u : bit == 0x40u ? 0x623u : bit == 0x08u ? 0x624u
                                  : bit == 0x20u ? 0x625u : 0u;
            if (window != 0u)
                rdram[(GPR_U32(ctx, 17) + window) & 0x1FFFFFFu] = 0u;
        }

        // places(): worked out again when the bindings or the device in use change.
        std::mutex g_placesMutex;
        uint32_t g_placesGeneration = ~0u;
        int g_placesDevice = -1;
        Places g_places{Input::Cross, Input::Circle, Input::Triangle, Input::Square, Input::Up, Input::Down, Input::Left, Input::Right};
        bool g_controllerPlaces = false; // installDiscText: the save menu's text is written once, for the controller
    }

    Places placesFor(const bindings::Snapshot &snapshot, bool controller)
    {
        Places p{Input::Cross, Input::Circle, Input::Triangle, Input::Square, Input::Up, Input::Down, Input::Left, Input::Right};
        if (!controller || snapshot.generation == 0u)
            return p;
        p.south = placeOf(snapshot, Input::Cross);
        p.east = placeOf(snapshot, Input::Circle);
        p.north = placeOf(snapshot, Input::Triangle);
        p.west = placeOf(snapshot, Input::Square);
        p.up = placeOf(snapshot, Input::Up);
        p.down = placeOf(snapshot, Input::Down);
        p.left = placeOf(snapshot, Input::Left);
        p.right = placeOf(snapshot, Input::Right);
        return p;
    }

    Places places()
    {
        const uint32_t generation = bindings::generation();
        const int device = g_controllerPlaces ? 2 : static_cast<int>(bindings::lastDevice());
        std::lock_guard<std::mutex> lock(g_placesMutex);
        if (generation != g_placesGeneration || device != g_placesDevice)
        {
            const Places was = g_places;
            g_places = placesFor(bindings::snapshot(), device != static_cast<int>(bindings::Device::Keys));
            g_placesGeneration = generation;
            g_placesDevice = device;
            if (std::memcmp(&was, &g_places, sizeof(Places)) != 0)
                KM_INFO(Game, "menu buttons: the menus' places are the PS2 buttons " << int(g_places.south) << " (south), "
                                                                                    << int(g_places.east) << " (east), " << int(g_places.north)
                                                                                    << " (north), " << int(g_places.west) << " (west)");
        }
        return g_places;
    }

    uint16_t placeIcon(uint16_t icon)
    {
        if (!isFace(icon))
            return icon;
        const Places p = places();
        const uint8_t input = icon == kCross ? p.south : icon == kCircle ? p.east : icon == kTriangle ? p.north : p.west;
        const uint16_t out = input < kButtons ? kIcon[input] : 0xFFFFu;
        return out == 0xFFFFu ? icon : out;
    }

    uint16_t padBitAtPlace(uint16_t padBit)
    {
        for (uint8_t i = 0; i < kButtons; ++i)
            if (kPadBit[i] == padBit)
                return kPadBit[placeOf(bindings::snapshot(), i)];
        return padBit;
    }

    void noteRightStick(uint8_t x) { g_rightX.store(x, std::memory_order_relaxed); }

    const char *styleName(Style style)
    {
        return style == Style::Classic ? "classic" : style == Style::Japanese ? "japanese" : "western";
    }

    void install(uint8_t *rdram)
    {
        // Western unless [Game] MenuButtons says otherwise (the launcher's Controls > Menu controls, F2 > Controls).
        const char *value = std::getenv("KAMITE_MENU_BUTTONS");
        std::string text = value && *value ? value : "western";
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const Style style = text == "classic" || text == "0" ? Style::Classic : text == "japanese" ? Style::Japanese : Style::Western;
        g_style.store(static_cast<int>(style), std::memory_order_relaxed);
        ps2xRegisterHostHook("km_roulette_ok", &rouletteOk);
        ps2xRegisterHostHook("km_roulette_cancel_source", &rouletteCancelSource);
        ps2xRegisterHostHook("km_roulette_cancel_window", &rouletteCancelWindow);
        ps2xSetPatchFlag("menu_roulette", true); // kamite.toml: the roulette's own buttons (on their places in every style)
        g_ram = rdram;
        KM_INFO(Game, "menu buttons: " << styleName(style)
                                       << (style == Style::Western    ? " (the south button confirms and the east one cancels; play unchanged)"
                                           : style == Style::Japanese ? " (the east button confirms and the south one cancels; play unchanged)"
                                                                      : " (circle and cross confirm, triangle cancels, as on the PS2)"));
        if (!g_ram)
            return;
        // The menu files on the disc are written once, at start: their icons follow the controller's places (a later
        // change of the mapping or the style reaches them at the next start; the core text follows at once).
        g_controllerPlaces = true;
        installDiscText();
        g_controllerPlaces = false;
        ps2x::pnach::addFrameHook(&onVblank);
        if (!g_menuFilesOnDisc)
            std::thread(scanLoop).detach();
    }

    Style style()
    {
        return current();
    }

    bool modern()
    {
        return current() != Style::Classic;
    }

    void setStyle(Style style)
    {
        if (static_cast<Style>(g_style.exchange(static_cast<int>(style))) != style)
            KM_INFO(Game, "menu buttons: " << styleName(style) << " (F2); the save menu's own wording follows at the next start");
    }

    const std::vector<TextPatch> &textPatches()
    {
        // Offsets in core_usa.bin (US), found by their icon codes (Oct 2026); each patch keeps its length. The icons
        // name the buttons that work the menu: the game's "triangle" (cancel) is now the circle, its two confirms the
        // cross alone.
        static const std::vector<TextPatch> patches = {
            // The roulette's bar ("[R1]Select [cross][circle]OK [triangle]Cancel", 0x591CC): the roulette follows the
            // menus too (kamite.toml "menu_roulette": its OK, func_00145810 0x145CE8, reads the cross only; its cancel,
            // Gene's +0x626 window filled at 0x108FA0, comes from the circle), so the bar reads "[R1]Cycle [cross]OK
            // [circle]Cancel" (roulette_bar.cpp lays it out at full letter width on a bar cut to fit).
            {0x591CEu, text({}, "Select"), [] { auto t = text({}, "Cycle"); t.push_back(0xB301u); return t; }()},
            {0x591DCu, text({kCross, kCircle}, "OK"), text({kCross, 0xB301u}, "OK")},
            {0x591E6u, {kTriangle}, {kCircle}},
            // The Techniques screen's "[triangle] Exit" (0x59224: colour code 0xB602 with its colour, the icon, 0xB301
            // back to white): exiting is the menus' Cancel, now the circle, in the circle's own colour (1, as the
            // "[circle] Default" hint has it).
            {0x59224u, {0xB602u, 2u, kTriangle, 0xB301u}, {0xB602u, 1u, kCircle, 0xB301u}},
            // "[cross][circle]Change" and "[cross][circle]Select": the circle becomes a colour-restore code (no width:
            // the word follows the cross as closely as "Exit" follows its circle).
            {0x591F8u, text({kCross, kCircle}, "Change"), text({kCross, 0xB301u}, "Change")},
            {0x59238u, text({kCross, kCircle}, "Select"), text({kCross, 0xB301u}, "Select")},
            // The dialog choices "[circle] Of course / Yes" and "[triangle] Leave me alone / No": cross and circle.
            {0x577A8u, {kCircle, 0xFFFFu, 0xFFFFu}, {kCross, 0xFFFFu, 0xFFFFu}},
            {0x577C8u, {kTriangle, 0xFFFFu, 0xFFFFu}, {kCircle, 0xFFFFu, 0xFFFFu}},
            {0x57820u, {kCircle, 0xFFFFu, 0xFFFFu}, {kCross, 0xFFFFu, 0xFFFFu}},
            {0x57830u, {kTriangle, 0xFFFFu, 0xFFFFu}, {kCircle, 0xFFFFu, 0xFFFFu}},
            {0x57866u, {kCircle, 0xFFFFu, 0xFFFFu}, {kCross, 0xFFFFu, 0xFFFFu}},
            {0x57876u, {kTriangle, 0xFFFFu, 0xFFFFu}, {kCircle, 0xFFFFu, 0xFFFFu}},
        };
        return patches;
    }

    const std::vector<TextRewrite> &textRewrites()
    {
        // id/local/us/idsavecore_us.idd (the save and load menus; the idsubscr*_us.idd files carry the same bank):
        // "[cross] [circle] : Confirm" and "     [triangle] : Cancel" (0x35 is that font's space, 0x31 its colon; the
        // five spaces put the triangle under the circle, both lines start at the same x). Now "[cross]Confirm" and
        // "[circle]Cancel", the buttons one above the other and the words right after them (K, Oct 25); the
        // lengths are kept with spaces at the end.
        static const std::vector<TextRewrite> rewrites = {
            {"save menu Confirm",
             cat({{kCross, kSpace2, kCircle, kSpace2, kColon, kSpace2, kSpace2}, text({}, "Confirm")}),
             cat({{kCross}, text({}, "Confirm"), {kSpace2, kSpace2, kSpace2, kSpace2, kSpace2, kSpace2}})},
            {"save menu Cancel",
             cat({{kSpace2, kSpace2, kSpace2, kSpace2, kSpace2, kTriangle, kSpace2, kColon, kSpace2, kSpace2}, text({}, "Cancel")}),
             cat({{kCircle}, text({}, "Cancel"), {kSpace2, kSpace2, kSpace2, kSpace2, kSpace2, kSpace2, kSpace2, kSpace2, kSpace2}})},
        };
        return rewrites;
    }
}
