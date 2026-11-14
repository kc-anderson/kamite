#include "kamite/button_prompts.h"
#include "kamite/hd_textures.h"
#include "kamite/prompt_hd.h"
#include "runtime/gs/gs_pcsx2_backend.h"

#include "kamite/button_prompt_sheets.h"
#include "kamite/crate_prompts.h"
#include "kamite/disc_files.h"
#include "kamite/log.h"
#include "kamite/menu_buttons.h"
#include "kamite/modern_controls.h"
#include "kamite/prompt_art.h"
#include "kamite/sha256.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/ps2_pnach.h"

#include <cctype>
#include <array>
#include <algorithm>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kamite::button_prompts
{
    namespace
    {
        namespace sheets = kamite::prompt_sheets;
        namespace art = kamite::prompt_art;
        namespace bindings = ps2_host_input::prompts;

        using Payload = std::array<uint8_t, sheets::kPayloadBytes>;
        using Clut = std::array<uint8_t, sheets::kClutBytes>;

        constexpr int kSheetCount = 2;
        struct SheetPlace
        {
            const char *name;
            uint32_t data; // offset from the core_usa.bin base (*(u32*)0x7479F8)
            uint32_t clut;
        };
        constexpr SheetPlace kPlaces[kSheetCount] = {
            {"text icons (texture 0x18)", sheets::kIconSheetData, sheets::kIconSheetClut},
            {"action prompt (texture 0x0F)", sheets::kActionSheetData, sheets::kActionSheetClut},
        };
        // sha256 prefixes of the disc's payloads (button_prompt_sheets.h).
        constexpr const char *kShaPrefix[kSheetCount] = {"4d625e757c635e93", "dc3f1442ad970cb2"};
        // The core block reaches at least to the end of the icon sheet's CLUT.
        constexpr uint32_t kCoreSpan = sheets::kIconSheetClut + static_cast<uint32_t>(sheets::kClutBytes);

        // ---- originals: captured once on the EE thread, read-only after g_captured ----
        Payload g_origPayload[kSheetCount];
        Clut g_origClut[kSheetCount];
        std::atomic<bool> g_captured{false};
        bool g_fromDisc = false; // EE thread: the originals came from the disc's core_usa.bin

        // ---- pirate minigame crates (crate_prompts.h): a TIM3 file in the room heap, found by scanning ----
        using CratePayload = std::array<uint8_t, crate_prompts::kPayloadBytes>;
        constexpr uint32_t kTim3Magic = 0x334D4954u;          // "TIM3", 0x40 bytes before the pixels
        constexpr uint64_t kCratePrefix = 0x0221111112221111ull; // first 8 payload bytes (little endian)
        constexpr const char *kCrateShaPrefix = "ac15f235994f08f3";
        using CrateClut = std::array<uint8_t, crate_prompts::kClutBytes>;
        CratePayload g_crateOrig;                              // builder thread writes once, then g_crateCaptured
        CrateClut g_crateOrigClut;                             // its CLUT (right after the pixels), captured with it
        std::array<uint8_t, 0x40> g_crateHeader{};             // its TIM3 header (the 0x40 bytes before the pixels)
        std::atomic<uint64_t> g_crateFoundHash{0u};            // the pixels found in RAM (scanCrate -> applyCrate)
        std::atomic<bool> g_crateCaptured{false};
        std::atomic<uint32_t> g_crateAddress{0u};              // guest address of the pixels, 0 = not located
        std::atomic<bool> g_crateRebuild{false};
        std::atomic<bool> g_promptsRebuild{false}; // the F2 menu changed the prompt set

        // ---- built images: builder thread -> EE thread (whole object replaced) ----
        struct Images
        {
            uint64_t version = 0u;
            bool patched[kSheetCount] = {}; // false: this sheet shows the original art
            Payload payload[kSheetCount];
            Clut clut[kSheetCount];         // with it: the original CLUT, or one with its pink ramp made yellow
            uint64_t hash[kSheetCount] = {};
            bool cratePatched = false;
            CratePayload crate;
            CrateClut crateClut;
            uint64_t crateHash = 0u;
        };
        std::mutex g_imagesMutex;
        // The OPTIONS screen's confirm / cancel hints (menuArt()): the set and the labels of the menu buttons.
        std::mutex g_menuArtMutex;
        MenuArt g_menuArt;
        std::shared_ptr<const Images> g_images;

        // ---- EE thread only ----
        uint8_t *g_ram = nullptr;
        std::shared_ptr<const Images> g_applied;
        std::vector<uint64_t> g_known[kSheetCount]; // hashes of every sheet content this code put there (or found there)
        std::vector<uint64_t> g_crateKnown;         // the same for the crate texture
        std::vector<uint64_t> g_crateClutKnown;     // and for its CLUT
        uint32_t g_vblanks = 0u;
        bool g_warnedForeign[kSheetCount] = {};

        uint64_t fnv1a64(const uint8_t *data, size_t size)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (size_t i = 0; i < size; ++i)
                h = (h ^ data[i]) * 0x100000001B3ull;
            return h;
        }

        uint32_t rd32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_ram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        bool coreOk(uint32_t core)
        {
            return core >= 0x100000u && core <= 0x2000000u - kCoreSpan && (core & 0xFu) == 0u;
        }

        // The sheet's TEX0 (data - 0xC0) is the 128x128 PSMT8 TBW 2 the design checked in every dump.
        bool sheetThere(uint32_t core, int sheet)
        {
            uint64_t tex0;
            std::memcpy(&tex0, g_ram + core + kPlaces[sheet].data - sheets::kTex0Offset, sizeof(tex0));
            return tex0 == sheets::kSheetTex0;
        }

        bool known(int sheet, uint64_t hash)
        {
            for (uint64_t h : g_known[sheet])
                if (h == hash)
                    return true;
            return false;
        }

        // The originals from the disc's core_usa.bin (the block in RAM is the file, as for radar_glow): whatever a save
        // state brought back into the sheets (an earlier session's set mark, any build's style arrows or letters) is then
        // replaced like the port's own bytes. K Nov 2: loading a save state made in another session or build showed
        // the PS2 art ("the text icons sheet is not the disc's" in every replay).
        bool captureFromDisc()
        {
            if (!disc_files::open())
                return false;
            const auto file = disc_files::find("core_usa.bin");
            std::vector<uint8_t> data;
            if (!file || !disc_files::read(*file, data) || data.size() < kCoreSpan)
                return false;
            for (int i = 0; i < kSheetCount; ++i)
            {
                Sha256 sha;
                sha.update(data.data() + kPlaces[i].data, sheets::kPayloadBytes);
                if (sha.finishHex().rfind(kShaPrefix[i], 0) != 0)
                    return false;
            }
            for (int i = 0; i < kSheetCount; ++i)
            {
                std::memcpy(g_origPayload[i].data(), data.data() + kPlaces[i].data, sheets::kPayloadBytes);
                std::memcpy(g_origClut[i].data(), data.data() + kPlaces[i].clut, sheets::kClutBytes);
                g_known[i].push_back(fnv1a64(g_origPayload[i].data(), sheets::kPayloadBytes));
            }
            g_fromDisc = true;
            return true;
        }

        // First sight of both sheets: the originals from the disc, or else from RAM, but only when RAM holds the disc's
        // (a savestate taken while patched would otherwise be taken for the original).
        bool capture(uint32_t core)
        {
            static uint32_t nextTry = 0u;
            if (g_vblanks < nextTry)
                return false;
            nextTry = g_vblanks + 60u;
            static bool discTried = false;
            if (!discTried)
            {
                discTried = true;
                if (captureFromDisc())
                {
                    g_captured.store(true, std::memory_order_release);
                    KM_INFO(Game, "button prompts: sheets found at core 0x" << std::hex << core << std::dec
                                                                             << " (originals from the disc); drawing the bound keys");
                    return true;
                }
            }
            for (int i = 0; i < kSheetCount; ++i)
            {
                const uint8_t *data = g_ram + core + kPlaces[i].data;
                Sha256 sha;
                sha.update(data, sheets::kPayloadBytes);
                if (sha.finishHex().rfind(kShaPrefix[i], 0) != 0)
                {
                    static bool warned = false;
                    if (!warned)
                    {
                        warned = true;
                        KM_WARN(Game, "button prompts: the " << kPlaces[i].name << " sheet is not the disc's; prompts keep the PS2 art");
                    }
                    return false;
                }
            }
            for (int i = 0; i < kSheetCount; ++i)
            {
                std::memcpy(g_origPayload[i].data(), g_ram + core + kPlaces[i].data, sheets::kPayloadBytes);
                std::memcpy(g_origClut[i].data(), g_ram + core + kPlaces[i].clut, sheets::kClutBytes);
                g_known[i].push_back(fnv1a64(g_origPayload[i].data(), sheets::kPayloadBytes));
            }
            g_captured.store(true, std::memory_order_release);
            KM_INFO(Game, "button prompts: sheets found at core 0x" << std::hex << core << std::dec << "; drawing the bound keys");
            return true;
        }

        bool crateKnown(uint64_t hash)
        {
            for (uint64_t h : g_crateKnown)
                if (h == hash)
                    return true;
            return false;
        }

        // The crate texture (EE thread, with the sheets below): the game uploads it from guest RAM every frame, so the
        // built payload goes over it while the minigame's room is loaded. Bytes it does not know mean the room heap was
        // reused: the address is dropped and the builder thread scans again.
        void applyCrate(const Images &images, bool fresh)
        {
            const uint32_t address = g_crateAddress.load(std::memory_order_acquire);
            if (address == 0u || !g_crateCaptured.load(std::memory_order_acquire))
                return;
            if (g_crateKnown.empty())
                g_crateKnown.push_back(fnv1a64(g_crateOrig.data(), crate_prompts::kPayloadBytes));
            if (fresh && images.cratePatched && !crateKnown(images.crateHash))
                g_crateKnown.push_back(images.crateHash);
            // What the scan accepted there (e.g. an earlier build's patch that a save state brought back) may be
            // replaced too.
            if (const uint64_t found = g_crateFoundHash.load(std::memory_order_acquire); found != 0u && !crateKnown(found))
                g_crateKnown.push_back(found);
            uint8_t *guest = g_ram + address;
            const uint8_t *want = images.cratePatched ? images.crate.data() : g_crateOrig.data();
            if (std::memcmp(guest, want, crate_prompts::kPayloadBytes) != 0)
            {
                if (!crateKnown(fnv1a64(guest, crate_prompts::kPayloadBytes)))
                {
                    g_crateAddress.store(0u, std::memory_order_release);
                    return;
                }
                std::memcpy(guest, want, crate_prompts::kPayloadBytes);
            }
            // The CLUT right after the pixels: the set's letter colours (the disc's for PlayStation). Only over a CLUT
            // this code knows (the disc's or one it wrote).
            uint8_t *guestClut = guest + crate_prompts::kPayloadBytes;
            const uint8_t *wantClut = images.cratePatched ? images.crateClut.data() : g_crateOrigClut.data();
            if (std::memcmp(guestClut, wantClut, crate_prompts::kClutBytes) == 0)
                return;
            if (g_crateClutKnown.empty())
                g_crateClutKnown.push_back(fnv1a64(g_crateOrigClut.data(), crate_prompts::kClutBytes));
            const uint64_t wantHash = fnv1a64(wantClut, crate_prompts::kClutBytes);
            if (std::find(g_crateClutKnown.begin(), g_crateClutKnown.end(), wantHash) == g_crateClutKnown.end())
                g_crateClutKnown.push_back(wantHash);
            const uint64_t guestHash = fnv1a64(guestClut, crate_prompts::kClutBytes);
            if (std::find(g_crateClutKnown.begin(), g_crateClutKnown.end(), guestHash) != g_crateClutKnown.end())
                std::memcpy(guestClut, wantClut, crate_prompts::kClutBytes);
        }

        // Every vblank (EE thread, ps2x::pnach frame hook): copy the newest built image over the sheets when it changed,
        // and every 30 vblanks check that the sheets still hold what was put there (a savestate load or a reload of
        // core_usa.bin brings other bytes back). Bytes this code does not know are never overwritten.
        void onVblank()
        {
            ++g_vblanks;
            const uint32_t core = rd32(sheets::kCorePointer);
            if (!coreOk(core) || !sheetThere(core, 0) || !sheetThere(core, 1))
                return;
            if (!g_captured.load(std::memory_order_relaxed) && !capture(core))
                return;

            std::shared_ptr<const Images> images;
            {
                std::unique_lock<std::mutex> lock(g_imagesMutex, std::try_to_lock);
                images = lock.owns_lock() ? g_images : g_applied;
            }
            if (!images)
                return;
            const bool fresh = images != g_applied;
            if (!fresh && g_vblanks % 30u != 0u)
                return;
            if (fresh)
                for (int i = 0; i < kSheetCount; ++i)
                    if (images->patched[i] && !known(i, images->hash[i]))
                        g_known[i].push_back(images->hash[i]);

            for (int i = 0; i < kSheetCount; ++i)
            {
                uint8_t *guest = g_ram + core + kPlaces[i].data;
                const uint8_t *want = images->patched[i] ? images->payload[i].data() : g_origPayload[i].data();
                if (std::memcmp(guest, want, sheets::kPayloadBytes) == 0)
                    continue;
                // With the disc's originals, the sheet's slot (its TEX0 checked above) is the port's to write whatever
                // a save state left there; without them only bytes this code knows are replaced.
                if (!g_fromDisc && !known(i, fnv1a64(guest, sheets::kPayloadBytes)))
                {
                    if (!g_warnedForeign[i])
                    {
                        g_warnedForeign[i] = true;
                        KM_WARN(Game, "button prompts: the " << kPlaces[i].name << " sheet holds unknown bytes; left alone");
                    }
                    continue;
                }
                std::memcpy(guest, want, sheets::kPayloadBytes);
            }
            for (int i = 0; i < kSheetCount; ++i)
            {
                // The CLUT follows its sheet: ours while patched (the yellow ramp), the disc's otherwise. Only a CLUT
                // that is the disc's or one of ours is replaced.
                uint8_t *guest = g_ram + core + kPlaces[i].clut;
                const uint8_t *want = images->patched[i] ? images->clut[i].data() : g_origClut[i].data();
                if (std::memcmp(guest, want, sheets::kClutBytes) == 0)
                    continue;
                const bool ours = g_fromDisc || std::memcmp(guest, g_origClut[i].data(), sheets::kClutBytes) == 0 ||
                                  (g_applied && std::memcmp(guest, g_applied->clut[i].data(), sheets::kClutBytes) == 0) ||
                                  std::memcmp(guest, images->clut[i].data(), sheets::kClutBytes) == 0;
                if (ours)
                    std::memcpy(guest, want, sheets::kClutBytes);
            }
            applyCrate(*images, fresh);
            g_applied = images;
        }

        // Gives a sheet's CLUT its art set's mark (see the builder): an entry with alpha 0 that no texel of the sheet
        // uses (else the first with alpha 0) gets 0xA0 + the set's number in its red and `variant` in its green and
        // blue. Nothing shows it.
        void markClut(const uint8_t *payload, uint8_t *clut, uint8_t mark, uint16_t variant)
        {
            const auto put = [&](uint8_t *e) {
                e[0] = static_cast<uint8_t>(0xA0u | mark);
                e[1] = static_cast<uint8_t>(variant & 0xFFu);
                e[2] = static_cast<uint8_t>(variant >> 8);
            };
            std::vector<uint8_t> indices(static_cast<size_t>(sheets::kSheetSize) * sheets::kSheetSize);
            sheets::unswizzle(payload, indices.data());
            std::array<bool, 256> used{};
            for (uint8_t v : indices)
                used[v] = true;
            uint8_t *first = nullptr;
            for (int k = 0; k < 256; ++k)
            {
                const int slot = (k & 0xE7) | ((k & 0x08) << 1) | ((k & 0x10) >> 1); // CSM1
                uint8_t *e = clut + slot * 4;
                if (e[3] != 0u)
                    continue;
                if (!used[static_cast<size_t>(k)])
                {
                    put(e);
                    return;
                }
                if (!first)
                    first = e;
            }
            if (first)
                put(first);
        }

        // The labels' hash for markClut (16 bits: the sheets' names differ per binding).
        uint16_t labelsHash(const sheets::PromptLabels &labels)
        {
            std::string all;
            for (const std::string &l : labels.icon)
                all += l + '\x1F';
            for (const std::string &l : labels.leftStick)
                all += l + '\x1F';
            for (const std::string &l : labels.rightStick)
                all += l + '\x1F';
            const uint64_t h = fnv1a64(reinterpret_cast<const uint8_t *>(all.data()), all.size());
            return static_cast<uint16_t>(h ^ (h >> 16) ^ (h >> 32) ^ (h >> 48));
        }

        // ---- builder thread ----
        std::string labelOf(const bindings::Binding &binding)
        {
            switch (binding.type)
            {
            case bindings::Type::Key:
                return art::keyLabel(binding.code).compact;
            case bindings::Type::Mouse:
                return art::mouseLabel(binding.code).compact;
            case bindings::Type::PadButton:
                return art::padButtonLabel(binding.code).compact;
            case bindings::Type::PadAxis:
                return art::padAxisLabel(binding.code, binding.sign).compact;
            case bindings::Type::None:
            default:
                return std::string(art::kUnbound);
            }
        }

        sheets::PromptLabels labelsFor(const bindings::Snapshot &snapshot, bindings::Device device)
        {
            const bool pad = device == bindings::Device::Controller;
            const auto &set = pad ? snapshot.controller : snapshot.keys;
            sheets::PromptLabels labels;
            const auto put = [&](sheets::Icon icon, bindings::Input input) { labels.icon[icon] = labelOf(set[input]); };
            // Each cell shows its own button whatever [Game] MenuButtons says: a move slot's label is the button that
            // throws it, and Modern reworded the menu hints to the buttons that work them (menu_buttons.h).
            put(sheets::Circle, bindings::Circle);
            put(sheets::Triangle, bindings::Triangle);
            put(sheets::Square, bindings::Square);
            put(sheets::Cross, bindings::Cross);
            put(sheets::L1, bindings::L1);
            put(sheets::R1, bindings::R1);
            put(sheets::L2, bindings::L2);
            put(sheets::R2, bindings::R2);
            put(sheets::L3, bindings::L3);
            put(sheets::R3, bindings::R3);
            put(sheets::Start, bindings::Start);
            put(sheets::Select, bindings::Select);
            // Sticks: on keys an inverted T of the four direction keys (Up Right Down Left); a controller's stick keeps
            // the original stick art (all four empty).
            if (!pad)
            {
                const bindings::Input left[4] = {bindings::LUp, bindings::LRight, bindings::LDown, bindings::LLeft};
                const bindings::Input right[4] = {bindings::RUp, bindings::RRight, bindings::RDown, bindings::RLeft};
                bool anyLeft = false, anyRight = false;
                for (int d = 0; d < 4; ++d)
                {
                    anyLeft |= set[left[d]].type != bindings::Type::None;
                    anyRight |= set[right[d]].type != bindings::Type::None;
                }
                for (int d = 0; d < 4; ++d)
                {
                    if (anyLeft)
                        labels.leftStick[d] = labelOf(set[left[d]]);
                    if (anyRight)
                        labels.rightStick[d] = labelOf(set[right[d]]);
                }
            }
            return labels;
        }

        // Builder thread, about once a second while the crates are not located: look for the TIM3 header in front of
        // the crate pixels (16-byte aligned) and check the pixels are the disc's (first sight) or an image this
        // thread built (a savestate taken while patched).
        // The disc's crate, from st3/r301.dat (the room's data file holds the TIM3 uncompressed, pixels at 0xB820 on the USA
        // disc): the original even when RAM only ever shows a crate an earlier build patched (a save state taken in
        // the room keeps it; such a run never saw the disc's pixels, K Nov 2: the old letter colours stayed).
        bool captureCrateFromDisc()
        {
            if (!disc_files::open())
                return false;
            const auto file = disc_files::find("st3/r301.dat");
            std::vector<uint8_t> data;
            if (!file || !disc_files::read(*file, data))
                return false;
            const size_t need = 0x40u + crate_prompts::kPayloadBytes + crate_prompts::kClutBytes;
            for (size_t at = 0; at + need <= data.size(); at += 16u)
            {
                uint32_t magic;
                uint64_t prefix;
                std::memcpy(&magic, data.data() + at, sizeof(magic));
                std::memcpy(&prefix, data.data() + at + 0x40u, sizeof(prefix));
                if (magic != kTim3Magic || prefix != kCratePrefix)
                    continue;
                const uint8_t *pixels = data.data() + at + 0x40u;
                Sha256 sha;
                sha.update(pixels, crate_prompts::kPayloadBytes);
                if (sha.finishHex().rfind(kCrateShaPrefix, 0) != 0)
                    continue;
                std::memcpy(g_crateHeader.data(), data.data() + at, g_crateHeader.size());
                std::memcpy(g_crateOrig.data(), pixels, crate_prompts::kPayloadBytes);
                std::memcpy(g_crateOrigClut.data(), pixels + crate_prompts::kPayloadBytes, crate_prompts::kClutBytes);
                g_crateCaptured.store(true, std::memory_order_release);
                g_crateRebuild.store(true, std::memory_order_release);
                return true;
            }
            return false;
        }

        void scanCrate(const std::vector<uint64_t> &built, const std::vector<uint64_t> &builtCluts)
        {
            static bool discTried = false;
            if (!discTried && !g_crateCaptured.load(std::memory_order_acquire))
            {
                discTried = true;
                if (captureCrateFromDisc())
                    KM_INFO(Game, "button prompts: the minigame crates' original read from the disc");
            }
            constexpr uint32_t kEnd = 0x2000000u - 0x40u - static_cast<uint32_t>(crate_prompts::kPayloadBytes);
            for (uint32_t address = 0x100000u; address < kEnd; address += 16u)
            {
                uint32_t magic;
                std::memcpy(&magic, g_ram + address, sizeof(magic));
                if (magic != kTim3Magic)
                    continue;
                const uint32_t pixels = address + 0x40u;
                const uint8_t *data = g_ram + pixels;
                uint64_t prefix;
                std::memcpy(&prefix, data, sizeof(prefix));
                if (!g_crateCaptured.load(std::memory_order_acquire))
                {
                    if (prefix != kCratePrefix)
                        continue;
                    Sha256 sha;
                    sha.update(data, crate_prompts::kPayloadBytes);
                    if (sha.finishHex().rfind(kCrateShaPrefix, 0) != 0)
                        continue;
                    std::memcpy(g_crateHeader.data(), data - 0x40u, g_crateHeader.size());
                    std::memcpy(g_crateOrig.data(), data, crate_prompts::kPayloadBytes);
                    std::memcpy(g_crateOrigClut.data(), data + crate_prompts::kPayloadBytes, crate_prompts::kClutBytes);
                    g_crateCaptured.store(true, std::memory_order_release);
                    g_crateRebuild.store(true, std::memory_order_release);
                }
                else
                {
                    const uint64_t hash = fnv1a64(data, crate_prompts::kPayloadBytes);
                    bool ours = hash == fnv1a64(g_crateOrig.data(), crate_prompts::kPayloadBytes);
                    for (uint64_t h : built)
                        ours |= h == hash;
                    // Or the crate as any build patched it: the disc's TIM3 header and a CLUT that is the disc's or
                    // one built here (builds before f44adae kept the disc's CLUT).
                    if (!ours && std::memcmp(data - 0x40u, g_crateHeader.data(), g_crateHeader.size()) == 0)
                    {
                        const uint8_t *clut = data + crate_prompts::kPayloadBytes;
                        ours = std::memcmp(clut, g_crateOrigClut.data(), crate_prompts::kClutBytes) == 0;
                        const uint64_t clutHash = fnv1a64(clut, crate_prompts::kClutBytes);
                        for (uint64_t h : builtCluts)
                            ours |= h == clutHash;
                    }
                    if (!ours)
                        continue;
                    g_crateFoundHash.store(hash, std::memory_order_release);
                }
                g_crateAddress.store(pixels, std::memory_order_release);
                KM_INFO(Game, "button prompts: pirate minigame crates found at 0x" << std::hex << pixels << std::dec);
                return;
            }
        }

        // The crates' letter colours (K, Nov 2): the Xbox 360 face buttons in their own colours (any other label, e.g.
        // a shoulder button bound there, white); the Switch and Steam Deck sets draw their buttons grey, so white
        // letters; keys a warm white like the key caps.
        uint32_t crateColour(prompt_hd::Set set, const std::string &label)
        {
            if (set == prompt_hd::Set::Xbox360)
            {
                if (label == "Y")
                    return 0xFFCD28u;
                if (label == "B")
                    return 0xEB3C2Du;
                if (label == "X")
                    return 0x3278FFu;
                if (label == "A")
                    return 0x5AD73Cu;
            }
            if (set == prompt_hd::Set::Keyboard)
                return 0xF5EBD7u;
            return 0xF0F0F0u;
        }

        // The tutorials' stick sheet: id/local/us/idtutorial_us.idd's third TIM2 (file 0x4FD0; 128x64 PSMT8, the LEFT and
        // RIGHT sticks Tutorial 1 and the dash tutorial draw beside their text). With a set that has stick art the GS gets
        // the pack's sticks for it (prompt_hd::composeSticks, K Nov 3: the same side view as the stick icons); PlayStation
        // and keys keep the disc's (or the texture pack's, when it is a PNG) art. The LEFT cell is always empty: the dash
        // tutorial draws its stick icon in the text (tutorial_text.cpp; K Nov 4); with Modern the RIGHT cell is too:
        // Modern's Tutorial 1 draws both its stick icons in the text, the big one would sit under it. The game uploads the TIM2 as stored, so the replacement name
        // is the disc's (hd_textures::name8Linear; K's pack has 432ba500d8d45a46-6d99a9e671d37be4-000059d3) and the RAM
        // copy is left alone. The images live in cache/hd-sticks/<set>/, outside the folders PCSX2 lists when the GS
        // opens (an earlier session's set never applies), and each is handed over with texture_add.
        constexpr size_t kStickTim = 0x4FD0u, kStickPixels = 128u * 64u, kStickClut = 1024u;
        struct StickSheet
        {
            bool tried = false, ok = false;
            std::string name;
            hd_textures::Image disc; // the disc's art at 1x (when ours must go and the pack has none)
        };
        StickSheet g_stickSheet;
        int g_sticksShown = -1; // builder thread: what the GS has (set * 2 + Modern; -1: the disc's or the pack's)

        bool loadStickSheet()
        {
            if (g_stickSheet.tried)
                return g_stickSheet.ok;
            g_stickSheet.tried = true;
            const auto file = disc_files::open() ? disc_files::find("idtutorial_us.idd") : std::nullopt;
            std::vector<uint8_t> data;
            if (!file || !disc_files::read(*file, data) || data.size() < kStickTim + 0x40u + kStickPixels + kStickClut ||
                std::memcmp(data.data() + kStickTim, "TIM2", 4) != 0)
            {
                KM_WARN(Game, "button prompts: the tutorials' stick sheet is not where expected; their sticks stay the disc's");
                return false;
            }
            std::error_code ec;
            std::filesystem::remove_all(hd_textures::exeDirectory() / "cache" / "hd-sticks", ec); // an older build's art
            const uint8_t *pixels = data.data() + kStickTim + 0x40u;
            const uint8_t *clut = pixels + kStickPixels;
            g_stickSheet.name = hd_textures::name8Linear(pixels, clut, 128, 64);
            hd_textures::Image &disc = g_stickSheet.disc;
            disc.width = 128;
            disc.height = 64;
            disc.rgba.resize(kStickPixels * 4u);
            for (size_t i = 0; i < kStickPixels; ++i)
            {
                const int k = pixels[i];
                const int slot = (k & 0xE7) | ((k & 0x08) << 1) | ((k & 0x10) >> 1); // CSM1
                std::memcpy(&disc.rgba[i * 4u], clut + slot * 4, 4u);                 // alpha already in the PS2's scale
            }
            g_stickSheet.ok = true;
            return true;
        }

        // Builder thread, after every rebuild.
        void updateSticks(prompt_hd::Set set, bool modern)
        {
            if (!hd_textures::enabled() || !loadStickSheet())
                return;
            const bool art = set != prompt_hd::Set::PlayStation && set != prompt_hd::Set::Keyboard;
            const int want = static_cast<int>(set) * 2 + (modern ? 1 : 0);
            if (want == g_sticksShown)
                return;
            namespace fs = std::filesystem;
            const fs::path dir = hd_textures::exeDirectory() / "cache" / "hd-sticks";
            std::error_code ec;
            const fs::path path = dir / ("look" + std::to_string(want)) / (g_stickSheet.name + ".png");
            hd_textures::Image image;
            if (!art)
            {
                // The pack's art (a PNG; a DDS can't be read here) or the disc's, the LEFT cell (and with Modern the
                // RIGHT one) cleared.
                const fs::path pack = hd_textures::packFile(g_stickSheet.name);
                std::string ext = pack.extension().string();
                for (char &c : ext)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (pack.empty() || ext != ".png" || !hd_textures::loadPng(pack, image) || image.width < 2 || image.height < 1)
                    image = g_stickSheet.disc;
                const int half = image.width / 2;
                for (int y = 0; y < image.height; ++y)
                {
                    uint8_t *row = &image.rgba[static_cast<size_t>(y) * image.width * 4u];
                    std::fill_n(row, static_cast<size_t>(half) * 4u, uint8_t{0});
                    if (modern)
                        std::fill_n(row + static_cast<size_t>(half) * 4u, static_cast<size_t>(image.width - half) * 4u, uint8_t{0});
                }
            }
            if (!fs::exists(path, ec) &&
                ((art && !prompt_hd::composeSticks(set, true, modern, image)) || (!fs::create_directories(path.parent_path(), ec) && ec) ||
                 !hd_textures::savePng(path, image)))
                return;
            const std::u8string native = path.u8string();
            GSPcsx2Backend::SetOption("texture_add", std::string(native.begin(), native.end()).c_str());
            g_sticksShown = want;
            KM_INFO(Game, "button prompts: the tutorials' sticks: " << (art ? prompt_hd::setName(set) : "the original art")
                                                                    << (modern ? ", no stick pictures (Modern)" : ", no LEFT stick picture"));
        }

        void builderLoop()
        {
            std::vector<uint64_t> builtCrates, builtCrateCluts;
            uint32_t ticks = 0u;
            uint32_t lastGeneration = 0u;
            bindings::Device lastDevice = bindings::Device::Keys;
            bindings::PadStyle lastStyle = bindings::PadStyle::Unknown;
            bool lastModern = false;
            bool built = false;
            uint64_t version = 0u;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!g_captured.load(std::memory_order_acquire))
                    continue;
                const uint32_t generation = bindings::generation();
                if (generation == 0u)
                    continue; // bindings not published yet
                // KAMITE_PROMPTS_DEVICE=keys|xbox (tests, screenshots): that art whatever was used last (hidden runs see
                // the player's connected controller).
                static const char forced = [] { const char *v = std::getenv("KAMITE_PROMPTS_DEVICE"); return v ? *v : '\0'; }();
                const bindings::Device device = forced == 'k'   ? bindings::Device::Keys
                                                : forced == 'x' ? bindings::Device::Controller
                                                                : bindings::lastDevice();
                const bindings::PadStyle style = forced == 'x' ? bindings::PadStyle::Xbox : bindings::padStyle();
                if (++ticks % 10u == 0u && g_crateAddress.load(std::memory_order_acquire) == 0u)
                    scanCrate(builtCrates, builtCrateCluts);
                const bool crateNew = g_crateRebuild.exchange(false, std::memory_order_acq_rel);
                const bool setNew = g_promptsRebuild.exchange(false, std::memory_order_acq_rel);
                const bool modern = modern::enabled();
                if (built && !crateNew && !setNew && generation == lastGeneration && device == lastDevice && style == lastStyle &&
                    modern == lastModern)
                    continue;
                lastModern = modern;
                built = true;
                lastGeneration = generation;
                lastDevice = device;
                lastStyle = style;

                auto images = std::make_shared<Images>();
                images->version = ++version;
                // The art set: keys get the keyboard's; a controller the one [Game] ButtonPrompts names, or with Auto the
                // one its type asks for (PlayStation pads keep the game's own art).
                prompt_hd::Set set = prompt_hd::Set::Keyboard;
                if (device == bindings::Device::Controller)
                {
                    switch (prompt_hd::choice())
                    {
                    case prompt_hd::Choice::Xbox: set = prompt_hd::Set::Xbox360; break;
                    case prompt_hd::Choice::PlayStation: set = prompt_hd::Set::PlayStation; break;
                    case prompt_hd::Choice::Nintendo: set = prompt_hd::Set::Switch; break;
                    case prompt_hd::Choice::Steam: set = prompt_hd::Set::SteamDeck; break;
                    case prompt_hd::Choice::Auto:
                    default:
                        set = style == bindings::PadStyle::PlayStation ? prompt_hd::Set::PlayStation
                              : style == bindings::PadStyle::Nintendo  ? prompt_hd::Set::Switch
                              : style == bindings::PadStyle::Steam     ? prompt_hd::Set::SteamDeck
                                                                       : prompt_hd::Set::Xbox360;
                        break;
                    }
                }
                const bool playStation = set == prompt_hd::Set::PlayStation;
                const sheets::PromptLabels labels = labelsFor(bindings::snapshot(), device);
                {
                    std::lock_guard<std::mutex> lock(g_menuArtMutex);
                    // The menus' buttons by their place on the pad (menu_buttons.h "Pinned menus"): cross = the south
                    // button's label, circle the east's, triangle the north's. Sheet icons and text icons share numbers.
                    const menu_buttons::Places places =
                        menu_buttons::placesFor(bindings::snapshot(), device == bindings::Device::Controller);
                    const auto placed = [&](uint8_t input, sheets::Icon fallback) {
                        static constexpr int kSheetIcon[12] = {sheets::Cross, sheets::Circle, sheets::Square, sheets::Triangle,
                                                               sheets::L1,    sheets::R1,     sheets::L2,     sheets::R2,
                                                               sheets::L3,    sheets::R3,     sheets::Start,  sheets::Select};
                        return labels.icon[input < 12u ? kSheetIcon[input] : fallback];
                    };
                    // LB / RB by their place too (the OPTIONS categories: the physical bumpers, the menu layer).
                    const bool pad = device == bindings::Device::Controller;
                    const auto shoulder = [&](uint8_t place) {
                        const uint8_t input = pad ? menu_buttons::inputOnPlace(bindings::snapshot(), place) : place;
                        return input == 0xFFu ? place : input;
                    };
                    g_menuArt = MenuArt{true, set, placed(places.south, sheets::Cross), placed(places.east, sheets::Circle),
                                        placed(places.north, sheets::Triangle), placed(shoulder(4u), sheets::L1),
                                        placed(shoulder(5u), sheets::R1)};
                }
                std::string summary = std::string(prompt_hd::setName(set)) + " prompts";
                // Every set starts from the disc's art. PlayStation keeps it; the others get full-resolution images
                // (prompt_hd) of the pack's buttons over it, under the sheets' own texture names. The CLUT's mark makes the
                // names: the set, and a hash of the labels (a rebinding gets new images), so a texture pack's image for
                // the original sheets (often Xbox buttons) never applies over them.
                for (int i = 0; i < kSheetCount; ++i)
                {
                    images->payload[i] = g_origPayload[i];
                    images->clut[i] = g_origClut[i];
                    images->patched[i] = true;
                }
                if (!playStation)
                    summary += std::string(device == bindings::Device::Controller ? " (controller" : " (keys") + ": circle " +
                               labels.icon[sheets::Circle] + ", triangle " + labels.icon[sheets::Triangle] + ", square " +
                               labels.icon[sheets::Square] + ", cross " + labels.icon[sheets::Cross] + ", start " +
                               labels.icon[sheets::Start] + ")";
                // The style list's arrows in the icon sheet's empty cells (every set, PlayStation's too).
                if (images->patched[0] && !sheets::addStyleArrows(images->payload[0].data(), images->clut[0].data()))
                    summary += " (style arrows not added: the icon sheet's empty cells are not empty)";
                // The set's mark: the red of the first CLUT entry with alpha 0 (nothing draws it) gets the set's number,
                // so each set's sheets have their own texture names, then the full-resolution images for them.
                bool added = false;
                for (int i = 0; i < kSheetCount; ++i)
                {
                    if (!images->patched[i])
                        continue;
                    markClut(images->payload[i].data(), images->clut[i].data(), static_cast<uint8_t>(static_cast<int>(set) + 1),
                             playStation ? 0u : labelsHash(labels));
                    if (playStation || !hd_textures::enabled())
                        continue;
                    const std::string name = hd_textures::name8(images->payload[i].data(), 64, 64, images->clut[i].data(),
                                                                sheets::kSheetSize, sheets::kSheetSize);
                    const auto path = hd_textures::generatedDir() / (name + ".png");
                    std::error_code ec;
                    if (!std::filesystem::exists(path, ec))
                    {
                        hd_textures::Image hd;
                        if (!prompt_hd::compose(set, i, images->payload[i].data(), images->clut[i].data(), labels, hd) ||
                            !hd_textures::savePng(path, hd))
                            continue;
                    }
                    const std::u8string native = path.u8string();
                    GSPcsx2Backend::SetOption("texture_add", std::string(native.begin(), native.end()).c_str());
                    added = true;
                }
                if (!playStation && !added)
                    summary += " (no full-resolution images: the disc's art shows)";
                if (added)
                {
                    summary += ", full resolution";
                    // The GS adds the images at its next vsync: they are there before the sheets change.
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!playStation)
                {
                    if (g_crateCaptured.load(std::memory_order_acquire))
                    {
                        // The cannon game is a minigame: it sees the controller's default layout (the menu layer, K Nov
                        // 14), so the crates name the default buttons in the set; keys keep their bindings.
                        const bool padCrates = device == bindings::Device::Controller;
                        const std::array<std::string, 4> crateLabels =
                            padCrates ? std::array<std::string, 4>{art::padButtonLabel(ps2_host_gamepads::North).compact,
                                                                   art::padButtonLabel(ps2_host_gamepads::East).compact,
                                                                   art::padButtonLabel(ps2_host_gamepads::West).compact,
                                                                   art::padButtonLabel(ps2_host_gamepads::South).compact}
                                      : std::array<std::string, 4>{labels.icon[sheets::Triangle], labels.icon[sheets::Circle],
                                                                   labels.icon[sheets::Square], labels.icon[sheets::Cross]};
                        std::array<uint32_t, 4> crateColours{};
                        for (int c = 0; c < 4; ++c)
                            crateColours[c] = crateColour(set, crateLabels[c]);
                        hd_textures::Image crateHd;
                        const bool wantHd = hd_textures::enabled();
                        images->cratePatched = crate_prompts::build(g_crateOrig.data(), g_crateOrigClut.data(), crateLabels, crateColours,
                                                                    images->crate.data(), images->crateClut.data(),
                                                                    wantHd ? &crateHd : nullptr);
                        bool crateHdAdded = false;
                        if (images->cratePatched)
                        {
                            images->crateHash = fnv1a64(images->crate.data(), crate_prompts::kPayloadBytes);
                            builtCrates.push_back(images->crateHash);
                            builtCrateCluts.push_back(fnv1a64(images->crateClut.data(), crate_prompts::kClutBytes));
                            if (wantHd && crateHd.width > 0)
                            {
                                const auto path = hd_textures::generatedDir() /
                                                  (hd_textures::name4(images->crate.data(), images->crateClut.data()) + ".png");
                                std::error_code ec;
                                if (std::filesystem::exists(path, ec) || hd_textures::savePng(path, crateHd))
                                {
                                    const std::u8string native = path.u8string();
                                    GSPcsx2Backend::SetOption("texture_add", std::string(native.begin(), native.end()).c_str());
                                    crateHdAdded = true;
                                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                }
                            }
                        }
                        summary += images->cratePatched ? (crateHdAdded ? "; minigame crates too (full resolution)" : "; minigame crates too")
                                                        : "; minigame crates kept: build failed";
                    }
                }
                updateSticks(set, modern);
                for (int i = 0; i < kSheetCount; ++i)
                    if (images->patched[i])
                        images->hash[i] = fnv1a64(images->payload[i].data(), sheets::kPayloadBytes);
                {
                    std::lock_guard<std::mutex> lock(g_imagesMutex);
                    g_images = std::move(images);
                }
                KM_INFO(Game, "button prompts: " << summary);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *value = std::getenv("KAMITE_PROMPTS"); value && *value == '0')
        {
            KM_INFO(Game, "button prompts: off (KAMITE_PROMPTS=0), PS2 art");
            return;
        }
        g_ram = runtime.memory().getRDRAM();
        ps2x::pnach::addFrameHook(&onVblank);
        std::thread(builderLoop).detach();
        KM_INFO(Game, "button prompts: on (the bound keys replace the PS2 button art once the sheets load)");
    }

    void requestRebuild()
    {
        g_promptsRebuild.store(true, std::memory_order_release);
    }

    MenuArt menuArt()
    {
        std::lock_guard<std::mutex> lock(g_menuArtMutex);
        return g_menuArt;
    }
}
