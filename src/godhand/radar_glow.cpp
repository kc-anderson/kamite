// The radar's "original controls" glow. See radar_glow.h.
//
// The radar texture (128x256, 8-bit): the disc (centre 53, 54.5; the dark floor to r 35, the brown rim 36..48) in the
// top left, the blips on the right, the sweep rings below. Its CLUT uses all 256 entries, so the glow takes the eight
// least-used ones (one to three texels each, all inside the orange and tan blips, which move to their nearest other
// colours) for a ramp from the floor's dark brown to orange, and paints the floor's outer band (r 26..35.5) with it,
// strongest at the rim. The cone and anything lighter in the band quantize to the CLUT's own entries where closer.
#include "godhand/radar_glow.h"

#include "godhand/log.h"
#include "godhand/modern_controls.h"
#include "godhand/sha256.h"
#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"
#include "runtime/ps2_pnach.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace godhand::radar_glow
{
    namespace
    {
        constexpr uint32_t kCorePointer = 0x7479F8u;
        constexpr uint32_t kData = 0x11C830u, kClut = 0x124930u; // from the core block's base
        constexpr int kWidth = 128, kHeight = 256;
        constexpr size_t kPayloadBytes = static_cast<size_t>(kWidth) * kHeight, kClutBytes = 1024u;
        constexpr const char *kDataSha = "16626ddaf1f7eb68", *kClutSha = "17387e96aadc1529"; // the US disc's
        constexpr float kCentreX = 53.0f, kCentreY = 54.5f, kInner = 26.0f, kOuter = 35.5f;
        constexpr int kDiscSpan = 110; // the disc is inside x, y < 110
        constexpr float kStrength = 0.65f; // the blend toward orange at the rim
        constexpr float kGlow[3] = {255.0f, 165.0f, 40.0f};
        constexpr int kRampEntries = 8;

        uint8_t *g_ram = nullptr;
        bool g_captured = false, g_built = false, g_warned = false;
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

        void build()
        {
            std::vector<uint8_t> tex(kPayloadBytes);
            for (size_t i = 0; i < kPayloadBytes; ++i)
                tex[i] = g_origData[g_offsets[i]];
            std::array<std::array<int, 4>, 256> pal{};
            for (int k = 0; k < 256; ++k)
                for (int c = 0; c < 4; ++c)
                    pal[static_cast<size_t>(k)][static_cast<size_t>(c)] = g_origClut[static_cast<size_t>(clutSlot(k) * 4 + c)];
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
            // The ramp: the floor's colour at the band's inside (r = kInner) blended toward the glow by the band's
            // weights, in kRampEntries steps.
            const auto &floor = pal[tex[static_cast<size_t>(55 * kWidth + 30)]]; // a floor texel left of the centre (r 23)
            std::vector<int> rampIndex;
            for (int k = 0; k < 256; ++k)
                if (ramp[static_cast<size_t>(k)])
                    rampIndex.push_back(k);
            g_glowClut = g_origClut;
            for (size_t i = 0; i < rampIndex.size(); ++i)
            {
                const float t = (static_cast<float>(i) + 1.0f) / static_cast<float>(rampIndex.size());
                const float w = kStrength * t;
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
            for (int y = 0; y < kDiscSpan; ++y)
            {
                for (int x = 0; x < kDiscSpan; ++x)
                {
                    const float r = std::hypot(static_cast<float>(x) - kCentreX, static_cast<float>(y) - kCentreY);
                    if (r < kInner || r > kOuter)
                        continue;
                    uint8_t &v = tex[static_cast<size_t>(y * kWidth + x)];
                    const auto &p = pal[v];
                    if (p[3] < 0x80)
                        continue;
                    const float t = (r - kInner) / (kOuter - kInner);
                    const float w = kStrength * t * t;
                    float col[3];
                    for (int c = 0; c < 3; ++c)
                        col[c] = static_cast<float>(p[static_cast<size_t>(c)]) * (1.0f - w) + kGlow[c] * w;
                    v = static_cast<uint8_t>(nearest(col, true));
                }
            }
            g_glowData.assign(kPayloadBytes, 0u);
            for (size_t i = 0; i < kPayloadBytes; ++i)
                g_glowData[g_offsets[i]] = tex[i];
            g_built = true;
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
                Sha256 sd, sc;
                sd.update(data, kPayloadBytes);
                sc.update(clut, kClutBytes);
                if (sd.finishHex().rfind(kDataSha, 0) != 0 || sc.finishHex().rfind(kClutSha, 0) != 0)
                {
                    if (!g_warned)
                    {
                        g_warned = true;
                        GH_WARN(Game, "radar glow: the radar texture in the core block is not the US disc's; no glow");
                    }
                    return;
                }
                g_origData.assign(data, data + kPayloadBytes);
                g_origClut.assign(clut, clut + kClutBytes);
                g_captured = true;
                makeOffsets();
                build();
                GH_INFO(Game, "radar glow: radar texture found; the glow shows the original controls while Modern is on");
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
        ps2x::pnach::addFrameHook(&onVblank);
    }
}
