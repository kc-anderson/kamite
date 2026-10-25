#include "godhand/button_prompts.h"

#include "godhand/button_prompt_sheets.h"
#include "godhand/crate_prompts.h"
#include "godhand/log.h"
#include "godhand/menu_buttons.h"
#include "godhand/prompt_art.h"
#include "godhand/sha256.h"
#include "ps2_host_input.h"
#include "runtime/ps2_pnach.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace godhand::button_prompts
{
    namespace
    {
        namespace sheets = godhand::prompt_sheets;
        namespace art = godhand::prompt_art;
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

        // ---- pirate minigame crates (crate_prompts.h): a TIM3 file in the room heap, found by scanning ----
        using CratePayload = std::array<uint8_t, crate_prompts::kPayloadBytes>;
        constexpr uint32_t kTim3Magic = 0x334D4954u;          // "TIM3", 0x40 bytes before the pixels
        constexpr uint64_t kCratePrefix = 0x0221111112221111ull; // first 8 payload bytes (little endian)
        constexpr const char *kCrateShaPrefix = "ac15f235994f08f3";
        CratePayload g_crateOrig;                              // builder thread writes once, then g_crateCaptured
        std::atomic<bool> g_crateCaptured{false};
        std::atomic<uint32_t> g_crateAddress{0u};              // guest address of the pixels, 0 = not located
        std::atomic<bool> g_crateRebuild{false};

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
            uint64_t crateHash = 0u;
        };
        std::mutex g_imagesMutex;
        std::shared_ptr<const Images> g_images;

        // ---- EE thread only ----
        uint8_t *g_ram = nullptr;
        std::shared_ptr<const Images> g_applied;
        std::vector<uint64_t> g_known[kSheetCount]; // hashes of every sheet content this code put there (or found there)
        std::vector<uint64_t> g_crateKnown;         // the same for the crate texture
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

        // First sight of both sheets: keep the original bytes, but only when they are the disc's (a savestate taken
        // while patched would otherwise be taken for the original).
        bool capture(uint32_t core)
        {
            static uint32_t nextTry = 0u;
            if (g_vblanks < nextTry)
                return false;
            nextTry = g_vblanks + 60u;
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
                        GH_WARN(Game, "button prompts: the " << kPlaces[i].name << " sheet is not the disc's; prompts keep the PS2 art");
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
            GH_INFO(Game, "button prompts: sheets found at core 0x" << std::hex << core << std::dec << "; drawing the bound keys");
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
            uint8_t *guest = g_ram + address;
            const uint8_t *want = images.cratePatched ? images.crate.data() : g_crateOrig.data();
            if (std::memcmp(guest, want, crate_prompts::kPayloadBytes) == 0)
                return;
            if (!crateKnown(fnv1a64(guest, crate_prompts::kPayloadBytes)))
            {
                g_crateAddress.store(0u, std::memory_order_release);
                return;
            }
            std::memcpy(guest, want, crate_prompts::kPayloadBytes);
        }

        // Every vblank (EE thread, ps2x::pnach frame hook): copy the newest built image over the sheets when it changed,
        // and every 30 vblanks check that the sheets still hold what was put there (a savestate load or a reload of
        // core_usa.bin brings other bytes back). Bytes this code does not know are never overwritten.
        // [Game] MenuButtons=modern: the hint strings in the core block (menu_buttons::textPatches), checked against
        // the disc's words first (another language's core or a changed one is left alone, once with a warning).
        void applyText(uint32_t core)
        {
            if (!menu_buttons::modern())
                return;
            static bool warned = false;
            for (const menu_buttons::TextPatch &patch : menu_buttons::textPatches())
            {
                uint8_t *guest = g_ram + core + patch.offset;
                const size_t bytes = patch.original.size() * 2u;
                if (core + patch.offset + bytes > 0x2000000u)
                    continue;
                if (std::memcmp(guest, patch.modern.data(), bytes) == 0)
                    continue;
                if (std::memcmp(guest, patch.original.data(), bytes) != 0)
                {
                    if (!warned)
                    {
                        warned = true;
                        GH_WARN(Game, "menu buttons: the hint text at core+0x" << std::hex << patch.offset << std::dec
                                                                               << " is not the US disc's; its wording is left alone");
                    }
                    continue;
                }
                std::memcpy(guest, patch.modern.data(), bytes);
            }
        }

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
                if (!known(i, fnv1a64(guest, sheets::kPayloadBytes)))
                {
                    if (!g_warnedForeign[i])
                    {
                        g_warnedForeign[i] = true;
                        GH_WARN(Game, "button prompts: the " << kPlaces[i].name << " sheet holds unknown bytes; left alone");
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
                const bool ours = std::memcmp(guest, g_origClut[i].data(), sheets::kClutBytes) == 0 ||
                                  (g_applied && std::memcmp(guest, g_applied->clut[i].data(), sheets::kClutBytes) == 0) ||
                                  std::memcmp(guest, images->clut[i].data(), sheets::kClutBytes) == 0;
                if (ours)
                    std::memcpy(guest, want, sheets::kClutBytes);
            }
            applyCrate(*images, fresh);
            applyText(core);
            g_applied = images;
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
            // Modern menu buttons: the game's circle is the triangle binding and the other way round (menu_buttons.h).
            const bool traded = menu_buttons::modern();
            put(sheets::Circle, traded ? bindings::Triangle : bindings::Circle);
            put(sheets::Triangle, traded ? bindings::Circle : bindings::Triangle);
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
            // A controller's A/B/X/Y in an Xbox pad's colours (the PS2 art is coloured symbols on grey caps; white
            // letters read as a Switch pad). Keys stay white.
            if (pad)
            {
                for (const sheets::Icon icon : {sheets::Circle, sheets::Triangle, sheets::Square, sheets::Cross})
                {
                    const std::string &l = labels.icon[icon];
                    labels.tint[icon] = l == "A"   ? sheets::Tint::Green
                                        : l == "B" ? sheets::Tint::Red
                                        : l == "X" ? sheets::Tint::Blue
                                        : l == "Y" ? sheets::Tint::Yellow
                                                   : sheets::Tint::White;
                }
            }
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
        void scanCrate(const std::vector<uint64_t> &built)
        {
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
                    std::memcpy(g_crateOrig.data(), data, crate_prompts::kPayloadBytes);
                    g_crateCaptured.store(true, std::memory_order_release);
                    g_crateRebuild.store(true, std::memory_order_release);
                }
                else
                {
                    const uint64_t hash = fnv1a64(data, crate_prompts::kPayloadBytes);
                    bool ours = hash == fnv1a64(g_crateOrig.data(), crate_prompts::kPayloadBytes);
                    for (uint64_t h : built)
                        ours |= h == hash;
                    if (!ours)
                        continue;
                }
                g_crateAddress.store(pixels, std::memory_order_release);
                GH_INFO(Game, "button prompts: pirate minigame crates found at 0x" << std::hex << pixels << std::dec);
                return;
            }
        }

        void builderLoop()
        {
            std::vector<uint64_t> builtCrates;
            uint32_t ticks = 0u;
            uint32_t lastGeneration = 0u;
            bindings::Device lastDevice = bindings::Device::Keys;
            bindings::PadStyle lastStyle = bindings::PadStyle::Unknown;
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
                // GODHAND_PROMPTS_DEVICE=keys|xbox (tests, screenshots): that art whatever was used last (hidden runs see
                // the player's connected controller).
                static const char forced = [] { const char *v = std::getenv("GODHAND_PROMPTS_DEVICE"); return v ? *v : '\0'; }();
                const bindings::Device device = forced == 'k'   ? bindings::Device::Keys
                                                : forced == 'x' ? bindings::Device::Controller
                                                                : bindings::lastDevice();
                const bindings::PadStyle style = forced == 'x' ? bindings::PadStyle::Xbox : bindings::padStyle();
                if (++ticks % 10u == 0u && g_crateAddress.load(std::memory_order_acquire) == 0u)
                    scanCrate(builtCrates);
                const bool crateNew = g_crateRebuild.exchange(false, std::memory_order_acq_rel);
                if (built && !crateNew && generation == lastGeneration && device == lastDevice && style == lastStyle)
                    continue;
                built = true;
                lastGeneration = generation;
                lastDevice = device;
                lastStyle = style;

                auto images = std::make_shared<Images>();
                images->version = ++version;
                const bool playStation = device == bindings::Device::Controller && style == bindings::PadStyle::PlayStation;
                for (int i = 0; i < kSheetCount; ++i)
                    images->clut[i] = g_origClut[i];
                std::string summary = playStation ? "PlayStation controller: original art" : "";
                if (playStation && menu_buttons::modern())
                {
                    // Modern menu buttons on a PlayStation pad: the circle cell shows the triangle and the other way
                    // round (the floating action prompt keeps its shapes: their rects differ in size).
                    images->patched[0] = sheets::swapIconCells(g_origPayload[0].data(), sheets::Circle, sheets::Triangle, images->payload[0].data());
                    summary += images->patched[0] ? ", circle and triangle traded" : " (trade failed: text icons kept)";
                }
                if (!playStation)
                {
                    const sheets::PromptLabels labels = labelsFor(bindings::snapshot(), device);
                    images->patched[0] = sheets::buildIconSheet(g_origPayload[0].data(), g_origClut[0].data(), labels, images->payload[0].data(),
                                                                images->clut[0].data());
                    images->patched[1] = sheets::buildActionSheet(g_origPayload[1].data(), g_origClut[1].data(), labels, images->payload[1].data(),
                                                                  images->clut[1].data());
                    summary = std::string(device == bindings::Device::Controller ? "controller" : "keys") + ": circle " +
                              labels.icon[sheets::Circle] + ", triangle " + labels.icon[sheets::Triangle] + ", square " +
                              labels.icon[sheets::Square] + ", cross " + labels.icon[sheets::Cross] + ", start " +
                              labels.icon[sheets::Start] + (images->patched[0] ? "" : " (text icons kept: build failed)") +
                              (images->patched[1] ? "" : " (action prompt kept: build failed)");
                    if (g_crateCaptured.load(std::memory_order_acquire))
                    {
                        const std::array<std::string, 4> crateLabels{labels.icon[sheets::Triangle], labels.icon[sheets::Circle],
                                                                     labels.icon[sheets::Square], labels.icon[sheets::Cross]};
                        images->cratePatched = crate_prompts::build(g_crateOrig.data(), crateLabels, images->crate.data());
                        if (images->cratePatched)
                        {
                            images->crateHash = fnv1a64(images->crate.data(), crate_prompts::kPayloadBytes);
                            builtCrates.push_back(images->crateHash);
                        }
                        summary += images->cratePatched ? "; minigame crates too" : "; minigame crates kept: build failed";
                    }
                }
                for (int i = 0; i < kSheetCount; ++i)
                    if (images->patched[i])
                        images->hash[i] = fnv1a64(images->payload[i].data(), sheets::kPayloadBytes);
                {
                    std::lock_guard<std::mutex> lock(g_imagesMutex);
                    g_images = std::move(images);
                }
                GH_INFO(Game, "button prompts: " << summary);
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *value = std::getenv("GODHAND_PROMPTS"); value && *value == '0')
        {
            GH_INFO(Game, "button prompts: off (GODHAND_PROMPTS=0), PS2 art");
            return;
        }
        g_ram = runtime.memory().getRDRAM();
        ps2x::pnach::addFrameHook(&onVblank);
        std::thread(builderLoop).detach();
        GH_INFO(Game, "button prompts: on (the bound keys replace the PS2 button art once the sheets load)");
    }
}
