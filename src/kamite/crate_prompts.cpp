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
    }

    bool build(const uint8_t *original, const std::array<std::string, 4> &labels, uint8_t *out)
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

            // The label where the symbol was, in the symbol's colour, with a two-step glow around it.
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
                    if (ink[y][x])
                    {
                        cellTexels[y][x] = kInks[cell].core;
                        continue;
                    }
                    int nearest = 3;
                    for (int ny = std::max(0, y - 2); ny <= std::min(kCell - 1, y + 2); ++ny)
                        for (int nx = std::max(0, x - 2); nx <= std::min(kCell - 1, x + 2); ++nx)
                            if (ink[ny][nx])
                                nearest = std::min(nearest, std::max(std::abs(nx - x), std::abs(ny - y)));
                    if (nearest == 1)
                        cellTexels[y][x] = kInks[cell].glow1;
                    else if (nearest == 2)
                        cellTexels[y][x] = kInks[cell].glow2;
                }
            for (int y = 0; y < kCell; ++y)
                for (int x = 0; x < kCell; ++x)
                    texels[oy + y][ox + x] = cellTexels[y][x];
        }

        std::memcpy(out, original, kPayloadBytes);
        for (int y = 0; y < kSize; ++y)
            for (int x = 0; x < kSize; ++x)
                writeTexel(out, x, y, texels[y][x]);
        return true;
    }
}
