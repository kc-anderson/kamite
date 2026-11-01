#include "kamite/button_prompt_sheets.h"

#include "runtime/gs/ps2_gs_psmct32.h"
#include "runtime/gs/ps2_gs_psmt8.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

namespace kamite::prompt_sheets
{
    namespace
    {
        // Offset in the payload of every texel, row-major (see the header).
        std::vector<uint16_t> payloadOffsets()
        {
            std::vector<uint16_t> inv(kPayloadBytes / 4u);
            for (uint32_t y = 0; y < 64u; ++y)
                for (uint32_t x = 0; x < 64u; ++x)
                    inv[GSPSMCT32::addrPSMCT32(0u, 1u, x, y) >> 2u] = static_cast<uint16_t>(y * 64u + x);
            std::vector<uint16_t> offsets(kPayloadBytes);
            for (uint32_t y = 0; y < static_cast<uint32_t>(kSheetSize); ++y)
            {
                for (uint32_t x = 0; x < static_cast<uint32_t>(kSheetSize); ++x)
                {
                    const uint32_t a = GSPSMT8::addrPSMT8(0u, 2u, x, y);
                    offsets[y * kSheetSize + x] = static_cast<uint16_t>(inv[a >> 2u] * 4u + (a & 3u));
                }
            }
            return offsets;
        }
    }

    void unswizzle(const uint8_t *payload, uint8_t *indices)
    {
        const std::vector<uint16_t> off = payloadOffsets();
        for (size_t i = 0; i < kPayloadBytes; ++i)
            indices[i] = payload[off[i]];
    }

    void swizzle(const uint8_t *indices, uint8_t *payload)
    {
        const std::vector<uint16_t> off = payloadOffsets();
        for (size_t i = 0; i < kPayloadBytes; ++i)
            payload[off[i]] = indices[i];
    }

    void paletteRgba(const uint8_t *clut, uint8_t *rgba)
    {
        // CSM1: the CLUT is stored as 16x16 CT32 with bits 3 and 4 of the index swapped.
        for (int c = 0; c < 256; ++c)
        {
            const int j = (c & 0xE7) | ((c & 0x08) << 1) | ((c & 0x10) >> 1);
            std::memcpy(rgba + c * 4, clut + j * 4, 4u);
        }
    }

    // ---- the style list's arrows and plus ----
    namespace
    {
        // The down arrow's core as the game draws it (system texture 20, its "+down" piece: 14 x 19 texels): a shaft 4
        // texels wide down to row 12, then a head 14 wide narrowing to a point at row 19. The right arrow is the same
        // turned a quarter. The plus is the same art's: two bars 3 texels thick and 15 long crossing at their middles.
        // Each piece sits in the free area with two texels of outline round the core (the plus's outline touches both
        // arrows' in the sheet, which never shows: each piece is drawn by its own rectangle).
        constexpr ArrowRect kRects[ArrowPieceCount] = {
            {64.0f, 100.0f, 82.0f, 123.0f},   // down: core 66..80 x 102..121
            {83.0f, 102.0f, 102.0f, 121.0f},  // plus: core 85..100 x 104..119
            {103.0f, 103.0f, 126.0f, 121.0f}, // right: core 105..124 x 105..119
        };

        // Distance from a point (core units: x across 0..14, y along 0..19, the point of the arrow at y = 19) to the
        // arrow; 0 inside.
        float coreDistance(float x, float y)
        {
            // The shaft: x 5..9, y 0..12.
            const float sx = std::max({5.0f - x, 0.0f, x - 9.0f}), sy = std::max({0.0f - y, 0.0f, y - 12.0f});
            const float shaft = std::sqrt(sx * sx + sy * sy);
            // The head: the triangle (0,12) (14,12) (7,19).
            const float ax[3] = {0.0f, 14.0f, 7.0f}, ay[3] = {12.0f, 12.0f, 19.0f};
            bool inside = true;
            float edge = 1e9f;
            for (int i = 0; i < 3; ++i)
            {
                const int j = (i + 1) % 3;
                const float ex = ax[j] - ax[i], ey = ay[j] - ay[i];
                const float cross = ex * (y - ay[i]) - ey * (x - ax[i]);
                if (cross < 0.0f) // the triangle is clockwise in screen space (y down): inside is cross >= 0
                    inside = false;
                const float t = std::clamp(((x - ax[i]) * ex + (y - ay[i]) * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
                const float dx = x - (ax[i] + t * ex), dy = y - (ay[i] + t * ey);
                edge = std::min(edge, std::sqrt(dx * dx + dy * dy));
            }
            const float head = inside ? 0.0f : edge;
            return std::min(shaft, head);
        }

        // Distance from a point (core units: 0..15 each way) to the plus; 0 inside. The bars: x 6..9 and y 6..9 across,
        // 0..15 along.
        float plusDistance(float x, float y)
        {
            const float ax = std::max({6.0f - x, 0.0f, x - 9.0f}), ay = std::max({0.0f - y, 0.0f, y - 15.0f});
            const float bx = std::max({0.0f - x, 0.0f, x - 15.0f}), by = std::max({6.0f - y, 0.0f, y - 9.0f});
            return std::min(std::sqrt(ax * ax + ay * ay), std::sqrt(bx * bx + by * by));
        }

        // The piece at a sheet point: -1 outside all, else its distance to the core.
        int pieceAt(float x, float y, float &distance)
        {
            for (int piece = 0; piece < ArrowPieceCount; ++piece)
            {
                const ArrowRect &r = kRects[piece];
                if (x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1)
                    continue;
                const float cx = r.x0 + 2.0f, cy = r.y0 + 2.0f;
                // Down: across = x, along = y. Right: across = y, along = x (the point at the right).
                distance = piece == ArrowDown    ? coreDistance(x - cx, y - cy)
                           : piece == ArrowPlus ? plusDistance(x - cx, y - cy)
                                                : coreDistance(y - cy, x - cx);
                return piece;
            }
            return -1;
        }

        // The game art's colours (texture 20's palette): the cream fill, the dark ring next to it, the softer one out.
        // The plus's two rings are both the solid dark (its palette entries 5 and 6).
        constexpr float kFill[4] = {224.0f, 216.0f, 191.0f, 128.0f};
        constexpr float kDark[4] = {42.0f, 40.0f, 36.0f, 128.0f};
        constexpr float kSoft[4] = {53.0f, 50.0f, 49.0f, 108.0f};
        constexpr float kPlusDark[4] = {54.0f, 51.0f, 47.0f, 128.0f};

        const float *shade(int piece, float distance)
        {
            if (distance <= 0.0f)
                return kFill;
            if (piece == ArrowPlus)
                return distance <= 2.0f ? kPlusDark : nullptr;
            if (distance <= 1.0f)
                return kDark;
            if (distance <= 2.0f)
                return kSoft;
            return nullptr;
        }
    }

    ArrowRect arrowRect(int piece)
    {
        return kRects[std::clamp(piece, 0, ArrowPieceCount - 1)];
    }

    bool addStyleArrows(uint8_t *payload, const uint8_t *clut)
    {
        if (!payload || !clut)
            return false;
        std::vector<uint8_t> indices(static_cast<size_t>(kSheetSize) * kSheetSize);
        unswizzle(payload, indices.data());
        std::array<uint8_t, 1024> pal{};
        paletteRgba(clut, pal.data());
        // The cells must be empty (index 0 or a fully transparent entry) in the sheet we were given.
        for (int y = 96; y < 128; ++y)
            for (int x = 64; x < 128; ++x)
                if (pal[static_cast<size_t>(indices[static_cast<size_t>(y) * kSheetSize + x]) * 4u + 3u] != 0u)
                    return false;
        // Nearest palette entry (premultiplied RGBA) for each of the three colours.
        auto nearest = [&](const float *c) {
            int best = 0;
            float bestD = 1e30f;
            for (int i = 0; i < 256; ++i)
            {
                const uint8_t *e = &pal[static_cast<size_t>(i) * 4u];
                if (e[3] == 0u)
                    continue;
                const float a = e[3] / 128.0f, ca = c[3] / 128.0f;
                float d = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float diff = e[k] * a - c[k] * ca;
                    d += diff * diff;
                }
                const float da = (e[3] - c[3]) * 2.0f;
                d += da * da;
                if (d < bestD)
                {
                    bestD = d;
                    best = i;
                }
            }
            return static_cast<uint8_t>(best);
        };
        const uint8_t fill = nearest(kFill), dark = nearest(kDark), soft = nearest(kSoft), plusDark = nearest(kPlusDark);
        for (int y = 96; y < 128; ++y)
            for (int x = 64; x < 128; ++x)
            {
                float distance = 0.0f;
                const int piece = pieceAt(x + 0.5f, y + 0.5f, distance);
                if (piece < 0)
                    continue;
                const float *c = shade(piece, distance);
                if (c)
                    indices[static_cast<size_t>(y) * kSheetSize + x] =
                        c == kFill ? fill : c == kDark ? dark : c == kPlusDark ? plusDark : soft;
            }
        swizzle(indices.data(), payload);
        return true;
    }

    void arrowColour(float x, float y, float out[4])
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        float distance = 0.0f;
        const int piece = pieceAt(x, y, distance);
        if (piece < 0)
            return;
        const float *c = shade(piece, distance);
        if (!c)
            return;
        const float a = c[3] / 128.0f;
        for (int k = 0; k < 3; ++k)
            out[k] = c[k] / 255.0f * a;
        out[3] = a;
    }
}
