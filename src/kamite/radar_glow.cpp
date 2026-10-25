// The radar's "original controls" glow. See radar_glow.h.
//
// The radar texture (128x256, 8-bit): the disc (centre 53, 53 in texel units; its dark floor out to 0.765 of the
// disc's radius, then the brown rim) in the top left, the blips on the right, the sweep rings below (the "rope" rings:
// texture packs erase them). Its CLUT uses all 256 entries.
//
// Full-resolution route (a pack replaces the radar): the glow's version keeps every texel and changes the red of one
// CLUT entry that has alpha 0 (nothing shows it), which gives it a name of its own; its image is the pack's radar
// with the glow painted over the floor's outer band (the disc measured on that image). PS2-resolution route: the eight
// least-used opaque CLUT entries (one to three texels each, inside the orange and tan blips, which move to their
// nearest other colours) become a ramp from the floor's colour to the glow's, and the band is painted with it.
#include "kamite/radar_glow.h"

#include "kamite/disc_files.h"
#include "kamite/hd_textures.h"
#include "kamite/log.h"
#include "kamite/modern_controls.h"
#include "kamite/sha256.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace kamite::radar_glow
{
    namespace
    {
        constexpr uint32_t kCorePointer = 0x7479F8u;
        constexpr uint32_t kData = 0x11C830u, kClut = 0x124930u; // from the core block's base
        constexpr int kWidth = 128, kHeight = 256;
        constexpr size_t kPayloadBytes = static_cast<size_t>(kWidth) * kHeight, kClutBytes = 1024u;
        constexpr const char *kDataSha = "16626ddaf1f7eb68", *kClutSha = "17387e96aadc1529"; // the US disc's
        // The disc's floor ends at this share of the disc's radius (measured on the PS2 art: 36 of 48 texels; on a
        // pack's 4x art: 146 of 191.5 px); the glow fills the floor's outer third and fades out just past its edge.
        constexpr float kFloorShare = 0.765f, kInnerShare = 0.66f, kFadeShare = 0.05f;
        constexpr float kStrength = 0.85f;
        constexpr float kGlow[3] = {255.0f, 214.0f, 72.0f}; // warm yellow
        constexpr int kRampEntries = 8;

        uint8_t *g_ram = nullptr;
        bool g_captured = false, g_built = false, g_warned = false, g_fromDisc = false;
        std::vector<uint8_t> g_origData, g_origClut, g_glowData, g_glowClut;
        std::vector<uint16_t> g_offsets; // texel (row-major) -> byte in the payload

        uint32_t rd32(uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, g_ram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        // The payload is the texture as uploaded: PSMCT32 64x128 words holding the PSMT8 texels.
        void makeOffsets()
        {
            std::vector<uint16_t> inv(kPayloadBytes / 4u);
            for (uint32_t y = 0; y < 128u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                    inv[GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u] = static_cast<uint16_t>(y * 64u + x);
            g_offsets.resize(kPayloadBytes);
            for (uint32_t y = 0; y < static_cast<uint32_t>(kHeight); ++y)
            {
                for (uint32_t x = 0; x < static_cast<uint32_t>(kWidth); ++x)
                {
                    const uint32_t a = GSPSMT8::addrPSMT8(0u, 2u, x, y);
                    g_offsets[y * kWidth + x] = static_cast<uint16_t>(inv[a >> 2u] * 4u + (a & 3u));
                }
            }
        }

        int clutSlot(int index) // CSM1: 16x16 CT32 with bits 3 and 4 of the index swapped
        {
            return (index & 0xE7) | ((index & 0x08) << 1) | ((index & 0x10) >> 1);
        }

        // The disc in an image of the radar (any scale): the box of its near-opaque pixels in the top-left part of
        // the texture (the disc is inside x < 0.86 w, y < 0.41 h; the blips are to its right, the rings below).
        struct Disc
        {
            float cx = 0.0f, cy = 0.0f, radius = 0.0f;
        };
        template <typename AlphaAt>
        Disc findDisc(int width, int height, AlphaAt alphaAt)
        {
            const int w = static_cast<int>(static_cast<float>(width) * 0.86f), h = static_cast<int>(static_cast<float>(height) * 0.41f);
            int maxAlpha = 0;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    maxAlpha = std::max(maxAlpha, alphaAt(x, y));
            int x0 = w, x1 = -1, y0 = h, y1 = -1;
            for (int y = 0; y < h; ++y)
            {
                for (int x = 0; x < w; ++x)
                {
                    if (static_cast<float>(alphaAt(x, y)) < static_cast<float>(maxAlpha) * 0.9f)
                        continue;
                    x0 = std::min(x0, x);
                    x1 = std::max(x1, x);
                    y0 = std::min(y0, y);
                    y1 = std::max(y1, y);
                }
            }
            Disc disc;
            if (x1 < x0 || y1 < y0)
                return disc;
            disc.cx = static_cast<float>(x0 + x1 + 1) * 0.5f;
            disc.cy = static_cast<float>(y0 + y1 + 1) * 0.5f;
            disc.radius = static_cast<float>((x1 - x0 + 1) + (y1 - y0 + 1)) * 0.25f;
            return disc;
        }

        // The glow's share at distance r from the disc's centre (0 outside the band).
        float glowWeight(float r, const Disc &disc)
        {
            const float floorR = kFloorShare * disc.radius, inner = kInnerShare * floorR, fadeEnd = floorR + kFadeShare * disc.radius;
            if (r < inner || r >= fadeEnd)
                return 0.0f;
            if (r > floorR)
            {
                const float f = 1.0f - (r - floorR) / (fadeEnd - floorR);
                return kStrength * f * f;
            }
            const float t = (r - inner) / (floorR - inner), s = t * t * (3.0f - 2.0f * t);
            return kStrength * std::pow(s, 1.4f);
        }

        // PS2-resolution glow: texels and CLUT.
        void buildNative()
        {
            std::vector<uint8_t> tex(kPayloadBytes);
            for (size_t i = 0; i < kPayloadBytes; ++i)
                tex[i] = g_origData[g_offsets[i]];
            std::array<std::array<int, 4>, 256> pal{};
            for (int k = 0; k < 256; ++k)
                for (int c = 0; c < 4; ++c)
                    pal[static_cast<size_t>(k)][static_cast<size_t>(c)] = g_origClut[static_cast<size_t>(clutSlot(k) * 4 + c)];
            const Disc disc = findDisc(kWidth, kHeight, [&](int x, int y) { return pal[tex[static_cast<size_t>(y * kWidth + x)]][3]; });
            std::array<int, 256> count{};
            for (uint8_t v : tex)
                ++count[v];

            // The ramp's entries: the least-used opaque ones (ties by index).
            std::vector<int> order;
            for (int k = 0; k < 256; ++k)
                if (pal[static_cast<size_t>(k)][3] >= 0x80)
                    order.push_back(k);
            std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return count[static_cast<size_t>(a)] < count[static_cast<size_t>(b)]; });
            std::array<bool, 256> ramp{};
            for (int i = 0; i < kRampEntries && i < static_cast<int>(order.size()); ++i)
                ramp[static_cast<size_t>(order[static_cast<size_t>(i)])] = true;

            const auto nearest = [&](const float *col, bool withRamp) {
                int best = 0;
                float bestD = 1e30f;
                for (int k = 0; k < 256; ++k)
                {
                    const auto &p = pal[static_cast<size_t>(k)];
                    if (p[3] < 0x80 || (ramp[static_cast<size_t>(k)] && !withRamp))
                        continue;
                    const float d = (p[0] - col[0]) * (p[0] - col[0]) + (p[1] - col[1]) * (p[1] - col[1]) + (p[2] - col[2]) * (p[2] - col[2]);
                    if (d < bestD)
                    {
                        bestD = d;
                        best = k;
                    }
                }
                return best;
            };

            // The ramp's old texels move to their nearest other colour.
            for (size_t i = 0; i < kPayloadBytes; ++i)
            {
                if (!ramp[tex[i]])
                    continue;
                const auto &p = pal[tex[i]];
                const float col[3] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
                tex[i] = static_cast<uint8_t>(nearest(col, false));
            }
            // The ramp: the floor's colour blended toward the glow, in kRampEntries steps up to the full strength.
            const int floorX = static_cast<int>(disc.cx - 0.3f * disc.radius), floorY = static_cast<int>(disc.cy + 0.3f * disc.radius);
            const auto &floor = pal[tex[static_cast<size_t>(std::clamp(floorY, 0, kHeight - 1) * kWidth + std::clamp(floorX, 0, kWidth - 1))]];
            std::vector<int> rampIndex;
            for (int k = 0; k < 256; ++k)
                if (ramp[static_cast<size_t>(k)])
                    rampIndex.push_back(k);
            g_glowClut = g_origClut;
            for (size_t i = 0; i < rampIndex.size(); ++i)
            {
                const float w = kStrength * (static_cast<float>(i) + 1.0f) / static_cast<float>(rampIndex.size());
                uint8_t *e = g_glowClut.data() + clutSlot(rampIndex[i]) * 4;
                for (int c = 0; c < 3; ++c)
                {
                    const float v = static_cast<float>(floor[static_cast<size_t>(c)]) * (1.0f - w) + kGlow[c] * w;
                    e[c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 255.0f)));
                    pal[static_cast<size_t>(rampIndex[i])][static_cast<size_t>(c)] = e[c];
                }
                e[3] = 0x80;
                pal[static_cast<size_t>(rampIndex[i])][3] = 0x80;
            }
            // The band.
            for (int y = 0; y < kHeight; ++y)
            {
                for (int x = 0; x < kWidth; ++x)
                {
                    const float r = std::hypot(static_cast<float>(x) + 0.5f - disc.cx, static_cast<float>(y) + 0.5f - disc.cy);
                    const float w = glowWeight(r, disc);
                    if (w <= 0.0f)
                        continue;
                    uint8_t &v = tex[static_cast<size_t>(y * kWidth + x)];
                    const auto &p = pal[v];
                    if (p[3] < 0x80)
                        continue;
                    float col[3];
                    for (int c = 0; c < 3; ++c)
                        col[c] = static_cast<float>(p[static_cast<size_t>(c)]) * (1.0f - w) + kGlow[c] * w;
                    v = static_cast<uint8_t>(nearest(col, true));
                }
            }
            g_glowData.assign(kPayloadBytes, 0u);
            for (size_t i = 0; i < kPayloadBytes; ++i)
                g_glowData[g_offsets[i]] = tex[i];
        }

        // Full-resolution glow over the pack's radar. False when the pack has no PNG for the radar (a .dds is left
        // alone) or the image cannot be written; the PS2-resolution route is used then.
        bool buildFull()
        {
            if (!hd_textures::enabled())
                return false;
            const std::string original = hd_textures::name8(g_origData.data(), 64, 128, g_origClut.data(), kWidth, kHeight);
            const auto packImage = hd_textures::packFile(original);
            if (packImage.empty())
            {
                KM_INFO(Game, "radar glow: the texture pack has no image for the radar (" << original << "); the glow is drawn at the PS2's resolution");
                return false;
            }
            hd_textures::Image image;
            if (!hd_textures::loadPng(packImage, image))
            {
                KM_INFO(Game, "radar glow: the pack's radar is not a PNG; the glow is drawn at the PS2's resolution");
                return false;
            }
            // The marked CLUT: the red of the first entry with alpha 0 (in index order) changed by one.
            std::vector<uint8_t> marked = g_origClut;
            bool markedOne = false;
            for (int k = 0; k < 256 && !markedOne; ++k)
            {
                uint8_t *e = marked.data() + clutSlot(k) * 4;
                if (e[3] != 0u)
                    continue;
                e[0] ^= 1u;
                markedOne = true;
            }
            if (!markedOne)
                return false;
            const std::string glowName = hd_textures::name8(g_origData.data(), 64, 128, marked.data(), kWidth, kHeight);

            const auto alphaAt = [&](int x, int y) { return static_cast<int>(image.rgba[(static_cast<size_t>(y) * image.width + x) * 4u + 3u]); };
            const Disc disc = findDisc(image.width, image.height, alphaAt);
            if (disc.radius < 8.0f)
                return false;
            for (int y = 0; y < image.height; ++y)
            {
                for (int x = 0; x < image.width; ++x)
                {
                    const float w = glowWeight(std::hypot(static_cast<float>(x) + 0.5f - disc.cx, static_cast<float>(y) + 0.5f - disc.cy), disc);
                    if (w <= 0.0f)
                        continue;
                    uint8_t *p = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4u];
                    for (int c = 0; c < 3; ++c)
                        p[c] = static_cast<uint8_t>(std::lround(std::clamp(static_cast<float>(p[c]) * (1.0f - w) + kGlow[c] * w, 0.0f, 255.0f)));
                }
            }
            const auto out = hd_textures::generatedDir() / (glowName + ".png");
            if (!hd_textures::savePng(out, image))
            {
                KM_WARN(Game, "radar glow: could not write the full-resolution glow image; drawn at the PS2's resolution");
                return false;
            }
            g_glowData = g_origData;
            g_glowClut = std::move(marked);
            KM_INFO(Game, "radar glow: full resolution, over the pack's radar (" << image.width << "x" << image.height << ", disc at "
                                                                                 << disc.cx << "," << disc.cy << " r " << disc.radius << ")");
            return true;
        }

        void build()
        {
            makeOffsets();
            if (!buildFull())
                buildNative();
            g_built = true;
        }

        bool shaMatches(const uint8_t *data, const uint8_t *clut)
        {
            Sha256 sd, sc;
            sd.update(data, kPayloadBytes);
            sc.update(clut, kClutBytes);
            return sd.finishHex().rfind(kDataSha, 0) == 0 && sc.finishHex().rfind(kClutSha, 0) == 0;
        }

        // Before the first frame: the radar from the disc's core_usa.bin (the block in RAM is the file), so the
        // full-resolution image exists when PCSX2 lists its replacement folders.
        void prepareFromDisc()
        {
            if (!disc_files::open())
                return;
            const auto core = disc_files::find("core_usa.bin");
            std::vector<uint8_t> file;
            if (!core || !disc_files::read(*core, file) || file.size() < kClut + kClutBytes)
                return;
            if (!shaMatches(file.data() + kData, file.data() + kClut))
                return;
            g_origData.assign(file.begin() + kData, file.begin() + kData + kPayloadBytes);
            g_origClut.assign(file.begin() + kClut, file.begin() + kClut + kClutBytes);
            g_fromDisc = true;
            build();
        }

        bool known(const uint8_t *data, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
        {
            return std::memcmp(data, a.data(), a.size()) == 0 || std::memcmp(data, b.data(), b.size()) == 0;
        }

        void onVblank()
        {
            static uint32_t vblanks = 0u;
            ++vblanks;
            const uint32_t core = rd32(kCorePointer);
            if (core < 0x100000u || core > 0x2000000u - kClut - kClutBytes || (core & 0xFu) != 0u)
                return;
            uint8_t *data = g_ram + core + kData, *clut = g_ram + core + kClut;
            if (!g_captured)
            {
                if (vblanks % 60u != 0u)
                    return;
                const bool same = g_fromDisc ? (std::memcmp(data, g_origData.data(), kPayloadBytes) == 0 && std::memcmp(clut, g_origClut.data(), kClutBytes) == 0)
                                             : shaMatches(data, clut);
                if (!same)
                {
                    if (!g_warned)
                    {
                        g_warned = true;
                        KM_WARN(Game, "radar glow: the radar texture in the core block is not the US disc's (yet); no glow");
                    }
                    return;
                }
                if (!g_fromDisc)
                {
                    g_origData.assign(data, data + kPayloadBytes);
                    g_origClut.assign(clut, clut + kClutBytes);
                    build();
                }
                g_captured = true;
                KM_INFO(Game, "radar glow: radar texture found; the glow shows the original controls while Modern is on");
                return;
            }
            if (!g_built)
                return;
            const bool glow = modern::originalControlsNow();
            const std::vector<uint8_t> &wantData = glow ? g_glowData : g_origData, &wantClut = glow ? g_glowClut : g_origClut;
            if (std::memcmp(data, wantData.data(), kPayloadBytes) != 0 && known(data, g_origData, g_glowData))
                std::memcpy(data, wantData.data(), kPayloadBytes);
            if (std::memcmp(clut, wantClut.data(), kClutBytes) != 0 && known(clut, g_origClut, g_glowClut))
                std::memcpy(clut, wantClut.data(), kClutBytes);
        }
    }

    void install(uint8_t *rdram)
    {
        g_ram = rdram;
        if (!g_ram || !modern::enabled())
            return;
        prepareFromDisc();
        ps2x::pnach::addFrameHook(&onVblank);
    }
}
