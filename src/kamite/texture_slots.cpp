// Texture slots that fit. See texture_slots.h.
#include "kamite/texture_slots.h"

#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace kamite::texture_slots
{
    namespace
    {
        // GS block order inside a page (8 x 4 blocks for CT32/CT24/T8, 4 x 8 for CT16/T4).
        constexpr uint8_t kTable32[4][8] = {{0, 1, 4, 5, 16, 17, 20, 21},
                                            {2, 3, 6, 7, 18, 19, 22, 23},
                                            {8, 9, 12, 13, 24, 25, 28, 29},
                                            {10, 11, 14, 15, 26, 27, 30, 31}};
        constexpr uint8_t kTable16[8][4] = {{0, 2, 8, 10},   {1, 3, 9, 11},   {4, 6, 12, 14},  {5, 7, 13, 15},
                                            {16, 18, 24, 26}, {17, 19, 25, 27}, {20, 22, 28, 30}, {21, 23, 29, 31}};

        std::atomic<uint32_t> g_grown{0u};

        // At 0x151C70 in func_00151990 (the TIM2 loader), before `srl v1,v1,8` turns the picture's size into blocks.
        void onTimSize(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t header = (GPR_U32(ctx, 21) - 0x30u) & 0x1FFFFFFu; // $s5 is past the 0x30-byte picture header
            if (header + 0x30u > 0x2000000u)
                return;
            const uint8_t type = rdram[header + 0x13u];
            uint16_t width, height;
            std::memcpy(&width, rdram + header + 0x14u, 2u);
            std::memcpy(&height, rdram + header + 0x16u, 2u);
            uint32_t psm;
            switch (type)
            {
            case 1: psm = 0x02u; break; // CT16
            case 2: psm = 0x01u; break; // CT24
            case 3: psm = 0x00u; break; // CT32
            case 4: psm = 0x14u; break; // T4
            case 5: psm = 0x13u; break; // T8
            default: return;
            }
            if (width == 0u || height == 0u || width > 1024u || height > 1024u)
                return;
            // TBW as the game sets it (func_00309C30): 64-texel units, even for the palette formats.
            uint32_t tbw = (width + 63u) / 64u;
            if ((psm == 0x13u || psm == 0x14u) && (tbw & 1u))
                ++tbw;
            const uint32_t need = footprintBlocks(psm, width, height, tbw) * 256u;
            if (need > GPR_U32(ctx, 3))
            {
                SET_GPR_U32(ctx, 3, need);
                if (g_grown.fetch_add(1u, std::memory_order_relaxed) < 3u)
                    KM_INFO(Game, "texture slots: a " << width << "x" << height << " texture (PSM 0x" << std::hex << psm
                                                      << std::dec << ") takes " << need / 256u
                                                      << " blocks, not its size's; the next one goes after them");
            }
        }
    }

    uint32_t footprintBlocks(uint32_t psm, uint32_t width, uint32_t height, uint32_t tbw)
    {
        uint32_t pageW, pageH, blockW, blockH;
        bool wide; // 8 x 4 blocks a page (CT32 order), else 4 x 8
        switch (psm)
        {
        case 0x00u:
        case 0x01u: pageW = 64u; pageH = 32u; blockW = 8u; blockH = 8u; wide = true; break;
        case 0x02u: pageW = 64u; pageH = 64u; blockW = 16u; blockH = 8u; wide = false; break;
        case 0x13u: pageW = 128u; pageH = 64u; blockW = 16u; blockH = 16u; wide = true; break;
        case 0x14u: pageW = 128u; pageH = 128u; blockW = 32u; blockH = 16u; wide = false; break;
        default: return 0u;
        }
        const uint32_t pagesPerRow = std::max<uint32_t>(1u, tbw * 64u / pageW);
        uint32_t highest = 0u;
        for (uint32_t y = 0; y < height; y += blockH)
            for (uint32_t x = 0; x < width; x += blockW)
            {
                const uint32_t page = (y / pageH) * pagesPerRow + x / pageW;
                const uint32_t bx = (x % pageW) / blockW, by = (y % pageH) / blockH;
                const uint32_t block = page * 32u + (wide ? kTable32[by][bx] : kTable16[by][bx]);
                highest = std::max(highest, block);
            }
        return highest + 1u;
    }

    void install()
    {
        ps2xRegisterHostHook("km_texture_slot_size", &onTimSize);
        ps2xSetPatchFlag("texture_slots", true);
    }
}
