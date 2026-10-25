// Menu buttons: Classic (PS2) or Modern (south confirms, east cancels), menus only. See menu_buttons.h.
//
// The first version (aa44ccf) traded circle and triangle at the pad, which also traded two attack slots in play;
// the game's own split between the raw word (play) and the command word (menus) is what keeps this one to the menus.
#include "kamite/menu_buttons.h"
#include "kamite/disc_files.h"
#include "kamite/log.h"
#include "runtime/ps2_cd_image.h"
#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

namespace kamite::menu_buttons
{
    namespace
    {
        std::atomic<bool> g_modern{false};
        uint8_t *g_ram = nullptr;
        // Set at start when every patch of the kind went onto the disc image (the file as the game reads it): the
        // run-time rewrites below are then not needed (no RAM scan thread, no core check every 30 vblanks).
        bool g_coreOnDisc = false, g_menuFilesOnDisc = false;

        // The command tables (SLUS_215.03 .data): 64 raw masks, entry i = the raw bits that set command bit i. The raw
        // word is the pad's two button bytes high-to-low: 0x1000 up, 0x2000 right, 0x4000 down, 0x8000 left (+ select
        // 0x100, L3, R3, start 0x800), 0x01 L2, 0x02 R2, 0x04 L1, 0x08 R1, 0x10 triangle, 0x20 circle, 0x40 cross,
        // 0x80 square. Command bit 28 is the menus' "OK", bit 29 their "Cancel" (the decomp's matched menus test
        // +0x1A0 & 0x10000000 with the confirm sound 0x15E and & 0x20000000 with the cancel sound 0x161). The table the
        // game uses is 0x3C3E50 when the language word 0x747A0C is not 0 (the US release: OK = circle|cross, Cancel =
        // triangle), 0x3C3D50 otherwise (OK = circle, Cancel = cross, the Japanese way).
        constexpr uint32_t kTables[2] = {0x3C3D50u, 0x3C3E50u};
        constexpr uint32_t kOkEntry = 28u, kCancelEntry = 29u;
        constexpr uint32_t kDiscOk[2] = {0x20u, 0x60u}, kDiscCancel[2] = {0x40u, 0x10u};
        constexpr uint32_t kRawCircle = 0x20u, kRawCross = 0x40u;
        constexpr uint32_t kCorePointer = 0x7479F8u; // *(u32*) = where core_usa.bin was loaded
        constexpr uint32_t kRamBytes = 0x2000000u;

        // Glyph codes of the game's text (core_usa.bin, 16-bit).
        constexpr uint16_t kSpace = 0x10u, kSpace2 = 0x35u, kColon = 0x31u;
        constexpr uint16_t kCircle = 0u, kTriangle = 1u, kCross = 3u;
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

        // EE thread, every vblank: the OK and Cancel entries. Only the disc's pair or ours is touched (a mod that remaps
        // the buttons keeps its table).
        void applyTables()
        {
            const bool modern = g_modern.load(std::memory_order_relaxed);
            for (size_t i = 0; i < 2u; ++i)
            {
                const uint32_t table = kTables[i];
                const uint32_t wantOk = modern ? kRawCross : kDiscOk[i], wantCancel = modern ? kRawCircle : kDiscCancel[i];
                const uint32_t ok = rd32(table + kOkEntry * 4u), cancel = rd32(table + kCancelEntry * 4u);
                if (ok == wantOk && cancel == wantCancel)
                    continue;
                const bool disc = ok == kDiscOk[i] && cancel == kDiscCancel[i], ours = ok == kRawCross && cancel == kRawCircle;
                if (!disc && !ours)
                    continue;
                wr32(table + kOkEntry * 4u, wantOk);
                wr32(table + kCancelEntry * 4u, wantCancel);
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
            const bool modern = g_modern.load(std::memory_order_relaxed);
            static bool warned = false;
            for (const TextPatch &patch : textPatches())
            {
                uint8_t *guest = g_ram + core + patch.offset;
                const size_t bytes = patch.original.size() * 2u;
                if (core + patch.offset + bytes > kRamBytes)
                    continue;
                const std::vector<uint16_t> &want = modern ? patch.modern : patch.original;
                const std::vector<uint16_t> &other = modern ? patch.original : patch.modern;
                if (std::memcmp(guest, want.data(), bytes) == 0)
                    continue;
                if (std::memcmp(guest, other.data(), bytes) != 0)
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
                std::memcpy(guest, rw.modern.data(), bytes);
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
                    patches.emplace_back(core->imageOffset + patch.offset, bytesOf(patch.modern));
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
            for (const char *path : {"id/local/us/idsavecore_us.idd"})
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
                        patches.emplace_back(menu->imageOffset + at, bytesOf(rewrites[i].modern));
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

        void onVblank()
        {
            static uint32_t vblanks = 0u;
            ++vblanks;
            applyTables();
            if (vblanks % 30u == 0u)
                applyCoreText();
            if (!g_modern.load(std::memory_order_relaxed))
                return;
            if (!g_menuFilesOnDisc)
                applyFoundText();
        }
    }

    void install(uint8_t *rdram)
    {
        // Modern unless [Game] MenuButtons says classic (a testing option in F2 > Controls; it becomes the only way once
        // the whole game has been played with it).
        const char *value = std::getenv("KAMITE_MENU_BUTTONS");
        std::string text = value && *value ? value : "modern";
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        g_modern.store(!(text == "classic" || text == "0"), std::memory_order_relaxed);
        g_ram = rdram;
        KM_INFO(Game, "menu buttons: " << (g_modern.load() ? "modern (in menus the south button confirms and the east one cancels; play unchanged)"
                                                            : "classic (circle and cross confirm, triangle cancels, as on the PS2)"));
        if (!g_ram)
            return;
        if (g_modern.load())
            installDiscText();
        ps2x::pnach::addFrameHook(&onVblank);
        if (g_modern.load() && !g_menuFilesOnDisc)
            std::thread(scanLoop).detach();
    }

    bool modern()
    {
        return g_modern.load(std::memory_order_relaxed);
    }

    void setModern(bool modern)
    {
        if (g_modern.exchange(modern) != modern)
            KM_INFO(Game, "menu buttons: " << (modern ? "modern" : "classic") << " (F2); the save menu's own wording follows at the next start");
    }

    const std::vector<TextPatch> &textPatches()
    {
        // Offsets in core_usa.bin (US), found by their icon codes (Oct 2026); each patch keeps its length. The icons
        // name the buttons that work the menu: the game's "triangle" (cancel) is now the circle, its two confirms the
        // cross alone.
        static const std::vector<TextPatch> patches = {
            // The roulette's bar ("[R1]Select [cross][circle]OK [triangle]Cancel", 0x591CC) is left as it is: the roulette
            // is part of play (its OK reads the cross and circle bits 33/35 directly, func_00145810 at 0x145CE8, and its
            // cancel comes from the player side), so it keeps the game's buttons in both modes.
            // The Techniques screen's "[triangle] Exit" (0x59224: colour code 0xB602 with its colour, the icon, 0xB301
            // back to white): exiting is the menus' Cancel, now the circle, in the circle's own colour (1, as the
            // "[circle] Default" hint has it).
            {0x59224u, {0xB602u, 2u, kTriangle, 0xB301u}, {0xB602u, 1u, kCircle, 0xB301u}},
            // "[cross][circle]Change" and "[cross][circle]Select": the circle becomes a space.
            {0x591F8u, text({kCross, kCircle}, "Change"), text({kCross, kSpace}, "Change")},
            {0x59238u, text({kCross, kCircle}, "Select"), text({kCross, kSpace}, "Select")},
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
        // id/local/us/idsavecore_us.idd (the save and load menus): "[cross] [circle] : Confirm" and
        // "[triangle] : Cancel" (0x35 is that font's space, 0x31 its colon).
        static const std::vector<TextRewrite> rewrites = {
            {"save menu Confirm",
             cat({{kCross, kSpace2, kCircle, kSpace2, kColon, kSpace2, kSpace2}, text({}, "Confirm")}),
             cat({{kCross, kSpace2, kSpace2, kSpace2, kColon, kSpace2, kSpace2}, text({}, "Confirm")})},
            {"save menu Cancel",
             cat({{kSpace2, kTriangle, kSpace2, kColon, kSpace2, kSpace2}, text({}, "Cancel")}),
             cat({{kSpace2, kCircle, kSpace2, kColon, kSpace2, kSpace2}, text({}, "Cancel")})},
        };
        return rewrites;
    }
}
