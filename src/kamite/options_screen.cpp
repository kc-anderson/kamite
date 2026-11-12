// The OPTIONS screen of God Hand: Rearmed. See options_screen.h; the plan and the game's side: claude/rearmed-options.md.
//
// The game's two Options screens are cIDBase layouts with a sub-screen object around them (vtables 0x41FB20 pause,
// 0x41FBE8 title; 8-byte entries {delta, pfn}): entry 2 opens (loads id/local/us/idsubscroption[t]_us.idd: the file
// at +0x50, mode +0x5A = 0, result +0x60 = 4 / 5, the layout's entries by id at +0x80 + 4 x id, the cursor row at
// +0x130 / +0x110), and every frame the sub-screen manager (0x160474) calls entry 5 (move: the pad) then entry 4
// (update: shows / hides entries, cIDBase_move) and reads +0x60: 0xA = back to the menu before, -1 = close the pause
// (Start), 0xB = reset the game, 0xD = load a game; anything else = stay. Entry 6 draws (cIDBase_trans). Modes: 0 the
// rows, 1 / 2 / 3 the controller page opening / up / closing (its own layout at +0xE0 / +0xC0), 4 (pause) the
// reset Yes / No (+0x131: 0 Yes, 1 No; the help entries show message 0x2008).
//
// We replace entries 2 (wrapped: our state starts), 4, 5 and 6 of both. In mode 0 they are ours: the pad (the menus'
// command word at 0x7474A0: +0x1A0 pressed, +0x1B0 repeating; bit 28 OK, 29 Cancel, 21 Start, 24 / 25 up / down and
// 56 / 57 the left stick's; menu_buttons sets which buttons OK and Cancel are), the game's menu sounds (0x15E OK,
// 0x15F move, 0x161 back), every entry hidden but the backdrop and the cursor, and our text drawn after the layout
// through the game's text drawer (the font object 0x5E7510, set up as cIDBase_transText does, its body pointed at our
// strings in guest scratch 0xFC000..0xFE000). Other modes run the game's own functions (the controller page; the
// reset Yes / No, drawn over our list).
//
// Aspect ratio: while our list is up the screen is the port's own (hud::claimOwnScreen): nothing is narrowed or
// pillarboxed. The backdrop quad is stretched over the whole picture (the wall image is made for the aspect ratio);
// everything else is placed here in "4:3 units" of a text area as wide as the picture (at most 21:9, centred), whose
// left edge is at game x L, one unit = k game units (k = (4/3) / aspect): game x = L + x * k, glyphs and sprites k
// times as wide. The game's own entries still shown (the cursor, the reset Yes / No) are moved that way while drawn.
//
// The wall: the backdrop is texture 1 of the layout (a 512x512 PSMT8 picture uploaded as CT32 64x1024 from the file in
// RAM every frame; rows 0..447 the screen, 448..511 a strip of sprites: the bold row names, the cursor). A thread made
// at start reads the disc's file, maps the wall (cropped to the aspect ratio) onto the texture's own palette for
// rows 0..447, keeps the strip, and writes the result into the file in RAM while the screen is up; for that texture's
// name it writes the full image (the wall at 6x, the strip from the texture pack's image of the original texture when
// there is one, else the disc's scaled up) into cache/hd-options (kept between runs) and hands it to the GS.
#include "kamite/options_screen.h"

#include "kamite/disc_files.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/hd_textures.h"
#include "kamite/hud_layout.h"
#include "kamite/log.h"
#include "kamite/widescreen.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_pcsx2_backend.h"

// raylib's image functions (CPU only): the wall's JPEG, cropping and resampling. Its ::Image is not hd_textures::Image.
#include "raylib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kamite::options_screen
{
    namespace
    {
        namespace fs = std::filesystem;

        // ---- the game ------------------------------------------------------------------------------------------
        constexpr uint32_t kFont = game::MessDrawFont::kInstance;
        constexpr uint32_t kFontFace = 0x2AB9A8u;  // (unused, entry +0x8B): the font face and colour set for that code
        constexpr uint32_t kFontDraw = 0x2AF728u;  // (font, 0, 0): lays out and queues the body
        constexpr uint32_t kIdTrans = 0x2AAC28u;   // cIDBase_trans(layout)
        constexpr uint32_t kIdMove = 0x2AAA40u;    // cIDBase_move(layout)
        constexpr uint32_t kSePlay = 0x2CB8A0u;    // (0x5FEE00, 0, id, -1, -1, 0, 0): the menus' sound calls
        constexpr uint32_t kSeManager = 0x5FEE00u;
        constexpr uint16_t kSeOk = 0x15Eu, kSeMove = 0x15Fu, kSeBack = 0x161u;
        constexpr uint32_t kCommand = game::GameWork::address(game::GameWork::kTrg);
        constexpr uint32_t kCommandRepeat = game::GameWork::address(game::GameWork::kRep);
        constexpr uint64_t kOk = 1ull << 28, kCancel = 1ull << 29, kStart = 1ull << 21;
        constexpr uint64_t kUp = (1ull << 24) | (1ull << 56), kDown = (1ull << 25) | (1ull << 57);
        constexpr uint32_t kNoDraw = game::kIdentFlagNoDraw, kActive = 0x80000000u;
        constexpr int32_t kResultBack = 0xA, kResultClose = -1, kResultReset = 0xB, kResultLoad = 0xD;

        // Sub-screen object fields (both screens).
        constexpr uint32_t kFile = 0x50u, kMode = 0x5Au, kResult = 0x60u, kIds = 0x80u;
        constexpr uint32_t kLayoutEnt = game::IDBase::kEnt, kLayoutEntNum = game::IDBase::kEntNum, kLayoutMode = 0x46u;
        namespace ent = game::IDBaseEnt;
        constexpr uint32_t kEntPosX = 0x38u, kEntPosY = 0x3Cu, kEntColour = 0x64u, kEntList = 0x8Du, kEntZ = 0x8Eu;

        enum class Action : uint8_t
        {
            Controls,
            Display,
            Interface,
            Gameplay,
            Sound,
            LoadGame,
            ResetGame,
            Restore,
            Exit,
        };

        struct Row
        {
            Action action;
            const char *name;
            const char *help; // two lines at most ('\n')
        };

        // Help lines: drafts for K's one editing pass (claude/rearmed-options.md).
        constexpr Row kControlsRow{Action::Controls, "CONTROLS", "Display controls"};
        constexpr Row kDisplayRow{Action::Display, "DISPLAY", "Picture and camera."};
        constexpr Row kInterfaceRow{Action::Interface, "INTERFACE", "The HUD, button prompts and gauges."};
        constexpr Row kGameplayRow{Action::Gameplay, "GAMEPLAY", "Techniques, enemies and new\nways to play."};
        constexpr Row kSoundRow{Action::Sound, "SOUND", "Sound mode and volume levels."};
        constexpr Row kLoadRow{Action::LoadGame, "LOAD GAME", "Select a save file and\nrestart the game."};
        constexpr Row kResetRow{Action::ResetGame, "RESET GAME", "Return to title screen"};
        constexpr Row kRestoreRow{Action::Restore, "RESTORE DEFAULTS", "Set every option back to\nits default."};
        constexpr Row kExitRow{Action::Exit, "EXIT", ""};

        constexpr std::array<Row, 9> kPauseRows{kControlsRow, kDisplayRow, kInterfaceRow, kGameplayRow, kSoundRow,
                                                kLoadRow,     kResetRow,   kRestoreRow,   kExitRow};
        constexpr std::array<Row, 7> kTitleRows{kControlsRow, kDisplayRow, kInterfaceRow, kGameplayRow, kSoundRow, kRestoreRow, kExitRow};

        struct Kind
        {
            const char *name;
            const char *file; // the layout on the disc
            uint32_t open, update, move, draw; // vtable entries 2, 4, 5, 6
            uint32_t cursorRow, choice;        // the game's cursor row (s8) and Yes / No (s8) fields
            uint32_t page;                     // the controller page's layout
            int ids;                           // entries by id at +0x80
            int cursorId, helpId, hideUpTo;    // ids: the cursor, the help text; rows, values and cursor are 0..hideUpTo
            int gameControlsRow;               // the game's row of CONTROLS
            const Row *rows;
            int rowCount;
            float rowStep;
        };
        const Kind kKinds[2] = {
            {"pause", "idsubscroption_us.idd", 0x166638u, 0x166780u, 0x166BB8u, 0x166B60u, 0x130u, 0x131u, 0xE0u, 24, 15, 16, 15, 0,
             kPauseRows.data(), static_cast<int>(kPauseRows.size()), 28.0f},
            {"title", "idsubscroptiont_us.idd", 0x1673D0u, 0x167518u, 0x167908u, 0x1678B8u, 0x110u, 0x111u, 0xC0u, 16, 13, 14, 13, 0,
             kTitleRows.data(), static_cast<int>(kTitleRows.size()), 34.0f},
        };
        enum
        {
            kPause = 0,
            kTitle = 1,
        };

        // ---- guest memory --------------------------------------------------------------------------------------
        uint8_t *g_ram = nullptr;
        uint8_t *at(uint32_t address) { return g_ram + (address & 0x01FFFFFFu); }
        uint32_t rd32(uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, at(address), sizeof(v));
            return v;
        }
        uint64_t rd64(uint32_t address)
        {
            uint64_t v;
            std::memcpy(&v, at(address), sizeof(v));
            return v;
        }
        float rdF(uint32_t address)
        {
            float v;
            std::memcpy(&v, at(address), sizeof(v));
            return v;
        }
        void wr32(uint32_t address, uint32_t v) { std::memcpy(at(address), &v, sizeof(v)); }
        void wrF(uint32_t address, float v) { std::memcpy(at(address), &v, sizeof(v)); }
        bool valid(uint32_t address, uint32_t bytes = 4u) { return address >= 0x100000u && address + bytes <= 0x2000000u; }

        // A guest function run to its end on a copy of the registers (game_options' callGuest).
        bool callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address, std::initializer_list<uint32_t> args)
        {
            if (!runtime || !runtime->hasFunction(address))
                return false;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return false;
            const R5900Context saved = *ctx;
            int reg = 4;
            for (uint32_t a : args)
            {
                if (reg > 11)
                    break;
                SET_GPR_U32(ctx, reg, a);
                ++reg;
            }
            ctx->pc = address;
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            *ctx = saved;
            return true;
        }

        void sound(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint16_t id)
        {
            callGuest(rdram, ctx, runtime, kSePlay, {kSeManager, 0u, id, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u});
        }

        uint32_t idEntry(uint32_t self, int id) { return rd32(self + kIds + 4u * static_cast<uint32_t>(id)); }
        void setHidden(uint32_t entry, bool hidden)
        {
            if (!valid(entry, ent::kSize))
                return;
            const uint32_t flags = rd32(entry + ent::kFlags);
            wr32(entry + ent::kFlags, hidden ? (flags | kNoDraw) : (flags & ~kNoDraw));
        }

        // ---- the wall (prepared by a thread at start) ----------------------------------------------------------
        constexpr int kTexW = 512, kTexH = 512, kWallH = 448, kCtW = 64, kCtH = 1024;
        constexpr size_t kPayloadBytes = static_cast<size_t>(kCtW) * kCtH * 4u, kClutBytes = 1024u;
        constexpr int kHdScale = 6; // the full image: 3072x3072
        constexpr uint32_t kSampleAt[3] = {0x0u, 0x20000u, 0x3F000u};
        constexpr uint32_t kSampleBytes = 64u;

        struct Wall
        {
            std::atomic<bool> ready{false};
            uint32_t payloadOffset = 0; // in the file
            std::vector<uint8_t> original, ours; // the payload (CT32 words)
            bool warned = false;
        };
        Wall g_wall[2];

        bool sampleMatches(const uint8_t *ram, const std::vector<uint8_t> &payload)
        {
            for (uint32_t s : kSampleAt)
                if (std::memcmp(ram + s, payload.data() + s, kSampleBytes) != 0)
                    return false;
            return true;
        }

        // Game thread: the stand-in into the loaded file (the game uploads the texture from there every frame).
        void patchWall(int kind, uint32_t self)
        {
            Wall &w = g_wall[kind];
            if (!w.ready.load(std::memory_order_acquire))
                return;
            const uint32_t file = rd32(self + kFile);
            const uint32_t payload = file + w.payloadOffset;
            if (!valid(file) || !valid(payload, static_cast<uint32_t>(kPayloadBytes)))
                return;
            uint8_t *ram = at(payload);
            if (sampleMatches(ram, w.ours))
                return;
            if (!sampleMatches(ram, w.original))
            {
                if (!w.warned)
                    KM_WARN(Game, "options screen: the " << kKinds[kind].name << " backdrop is not where expected in RAM; the game's art stays");
                w.warned = true;
                return;
            }
            std::memcpy(ram, w.ours.data(), kPayloadBytes);
        }

        // The texture's place in the layout file: the file is a TBL (u32 count, u32 offsets, 4-byte tags) of TBLs; the
        // second holds the textures, the first of them (T32) the backdrop: a GIF packet with the 64x1024 CT32 image at
        // +0xE0 and its CLUT 0x100 bytes after it.
        bool findTexture(const std::vector<uint8_t> &file, uint32_t &header)
        {
            auto u32 = [&](size_t o) -> uint32_t {
                if (o + 4u > file.size())
                    return 0u;
                uint32_t v;
                std::memcpy(&v, file.data() + o, 4u);
                return v;
            };
            if (u32(0) < 2u)
                return false;
            const uint32_t textures = u32(8);
            if (u32(textures) < 1u)
                return false;
            header = textures + u32(textures + 4u);
            return u32(header + 4u) == 0x04000040u && header + 0xE0u + kPayloadBytes + 0x100u + kClutBytes <= file.size();
        }

        ::Image toRaylib(const hd_textures::Image &image)
        {
            ::Image out{};
            out.width = image.width;
            out.height = image.height;
            out.mipmaps = 1;
            out.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            out.data = MemAlloc(static_cast<unsigned int>(image.rgba.size()));
            std::memcpy(out.data, image.rgba.data(), image.rgba.size());
            return out;
        }

        // The texture's strip (rows 448..511) at the full image's scale: the pack's image of the original texture when
        // it is a PNG, else the disc's texels scaled up.
        ::Image stripImage(const std::string &originalName, const uint8_t *indices, const uint8_t *clut)
        {
            const fs::path pack = hd_textures::packFile(originalName);
            hd_textures::Image packed;
            if (!pack.empty() && pack.extension() == ".png" && hd_textures::loadPng(pack, packed) && packed.width >= kTexW &&
                packed.height >= kTexH)
            {
                ::Image image = toRaylib(packed);
                const int top = packed.height * kWallH / kTexH;
                ImageCrop(&image, Rectangle{0.0f, static_cast<float>(top), static_cast<float>(packed.width), static_cast<float>(packed.height - top)});
                ImageResize(&image, kTexW * kHdScale, (kTexH - kWallH) * kHdScale);
                return image;
            }
            hd_textures::Image strip;
            strip.width = kTexW;
            strip.height = kTexH - kWallH;
            strip.rgba.resize(static_cast<size_t>(strip.width) * strip.height * 4u);
            for (int i = 0; i < strip.width * strip.height; ++i)
            {
                const int k = indices[static_cast<size_t>(kWallH) * kTexW + i];
                const int slot = (k & 0xE7) | ((k & 0x08) << 1) | ((k & 0x10) >> 1); // CSM1
                std::memcpy(&strip.rgba[static_cast<size_t>(i) * 4u], clut + slot * 4, 4u);
            }
            ::Image image = toRaylib(strip);
            ImageResize(&image, kTexW * kHdScale, (kTexH - kWallH) * kHdScale);
            return image;
        }

        void prepareKind(int kind, const ::Image &standIn, const ::Image &full, const fs::path &cacheDir, std::vector<std::string> &keep)
        {
            const Kind &k = kKinds[kind];
            const auto found = disc_files::find(k.file);
            std::vector<uint8_t> file;
            uint32_t header = 0;
            if (!found || !disc_files::read(*found, file) || !findTexture(file, header))
            {
                KM_WARN(Game, "options screen: " << k.file << " not readable; the " << k.name << " screen keeps the game's backdrop");
                return;
            }
            const uint8_t *payload = file.data() + header + 0xE0u;
            const uint8_t *clut = payload + kPayloadBytes + 0x100u;
            std::vector<uint8_t> indices(static_cast<size_t>(kTexW) * kTexH);
            hd_textures::unpack8(payload, kCtW, kCtH, kTexW, kTexH, indices.data());

            // The wall's texels: each pixel of the stand-in -> the nearest colour the original's screen part uses.
            bool used[256] = {};
            for (size_t i = 0; i < static_cast<size_t>(kTexW) * kWallH; ++i)
                used[indices[i]] = true;
            int rgb[256][3];
            for (int i = 0; i < 256; ++i)
            {
                const int slot = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
                for (int c = 0; c < 3; ++c)
                    rgb[i][c] = clut[slot * 4 + c];
            }
            std::vector<uint8_t> nearest(32u * 32u * 32u);
            for (int r = 0; r < 32; ++r)
                for (int g = 0; g < 32; ++g)
                    for (int b = 0; b < 32; ++b)
                    {
                        const int cr = r * 8 + 4, cg = g * 8 + 4, cb = b * 8 + 4;
                        int best = 0, bestD = 1 << 30;
                        for (int i = 0; i < 256; ++i)
                        {
                            if (!used[i])
                                continue;
                            const int dr = cr - rgb[i][0], dg = cg - rgb[i][1], db = cb - rgb[i][2];
                            const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                            if (d < bestD)
                                bestD = d, best = i;
                        }
                        nearest[static_cast<size_t>((r << 10) | (g << 5) | b)] = static_cast<uint8_t>(best);
                    }
            std::vector<uint8_t> ours = indices;
            const auto *px = static_cast<const uint8_t *>(standIn.data);
            for (int i = 0; i < kTexW * kWallH; ++i)
            {
                const uint8_t *p = px + static_cast<size_t>(i) * 4u;
                ours[static_cast<size_t>(i)] = nearest[static_cast<size_t>(((p[0] >> 3) << 10) | ((p[1] >> 3) << 5) | (p[2] >> 3))];
            }
            Wall &w = g_wall[kind];
            w.original.assign(payload, payload + kPayloadBytes);
            w.ours.assign(kPayloadBytes, 0u);
            hd_textures::pack8(ours.data(), kTexW, kTexH, kCtW, kCtH, w.ours.data());
            w.payloadOffset = header + 0xE0u;
            const std::string originalName = hd_textures::name8(payload, kCtW, kCtH, clut, kTexW, kTexH);
            const std::string name = hd_textures::name8(w.ours.data(), kCtW, kCtH, clut, kTexW, kTexH);
            w.ready.store(true, std::memory_order_release);

            // The full image (kept between runs: its name follows the wall, the aspect ratio and the disc).
            const fs::path path = cacheDir / (name + ".png");
            keep.push_back(name + ".png");
            std::error_code ec;
            if (!fs::exists(path, ec))
            {
                const auto t0 = std::chrono::steady_clock::now();
                ::Image strip = stripImage(originalName, indices.data(), clut);
                hd_textures::Image image;
                image.width = kTexW * kHdScale;
                image.height = kTexH * kHdScale;
                image.rgba.assign(static_cast<size_t>(image.width) * image.height * 4u, 0u);
                const size_t wallBytes = static_cast<size_t>(image.width) * kWallH * kHdScale * 4u;
                std::memcpy(image.rgba.data(), full.data, wallBytes);
                for (size_t i = 3; i < wallBytes; i += 4)
                    image.rgba[i] = 0x80u; // opaque in the PS2's alpha scale
                std::memcpy(image.rgba.data() + wallBytes, strip.data, image.rgba.size() - wallBytes);
                UnloadImage(strip);
                if (!hd_textures::savePng(path, image))
                {
                    KM_WARN(Game, "options screen: could not write " << path.string() << "; the " << k.name << " wall stays at the PS2's resolution");
                    return;
                }
                KM_INFO(Game, "options screen: the " << k.name << " wall written ("
                                                     << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()
                                                     << " ms)");
            }
            const std::u8string native = path.u8string();
            for (int tries = 0; tries < 2400; ++tries) // the GS may not be up yet (10 minutes)
            {
                if (GSPcsx2Backend::SetOption("texture_add", std::string(native.begin(), native.end()).c_str()))
                {
                    KM_INFO(Game, "options screen: the " << k.name << " wall is " << name);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        void prepare(fs::path exeDir, float aspect)
        {
            const fs::path source = exeDir / "resources" / "options" / "wall.jpg";
            std::ifstream in(source, std::ios::binary);
            std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ::Image wall = bytes.empty() ? ::Image{} : LoadImageFromMemory(".jpg", bytes.data(), static_cast<int>(bytes.size()));
            if (!wall.data || wall.width < 16 || wall.height < 16)
            {
                KM_WARN(Game, "options screen: " << source.string() << " missing or unreadable; the game's backdrop stays");
                if (wall.data)
                    UnloadImage(wall);
                return;
            }
            ImageFormat(&wall, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            // The picture's shape out of the middle (the file is 32:9: K's 16:9 wall widened with its blank version);
            // wider than the file: all of it, stretched.
            const float width = std::min(static_cast<float>(wall.width), std::round(static_cast<float>(wall.height) * aspect));
            ImageCrop(&wall, Rectangle{std::floor(0.5f * (static_cast<float>(wall.width) - width)), 0.0f, width, static_cast<float>(wall.height)});
            ::Image standIn = ImageCopy(wall);
            ImageResize(&standIn, kTexW, kWallH);
            ImageResize(&wall, kTexW * kHdScale, kWallH * kHdScale);
            if (!disc_files::open())
            {
                KM_WARN(Game, "options screen: the disc's files are not readable; the game's backdrop stays");
                UnloadImage(standIn);
                UnloadImage(wall);
                return;
            }
            const fs::path cacheDir = exeDir / "cache" / "hd-options";
            std::error_code ec;
            fs::create_directories(cacheDir, ec);
            std::vector<std::string> keep;
            for (int kind = 0; kind < 2; ++kind)
                prepareKind(kind, standIn, wall, cacheDir, keep);
            UnloadImage(standIn);
            UnloadImage(wall);
            // Images of other aspect ratios or walls go.
            for (const auto &entry : fs::directory_iterator(cacheDir, ec))
                if (entry.is_regular_file(ec) && std::find(keep.begin(), keep.end(), entry.path().filename().string()) == keep.end())
                    fs::remove(entry.path(), ec);
        }

        // ---- layout --------------------------------------------------------------------------------------------
        float g_aspect = 4.0f / 3.0f;

        // The text area in 4:3 units (see the top of the file).
        struct Area
        {
            float k = 1.0f, left = 0.0f, width = 512.0f;
            float x(float units) const { return left + units * k; }
        };
        Area area()
        {
            Area a;
            a.k = (4.0f / 3.0f) / g_aspect;
            a.width = 512.0f * std::min(g_aspect, 21.0f / 9.0f) / (4.0f / 3.0f);
            a.left = 256.0f - 0.5f * a.width * a.k;
            return a;
        }

        constexpr float kRowX = 46.0f, kRowY = 96.0f, kRowScale = 0.9f;
        constexpr float kCursorX = 30.0f, kCursorDropY = 13.0f; // the cursor's centre against the row's top
        constexpr float kTitleY = 24.0f, kTitleScale = 1.5f;
        constexpr float kHelpX = 30.0f, kHelpY = 362.0f, kHelpScale = 0.85f;
        constexpr float kShadow = 1.2f;
        constexpr uint32_t kWhite = 0x808080u, kGrey = 0x505050u, kHelpColour = 0x707070u, kTitleColour = 0x104080u; // BGR
        constexpr uint32_t kBlack = 0x000000u;

        // ---- text ----------------------------------------------------------------------------------------------
        // Menu font codes (claude/tutorial-text.md); anything else is a space.
        constexpr uint16_t kSpace = 0x35u, kNewline = 0xA001u, kEnd = 0x8001u, kColour = 0xB203u;
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
            case '?': return 0x22u;
            case '!': return 0x23u;
            case '\'': return 0x27u;
            case ':': return 0x31u;
            case ',': return 0x7Eu;
            case '-': return 0x1Fu;
            case '/': return 0x2Du;
            case '\n': return kNewline;
            default: return kSpace;
            }
        }

        // Our strings, one slot per draw this frame.
        constexpr uint32_t kTextRam = 0x000FC000u, kTextSlot = 0x100u, kTextSlots = 32u;
        uint32_t g_textSlot = 0;

        struct Pen
        {
            uint32_t face = 0; // entry +0x8B of the game's help text
            uint32_t list = 0;
            float z = 0.0f;
            float fade = 1.0f; // the screen's own fade (its backdrop's alpha)
        };

        // One string through the game's text drawer, as cIDBase_transText draws a text entry.
        void drawText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Pen &pen, float x, float y, float scaleX,
                      float scaleY, bool centred, uint32_t bgr, const char *text)
        {
            if (g_textSlot >= kTextSlots || !text || !*text)
                return;
            const uint32_t address = kTextRam + kTextSlot * g_textSlot++;
            const uint32_t alpha = static_cast<uint32_t>(std::lround(std::clamp(pen.fade, 0.0f, 1.0f) * 128.0f));
            const uint32_t colour = (alpha << 24) | bgr;
            std::vector<uint16_t> codes{kColour, static_cast<uint16_t>(colour >> 16), static_cast<uint16_t>(colour & 0xFFFFu)};
            for (const char *c = text; *c && codes.size() + 2u < kTextSlot / 2u; ++c)
                codes.push_back(glyph(*c));
            codes.push_back(kEnd);
            std::memcpy(at(address), codes.data(), codes.size() * sizeof(uint16_t));

            callGuest(rdram, ctx, runtime, kFontFace, {0u, pen.face});
            wr32(kFont + 0x58u, pen.list); // draw list
            wr32(kFont + 0x5Cu, 5u);       // position in it (as every text)
            wrF(kFont + 0x60u, pen.z);
            wrF(kFont + game::MessDrawFont::kZoomX, scaleX);
            wrF(kFont + game::MessDrawFont::kZoomY, scaleY);
            wrF(kFont + game::MessDrawFont::kCharGapX, 2.0f);
            wrF(kFont + game::MessDrawFont::kCharGapY, -scaleY * 26.0f + 8.0f);
            wrF(kFont + 0x20u, x);
            wrF(kFont + 0x24u, y);
            wr32(kFont + 0x28u, centred ? 1u : 0u);
            wr32(kFont + 0x04u, address); // body
            wr32(kFont + 0x0Cu, address);
            wr32(kFont + 0x10u, 0u);
            wr32(kFont + 0x08u, address); // ruby (not drawn: flag 1 off below)
            wr32(kFont + 0x14u, address);
            wr32(kFont + 0x18u, 0u);
            wr32(kFont + 0x1Cu, 0x0FFFFFFFu);
            const uint32_t flags = rd32(kFont);
            wr32(kFont, flags & ~0x5u); // no ruby, proportional
            callGuest(rdram, ctx, runtime, kFontDraw, {kFont, 0u, 0u});
            wr32(kFont, flags);
        }

        // With a drop shadow (drawn first: inside one draw slot the last queued is drawn first, so it goes second).
        void drawLine(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Pen &pen, const Area &a, float xUnits, float y,
                      float scale, bool centred, uint32_t bgr, const char *text)
        {
            drawText(rdram, ctx, runtime, pen, a.x(xUnits), y, scale * a.k, scale, centred, bgr, text);
            drawText(rdram, ctx, runtime, pen, a.x(xUnits + kShadow), y + kShadow, scale * a.k, scale, centred, kBlack, text);
        }

        // ---- the screen ----------------------------------------------------------------------------------------
        struct Screen
        {
            uint32_t self = 0;
            int row = 0;
            uint32_t backdrop = 0; // its backdrop entry (left stretched over the picture)
        };
        Screen g_screen[2];
        GuestFunction g_original[2][4] = {}; // open, update, move, draw

        Screen &screenOf(int kind, uint32_t self)
        {
            Screen &s = g_screen[kind];
            if (s.self != self)
                s = Screen{self, 0, 0u};
            return s;
        }

        int8_t mode(uint32_t self) { return static_cast<int8_t>(*at(self + kMode)); }

        // The backdrop: the full-screen entry at the layout's centre (the last of its entries in both files).
        uint32_t findBackdrop(uint32_t self)
        {
            const uint32_t entries = rd32(self + kLayoutEnt);
            const int32_t count = static_cast<int32_t>(rd32(self + kLayoutEntNum));
            if (!valid(entries) || count <= 0 || count > 256)
                return 0u;
            for (int32_t i = count - 1; i >= 0; --i)
            {
                const uint32_t e = entries + ent::kSize * static_cast<uint32_t>(i);
                if (std::fabs(rdF(e + kEntPosX) - 255.0f) < 2.0f && std::fabs(rdF(e + kEntPosY) - 223.0f) < 2.0f)
                    return e;
            }
            return 0u;
        }

        void act(int kind, Screen &s, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Kind &k = kKinds[kind];
            const uint32_t self = s.self;
            switch (k.rows[s.row].action)
            {
            case Action::Controls: // the game's controller page (until ours exists)
                sound(rdram, ctx, runtime, kSeOk);
                *at(self + k.cursorRow) = static_cast<uint8_t>(k.gameControlsRow);
                *at(self + kMode) = 1u;
                break;
            case Action::LoadGame:
                sound(rdram, ctx, runtime, kSeOk);
                wr32(self + kResult, static_cast<uint32_t>(kResultLoad));
                break;
            case Action::ResetGame: // the game's own Yes / No (its "No" comes back to mode 0)
                sound(rdram, ctx, runtime, kSeOk);
                *at(self + k.choice) = 1u;
                *at(self + kMode) = 4u;
                break;
            case Action::Exit:
                sound(rdram, ctx, runtime, kSeOk);
                wr32(self + kResult, static_cast<uint32_t>(kResultBack));
                break;
            default: // the pages and RESTORE DEFAULTS: the next build
                break;
            }
        }

        // Entry 2 (open), wrapped: our list starts at its top.
        template <int N>
        void openHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            g_original[N][0](rdram, ctx, runtime);
            g_screen[N] = Screen{self, 0, 0u};
        }

        // Entry 5 (move): the pad.
        template <int N>
        void moveHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            if (mode(self) != 0)
                return g_original[N][2](rdram, ctx, runtime);
            const uint32_t ra = GPR_U32(ctx, 31);
            Screen &s = screenOf(N, self);
            const Kind &k = kKinds[N];
            const uint64_t pressed = rd64(kCommand), repeat = rd64(kCommandRepeat);
            if (pressed & kOk)
                act(N, s, rdram, ctx, runtime);
            else if ((repeat & kUp) && s.row > 0)
            {
                --s.row;
                sound(rdram, ctx, runtime, kSeMove);
            }
            else if ((repeat & kDown) && s.row + 1 < k.rowCount)
            {
                ++s.row;
                sound(rdram, ctx, runtime, kSeMove);
            }
            if ((pressed & (kOk | kCancel)) == kCancel)
            {
                sound(rdram, ctx, runtime, kSeBack);
                wr32(self + kResult, static_cast<uint32_t>(kResultBack));
            }
            else if ((pressed & (kOk | kStart)) == kStart)
            {
                sound(rdram, ctx, runtime, kSeBack);
                wr32(self + kResult, static_cast<uint32_t>(kResultClose));
            }
            ctx->pc = ra;
        }

        // Entry 4 (update): what is shown, then the layouts move (as the game's does at its end).
        template <int N>
        void updateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const Kind &k = kKinds[N];
            patchWall(N, self);
            if (mode(self) != 0)
            {
                g_original[N][1](rdram, ctx, runtime);
                // The game's own modes over our wall: its row names and values (and the cursor) would float on it.
                for (int id = 0; id <= k.hideUpTo; ++id)
                    setHidden(idEntry(self, id), true);
                return;
            }
            const uint32_t ra = GPR_U32(ctx, 31);
            Screen &s = screenOf(N, self);
            if (s.backdrop == 0u)
                s.backdrop = findBackdrop(self);
            for (int id = 0; id < k.ids; ++id)
                setHidden(idEntry(self, id), id != k.cursorId);
            callGuest(rdram, ctx, runtime, kIdMove, {self + k.page});
            callGuest(rdram, ctx, runtime, kIdMove, {self});
            ctx->pc = ra;
        }

        // Entry 6 (draw).
        template <int N>
        void drawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            const int8_t m = mode(self);
            if (m != 0 && !(N == kPause && m == 4))
                return g_original[N][3](rdram, ctx, runtime);
            const uint32_t ra = GPR_U32(ctx, 31);
            const Kind &k = kKinds[N];
            Screen &s = screenOf(N, self);
            if (s.backdrop == 0u)
                s.backdrop = findBackdrop(self);
            hud::claimOwnScreen();
            const Area a = area();

            // The game's entries still shown (the cursor; the reset Yes / No) placed in the text area while drawn.
            struct Saved
            {
                uint32_t entry;
                float x, y, sclX;
            };
            std::vector<Saved> saved;
            const uint32_t entries = rd32(self + kLayoutEnt);
            const int32_t count = static_cast<int32_t>(rd32(self + kLayoutEntNum));
            const uint32_t cursor = idEntry(self, k.cursorId);
            for (int32_t i = 0; valid(entries) && i < count && i < 256; ++i)
            {
                const uint32_t e = entries + ent::kSize * static_cast<uint32_t>(i);
                const uint32_t flags = rd32(e + ent::kFlags);
                if (e == s.backdrop || (flags & kNoDraw) || !(flags & kActive))
                    continue;
                saved.push_back({e, rdF(e + ent::kDrawX), rdF(e + ent::kDrawY), rdF(e + ent::kDrawSclX)});
                if (e == cursor)
                {
                    wrF(e + ent::kDrawX, a.x(kCursorX));
                    wrF(e + ent::kDrawY, kRowY + k.rowStep * static_cast<float>(s.row) + kCursorDropY);
                }
                else
                    wrF(e + ent::kDrawX, a.x(rdF(e + ent::kDrawX)));
                wrF(e + ent::kDrawSclX, rdF(e + ent::kDrawSclX) * a.k);
            }
            callGuest(rdram, ctx, runtime, kIdTrans, {self});
            for (const Saved &v : saved)
            {
                wrF(v.entry + ent::kDrawX, v.x);
                wrF(v.entry + ent::kDrawY, v.y);
                wrF(v.entry + ent::kDrawSclX, v.sclX);
            }

            // Our text: the help text entry's font, list and depth.
            const uint32_t help = idEntry(self, k.helpId);
            if (valid(help, ent::kSize))
            {
                Pen pen;
                pen.face = *at(help + ent::kColourSet);
                const uint8_t layoutMode = *at(self + kLayoutMode);
                pen.list = layoutMode == 6u ? *at(help + kEntList) : layoutMode;
                int16_t z;
                std::memcpy(&z, at(help + kEntZ), sizeof(z));
                pen.z = static_cast<float>(z);
                pen.fade = valid(s.backdrop, ent::kSize) ? static_cast<float>(*at(s.backdrop + kEntColour + 3u)) / 128.0f : 1.0f;
                g_textSlot = 0;
                drawLine(rdram, ctx, runtime, pen, a, 0.5f * a.width, kTitleY, kTitleScale, true, kTitleColour, "OPTIONS");
                for (int i = 0; i < k.rowCount; ++i)
                    drawLine(rdram, ctx, runtime, pen, a, kRowX, kRowY + k.rowStep * static_cast<float>(i), kRowScale, false,
                             i == s.row && m == 0 ? kWhite : kGrey, k.rows[i].name);
                if (m == 0) // the reset Yes / No has the help box
                    drawLine(rdram, ctx, runtime, pen, a, kHelpX, kHelpY, kHelpScale, false, kHelpColour, k.rows[s.row].help);
            }
            ctx->pc = ra;
        }

        bool hook(PS2Runtime &runtime, uint32_t address, const char *name, GuestFunction implementation, GuestFunction &original)
        {
            if (!runtime.hasFunction(address) || !(original = runtime.lookupFunction(address)))
                return false;
            return FunctionHooks::instance().replace(address, name, implementation, ReplacementStatus::Native, "the OPTIONS screen of God Hand: Rearmed");
        }

        template <int N>
        bool hookKind(PS2Runtime &runtime)
        {
            const Kind &k = kKinds[N];
            GuestFunction *o = g_original[N];
            return hook(runtime, k.open, N == kPause ? "cSubScrOption open (options screen)" : "cSubScrOptionT open (options screen)", &openHook<N>, o[0]) &&
                   hook(runtime, k.update, N == kPause ? "cSubScrOption update (options screen)" : "cSubScrOptionT update (options screen)", &updateHook<N>, o[1]) &&
                   hook(runtime, k.move, N == kPause ? "cSubScrOption move (options screen)" : "cSubScrOptionT move (options screen)", &moveHook<N>, o[2]) &&
                   hook(runtime, k.draw, N == kPause ? "cSubScrOption draw (options screen)" : "cSubScrOptionT draw (options screen)", &drawHook<N>, o[3]);
        }
    }

    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir)
    {
        g_ram = runtime.memory().getRDRAM();
        g_aspect = std::clamp(aspectRatio(), 1.0f, 4.0f);
        // The functions are hooked first: a half-hooked screen would mix the game's and ours.
        for (int kind = 0; kind < 2; ++kind)
            for (const uint32_t address : {kKinds[kind].open, kKinds[kind].update, kKinds[kind].move, kKinds[kind].draw})
                if (!runtime.hasFunction(address))
                {
                    KM_WARN(Game, "options screen: 0x" << std::hex << address << std::dec << " is not hookable; the game's own Options screens stay");
                    return;
                }
        if (!hookKind<kPause>(runtime) || !hookKind<kTitle>(runtime))
        {
            KM_WARN(Game, "options screen: hooking failed; the Options screens may be the game's");
            return;
        }
        std::thread(prepare, exeDir, g_aspect).detach();
        KM_INFO(Game, "options screen: the title and pause Options open the God Hand: Rearmed screen (aspect " << g_aspect << ")");
    }
}
