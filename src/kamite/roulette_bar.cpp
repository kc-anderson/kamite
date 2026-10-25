// The roulette's hint bar, laid out again for the Modern wording. See roulette_bar.h.
#include "kamite/roulette_bar.h"

#include "kamite/menu_buttons.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kamite::roulette_bar
{
    namespace
    {
        constexpr uint16_t kBarMessage = 0x30F2u; // core_usa.bin bank 3 entry 0xF2: "[R1]Select [cross][circle]OK [triangle]Cancel"
        constexpr uint32_t kEntryKind = 0x88u, kEntryMsg = 0x90u, kEntryDrawX = 0x50u, kEntryDrawSclX = 0x58u, kEntryDrawSclY = 0x5Cu;
        constexpr uint8_t kKindText = 2u;
        constexpr float kMargin = 0.025f; // of the block's width, kept clear at its right end (and its left)

        struct Extent
        {
            float x0 = 1e9f, x1 = -1e9f;
            bool valid() const { return x0 <= x1; }
            void add(float a, float b)
            {
                x0 = std::min(x0, a);
                x1 = std::max(x1, b);
            }
        };

        uint32_t g_layout = 0u, g_entry = 0u; // the bar's layout and text entry, once seen
        Extent g_block, g_text;               // this frame's draws
        Extent g_lastBlock, g_lastText;       // the previous frame's
        float g_usedSclX = 0.0f, g_usedDrawX = 0.0f; // what the text in g_lastText was drawn with
        float g_pendSclX = 0.0f, g_pendDrawX = 0.0f; // what this frame's text is drawn with
        bool g_used = false, g_pend = false;

        float rdF(const uint8_t *rdram, uint32_t address)
        {
            float v;
            std::memcpy(&v, rdram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        void wrF(uint8_t *rdram, uint32_t address, float v)
        {
            std::memcpy(rdram + (address & 0x1FFFFFFu), &v, sizeof(v));
        }
        uint16_t rd16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t v;
            std::memcpy(&v, rdram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
    }

    void onEntry(uint8_t *rdram, uint32_t layout, uint32_t entry)
    {
        // Off: the roulette's bar keeps the game's own wording now (menu_buttons: the roulette keeps its buttons).
        constexpr bool kRelayout = false;
        if (!kRelayout || !menu_buttons::modern() || !rdram || entry == 0u)
            return;
        if (rdram[(entry + kEntryKind) & 0x1FFFFFFu] != kKindText || rd16(rdram, entry + kEntryMsg) != kBarMessage)
            return;
        if (entry != g_entry || layout != g_layout)
        {
            g_entry = entry;
            g_layout = layout;
            g_used = g_pend = false;
            g_lastBlock = g_lastText = Extent{};
        }
        const float sclY = rdF(rdram, entry + kEntryDrawSclY);
        float sclX = rdF(rdram, entry + kEntryDrawSclX), drawX = rdF(rdram, entry + kEntryDrawX);
        if (g_used && g_lastBlock.valid() && g_lastText.valid() && g_usedSclX > 0.0f && std::isfinite(sclY) && sclY > 0.0f)
        {
            // Last frame the text was drawn with g_usedSclX about g_usedDrawX and covered g_lastText. Its width per
            // unit of x scale, and where its centre sat relative to drawX (the line is centred; the spaces make the
            // visible part sit a little off), carry over.
            const float perScale = (g_lastText.x1 - g_lastText.x0) / g_usedSclX;
            const float centreOff = (0.5f * (g_lastText.x0 + g_lastText.x1) - g_usedDrawX) / g_usedSclX;
            const float blockW = g_lastBlock.x1 - g_lastBlock.x0;
            const float avail = blockW * (1.0f - 2.0f * kMargin);
            if (perScale > 1.0f && blockW > 8.0f)
            {
                sclX = sclY; // full letter width
                if (perScale * sclX > avail)
                    sclX = avail / perScale;
                const float width = perScale * sclX;
                const float right = g_lastBlock.x1 - blockW * kMargin;
                drawX = right - 0.5f * width - centreOff * sclX;
                wrF(rdram, entry + kEntryDrawSclX, sclX);
                wrF(rdram, entry + kEntryDrawX, drawX);
            }
        }
        g_pendSclX = sclX;
        g_pendDrawX = drawX;
        g_pend = true;
    }

    void onDraw(uint32_t layout, uint32_t entry, float x0, float x1, bool glyph)
    {
        if (g_layout == 0u || layout != g_layout)
            return;
        if (entry == g_entry)
        {
            if (glyph)
                g_text.add(x0, x1);
        }
        else if (!glyph)
            g_block.add(x0, x1);
    }

    void onFrame()
    {
        if (g_block.valid())
            g_lastBlock = g_block;
        if (g_text.valid() && g_pend)
        {
            g_lastText = g_text;
            g_usedSclX = g_pendSclX;
            g_usedDrawX = g_pendDrawX;
            g_used = true;
        }
        g_pend = false;
        g_block = g_text = Extent{};
    }
}
