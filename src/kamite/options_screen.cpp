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
// We replace entries 2 (wrapped: our state starts), 3 (wrapped: our text goes), 4, 5 and 6 of both. In mode 0 they
// are ours: the pad (the menus' command word at 0x7474A0: +0x1A0 pressed, +0x1B0 repeating; bit 28 OK, 29 Cancel, 21
// Start, then up / down / left / right (menuKey.h); menu_buttons sets which buttons OK and Cancel
// are), the game's menu sounds (0x15E OK, 0x15F move, 0x161 back) and every entry hidden but the backdrop. The game
// draws only the backdrop (K's wall); the title, the text, the cursor and the Yes / No are the renderer's overlay
// (options_overlay.cpp, pcsx2gs_set_screen), sent every frame (faded with the game's screen fade) and cleared when
// the screen closes.
//
// Aspect ratio: while our list is up the screen is the port's own (hud::claimOwnScreen): nothing is narrowed or
// pillarboxed. The backdrop quad is stretched over the whole picture (the wall image is made for the aspect ratio);
// the overlay places its text in a text area as wide as the picture (at most 21:9, centred).
//
// The wall: the backdrop is texture 1 of the layout (a 512x512 PSMT8 picture uploaded as CT32 64x1024 from the file in
// RAM every frame; rows 0..447 the screen, 448..511 a strip of sprites: the bold row names, the cursor). A thread made
// at start reads the disc's file, maps the wall (cropped to the aspect ratio) onto the texture's own palette for
// rows 0..447, keeps the strip, and writes the result into the file in RAM while the screen is up; for that texture's
// name it writes the full image (the wall at 6x, the strip from the texture pack's image of the original texture when
// there is one, else the disc's scaled up) into cache/hd-options (kept between runs) and hands it to the GS.
#include "kamite/options_screen.h"

#include "kamite/buck_breaker.h"
#include "kamite/button_prompts.h"
#include "kamite/camera.h"
#include "kamite/disc_files.h"
#include "kamite/frame_patches.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/hd_textures.h"
#include "kamite/game_options.h"
#include "kamite/hud_layout.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"
#include "kamite/menu_buttons.h"
#include "kamite/modern_controls.h"
#include "kamite/npc_poses.h"
#include "kamite/options_overlay.h"
#include "kamite/options_pages.h"
#include "kamite/prompt_hd.h"
#include "kamite/settings_table.h"
#include "kamite/sprint_traversal.h"
#include "kamite/style_presets.h"
#include "runtime/ps2_display_options.h"
#include "kamite/widescreen.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/ps2_game_menu.h"

// raylib's image functions (CPU only): the wall's JPEG, cropping and resampling. Its ::Image is not hd_textures::Image.
#include "raylib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
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
        // classFADE D_00747470, the screen fade the sub-screen manager runs between screens (col +0x10 -> col2 +0x14
        // over time +0x18 frames, count +0x1A, state +0x1C: bit 0 = drawn; to black holds, from black stops). Its
        // sprite is black at alpha 0x80 and above (the GS clamps).
        constexpr uint32_t kFade = 0x00747470u;
        constexpr uint32_t kIdTrans = 0x2AAC28u;   // cIDBase_trans(layout)
        constexpr uint32_t kIdMove = 0x2AAA40u;    // cIDBase_move(layout)
        constexpr uint32_t kSePlay = 0x2CB8A0u;    // (0x5FEE00, 0, id, -1, -1, 0, 0): the menus' sound calls
        constexpr uint32_t kSeManager = 0x5FEE00u;
        constexpr uint16_t kSeOk = 0x15Eu, kSeMove = 0x15Fu, kSeBack = 0x161u;
        constexpr uint32_t kCommand = game::GameWork::address(game::GameWork::kTrg);
        constexpr uint32_t kCommandRepeat = game::GameWork::address(game::GameWork::kRep);
        constexpr uint64_t kOk = 1ull << 28, kCancel = 1ull << 29, kStart = 1ull << 21;
        // The decomp's menuKey.h: one bit for the pad, one for the stick (left is bit 27, right 26).
        constexpr uint64_t kUp = 0x0001000001000000ull, kDown = 0x0002000002000000ull;
        constexpr uint64_t kLeft = 0x0004000008000000ull, kRight = 0x0008000004000000ull;
        constexpr uint32_t kNoDraw = game::kIdentFlagNoDraw;
        constexpr int32_t kResultBack = 0xA, kResultClose = -1, kResultReset = 0xB, kResultLoad = 0xD;

        // Sub-screen object fields (both screens).
        constexpr uint32_t kFile = 0x50u, kMode = 0x5Au, kResult = 0x60u, kIds = 0x80u;
        constexpr uint32_t kLayoutEnt = game::IDBase::kEnt, kLayoutEntNum = game::IDBase::kEntNum;
        namespace ent = game::IDBaseEnt;
        constexpr uint32_t kEntPosX = 0x38u, kEntPosY = 0x3Cu, kEntColour = 0x64u;

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

        using Text = options_overlay::Row;

        // Help lines: drafts for K's one editing pass (claude/rearmed-options.md).
        constexpr Text kControlsText{"CONTROLS", "Display controls"};
        constexpr Text kDisplayText{"DISPLAY", "Picture and camera."};
        constexpr Text kInterfaceText{"INTERFACE", "The HUD, button prompts and gauges."};
        constexpr Text kGameplayText{"GAMEPLAY", "Techniques, enemies and new\nways to play."};
        constexpr Text kSoundText{"SOUND", "Sound mode and volume levels."};
        constexpr Text kLoadText{"LOAD GAME", "Select a save file and\nrestart the game."};
        constexpr Text kResetText{"RESET GAME", "Return to title screen"};
        constexpr Text kRestoreText{"RESTORE DEFAULTS", "Set every option back to\nits default."};
        constexpr Text kExitText{"EXIT", ""};
        constexpr const char *kResetQuestion = "Would you really like to reset?"; // the game's own (message 0x2008)
        constexpr const char *kRestoreQuestion = "Set every option back to its default?";

        constexpr std::array<Text, 9> kPauseText{kControlsText, kDisplayText, kInterfaceText, kGameplayText, kSoundText,
                                                 kLoadText,     kResetText,   kRestoreText,   kExitText};
        constexpr std::array<Action, 9> kPauseActions{Action::Controls, Action::Display,   Action::Interface,
                                                      Action::Gameplay, Action::Sound,     Action::LoadGame,
                                                      Action::ResetGame, Action::Restore, Action::Exit};
        constexpr std::array<Text, 7> kTitleText{kControlsText, kDisplayText, kInterfaceText, kGameplayText, kSoundText, kRestoreText, kExitText};
        constexpr std::array<Action, 7> kTitleActions{Action::Controls, Action::Display, Action::Interface, Action::Gameplay,
                                                      Action::Sound,    Action::Restore, Action::Exit};

        struct Kind
        {
            const char *name;
            const char *file; // the layout on the disc
            uint32_t open, close, update, move, draw; // vtable entries 2, 3, 4, 5, 6
            uint32_t page;                            // the controller page's layout
            int ids;                                  // entries by id at +0x80
            int hideUpTo;                             // ids 0..hideUpTo: rows, values and the cursor
            const Text *text;
            const Action *actions;
            int rowCount;
        };
        const Kind kKinds[2] = {
            {"pause", "idsubscroption_us.idd", 0x166638u, 0x166730u, 0x166780u, 0x166BB8u, 0x166B60u, 0xE0u, 24, 15,
             kPauseText.data(), kPauseActions.data(), static_cast<int>(kPauseText.size())},
            {"title", "idsubscroptiont_us.idd", 0x1673D0u, 0x1674C8u, 0x167518u, 0x167908u, 0x1678B8u, 0xC0u, 16, 13,
             kTitleText.data(), kTitleActions.data(), static_cast<int>(kTitleText.size())},
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
            std::mutex lock;            // prepare (a thread) against patchWall (the game thread)
            uint32_t payloadOffset = 0; // in the file
            std::vector<uint8_t> original, ours; // the payload (CT32 words)
            std::vector<uint8_t> previous;       // the last shape's (the aspect ratio changed while playing)
            std::atomic<uint32_t> published{0u}; // the prepare generation now shown
            // A new shape made while playing waits here until the screen may swap (not while it fades to black).
            std::vector<uint8_t> staged, stagedOriginal;
            uint32_t stagedOffset = 0;
            std::atomic<uint32_t> stagedGeneration{0u};
            bool warned = false;
        };
        Wall g_wall[2];
        std::atomic<uint32_t> g_wallGeneration{0u}; // a newer prepare (the aspect ratio changed again) wins
        std::mutex g_prepareMutex;                  // one prepare at a time
        fs::path g_exeDir;

        bool wallSwapAllowed(); // not while a change's fade is going to black (the screen stays as it is)

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
            std::lock_guard<std::mutex> lock(w.lock);
            if (const uint32_t staged = w.stagedGeneration.load(std::memory_order_acquire);
                staged > w.published.load(std::memory_order_relaxed) && wallSwapAllowed())
            {
                w.previous = std::move(w.ours);
                w.ours = std::move(w.staged);
                w.original = std::move(w.stagedOriginal);
                w.payloadOffset = w.stagedOffset;
                w.published.store(staged, std::memory_order_release);
            }
            const uint32_t file = rd32(self + kFile);
            const uint32_t payload = file + w.payloadOffset;
            if (!valid(file) || !valid(payload, static_cast<uint32_t>(kPayloadBytes)))
                return;
            uint8_t *ram = at(payload);
            if (sampleMatches(ram, w.ours))
                return;
            if (!sampleMatches(ram, w.original) && (w.previous.empty() || !sampleMatches(ram, w.previous)))
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

        void prepareKind(int kind, const ::Image &standIn, const ::Image &full, const fs::path &cacheDir, std::vector<std::string> &keep,
                         uint32_t generation, bool live)
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
            std::vector<uint8_t> packed(kPayloadBytes, 0u);
            hd_textures::pack8(ours.data(), kTexW, kTexH, kCtW, kCtH, packed.data());
            const std::string originalName = hd_textures::name8(payload, kCtW, kCtH, clut, kTexW, kTexH);
            const std::string name = hd_textures::name8(packed.data(), kCtW, kCtH, clut, kTexW, kTexH);
            if (generation != g_wallGeneration.load(std::memory_order_relaxed))
                return; // the aspect ratio changed again meanwhile
            const auto publish = [&] {
                {
                    std::lock_guard<std::mutex> lock(w.lock);
                    if (w.ready.load(std::memory_order_relaxed))
                        w.previous = std::move(w.ours);
                    w.original.assign(payload, payload + kPayloadBytes);
                    w.ours = std::move(packed);
                    w.payloadOffset = header + 0xE0u;
                }
                w.ready.store(true, std::memory_order_release);
                w.published.store(generation, std::memory_order_release);
            };
            // While playing (the aspect ratio changed) the new shape is staged once its full image is with the GS (it
            // caches a texture's replacement the first time it sees it: a stand-in shown first stayed low-res); the
            // screen swaps it in when it may (patchWall: behind the black of the change's fade).
            const auto stage = [&] {
                {
                    std::lock_guard<std::mutex> lock(w.lock);
                    w.staged = std::move(packed);
                    w.stagedOriginal.assign(payload, payload + kPayloadBytes);
                    w.stagedOffset = header + 0xE0u;
                }
                w.stagedGeneration.store(generation, std::memory_order_release);
            };
            // At start the stand-in goes up at once (the screen is not open yet; the full image follows).
            if (!live)
                publish();

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
                    if (live && generation == g_wallGeneration.load(std::memory_order_relaxed))
                        stage();
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
                    if (live)
                    {
                        // The DLL takes the file at its next present: a few frames before the new stand-in goes up.
                        std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        if (generation == g_wallGeneration.load(std::memory_order_relaxed))
                            stage();
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        }

        // live: the aspect ratio changed while playing (generation > 0). The cache keeps every shape until the next
        // start: the GS has the earlier shapes' images on its list and loads them by path (a removed one crashed it).
        void prepare(fs::path exeDir, float aspect, uint32_t generation, int first)
        {
            const bool live = generation != 0u;
            std::lock_guard<std::mutex> one(g_prepareMutex);
            if (generation != g_wallGeneration.load(std::memory_order_relaxed))
                return; // a newer shape is waiting
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
            for (const int kind : {first, 1 - first}) // the screen that is up first (the fade waits for it)
                prepareKind(kind, standIn, wall, cacheDir, keep, generation, live);
            UnloadImage(standIn);
            UnloadImage(wall);
            // Images of other aspect ratios or walls go (at start only, before the GS has any of them).
            if (!live)
                for (const auto &entry : fs::directory_iterator(cacheDir, ec))
                    if (entry.is_regular_file(ec) && std::find(keep.begin(), keep.end(), entry.path().filename().string()) == keep.end())
                        fs::remove(entry.path(), ec);
        }

        // ---- layout --------------------------------------------------------------------------------------------
        float g_aspect = 4.0f / 3.0f;
        const settings::Setting *g_pendingApply = nullptr; // saved, applied when the cursor leaves its row
        std::string g_pendingFrom;                         // its value before (back to it: nothing to apply)

        // A change behind the game's own screen fade (K, Nov 13: hides the swap and gives it time): to black, the
        // change, held until it is done (an aspect change: the screen's new wall up), back in. Menu input waits.
        enum class FadeStep
        {
            Idle,
            Out,
            Hold,
            In
        };
        struct FadeRun
        {
            FadeStep step = FadeStep::Idle;
            const settings::Setting *setting = nullptr; // the change, or
            bool restore = false;                       // RESTORE DEFAULTS
            bool aspect = false;                        // it changes the aspect ratio: hold for the wall
            uint32_t generation = 0;                    // the wall's prepare to wait for
            bool api = false;                           // a graphics API switch is under way: hold for it
            int64_t stepMs = 0, readyMs = 0, apiMs = 0;
        };
        FadeRun g_fade;
        bool g_apiWatch = false; // a graphics API switch was asked for: graphicsApiSwitched() when it ends (EE thread)

        // ---- the screen ----------------------------------------------------------------------------------------
        struct Screen
        {
            uint32_t self = 0;
            int row = 0;
            uint32_t backdrop = 0; // its backdrop entry (left stretched over the picture)
            int confirmRow = -1;   // asking Yes / No for this row (-1 = not asking)
            bool confirmYes = false;
            bool inPage = false;   // the cursor is in the category's page (Right / A in, B out)
            int pageCursor = 0;    // its row (headers skipped)
            int pageScroll = 0;    // the first row shown
        };
        Screen g_screen[2];
        GuestFunction g_original[2][5] = {}; // open, close, update, move, draw

        Screen &screenOf(int kind, uint32_t self)
        {
            Screen &s = g_screen[kind];
            if (s.self != self)
                s = Screen{self};
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

        // ---- the settings (settings_table.cpp: the launcher draws the same list) --------------------------------
        fs::path g_iniPath;
        IniFile g_ini; // GHR.ini: read as the screen opens, every change saved at once

        // The game's own options, GHR.ini their one store (K): written into the game every frame they differ
        // (onFrame), so a loaded save's own values never stick.
        constexpr uint32_t kCoreSavePtr = 0x569B70u;   // cCoreSave *
        constexpr uint32_t kSaveStateBits = 0xBBAu;    // u16; bit 0 = the enemy life gauge hidden (cCoreSave_*StateBit0)
        constexpr uint32_t kSaveSoundMode = 0x170u;    // the saved sound mode (func_001F9E68 applies it after a load)
        constexpr uint32_t kSoundMode = 0x76A7A4u;     // the sound mode now (func_003756F0 reads it)
        constexpr uint32_t kSetSoundMode = 0x375700u;  // func_00375700(mode): 0 mono, 1 stereo, 2 Dolby Pro Logic II
        constexpr uint32_t kVibration = 0x5E85F8u;     // u8, 1 = on (the game's Restore Defaults, func_001672F0)
        std::atomic<bool> g_lifeGauge{true}, g_vibration{true};
        std::atomic<int> g_soundModeWanted{1};
        std::atomic<bool> g_reapplyVolumes{false};

        // The pad's raw button word (GameWork +0x08, pressed this frame; libpad's bytes 2..3 inverted): L1 / R1.
        constexpr uint32_t kPadPressed = game::GameWork::address(game::GameWork::kPadBtn);
        constexpr uint32_t kPadL1 = 0x0004u, kPadR1 = 0x0008u;

        settings::Tab tabOf(Action action)
        {
            switch (action)
            {
            case Action::Controls:
                return settings::Tab::Controls;
            case Action::Display:
                return settings::Tab::Display;
            case Action::Interface:
                return settings::Tab::Interface;
            case Action::Gameplay:
                return settings::Tab::Gameplay;
            default:
                return settings::Tab::Sound;
            }
        }
        bool isCategory(Action action)
        {
            return action == Action::Controls || action == Action::Display || action == Action::Interface ||
                   action == Action::Gameplay || action == Action::Sound;
        }

        // The pages (options_pages.cpp), from the GHR.ini read as the screen opened.
        using options_pages::firstOption;
        using options_pages::nextOption;
        using options_pages::PageData;
        PageData g_page;
        void buildPage(PageData &d, settings::Tab tab) { options_pages::buildPage(d, tab, g_ini, g_aspect); }

        // A changed setting takes effect at once (the game thread; the modules' own setters, as F2 uses them).
        void applyVolumes()
        {
            const auto percent = [](const char *key) {
                const settings::Setting *s = settings::find(key);
                return s ? std::atoi(settings::value(g_ini, *s).c_str()) : 100;
            };
            setVolumes(percent("MusicVolume"), percent("EffectsVolume"), percent("VoicesVolume"));
        }

        int soundModeIndex(const std::string &value)
        {
            const settings::Setting *s = settings::find("Mode");
            const int i = s ? settings::choiceIndex(*s, value) : -1;
            return i < 0 ? 1 : i; // mono 0, stereo 1, Pro Logic II 2: the game's own numbers
        }

        void readGameOptions()
        {
            const auto onOf = [](const char *key) {
                const settings::Setting *s = settings::find(key);
                return s ? settings::on(g_ini, *s) : true;
            };
            g_lifeGauge = onOf("EnemyLifeGauge");
            g_vibration = onOf("Vibration");
            const settings::Setting *mode = settings::find("Mode");
            g_soundModeWanted = soundModeIndex(mode ? settings::value(g_ini, *mode) : "stereo");
            applyVolumes();
        }

        int g_visibleKind = 0; // the screen up (pause / title): its wall is made first after an aspect change

        // Aspect Ratio while playing: the widescreen hooks, camera, HUD and display (widescreen.cpp), this screen's own
        // layout, a plain window's shape, and the wall made again in the new shape (a thread; the old one stays up
        // until it is ready).
        // The wall in a new shape (a thread); its generation.
        uint32_t startWallPrepare(float aspect)
        {
            const uint32_t generation = g_wallGeneration.fetch_add(1u) + 1u;
            if (!g_exeDir.empty())
                std::thread(prepare, g_exeDir, std::clamp(aspect, 1.0f, 4.0f), generation, g_visibleKind).detach();
            return generation;
        }

        bool g_wallStarted = false; // a change's fade already started the new wall (while going to black)

        void setAspectLive(const std::string &value)
        {
            setWidescreenAspect(value.c_str());
            g_aspect = std::clamp(aspectRatio(), 1.0f, 4.0f);
            ps2_host_display::requestWindowShape();
            if (!g_wallStarted)
                startWallPrepare(g_aspect);
            g_wallStarted = false;
        }

        // F11 (main thread) switched the window: saved as the Display Mode at the next game frame (onFrame).
        std::atomic<int> g_windowMode{-1};
        void onWindowMode(int mode)
        {
            g_windowMode.store(mode, std::memory_order_relaxed);
        }

        // The setting's environment variable follows the change (main.cpp read GHR.ini into them at start): a test case
        // records the settings from there, and a change made here before recording was lost (Nov 13).
        void syncEnv(const settings::Setting &s)
        {
            const auto one = [](const char *section, const char *key) {
                if (const char *variable = iniEnvVariable(section, key))
                    setEnvValue(variable, g_ini.get(section, key, ""));
            };
            if (std::strcmp(s.key, "CameraPreset") == 0)
                for (const char *key : {"FieldOfView", "FightFieldOfView", "CameraZoomOut", "CameraTilt"})
                    one("Video", key);
            else if (std::strcmp(s.id, "DisplayMode") == 0)
                for (const char *key : {"StartFullscreen", "StartMaximized"})
                    one("Video", key);
            else if (*s.key)
                one(s.section, s.key);
        }

        void apply(const settings::Setting &s)
        {
            syncEnv(s);
            const std::string id = s.id, value = settings::value(g_ini, s);
            const bool on = s.kind == settings::Kind::Toggle && settings::on(g_ini, s);
            if (id == "DisplayMode")
                ps2_host_display::requestMode(value == "fullscreen" ? 2 : value == "maximized" ? 1 : 0);
            else if (id == "AspectRatio")
                setAspectLive(value);
            else if (id == "InternalResolution")
                GSPcsx2Backend::SetScale(std::clamp(static_cast<float>(std::atof(value.c_str())), 1.0f, 8.0f));
            else if (id == "GraphicsAPI")
            {
                if (GSPcsx2Backend::SetGraphicsApi(value.c_str()))
                    g_apiWatch = true;
                else
                    KM_WARN(Game, "options screen: the graphics API cannot change while running (an older pcsx2gs.dll?); "
                                  "it applies at the next start");
            }
            else if (id == "MenuButtons")
                menu_buttons::setStyle(value == "japanese" ? menu_buttons::Style::Japanese
                                       : value == "classic" ? menu_buttons::Style::Classic
                                                            : menu_buttons::Style::Western);
            else if (id == "CameraPreset" || id == "FieldOfView" || id == "FightFieldOfView" || id == "CameraZoomOut" ||
                     id == "CameraTilt" || id == "SteadyIdleCamera" || id == "SeeThrough" ||
                     id == "DepthOfField")
                reloadCameraSettings();
            else if (id == "HudLayout")
                hud::requestLayout(value == "4:3" ? 1 : 0);
            else if (id == "MenuLayout")
                hud::requestMenuMode(value == "stretch" ? 1 : 0);
            else if (id == "SwayBar")
                setSwayBar(on);
            else if (id == "ButtonPrompts")
            {
                prompt_hd::setChoice(static_cast<prompt_hd::Choice>(std::max(settings::choiceIndex(s, value), 0)));
                button_prompts::requestRebuild();
            }
            else if (id == "Fps60")
                requestFps60(on);
            else if (id == "L3Dash")
                setL3Dash(on);
            else if (id == "ControlStyle")
                modern::setModern(value == "modern");
            else if (id == "ModernAuto")
                modern::setAuto(on);
            else if (id == "MouseCamera")
                modern::setMouseCamera(on);
            else if (id == "BallBusterWomen")
                setBallBusterWomen(on);
            else if (id == "TensionPlacable")
                setTensionPlacable(on);
            else if (id == "OffTheCuff")
                setOffTheCuff(on);
            else if (id == "RouletteTimer")
                setRouletteTimerDouble(value == "double");
            else if (id == "RouletteTargetCycle")
                setRouletteTargetCycle(on);
            else if (id == "RouletteNoOrbs")
                setRouletteNoOrbs(on);
            else if (id == "LiftChairs")
                setLiftChairs(on); // the next room's chairs
            else if (id == "StyleSwitching")
                style_presets::setEnabled(on);
            else if (id == "StyleSignal")
                style_presets::setSignal(on);
            else if (id == "BuckBreaker")
                buck_breaker::setEnabled(on);
            else if (id == "SprintTraversal")
                sprint_traversal::setEnabled(on);
            else if (id == "NpcPoses")
                npc_poses::setEnabled(on);
            else if (id == "EnemyLifeGauge" || id == "Vibration" || id == "Mode")
                readGameOptions(); // onFrame writes them into the game
            else if (id == "MusicVolume" || id == "EffectsVolume" || id == "VoicesVolume")
            {
                applyVolumes();
                g_reapplyVolumes = true; // the playing streams ask again (onFrame, with the game's registers)
            }
        }

        // Left / right (dir -1 / +1) on a page's row; ok: A on it (toggles flip, choices go round, sliders stay).
        // Returns true when the value changed (saved and applied).
        bool change(const PageData &d, int row, int dir, bool ok)
        {
            if (row < 0 || row >= static_cast<int>(d.rows.size()) || !d.setting[static_cast<size_t>(row)])
                return false;
            const settings::Setting &s = *d.setting[static_cast<size_t>(row)];
            const options_overlay::PageRow &r = d.rows[static_cast<size_t>(row)];
            const std::string before = settings::value(g_ini, s);
            std::string value;
            switch (s.kind)
            {
            case settings::Kind::Toggle:
            {
                const bool now = settings::on(g_ini, s);
                const bool next = ok ? !now : dir < 0; // ON is on the left
                if (next == now)
                    return false;
                value = next ? "1" : "0";
                break;
            }
            case settings::Kind::Slider:
            {
                if (ok)
                    return false;
                const int next = std::clamp(r.choice + dir, 0, s.valueCount - 1);
                if (next == r.choice)
                    return false;
                value = std::to_string(s.values[next]);
                break;
            }
            case settings::Kind::Choice:
            {
                if (s.gameSlider)
                {
                    if (ok)
                        return false;
                    const int next = r.choice + dir;
                    if (next < 0 || next >= r.valueCount)
                        return false;
                    value = std::to_string(r.values[next]);
                    break;
                }
                const int n = s.choiceCount, now = settings::choiceIndex(s, settings::value(g_ini, s));
                const bool wrap = ok || n > 3; // < value > lists go round; ones shown in a row stop at their ends
                int next = now < 0 ? (dir < 0 ? n - 1 : 0) : now + (ok ? 1 : dir);
                next = wrap ? (next % n + n) % n : std::clamp(next, 0, n - 1);
                if (next == now)
                    return false;
                value = s.choices[next].value;
                break;
            }
            case settings::Kind::Custom: // Display Mode: its choices in a row (two keys)
            {
                const int n = s.choiceCount, now = settings::choiceIndex(s, settings::value(g_ini, s));
                if (n <= 0)
                    return false;
                const int next = ((now < 0 ? 0 : now + (ok ? 1 : dir)) % n + n) % n; // a < value > list: goes round
                if (next == now)
                    return false;
                value = s.choices[next].value;
                break;
            }
            }
            if (std::strcmp(s.key, "CameraPreset") == 0)
                settings::writeCameraPreset(g_ini, value); // its four values (Custom: from Action's)
            else if (std::strcmp(s.id, "DisplayMode") == 0)
                settings::writeDisplayMode(g_ini, value);
            else
                g_ini.set(s.section, s.key, value);
            if (!g_ini.save())
                KM_WARN(Game, "options screen: could not write GHR.ini");
            if (s.applyOnLeave)
            {
                // A rebuild: once, behind a fade, when the cursor leaves the row (K, Nov 13).
                if (g_pendingApply != &s)
                    g_pendingFrom = before;
                g_pendingApply = &s;
            }
            else
                apply(s);
            return true;
        }

        // The row's change waiting for the cursor to leave it (Aspect Ratio, Internal Resolution, Post Effects), at once
        // (the screen is closing: the game's own closing fade covers it).
        void flushPending()
        {
            if (const settings::Setting *s = g_pendingApply)
            {
                g_pendingApply = nullptr;
                if (settings::value(g_ini, *s) != g_pendingFrom)
                    apply(*s);
            }
        }

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // Our own black over everything (the overlay; the game's fade object is not drawn over this screen): the
        // game's menu fade time, 15 frames at 30.
        constexpr int64_t kFadeMs = 500;

        float blackout()
        {
            const float t = std::clamp(static_cast<float>(nowMs() - g_fade.stepMs) / static_cast<float>(kFadeMs), 0.0f, 1.0f);
            switch (g_fade.step)
            {
            case FadeStep::Out:
                return t;
            case FadeStep::Hold:
                return 1.0f;
            case FadeStep::In:
                return 1.0f - t;
            default:
                return 0.0f;
            }
        }

        bool wallSwapAllowed() { return g_fade.step != FadeStep::Out; }

        // To black, the screen as it is (K, Nov 13); an aspect change starts its new wall at once, so the black waits
        // for as little as it can.
        void beginFade(const settings::Setting *setting, bool restore, const char *newAspect)
        {
            g_fade = FadeRun{};
            g_fade.setting = setting;
            g_fade.restore = restore;
            g_fade.step = FadeStep::Out;
            g_fade.stepMs = nowMs();
            if (newAspect)
            {
                g_fade.aspect = true;
                g_fade.generation = startWallPrepare(parseAspectSetting(newAspect));
                g_wallStarted = true;
            }
        }

        // The cursor left a row with a change waiting: the fade starts (nothing to do if it is back to its value).
        void leavePending(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const settings::Setting *s = g_pendingApply;
            g_pendingApply = nullptr;
            if (!s || settings::value(g_ini, *s) == g_pendingFrom)
                return;
            const std::string value = settings::value(g_ini, *s);
            beginFade(s, false, std::strcmp(s->id, "AspectRatio") == 0 ? value.c_str() : nullptr);
            (void)rdram, (void)ctx, (void)runtime;
        }

        void restoreDefaults();

        // A graphics API switch ended. If the API asked for could not start, the one before runs again (the DLL put it
        // back): GHR.ini and the row say so too, and a notice tells the player (K, Nov 13).
        void graphicsApiSwitched()
        {
            g_apiWatch = false;
            const int result = GSPcsx2Backend::GraphicsApiResult();
            const settings::Setting *api = settings::find("GraphicsAPI");
            if (!api)
                return;
            const std::string wanted = settings::value(g_ini, *api);
            const char *wantedName = wanted == "vulkan" ? "Vulkan" : "Direct3D 11";
            if (result > 0)
            {
                KM_INFO(Game, "options screen: graphics API " << (result == 2 ? "already " : "now ") << wantedName);
                return;
            }
            if (result < 0)
            {
                KM_WARN(Game, "options screen: the graphics API switch to " << wantedName << " failed and nothing is drawn");
                return;
            }
            const std::string running = wanted == "vulkan" ? "dx11" : "vulkan";
            g_ini.set(api->section, api->key, running);
            if (!g_ini.save())
                KM_WARN(Game, "options screen: could not write GHR.ini");
            syncEnv(*api);
            const std::string note = std::string(wantedName) + " could not start. Still using " +
                                     (running == "vulkan" ? "Vulkan" : "Direct3D 11") + ".";
            ps2x::menu::notify(note, 4.0f);
            KM_WARN(Game, "options screen: " << note);
        }

        // Each frame of the screen while a fade runs (its input waits). Each wait has a ceiling, so the menu never
        // stays dark.
        void stepFade(int kind, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int64_t now = nowMs();
            switch (g_fade.step)
            {
            case FadeStep::Idle:
                return;
            case FadeStep::Out:
                if (now - g_fade.stepMs < kFadeMs)
                    return;
                // Black: the change (an aspect change's wall was started with the fade).
                if (g_fade.restore)
                    restoreDefaults();
                else if (g_fade.setting)
                    apply(*g_fade.setting);
                g_wallStarted = false;
                g_fade.api = GSPcsx2Backend::GraphicsApiBusy();
                g_fade.step = FadeStep::Hold;
                g_fade.stepMs = now;
                g_fade.readyMs = 0;
                g_fade.apiMs = 0;
                return;
            case FadeStep::Hold:
            {
                bool ready = now - g_fade.stepMs >= 300; // the renderer's rebuild is done by its next present
                if (g_fade.aspect)
                {
                    // The screen's new wall swapped in (patchWall, now that it may), and a moment for its full image.
                    if (g_fade.readyMs == 0 && g_wall[kind].published.load(std::memory_order_acquire) >= g_fade.generation)
                        g_fade.readyMs = now;
                    ready = (g_fade.readyMs != 0 && now - g_fade.readyMs >= 250) || now - g_fade.stepMs >= 4000;
                }
                if (g_fade.api)
                {
                    // The new device and renderer up in their new window, and a moment for the wall's image to load
                    // again (the replacements are read anew).
                    if (g_fade.apiMs == 0 && !GSPcsx2Backend::GraphicsApiBusy())
                    {
                        g_fade.apiMs = now;
                        if (g_apiWatch)
                            graphicsApiSwitched();
                    }
                    ready = ready && ((g_fade.apiMs != 0 && now - g_fade.apiMs >= 400) || now - g_fade.stepMs >= 10000);
                }
                if (!ready)
                    return;
                g_fade.step = FadeStep::In;
                g_fade.stepMs = now;
                return;
            }
            case FadeStep::In:
                if (now - g_fade.stepMs >= kFadeMs)
                    g_fade = FadeRun{};
                return;
            }
        }

        // RESTORE DEFAULTS: every setting the screen shows back to its shipped default.
        // A setting RESTORE DEFAULTS would change that rebuilds something (the fade ones), or nullptr.
        const settings::Setting *restoreNeedsFade()
        {
            for (size_t i = 0; i < settings::kSettingCount; ++i)
            {
                const settings::Setting &s = settings::kSettings[i];
                if (s.live && s.applyOnLeave && settings::value(g_ini, s) != s.fallback)
                    return &s;
            }
            return nullptr;
        }

        void restoreDefaults()
        {
            g_pendingApply = nullptr; // everything is applied below
            for (size_t i = 0; i < settings::kSettingCount; ++i)
            {
                const settings::Setting &s = settings::kSettings[i];
                if (s.live && *s.key)
                    g_ini.set(s.section, s.key, s.fallback);
                else if (s.live && std::strcmp(s.id, "DisplayMode") == 0)
                    settings::writeDisplayMode(g_ini, s.fallback);
            }
            g_ini.save();
            for (size_t i = 0; i < settings::kSettingCount; ++i)
                if (settings::kSettings[i].live)
                    apply(settings::kSettings[i]);
            KM_INFO(Game, "options screen: every setting back to its default");
        }

        // Keeps the page's cursor among the rows shown.
        void scrollToCursor(Screen &s, const PageData &d)
        {
            s.pageScroll = std::clamp(s.pageScroll, 0, std::max(static_cast<int>(d.rows.size()) - 1, 0));
            if (s.pageCursor < s.pageScroll)
            {
                s.pageScroll = s.pageCursor;
                // a header right above the first option stays in view with it
                if (s.pageScroll > 0 && d.rows[static_cast<size_t>(s.pageScroll - 1)].header)
                    --s.pageScroll;
            }
            while (s.pageScroll < s.pageCursor && s.pageCursor >= s.pageScroll + options_overlay::pageRowsShown(d.page, s.pageScroll))
                ++s.pageScroll;
        }

        // ---- the overlay (options_overlay.cpp) ----------------------------------------------------------------
#if defined(KAMITE_HAS_SCREEN_OVERLAY)
        std::vector<Pcsx2GsScreenItem> g_items;
        std::string g_strings;
#endif

        // The confirm / cancel hints' images: the menu buttons (menu_buttons' style) in the prompts' art set and
        // bindings (button_prompts), looked up again only when one of those changes.
        struct HintArt
        {
            menu_buttons::Style style = menu_buttons::Style::Western;
            button_prompts::MenuArt art;
            bool made = false;
            std::string confirm[2], cancel;
        };
        HintArt g_hintArt;

        const HintArt &hintArt()
        {
            const menu_buttons::Style style = menu_buttons::style();
            button_prompts::MenuArt art = button_prompts::menuArt();
            if (!art.known) // prompts off or not built yet: the PlayStation buttons in their places
                art = button_prompts::MenuArt{false, prompt_hd::Set::PlayStation, "A", "B", "Y"};
            HintArt &h = g_hintArt;
            if (h.made && h.style == style && h.art.set == art.set && h.art.cross == art.cross && h.art.circle == art.circle &&
                h.art.triangle == art.triangle)
                return h;
            const auto file = [&](const std::string &label)
            {
                const std::u8string path = prompt_hd::artFile(art.set, label).u8string();
                return std::string(path.begin(), path.end());
            };
            h.style = style;
            h.art = art;
            h.made = true;
            // Western: the cross confirms, the circle cancels; Japanese the other way round; Classic as the US release:
            // circle and cross confirm (the load menu's order: cross first), triangle cancels.
            h.confirm[0] = style == menu_buttons::Style::Japanese ? file(art.circle) : file(art.cross);
            h.confirm[1] = style == menu_buttons::Style::Classic ? file(art.circle) : std::string();
            h.cancel = style == menu_buttons::Style::Western ? file(art.circle)
                       : style == menu_buttons::Style::Japanese ? file(art.cross)
                                                                : file(art.triangle);
            KM_INFO(Game, "options screen: hints in " << prompt_hd::setName(art.set) << " art (" << menu_buttons::styleName(style)
                                                      << " menu buttons)");
            return h;
        }

        void sendOverlay(int kind, const Screen &s, float fade)
        {
#if defined(KAMITE_HAS_SCREEN_OVERLAY)
            const Kind &k = kKinds[kind];
            options_overlay::View view;
            view.aspect = g_aspect;
            view.fade = fade;
            view.pause = kind == kPause;
            view.rows = k.text;
            view.rowCount = k.rowCount;
            view.cursor = s.row;
            view.confirmRow = s.confirmRow;
            view.confirmYes = s.confirmYes;
            if (s.confirmRow >= 0)
                view.question = k.actions[s.confirmRow] == Action::ResetGame ? kResetQuestion : kRestoreQuestion;
            // The category under the cursor: beside the list on 16:10 and wider (names only until it is opened), in its
            // place when open on a narrower screen.
            if (isCategory(k.actions[s.row]))
            {
                buildPage(g_page, tabOf(k.actions[s.row]));
                view.page = &g_page.page;
                view.inPage = s.inPage;
                view.pageCursor = s.inPage ? s.pageCursor : -1;
                view.pageScroll = s.inPage ? s.pageScroll : 0;
            }
            const HintArt &hints = hintArt();
            view.confirmHint.art[0] = hints.confirm[0].c_str();
            view.confirmHint.art[1] = hints.confirm[1].c_str();
            view.confirmHint.word = "Confirm";
            view.cancelHint.art[0] = hints.cancel.c_str();
            view.cancelHint.word = "Cancel";
            options_overlay::build(view, g_items, g_strings);
            if (const float black = blackout(); black > 0.0f)
                options_overlay::appendBlackout(black, g_items); // over everything, the menu as it is
            GSPcsx2Backend::SetScreen(g_items.data(), static_cast<uint32_t>(g_items.size()), g_strings.data(),
                                      static_cast<uint32_t>(g_strings.size()));
#else
            (void)kind, (void)s, (void)fade;
#endif
        }

        void clearOverlay() { GSPcsx2Backend::SetScreen(nullptr, 0u, nullptr, 0u); }

        // How much of the picture the game's fade lets through (1 = none drawn, 0 = black): our text fades with it.
        float screenFade()
        {
            if ((*at(kFade + 0x1Cu) & 1u) == 0u)
                return 1.0f;
            uint16_t time, count;
            std::memcpy(&time, at(kFade + 0x18u), sizeof(time));
            std::memcpy(&count, at(kFade + 0x1Au), sizeof(count));
            const float from = static_cast<float>(rd32(kFade + 0x10u) >> 24), to = static_cast<float>(rd32(kFade + 0x14u) >> 24);
            const float t = time ? std::min(1.0f, static_cast<float>(count) / static_cast<float>(time)) : 1.0f;
            return 1.0f - std::clamp((from + (to - from) * t) / 128.0f, 0.0f, 1.0f);
        }

        // Into the page of the category under the cursor (OK; K, Nov 13: confirm / cancel only), its first option.
        void enterPage(int kind, Screen &s, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Kind &k = kKinds[kind];
            if (!isCategory(k.actions[s.row]))
                return;
            buildPage(g_page, tabOf(k.actions[s.row]));
            const int first = firstOption(g_page);
            if (first < 0)
                return;
            s.inPage = true;
            s.pageCursor = first;
            s.pageScroll = 0;
            sound(rdram, ctx, runtime, kSeOk);
        }

        // The pad inside a page: up / down between options, left / right change the value (saved and applied at
        // once), A steps it on, L1 / R1 the neighbouring category, B back to the list.
        void pageInputMoves(int kind, Screen &s, uint64_t pressed, uint64_t repeat, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Kind &k = kKinds[kind];
            buildPage(g_page, tabOf(k.actions[s.row]));
            if (g_page.rows.empty() || firstOption(g_page) < 0)
            {
                s.inPage = false;
                return;
            }
            if (s.pageCursor >= static_cast<int>(g_page.rows.size()) || g_page.rows[static_cast<size_t>(s.pageCursor)].header)
                s.pageCursor = firstOption(g_page); // a sub-option above went away
            uint16_t pad = 0;
            std::memcpy(&pad, at(kPadPressed), sizeof(pad));
            if ((pressed & kCancel) != 0u && (pressed & kOk) == 0u)
            {
                s.inPage = false;
                sound(rdram, ctx, runtime, kSeBack);
                return;
            }
            if (pad & (kPadL1 | kPadR1))
            {
                const int step = (pad & kPadR1) ? 1 : -1;
                const int next = s.row + step;
                if (next >= 0 && next < k.rowCount && isCategory(k.actions[next]))
                {
                    s.row = next;
                    buildPage(g_page, tabOf(k.actions[s.row]));
                    s.pageCursor = std::max(firstOption(g_page), 0);
                    s.pageScroll = 0;
                    sound(rdram, ctx, runtime, kSeMove);
                }
                return;
            }
            int move = 0;
            if (repeat & kUp)
                move = -1;
            else if (repeat & kDown)
                move = 1;
            if (move != 0)
            {
                const int next = nextOption(g_page, s.pageCursor, move);
                if (next >= 0)
                {
                    s.pageCursor = next;
                    sound(rdram, ctx, runtime, kSeMove);
                }
            }
            else if ((pressed & kOk) && change(g_page, s.pageCursor, 1, true))
                sound(rdram, ctx, runtime, kSeMove);
            else if ((repeat & kLeft) && change(g_page, s.pageCursor, -1, false))
                sound(rdram, ctx, runtime, kSeMove);
            else if ((repeat & kRight) && change(g_page, s.pageCursor, 1, false))
                sound(rdram, ctx, runtime, kSeMove);
            buildPage(g_page, tabOf(k.actions[s.row])); // the change may add or drop sub-options
            if (s.pageCursor >= static_cast<int>(g_page.rows.size()))
                s.pageCursor = std::max(firstOption(g_page), 0);
            scrollToCursor(s, g_page);
        }

        // A change waiting for the cursor to leave its row is applied once it has (another row, category or the list).
        void pageInput(int kind, Screen &s, uint64_t pressed, uint64_t repeat, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const int row = s.row, cursor = s.pageCursor;
            pageInputMoves(kind, s, pressed, repeat, rdram, ctx, runtime);
            if (!s.inPage || s.row != row || s.pageCursor != cursor)
                leavePending(rdram, ctx, runtime);
        }

        // OK on a row.
        void act(int kind, Screen &s, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Kind &k = kKinds[kind];
            const uint32_t self = s.self;
            switch (k.actions[s.row])
            {
            case Action::LoadGame:
                sound(rdram, ctx, runtime, kSeOk);
                wr32(self + kResult, static_cast<uint32_t>(kResultLoad));
                break;
            case Action::ResetGame:
            case Action::Restore: // Yes / No first (No selected)
                sound(rdram, ctx, runtime, kSeOk);
                s.confirmRow = s.row;
                s.confirmYes = false;
                break;
            case Action::Exit:
                sound(rdram, ctx, runtime, kSeOk);
                wr32(self + kResult, static_cast<uint32_t>(kResultBack));
                break;
            default: // a category: its page
                enterPage(kind, s, rdram, ctx, runtime);
                break;
            }
        }

        // The Yes / No beside RESET GAME or RESTORE DEFAULTS.
        void confirm(int kind, Screen &s, uint64_t pressed, uint64_t repeat, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const Kind &k = kKinds[kind];
            if ((pressed & (kOk | kCancel)) == kCancel || ((pressed & kOk) && !s.confirmYes))
            {
                sound(rdram, ctx, runtime, kSeBack);
                s.confirmRow = -1;
            }
            else if (pressed & kOk)
            {
                sound(rdram, ctx, runtime, kSeOk);
                if (k.actions[s.confirmRow] == Action::ResetGame)
                    wr32(s.self + kResult, static_cast<uint32_t>(kResultReset));
                else if (restoreNeedsFade())
                {
                    // One fade round the whole reset when it rebuilds something (K, Nov 13).
                    const settings::Setting *aspect = settings::find("AspectRatio");
                    g_pendingApply = nullptr;
                    beginFade(nullptr, true,
                              aspect && settings::value(g_ini, *aspect) != aspect->fallback ? aspect->fallback : nullptr);
                }
                else
                    restoreDefaults();
                s.confirmRow = -1;
            }
            else if ((repeat & kLeft) && !s.confirmYes)
            {
                s.confirmYes = true;
                sound(rdram, ctx, runtime, kSeMove);
            }
            else if ((repeat & kRight) && s.confirmYes)
            {
                s.confirmYes = false;
                sound(rdram, ctx, runtime, kSeMove);
            }
        }

        // Entry 2 (open), wrapped: our list starts at its top.
        template <int N>
        void openHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            g_original[N][0](rdram, ctx, runtime);
            g_screen[N] = Screen{self};
            if (!g_iniPath.empty() && !g_ini.load(g_iniPath))
                KM_WARN(Game, "options screen: could not read GHR.ini");
        }

        // Entry 3 (close: releases the layout), wrapped: our text goes with it.
        template <int N>
        void closeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            // Closed mid-fade (the game's own closing fade takes over): a change not made yet is made now.
            if (g_fade.step == FadeStep::Out)
            {
                if (g_fade.restore)
                    restoreDefaults();
                else if (g_fade.setting)
                    apply(*g_fade.setting);
            }
            g_fade = FadeRun{};
            g_wallStarted = false;
            flushPending(); // the screen closed on a row with a change waiting
            clearOverlay();
            g_screen[N] = Screen{};
            g_original[N][1](rdram, ctx, runtime);
        }

        // Entry 5 (move): the pad.
        template <int N>
        void moveHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            if (mode(self) != 0)
                return g_original[N][3](rdram, ctx, runtime);
            const uint32_t ra = GPR_U32(ctx, 31);
            Screen &s = screenOf(N, self);
            const Kind &k = kKinds[N];
            g_visibleKind = N;
            if (g_fade.step != FadeStep::Idle)
            {
                stepFade(N, rdram, ctx, runtime); // the menu's input waits for the fade
                ctx->pc = ra;
                return;
            }
            const uint64_t pressed = rd64(kCommand), repeat = rd64(kCommandRepeat);
            if ((pressed & (kOk | kStart)) == kStart)
            {
                sound(rdram, ctx, runtime, kSeBack);
                wr32(self + kResult, static_cast<uint32_t>(kResultClose));
            }
            else if (s.confirmRow >= 0)
                confirm(N, s, pressed, repeat, rdram, ctx, runtime);
            else if (s.inPage)
                pageInput(N, s, pressed, repeat, rdram, ctx, runtime);
            else if (pressed & kOk)
                act(N, s, rdram, ctx, runtime);
            else if ((pressed & kCancel) != 0u)
            {
                sound(rdram, ctx, runtime, kSeBack);
                wr32(self + kResult, static_cast<uint32_t>(kResultBack));
            }
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
                g_original[N][2](rdram, ctx, runtime);
                // The game's own modes over our wall: its row names and values (and the cursor) would float on it.
                for (int id = 0; id <= k.hideUpTo; ++id)
                    setHidden(idEntry(self, id), true);
                return;
            }
            const uint32_t ra = GPR_U32(ctx, 31);
            Screen &s = screenOf(N, self);
            if (s.backdrop == 0u)
                s.backdrop = findBackdrop(self);
            for (int id = 0; id < k.ids; ++id) // the game draws only the backdrop
                setHidden(idEntry(self, id), true);
            callGuest(rdram, ctx, runtime, kIdMove, {self + k.page});
            callGuest(rdram, ctx, runtime, kIdMove, {self});
            ctx->pc = ra;
        }

        // Entry 6 (draw): the backdrop, and our list for the overlay.
        template <int N>
        void drawHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t self = GPR_U32(ctx, 4);
            if (mode(self) != 0)
            {
                clearOverlay();
                return g_original[N][4](rdram, ctx, runtime);
            }
            const uint32_t ra = GPR_U32(ctx, 31);
            Screen &s = screenOf(N, self);
            if (s.backdrop == 0u)
                s.backdrop = findBackdrop(self);
            hud::claimOwnScreen();
            callGuest(rdram, ctx, runtime, kIdTrans, {self});
            const float backdrop = valid(s.backdrop, ent::kSize) ? static_cast<float>(*at(s.backdrop + kEntColour + 3u)) / 128.0f : 1.0f;
            sendOverlay(N, s, std::min(backdrop, 1.0f) * screenFade());
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
            const bool pause = N == kPause;
            return hook(runtime, k.open, pause ? "cSubScrOption open (options screen)" : "cSubScrOptionT open (options screen)", &openHook<N>, o[0]) &&
                   hook(runtime, k.close, pause ? "cSubScrOption close (options screen)" : "cSubScrOptionT close (options screen)", &closeHook<N>, o[1]) &&
                   hook(runtime, k.update, pause ? "cSubScrOption update (options screen)" : "cSubScrOptionT update (options screen)", &updateHook<N>, o[2]) &&
                   hook(runtime, k.move, pause ? "cSubScrOption move (options screen)" : "cSubScrOptionT move (options screen)", &moveHook<N>, o[3]) &&
                   hook(runtime, k.draw, pause ? "cSubScrOption draw (options screen)" : "cSubScrOptionT draw (options screen)", &drawHook<N>, o[4]);
        }
    }

    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir)
    {
        g_ram = runtime.memory().getRDRAM();
        g_aspect = std::clamp(aspectRatio(), 1.0f, 4.0f);
        g_iniPath = exeDir / "GHR.ini";
        if (g_ini.load(g_iniPath))
            readGameOptions(); // the game's own options and the volumes, from the start
        // The functions are hooked first: a half-hooked screen would mix the game's and ours.
        for (int kind = 0; kind < 2; ++kind)
            for (const uint32_t address : {kKinds[kind].open, kKinds[kind].close, kKinds[kind].update, kKinds[kind].move, kKinds[kind].draw})
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
        const auto utf8 = [](const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(reinterpret_cast<const char *>(text.data()), text.size());
        };
        options_overlay::setAssets(utf8(exeDir / "resources" / "fonts" / "ArchivoNarrow-SemiBold.ttf"),
                                   utf8(exeDir / "resources" / "fonts" / "DejaVuSans-Bold.ttf"),
                                   utf8(exeDir / "resources" / "options"));
        options_overlay::setScrollBar(utf8(exeDir / "resources" / "options" / "scroll_track.png"),
                                      utf8(exeDir / "resources" / "options" / "scroll_thumb.png"));
        options_overlay::setSliderFill(utf8(exeDir / "resources" / "options" / "slider_fill.png"));
        g_exeDir = exeDir;
        ps2_host_display::setModeListener(&onWindowMode);
        std::thread(prepare, exeDir, g_aspect, g_wallGeneration.load(), 0).detach();
        KM_INFO(Game, "options screen: the title and pause Options open the God Hand: Rearmed screen (aspect " << g_aspect << ")");
    }

    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_ram)
            return;
        // A graphics API switch that ended outside the screen's fade (the screen closed with it waiting).
        if (g_apiWatch && !GSPcsx2Backend::GraphicsApiBusy())
            graphicsApiSwitched();
        if (const int mode = g_windowMode.exchange(-1, std::memory_order_relaxed); mode >= 0)
        {
            // F11: kept as the Display Mode (K, Nov 13), as if the row had changed.
            if (!g_iniPath.empty())
                g_ini.load(g_iniPath);
            settings::writeDisplayMode(g_ini, mode == 2 ? "fullscreen" : mode == 1 ? "maximized" : "window");
            if (!g_ini.save())
                KM_WARN(Game, "options screen: could not write GHR.ini");
            if (const settings::Setting *s = settings::find("DisplayMode"))
                syncEnv(*s);
        }
        const uint32_t save = rd32(kCoreSavePtr);
        if (save == 0u || !valid(save, kSaveStateBits + 2u))
            return; // not set up yet (the sound mode's setter needs the game's sound running)
        uint16_t bits = 0;
        std::memcpy(&bits, at(save + kSaveStateBits), sizeof(bits));
        const uint16_t gauge = g_lifeGauge ? static_cast<uint16_t>(bits & ~1u) : static_cast<uint16_t>(bits | 1u);
        if (gauge != bits)
            std::memcpy(at(save + kSaveStateBits), &gauge, sizeof(gauge));
        const uint32_t mode = static_cast<uint32_t>(g_soundModeWanted.load());
        if (rd32(save + kSaveSoundMode) != mode)
            wr32(save + kSaveSoundMode, mode);
        if (rd32(kSoundMode) != mode)
            callGuest(rdram, ctx, runtime, kSetSoundMode, {mode});
        const uint8_t vibration = g_vibration ? 1u : 0u;
        if (*at(kVibration) != vibration)
            *at(kVibration) = vibration;
        if (g_reapplyVolumes.exchange(false))
            reapplyStreamVolumes(rdram, ctx, runtime);
    }
}
