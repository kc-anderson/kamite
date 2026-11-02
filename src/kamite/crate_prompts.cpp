#include "kamite/crate_prompts.h"
#include "kamite/prompt_art.h"

#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt4.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace kamite::crate_prompts
{
    namespace
    {
        constexpr int kSize = 128;
        constexpr int kCell = 64;
        constexpr uint8_t kWoodMax = 5; // CLUT 0-5: the board's browns, darkest to lightest

        // Per cell: the symbol's colour and two glow shades (CLUT indices; port_prep/pirate palette dump):
        // 13 green, 9 red, 15 pink-white, 14 cyan; glows 12 pale green, 8/6 reds, 11/10/7 greys.
        struct Inks
        {
            uint8_t core;
            uint8_t glow1;
            uint8_t glow2;
        };
        constexpr std::array<Inks, 4> kInks{{{13, 12, 10}, {9, 8, 6}, {15, 11, 10}, {14, 12, 10}}};

        // Byte offset in the CT32 64x32 DBW 1 upload of each byte of the page, and the PSMT4 texel -> nibble address.
        struct Swizzle
        {
            std::array<uint16_t, kPayloadBytes> payloadOfByte{};
            Swizzle()
            {
                for (uint32_t p = 0; p < 64u * 32u; ++p)
                {
                    const uint32_t address = GSPSMCT32::addrPSMCT32(0u, 1u, p % 64u, p / 64u);
                    for (uint32_t k = 0; k < 4u; ++k)
                        payloadOfByte[(address + k) & (kPayloadBytes - 1u)] = static_cast<uint16_t>(p * 4u + k);
                }
            }
        };

        const Swizzle &swizzle()
        {
            static const Swizzle table;
            return table;
        }

        uint8_t readTexel(const uint8_t *payload, int x, int y)
        {
            const uint32_t nibble = GSPSMT4::addrPSMT4(0u, 2u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            const uint8_t byte = payload[swizzle().payloadOfByte[(nibble >> 1) & (kPayloadBytes - 1u)]];
            return (nibble & 1u) ? static_cast<uint8_t>(byte >> 4) : static_cast<uint8_t>(byte & 0x0F);
        }

        void writeTexel(uint8_t *payload, int x, int y, uint8_t index)
        {
            const uint32_t nibble = GSPSMT4::addrPSMT4(0u, 2u, static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            uint8_t &byte = payload[swizzle().payloadOfByte[(nibble >> 1) & (kPayloadBytes - 1u)]];
            byte = (nibble & 1u) ? static_cast<uint8_t>((byte & 0x0F) | (index << 4)) : static_cast<uint8_t>((byte & 0xF0) | (index & 0x0F));
        }

        // ---- the full-resolution image ----

        // Largest value within `r` texels (square), separable.
        std::vector<float> dilate(const std::vector<float> &in, int w, int h, int r)
        {
            std::vector<float> tmp(in.size()), out(in.size());
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float m = 0.0f;
                    for (int k = std::max(0, x - r); k <= std::min(w - 1, x + r); ++k)
                        m = std::max(m, in[static_cast<size_t>(y * w + k)]);
                    tmp[static_cast<size_t>(y * w + x)] = m;
                }
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                {
                    float m = 0.0f;
                    for (int k = std::max(0, y - r); k <= std::min(h - 1, y + r); ++k)
                        m = std::max(m, tmp[static_cast<size_t>(k * w + x)]);
                    out[static_cast<size_t>(y * w + x)] = m;
                }
            return out;
        }

        // Three box passes of radius r (about a Gaussian of sigma 1.1 r), edges clamped.
        void blur(std::vector<float> &v, int w, int h, int r)
        {
            std::vector<float> tmp(v.size());
            const float n = static_cast<float>(2 * r + 1);
            for (int pass = 0; pass < 3; ++pass)
            {
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        float sum = 0.0f;
                        for (int k = -r; k <= r; ++k)
                            sum += v[static_cast<size_t>(y * w + std::clamp(x + k, 0, w - 1))];
                        tmp[static_cast<size_t>(y * w + x)] = sum / n;
                    }
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x)
                    {
                        float sum = 0.0f;
                        for (int k = -r; k <= r; ++k)
                            sum += tmp[static_cast<size_t>(std::clamp(y + k, 0, h - 1) * w + x)];
                        v[static_cast<size_t>(y * w + x)] = sum / n;
                    }
            }
        }

        struct Rgb
        {
            float r, g, b;
        };
        Rgb rgbOf(uint32_t c)
        {
            return {static_cast<float>((c >> 16) & 0xFFu), static_cast<float>((c >> 8) & 0xFFu), static_cast<float>(c & 0xFFu)};
        }
        Rgb mix(Rgb a, Rgb b, float t)
        {
            return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
        }
        void over(float *px, Rgb c, float a)
        {
            px[0] += (c.r - px[0]) * a;
            px[1] += (c.g - px[1]) * a;
            px[2] += (c.b - px[2]) * a;
        }
    }

    bool build(const uint8_t *original, const uint8_t *originalClut, const std::array<std::string, 4> &labels,
               const std::array<uint32_t, 4> &colours, uint8_t *out, uint8_t *outClut, hd_textures::Image *hd)
    {
        std::array<std::array<uint8_t, kSize>, kSize> texels{};
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x)
                texels[y][x] = readTexel(original, x, y);

        // Layout check: each cell holds its own symbol colour, the cell's symbol bounding box comes from it.
        struct Box
        {
            int x0 = kCell, y0 = kCell, x1 = -1, y1 = -1;
        };
        std::array<Box, 4> boxes{};
        for (int cell = 0; cell < 4; ++cell)
        {
            const int ox = (cell & 1) * kCell, oy = (cell >> 1) * kCell;
            Box &box = boxes[cell];
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                    if (texels[oy + y][ox + x] == kInks[cell].core)
                    {
                        box.x0 = std::min(box.x0, x);
                        box.y0 = std::min(box.y0, y);
                        box.x1 = std::max(box.x1, x);
                        box.y1 = std::max(box.y1, y);
                    }
            if (box.x1 - box.x0 < 16 || box.y1 - box.y0 < 16)
                return false;
        }

        // One cap height for all four labels (the largest that fits every one).
        float cap = 1e9f;
        for (int cell = 0; cell < 4; ++cell)
        {
            prompt_art::TextBox box;
            box.cx = kCell / 2.0f;
            box.cy = kCell / 2.0f;
            box.width = 44.0f;
            box.capHeight = 28.0f;
            box.weight = prompt_art::Weight::Bold;
            const float h = prompt_art::textCapHeight(labels[cell], box);
            if (h > 0.0f)
                cap = std::min(cap, h);
        }
        if (cap > 28.0f)
            cap = 28.0f;

        // The palette: the wood (0-5) as the disc has it; per cell its letter (6 + cell) and its near glow (10 + cell,
        // the letter half-way to the disc's pale grey, as the disc's own glows are); one far glow shade (14, the disc's
        // grey 10) and a spare (15, the disc's grey 7).
        uint8_t clut[kClutBytes];
        std::memcpy(clut, originalClut, kClutBytes);
        auto setEntry = [&](int index, Rgb c)
        {
            clut[index * 4 + 0] = static_cast<uint8_t>(std::lround(std::clamp(c.r, 0.0f, 255.0f)));
            clut[index * 4 + 1] = static_cast<uint8_t>(std::lround(std::clamp(c.g, 0.0f, 255.0f)));
            clut[index * 4 + 2] = static_cast<uint8_t>(std::lround(std::clamp(c.b, 0.0f, 255.0f)));
            clut[index * 4 + 3] = 0x80u;
        };
        auto entryOf = [&](int index)
        {
            return Rgb{static_cast<float>(originalClut[index * 4]), static_cast<float>(originalClut[index * 4 + 1]),
                       static_cast<float>(originalClut[index * 4 + 2])};
        };
        const Rgb paleGrey = entryOf(11), farGlow = entryOf(10), spare = entryOf(7);
        for (int cell = 0; cell < 4; ++cell)
        {
            setEntry(6 + cell, rgbOf(colours[cell]));
            setEntry(10 + cell, mix(rgbOf(colours[cell]), paleGrey, 0.5f));
        }
        setEntry(14, farGlow);
        setEntry(15, spare);

        for (int cell = 0; cell < 4; ++cell)
        {
            const int ox = (cell & 1) * kCell, oy = (cell >> 1) * kCell;
            // Erase the symbol and its glow: every non-wood texel takes the rounded mean of its already-wood neighbours,
            // growing inwards from the edges (the browns 0-5 are an ordered ramp).
            std::array<std::array<uint8_t, kCell>, kCell> cellTexels{};
            std::array<std::array<bool, kCell>, kCell> hole{};
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                {
                    cellTexels[y][x] = texels[oy + y][ox + x];
                    hole[y][x] = cellTexels[y][x] > kWoodMax;
                }
            for (bool changed = true; changed;)
            {
                changed = false;
                auto next = hole;
                for (int y = 0; y < kCell; ++y)
                    for (int x = 0; x < kCell; ++x)
                    {
                        if (!hole[y][x])
                            continue;
                        int sum = 0, count = 0;
                        const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                        for (int i = 0; i < 4; ++i)
                        {
                            const int nx = x + dx[i], ny = y + dy[i];
                            if (nx < 0 || ny < 0 || nx >= kCell || ny >= kCell || hole[ny][nx])
                                continue;
                            sum += cellTexels[ny][nx];
                            ++count;
                        }
                        if (count == 0)
                            continue;
                        cellTexels[y][x] = static_cast<uint8_t>((sum + count / 2) / count);
                        next[y][x] = false;
                        changed = true;
                    }
                hole = next;
            }
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                    texels[oy + y][ox + x] = cellTexels[y][x];
        }
        const auto wood = texels; // the four crates with their symbols gone (the full-resolution image's base)

        // The labels where the symbols were, in the cell's colour, with a two-step glow around them.
        for (int cell = 0; cell < 4; ++cell)
        {
            const int ox = (cell & 1) * kCell, oy = (cell >> 1) * kCell;
            const Box &box = boxes[cell];
            prompt_art::TextBox textBox;
            textBox.cx = (box.x0 + box.x1 + 1) / 2.0f;
            textBox.cy = (box.y0 + box.y1 + 1) / 2.0f;
            textBox.width = 44.0f;
            textBox.capHeight = cap;
            textBox.weight = prompt_art::Weight::Bold;
            std::vector<float> coverage(static_cast<size_t>(kCell * kCell), 0.0f);
            prompt_art::textCoverage(labels[cell], textBox, kCell, kCell, coverage.data());
            std::array<std::array<bool, kCell>, kCell> ink{};
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                    ink[y][x] = coverage[static_cast<size_t>(y * kCell + x)] >= 0.45f;
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                {
                    uint8_t &t = texels[oy + y][ox + x];
                    if (ink[y][x])
                    {
                        t = static_cast<uint8_t>(6 + cell);
                        continue;
                    }
                    int nearest = 3;
                    for (int ny = std::max(0, y - 2); ny <= std::min(kCell - 1, y + 2); ++ny)
                        for (int nx = std::max(0, x - 2); nx <= std::min(kCell - 1, x + 2); ++nx)
                            if (ink[ny][nx])
                                nearest = std::min(nearest, std::max(std::abs(nx - x), std::abs(ny - y)));
                    if (nearest == 1)
                        t = static_cast<uint8_t>(10 + cell);
                    else if (nearest == 2)
                        t = 14u;
                }
        }

        // Full resolution: the symbol-free wood scaled up (bilinear, as the GS filters it), then per cell a soft glow
        // (the letter's colour lifted toward white), a thin dark rim for contrast with the wood, and the letter.
        if (hd)
        {
            constexpr int S = kHdScale, W = kSize * S, C = kCell * S;
            std::vector<float> rgb(static_cast<size_t>(W * W * 3));
            auto woodAt = [&](int x, int y, int k)
            {
                x = std::clamp(x, 0, kSize - 1);
                y = std::clamp(y, 0, kSize - 1);
                return static_cast<float>(originalClut[wood[y][x] * 4 + k]);
            };
            for (int y = 0; y < W; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const float u = (x + 0.5f) / S - 0.5f, v = (y + 0.5f) / S - 0.5f;
                    const int x0 = static_cast<int>(std::floor(u)), y0 = static_cast<int>(std::floor(v));
                    const float fx = u - x0, fy = v - y0;
                    for (int k = 0; k < 3; ++k)
                        rgb[static_cast<size_t>((y * W + x) * 3 + k)] =
                            (woodAt(x0, y0, k) * (1 - fx) + woodAt(x0 + 1, y0, k) * fx) * (1 - fy) +
                            (woodAt(x0, y0 + 1, k) * (1 - fx) + woodAt(x0 + 1, y0 + 1, k) * fx) * fy;
                }
            bool drawn = true;
            for (int cell = 0; cell < 4 && drawn; ++cell)
            {
                const int ox = (cell & 1) * C, oy = (cell >> 1) * C;
                const Box &box = boxes[cell];
                prompt_art::TextBox textBox;
                textBox.cx = (box.x0 + box.x1 + 1) / 2.0f * S;
                textBox.cy = (box.y0 + box.y1 + 1) / 2.0f * S;
                textBox.width = 44.0f * S;
                textBox.capHeight = cap * S;
                textBox.weight = prompt_art::Weight::Bold;
                std::vector<float> letter(static_cast<size_t>(C * C), 0.0f);
                if (prompt_art::textCoverage(labels[cell], textBox, C, C, letter.data()) <= 0.0f)
                {
                    drawn = labels[cell].empty();
                    continue;
                }
                std::vector<float> glow = dilate(letter, C, C, S);
                blur(glow, C, C, S + 1);
                const std::vector<float> rim = dilate(letter, C, C, S / 2);
                const Rgb core = rgbOf(colours[cell]);
                const Rgb glowColour = mix(core, Rgb{255.0f, 255.0f, 255.0f}, 0.45f);
                const Rgb rimColour{20.0f, 14.0f, 12.0f};
                for (int y = 0; y < C; ++y)
                    for (int x = 0; x < C; ++x)
                    {
                        const size_t i = static_cast<size_t>(y * C + x);
                        float *px = &rgb[static_cast<size_t>(((oy + y) * W + ox + x) * 3)];
                        over(px, glowColour, 0.75f * glow[i]);
                        over(px, rimColour, 0.5f * rim[i]);
                        over(px, core, letter[i]);
                    }
            }
            hd->width = hd->height = 0;
            hd->rgba.clear();
            if (drawn)
            {
                hd->width = hd->height = W;
                hd->rgba.resize(static_cast<size_t>(W * W * 4));
                for (size_t i = 0; i < static_cast<size_t>(W * W); ++i)
                {
                    for (int k = 0; k < 3; ++k)
                        hd->rgba[i * 4 + k] = static_cast<uint8_t>(std::lround(std::clamp(rgb[i * 3 + k], 0.0f, 255.0f)));
                    hd->rgba[i * 4 + 3] = 0x80u;
                }
            }
        }

        std::memcpy(out, original, kPayloadBytes);
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x)
                writeTexel(out, x, y, texels[y][x]);
        std::memcpy(outClut, clut, kClutBytes);
        return true;
    }
}
