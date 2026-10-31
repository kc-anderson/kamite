// The roulette's hint bar, laid out again for the Modern wording. See roulette_bar.h.
#include "kamite/roulette_bar.h"

#include "kamite/menu_buttons.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace kamite::roulette_bar
{
    namespace
    {
        constexpr uint16_t kBarMessage = 0x30F2u; // core_usa.bin bank 3 entry 0xF2: "[R1]Select [cross][circle]OK [triangle]Cancel"
        constexpr uint32_t kEntryKind = 0x88u, kEntryMsg = 0x90u, kEntryDrawX = 0x50u, kEntryDrawSclX = 0x58u, kEntryDrawSclY = 0x5Cu;
        constexpr uint8_t kKindText = 2u;
        constexpr float kPad = 14.0f; // screen units kept clear left and right of the text inside the shortened bar

        struct Extent
        {
            float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
            bool valid() const { return x0 <= x1; }
            void add(float a, float b, float c, float d)
            {
                x0 = std::min(x0, a);
                x1 = std::max(x1, b);
                y0 = std::min(y0, c);
                y1 = std::max(y1, d);
            }
        };

        // One entry of the bar's layout: what this frame drew, and what the previous frame drew with which values.
        struct Entry
        {
            Extent now, last;
            float usedSclX = 0.0f, usedDrawX = 0.0f, pendSclX = 0.0f, pendDrawX = 0.0f;
            bool used = false, pend = false, glyphs = false, lastGlyphs = false;
        };

        uint32_t g_layout = 0u, g_text = 0u; // the bar's layout and its text entry, once seen
        std::unordered_map<uint32_t, Entry> g_entries;
        float g_barRight = 0.0f, g_barWidth = 0.0f; // the game's own bar (untouched right edge and full width)
        bool g_barKnown = false;

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

        // The text's width per unit of x scale and its centre's offset from drawX per unit of x scale (the line is
        // centred; its spaces make the visible part sit a little off), from the previous frame.
        bool textMetrics(float &perScale, float &centreOff)
        {
            const auto it = g_entries.find(g_text);
            if (it == g_entries.end() || !it->second.used || !it->second.last.valid() || it->second.usedSclX <= 0.0f)
                return false;
            const Entry &t = it->second;
            perScale = (t.last.x1 - t.last.x0) / t.usedSclX;
            centreOff = (0.5f * (t.last.x0 + t.last.x1) - t.usedDrawX) / t.usedSclX;
            return perScale > 1.0f && std::isfinite(perScale);
        }

        bool overlapsText(const Extent &e)
        {
            const auto it = g_entries.find(g_text);
            if (it == g_entries.end() || !it->second.last.valid())
                return false;
            const Extent &t = it->second.last;
            const float centre = 0.5f * (t.y0 + t.y1); // a piece of the bar spans the text's middle (the dial above does not)
            return e.valid() && e.y0 <= centre && e.y1 >= centre;
        }
    }

    void onEntry(uint8_t *rdram, uint32_t layout, uint32_t entry)
    {
        if (!menu_buttons::modern() || !rdram || entry == 0u)
            return;
        const bool isText = rdram[(entry + kEntryKind) & 0x1FFFFFFu] == kKindText && rd16(rdram, entry + kEntryMsg) == kBarMessage;
        if (isText && (entry != g_text || layout != g_layout))
        {
            g_text = entry;
            g_layout = layout;
            g_entries.clear();
            g_barKnown = false;
        }
        if (g_layout == 0u || layout != g_layout)
            return;
        Entry &e = g_entries[entry];
        float sclX = rdF(rdram, entry + kEntryDrawSclX), drawX = rdF(rdram, entry + kEntryDrawX);
        float perScale = 0.0f, centreOff = 0.0f;
        const float textSclY = rdF(rdram, g_text + kEntryDrawSclY);
        if (g_barKnown && std::isfinite(textSclY) && textSclY > 0.0f && textMetrics(perScale, centreOff))
        {
            // The text at full letter width (x scale = y scale), or as wide as the game's bar allows.
            const float textSclX = std::min(textSclY, (g_barWidth - 2.0f * kPad) / perScale);
            const float width = perScale * textSclX;
            if (isText)
            {
                // Its right end kPad inside the bar's right edge.
                sclX = textSclX;
                drawX = g_barRight - kPad - 0.5f * width - centreOff * sclX;
                wrF(rdram, entry + kEntryDrawSclX, sclX);
                wrF(rdram, entry + kEntryDrawX, drawX);
            }
        }
        e.pendSclX = sclX;
        e.pendDrawX = drawX;
        e.pend = true;
    }

    void onDraw(uint32_t layout, uint32_t entry, float x0, float x1, float y0, float y1, bool glyph)
    {
        if (g_layout == 0u || layout != g_layout)
            return;
        const auto it = g_entries.find(entry);
        if (it == g_entries.end())
            return;
        it->second.now.add(x0, x1, y0, y1);
        it->second.glyphs = it->second.glyphs || glyph;
    }

    void onFrame()
    {
        // The game's bar: the non-glyph pieces level with the text, as the game would draw them (the pieces this frame
        // were drawn scaled by k about the right edge, so their own extents are mapped back with their scale).
        Extent bar;
        bool any = false;
        for (auto &[entry, e] : g_entries)
        {
            if (e.now.valid() && e.pend)
            {
                e.last = e.now;
                e.lastGlyphs = e.glyphs;
                e.usedSclX = e.pendSclX;
                e.usedDrawX = e.pendDrawX;
                e.used = true;
            }
            e.pend = false;
            e.glyphs = false;
            e.now = Extent{};
        }
        for (auto &[entry, e] : g_entries)
        {
            if (entry == g_text || e.lastGlyphs || !e.used || !overlapsText(e.last))
                continue;
            bar.add(e.last.x0, e.last.x1, e.last.y0, e.last.y1);
            any = true;
        }
        if (!any)
            return;
        if (!g_barKnown)
        {
            // First sight: the bar as the game drew it (nothing was scaled yet).
            g_barRight = bar.x1;
            g_barWidth = bar.x1 - bar.x0;
            g_barKnown = g_barWidth > 8.0f;
        }
    }
}
