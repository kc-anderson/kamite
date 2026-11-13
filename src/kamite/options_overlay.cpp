// The OPTIONS screen's overlay list. See options_overlay.h; measurements: claude/rearmed-options.md.
#include "kamite/options_overlay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace kamite::options_overlay
{
    namespace
    {
        std::string g_labelFont, g_helpFont, g_titleDir, g_scrollTrack, g_scrollThumb, g_sliderFill;

        // A page's rows (y in lines): centres from the list's first row, the span the pause's list fills.
        constexpr float kPageTop = 110.0f - 0.5f * 29.625f, kPageBottom = 347.0f + 0.5f * 29.625f;
        constexpr float kPageStep = 29.625f, kHeaderStep = 24.0f;
        float rowHeight(const PageRow &row) { return row.header ? kHeaderStep : kPageStep; }
    }

    // The first and last row centres a page shows from scroll.
    static std::pair<float, float> pageExtent(const Page &page, int scroll)
    {
        float y = kPageTop, first = kPageTop, last = kPageTop;
        const int from = std::clamp(scroll, 0, std::max(page.rowCount - 1, 0)), shown = pageRowsShown(page, from);
        for (int i = from; i < from + shown; ++i)
        {
            const float centre = y + 0.5f * rowHeight(page.rows[i]);
            first = i == from ? centre : first;
            last = centre;
            y += rowHeight(page.rows[i]);
        }
        return {first, last};
    }

    int pageRowsShown(const Page &page, int scroll)
    {
        float y = kPageTop;
        int shown = 0;
        for (int i = std::max(scroll, 0); i < page.rowCount; ++i, ++shown)
        {
            y += rowHeight(page.rows[i]);
            if (y > kPageBottom + 0.01f)
                break;
        }
        return shown;
    }

    void setScrollBar(const std::string &track, const std::string &thumb)
    {
        g_scrollTrack = track;
        g_scrollThumb = thumb;
    }

    void setSliderFill(const std::string &fill) { g_sliderFill = fill; }

    void setAssets(const std::string &labelFont, const std::string &helpFont, const std::string &titleDir)
    {
        g_labelFont = labelFont;
        g_helpFont = helpFont;
        g_titleDir = titleDir;
    }

#if defined(KAMITE_HAS_SCREEN_OVERLAY)
    namespace
    {
        constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255u) { return r | (g << 8) | (b << 16) | (a << 24); }

        // The original's colours.
        constexpr uint32_t kLabel = rgba(0xBC, 0x79, 0x48);         // a row name: flat tan
        constexpr uint32_t kLabelOn = rgba(0xFF, 0xF8, 0xCE);       // the selected one: cream...
        constexpr uint32_t kLabelOnStroke = rgba(0xA1, 0x2E, 0x02); // ...with a red-brown stroke (the cursor too)
        constexpr uint32_t kValue = rgba(0x87, 0x77, 0x6D);         // a value: grey
        constexpr uint32_t kValueOn = rgba(0xFF, 0xFF, 0xFF);       // the selected one: white...
        constexpr uint32_t kValueOnStroke = rgba(0x26, 0x32, 0x4A); // ...with a navy stroke
        constexpr uint32_t kHelp = rgba(0xDA, 0xD1, 0xB2);          // the help text: cream (the game's hint text)...
        constexpr uint32_t kHelpShadow = rgba(0, 0, 0, 190);       // ...with a hard dark drop shadow, no outline
        // Our labels' and values' softening (so they sit with the game's upscaled 2D) and drop shadow, in lines (K's
        // values from F2, Nov 13).
        constexpr float kBlur = 0.69f, kShadowAlpha = 1.0f, kShadowOffset = 0.88f;
        // A slider: the enemy health bar's fill over the dark band of the same gauge texture (K's pick, Nov 13: the
        // bar's own translucent black vanishes on the wall), with the text's drop shadow; the number in kSliderNumber
        // units at the right.
        constexpr float kSliderHalf = 3.5f, kSliderBorder = 1.0f, kSliderNumber = 40.0f;
        constexpr uint32_t kSliderBack = rgba(0x3B, 0x23, 0x1F);

        // Placement: x in layout units, y and sizes in lines.
        constexpr float kLabelX = 37.0f, kLabelCap = 21.5f, kStroke = 1.0f;
        constexpr float kFirstRowY = 110.0f, kLastRowY = 347.0f, kRowStep = 34.0f; // row centres (the pause's 8 rows)
        constexpr float kCursorX = 24.0f, kCursorHalfW = 6.0f, kCursorHalfH = 8.0f;
        constexpr float kValueX = 262.0f, kValueGap = 58.0f, kValueCap = 17.0f;
        // The help is centred up and down in the wall's lower band (lines 363.5..417), one line or two (K, Nov 12: as the
        // game's own help sits), a little larger than the game's hint text (K). Line pitch as the Techniques hints'.
        constexpr float kHelpX = 29.0f, kHelpBandTop = 363.5f, kHelpBandBottom = 417.0f;
        constexpr float kHelpCap = 14.0f, kHelpLine = 22.0f, kHelpScaleX = 0.73f;
        constexpr float kHelpShadowX = 0.12f * kHelpCap, kHelpShadowY = 0.11f * kHelpCap; // the hints' shadow, to scale
        // The titles (title_<name>.png: K's painted OPTIONS and the categories made to match by
        // tools/options/make_splash.py), placed as the original's painted one (its letters x 146..363, y 20..66 of the
        // 512 x 448 screen): one canvas for all, the letters centred across it at one height, 8.72 of its pixels per
        // unit and per line (K's lettering at half size), so a page's title swaps in exactly where OPTIONS was.
        constexpr float kTitlePixelsPerUnit = 8.72f, kTitleTop = 12.32f, kTitleWidth = 2467.0f, kTitleHeight = 535.0f;
        constexpr float kTitleY = 70.0f, kTitleCap = 30.0f; // the text stand-in when the image is missing
        constexpr float kMaxLabel = 190.0f;                  // the longest original (RESTORE DEFAULTS)

        // The page beside the list (16:10 and wider): the bracket's upright just past the list's longest name, the
        // page's cursor and names after it as the list's are after the screen's edge; its right end at the text area's
        // margin, at most kPageWidth on. Values sit in a block at the right, choices centred in equal slots.
        constexpr float kSplitAspect = 1.595f;
        constexpr float kBracketX = 240.0f, kBracketTick = 9.0f, kBracketGap = 7.0f, kBracketReach = 17.0f;
        constexpr float kPageCursorGap = kLabelX - kCursorX, kPageNameGap = kLabelX - kCursorX + kLabelX - kCursorX;
        constexpr float kPageWidth = 470.0f, kValueBlock = 136.0f, kScrollWidth = 8.0f, kScrollGap = 14.0f;
        constexpr float kSubIndent = 12.0f, kSubCap = 18.5f, kHeaderCap = 13.0f, kHeaderTracking = 3.0f;
        constexpr float kLinesToUnits = 6.0f / 7.0f;

        // The bracket, in the look of the Techniques screen's connector line (K's pick, Nov 13): layers drawn widest
        // first, each a thickness in lines and an offset toward the top / left (the lit side).
        struct BracketLayer
        {
            uint32_t colour;
            float thickness, lift;
        };
        constexpr BracketLayer kBracketLayers[] = {{rgba(0xB6, 0x1A, 0x00, 110), 6.0f, 0.0f}, // a faint red fringe,
                                                   {rgba(0xB8, 0x4F, 0x25), 4.0f, 0.0f},      // rust edges,
                                                   {rgba(0xFF, 0xFE, 0x11), 2.0f, 0.0f},      // a yellow core
                                                   {rgba(0xFF, 0xFE, 0xC4), 1.0f, 0.5f}};     // lit cream on top
        // A sub-option's elbow from its parent (K's pick, Nov 13): the unselected names' tan.
        constexpr float kElbowThickness = 1.4f;

        // Archivo Narrow fitted to the original's painted row names: horizontal scale, extra tracking (layout units).
        struct Fit
        {
            const char *label;
            float scaleX, tracking;
        };
        constexpr Fit kFits[] = {{"CONTROLS", 0.77f, 3.0f},   {"ENEMY LIFE GAUGE", 0.60f, 1.1f}, {"RESTORE DEFAULTS", 0.66f, 1.1f},
                                 {"LOAD GAME", 0.72f, 2.7f},  {"SOUND", 0.77f, 1.0f},            {"VIBRATION", 0.77f, 2.5f},
                                 {"RESET GAME", 0.71f, 2.0f}};
        constexpr Fit kNewLabel{"", 0.77f, 2.5f}; // anything else (squeezed to kMaxLabel when longer)
        const Fit &fitFor(const char *label)
        {
            for (const Fit &f : kFits)
                if (std::strcmp(f.label, label) == 0)
                    return f;
            return kNewLabel;
        }

        // Layout units -> fractions of the picture. The text area is the picture at most 21:9 wide, centred; one unit
        // is 7/6 of a line, so horizontal lengths as fractions of the height are units / 384.
        struct Space
        {
            float wu = 512.0f, area = 512.0f, left = 0.0f;
            explicit Space(float aspect)
            {
                wu = 384.0f * aspect;
                area = 384.0f * std::min(aspect, 21.0f / 9.0f);
                left = 0.5f * (wu - area);
            }
            float x(float units) const { return (left + units) / wu; }
            static float y(float lines) { return lines / 448.0f; }
            static float h(float units) { return units / 384.0f; }
            static float scale(float layoutScale) { return layoutScale * 7.0f / 6.0f; } // fitted in layout units -> square
        };

        struct Builder
        {
            std::vector<Pcsx2GsScreenItem> &items;
            std::string &strings;
            const Space space;
            float fade;
            uint32_t labelFont = 0, helpFont = 0;
            float dim = 1.0f; // a page shown before it is opened: half strength

            uint32_t add(const char *text, size_t length)
            {
                const uint32_t offset = static_cast<uint32_t>(strings.size());
                strings.append(text, length);
                strings.push_back('\0');
                return offset;
            }
            uint32_t faded(uint32_t colour, float alpha = 1.0f) const
            {
                const float a = static_cast<float>(colour >> 24) * std::clamp(fade * alpha * dim, 0.0f, 1.0f);
                return (colour & 0x00FFFFFFu) | (static_cast<uint32_t>(a + 0.5f) << 24);
            }
            Pcsx2GsScreenItem base() const
            {
                Pcsx2GsScreenItem item{};
                item.blur = Space::y(kBlur);
                item.shadow = faded(rgba(0, 0, 0), kShadowAlpha);
                item.shadow_x = item.shadow_y = Space::y(kShadowOffset);
                return item;
            }

            // One line: x in units, the baseline in lines.
            void text(const char *line, size_t length, uint32_t font, float x, float baseline, float cap, float scaleX,
                      float tracking, uint32_t fill, uint32_t stroke, float maxWidth = 0.0f, uint32_t align = PCSX2GS_ALIGN_LEFT)
            {
                if (length == 0u)
                    return;
                Pcsx2GsScreenItem item = base();
                item.kind = PCSX2GS_SCREEN_TEXT;
                item.align = align;
                item.text = add(line, length);
                item.font = font;
                item.fill = faded(fill);
                item.stroke = faded(stroke);
                item.stroke_width = Space::y(kStroke);
                item.x = space.x(x);
                item.y = Space::y(baseline);
                item.size = Space::y(cap);
                item.scale_x = Space::scale(scaleX);
                item.tracking = Space::h(tracking);
                item.max_width = Space::h(maxWidth);
                items.push_back(item);
            }

            void label(const char *name, float centre, bool on, float x = kLabelX, float maxWidth = kMaxLabel, float cap = kLabelCap)
            {
                const Fit &f = fitFor(name);
                text(name, std::strlen(name), labelFont, x, centre + 0.5f * cap, cap, f.scaleX, f.tracking,
                     on ? kLabelOn : kLabel, on ? kLabelOnStroke : 0u, maxWidth);
            }

            void value(const char *name, float x, float centre, bool on, float maxWidth = 0.0f, uint32_t align = PCSX2GS_ALIGN_LEFT)
            {
                text(name, std::strlen(name), labelFont, x, centre + 0.5f * kValueCap, kValueCap, kNewLabel.scaleX,
                     kNewLabel.tracking, on ? kValueOn : kValue, on ? kValueOnStroke : 0u, maxWidth, align);
            }

            // A small arrow either side of a value with more than three choices.
            void arrow(float x, float centre, bool right, bool on)
            {
                Pcsx2GsScreenItem item = base();
                item.kind = PCSX2GS_SCREEN_TRIANGLE;
                item.fill = faded(on ? kValueOn : kValue);
                item.stroke = on ? faded(kValueOnStroke) : 0u;
                item.stroke_width = Space::y(kStroke);
                const float tip = right ? x + 4.5f : x - 4.5f, back = right ? x - 4.5f : x + 4.5f;
                item.x = space.x(back);
                item.y = Space::y(centre - 6.0f);
                item.x1 = space.x(tip);
                item.y1 = Space::y(centre);
                item.x2 = space.x(back);
                item.y2 = Space::y(centre + 6.0f);
                items.push_back(item);
            }

            void rect(float x0, float y0, float x1, float y1, uint32_t colour, bool afterText = false)
            {
                Pcsx2GsScreenItem item{};
                item.kind = PCSX2GS_SCREEN_RECT;
                item.align = afterText ? PCSX2GS_ALIGN_AFTER_TEXT : PCSX2GS_ALIGN_LEFT;
                item.fill = faded(colour);
                item.x = afterText ? Space::h(x0) : space.x(x0);
                item.x1 = space.x(x1);
                item.y = Space::y(y0);
                item.y1 = Space::y(y1);
                items.push_back(item);
            }

            void bar(uint32_t path, float x0, float y0, float x1, float y1)
            {
                Pcsx2GsScreenItem item{};
                item.kind = PCSX2GS_SCREEN_IMAGE;
                item.text = path;
                item.fill = faded(rgba(255, 255, 255));
                item.size = 0.25f; // the rounded ends (tools/options/make_scrollbar.py)
                item.x = space.x(x0);
                item.x1 = space.x(x1);
                item.y = Space::y(y0);
                item.y1 = Space::y(y1);
                items.push_back(item);
            }

            // A number's slider in the values block: a bar in the look of the enemies' health bar (K, Nov 13: its fill
            // and dark background, no lag colour), the number at the right, white on every row as the game's chosen
            // values are.
            void slider(const PageRow &row, float x, float centre)
            {
                const int c = std::clamp(row.choice, 0, row.valueCount - 1);
                char number[24];
                std::snprintf(number, sizeof(number), "%d%s", row.values[c], row.unit ? row.unit : "");
                value(number, x + kValueBlock, centre, true, kSliderNumber - 4.0f, PCSX2GS_ALIGN_RIGHT);
                const float x1 = x + kValueBlock - kSliderNumber, first = static_cast<float>(row.values[0]);
                const float span = static_cast<float>(row.values[row.valueCount - 1]) - first;
                const float f = span > 0.0f ? std::clamp((static_cast<float>(row.values[c]) - first) / span, 0.0f, 1.0f) : 0.0f;
                const float border = kSliderBorder * kLinesToUnits;
                rect(x - border, centre - kSliderHalf - kSliderBorder, x1 + border, centre + kSliderHalf + kSliderBorder, kSliderBack);
                Pcsx2GsScreenItem &back = items.back(); // the text's drop shadow, setting the bar off from the wall (K)
                back.shadow = faded(rgba(0, 0, 0), kShadowAlpha);
                back.shadow_x = back.shadow_y = Space::y(kShadowOffset);
                if (f > 0.0f && !g_sliderFill.empty())
                {
                    Pcsx2GsScreenItem item{};
                    item.kind = PCSX2GS_SCREEN_IMAGE;
                    item.text = add(g_sliderFill.data(), g_sliderFill.size());
                    item.fill = faded(rgba(255, 255, 255));
                    item.x = space.x(x);
                    item.x1 = space.x(x + (x1 - x) * f);
                    item.y = Space::y(centre - kSliderHalf);
                    item.y1 = Space::y(centre + kSliderHalf);
                    items.push_back(item);
                }
            }

            // "-[": from the end of the last label drawn (the open category's) to an upright at x spanning top..bottom,
            // with short ticks into the page at both ends.
            void bracket(float x, float row, float top, float bottom)
            {
                const BracketLayer *layers = kBracketLayers;
                const size_t count = std::size(kBracketLayers);
                for (size_t i = 0; i < count; ++i)
                {
                    const float t = 0.5f * layers[i].thickness, lift = layers[i].lift;
                    const float tu = t * kLinesToUnits, lu = lift * kLinesToUnits, ux = x - lu;
                    const float wide = 0.5f * layers[0].thickness * kLinesToUnits; // the widest layer's half
                    rect(kBracketGap * kLinesToUnits - tu + wide, row - lift - t, ux + tu, row - lift + t, layers[i].colour, true);
                    rect(ux - tu, top - lift - t, ux + tu, bottom - lift + t, layers[i].colour);
                    const float tick = x + kBracketTick - (wide - tu); // the wider layers cap the ticks' ends
                    rect(ux - tu, top - lift - t, tick, top - lift + t, layers[i].colour);
                    rect(ux - tu, bottom - lift - t, tick, bottom - lift + t, layers[i].colour);
                }
            }

            void cursor(float centre, float x = kCursorX)
            {
                Pcsx2GsScreenItem item = base();
                item.kind = PCSX2GS_SCREEN_TRIANGLE;
                item.fill = faded(kLabelOn);
                item.stroke = faded(kLabelOnStroke);
                item.stroke_width = Space::y(kStroke);
                item.x = space.x(x - kCursorHalfW);
                item.y = Space::y(centre - kCursorHalfH);
                item.x1 = space.x(x + kCursorHalfW);
                item.y1 = Space::y(centre);
                item.x2 = space.x(x - kCursorHalfW);
                item.y2 = Space::y(centre + kCursorHalfH);
                items.push_back(item);
            }

            // Two lines at most.
            void help(const char *body)
            {
                int lines = body && *body ? 1 : 0;
                for (const char *c = body; c && *c; ++c)
                    lines += *c == '\n' && c[1] ? 1 : 0;
                const float block = kHelpCap + kHelpLine * static_cast<float>(std::max(lines - 1, 0));
                float baseline = 0.5f * (kHelpBandTop + kHelpBandBottom - block) + kHelpCap;
                for (const char *line = body; line && *line;)
                {
                    const char *end = std::strchr(line, '\n');
                    const size_t length = end ? static_cast<size_t>(end - line) : std::strlen(line);
                    text(line, length, helpFont, kHelpX, baseline, kHelpCap, kHelpScaleX, 0.0f, kHelp, 0u);
                    Pcsx2GsScreenItem &item = items.back();
                    item.shadow = faded(kHelpShadow);
                    item.shadow_x = Space::y(kHelpShadowX);
                    item.shadow_y = Space::y(kHelpShadowY);
                    baseline += kHelpLine;
                    line = end ? end + 1 : nullptr;
                }
            }

            // A page's rows between left (its cursor's x) and right (the scroll bar's right edge). selected < 0: no row
            // highlighted.
            void page(const Page &p, int scroll, int selected, float left, float right, bool namesOnly = false)
            {
                const float names = left + kPageNameGap - kPageCursorGap, values = right - kScrollWidth - kScrollGap - kValueBlock;
                float y = kPageTop;
                scroll = std::clamp(scroll, 0, std::max(p.rowCount - 1, 0));
                const int shown = pageRowsShown(p, scroll);
                float parent = kPageTop; // the last option's centre (a sub-option's elbow starts under it)
                for (int i = scroll; i < scroll + shown; ++i)
                {
                    const PageRow &row = p.rows[i];
                    const float centre = y + 0.5f * rowHeight(row);
                    y += rowHeight(row);
                    if (!row.header && !row.sub)
                        parent = centre;
                    if (row.header)
                    {
                        text(row.name, std::strlen(row.name), labelFont, names, centre + 0.5f * kHeaderCap, kHeaderCap, kNewLabel.scaleX,
                             kHeaderTracking, kHelp, 0u, right - names);
                        Pcsx2GsScreenItem &item = items.back();
                        item.shadow = faded(kHelpShadow);
                        item.shadow_x = Space::y(0.12f * kHeaderCap);
                        item.shadow_y = Space::y(0.11f * kHeaderCap);
                        continue;
                    }
                    const bool on = i == selected;
                    const float indent = row.sub ? 2.0f * kSubIndent : 0.0f;
                    const float x = names + indent;
                    if (row.sub)
                    {
                        // An elbow from under the parent's first letter to just before the sub-option's name.
                        const float t = 0.5f * kElbowThickness, tu = t * kLinesToUnits, ex = names + 5.0f;
                        rect(ex - tu, parent + 0.5f * kLabelCap + 3.0f, ex + tu, centre + t, kLabel);
                        rect(ex - tu, centre - t, x - 5.0f, centre + t, kLabel);
                    }
                    label(row.name, centre, on, x, values - 10.0f - x, row.sub ? kSubCap : kLabelCap);
                    if (on)
                        cursor(centre, left); // the cursor keeps its column, clear of a sub-option's mark
                    if (namesOnly)
                        continue;
                    if (row.valueCount > 0 && row.values)
                    {
                        slider(row, values, centre);
                        continue;
                    }
                    if (row.choiceCount <= 0)
                        continue;
                    if (row.choiceCount <= 3)
                    {
                        const float slot = kValueBlock / static_cast<float>(row.choiceCount);
                        for (int c = 0; c < row.choiceCount; ++c)
                            value(row.choices[c], values + slot * (static_cast<float>(c) + 0.5f), centre, c == row.choice,
                                  slot - 8.0f, PCSX2GS_ALIGN_CENTRE);
                    }
                    else
                    {
                        const int c = std::clamp(row.choice, 0, row.choiceCount - 1);
                        arrow(values + 5.0f, centre, false, on);
                        value(row.choices[c], values + 0.5f * kValueBlock, centre, true, kValueBlock - 40.0f, PCSX2GS_ALIGN_CENTRE);
                        arrow(values + kValueBlock - 5.0f, centre, true, on);
                    }
                }
                // The scroll bar, when the page is longer than it shows: the thumb's share and place by height.
                float total = 0.0f, before = 0.0f;
                for (int i = 0; i < p.rowCount; ++i)
                {
                    total += rowHeight(p.rows[i]);
                    before += i < scroll ? rowHeight(p.rows[i]) : 0.0f;
                }
                if (!namesOnly && total > kPageBottom - kPageTop + 0.01f && !g_scrollTrack.empty())
                {
                    const float x0 = right - kScrollWidth, top = kPageTop + 2.0f, span = kPageBottom - kPageTop - 4.0f;
                    bar(add(g_scrollTrack.data(), g_scrollTrack.size()), x0, top, right, top + span);
                    const float length = std::max(span * (kPageBottom - kPageTop) / total, 18.0f);
                    const float at = top + (span - length) * std::clamp(before / std::max(total - (kPageBottom - kPageTop), 1.0f), 0.0f, 1.0f);
                    bar(add(g_scrollThumb.data(), g_scrollThumb.size()), x0, at, right, at + length);
                }
            }

            void title(uint32_t path)
            {
                Pcsx2GsScreenItem item = base();
                item.kind = PCSX2GS_SCREEN_IMAGE;
                item.text = path;
                item.fill = faded(rgba(255, 255, 255));
                const float width = kTitleWidth / kTitlePixelsPerUnit, centre = 0.5f * space.area;
                item.x = space.x(centre - 0.5f * width);
                item.x1 = space.x(centre + 0.5f * width);
                item.y = Space::y(kTitleTop);
                item.y1 = Space::y(kTitleTop + kTitleHeight / kTitlePixelsPerUnit);
                items.push_back(item);
            }
        };
    }

    void build(const View &view, std::vector<Pcsx2GsScreenItem> &items, std::string &strings)
    {
        items.clear();
        strings.clear();
        if (view.fade <= 0.0f || !view.rows || view.rowCount <= 0)
            return;
        Builder b{items, strings, Space(std::clamp(view.aspect, 1.0f, 4.0f)), std::min(view.fade, 1.0f)};
        b.labelFont = b.add(g_labelFont.data(), g_labelFont.size());
        b.helpFont = b.add(g_helpFont.data(), g_helpFont.size());
        const bool split = view.aspect >= kSplitAspect;
        const Page *page = view.page && view.page->rowCount > 0 ? view.page : nullptr;
        const bool open = page && view.inPage;

        // The title: OPTIONS, or the open category's (K, Nov 13: it replaces OPTIONS until the page is left).
        const char *title = open ? page->name : "OPTIONS";
        if (!g_titleDir.empty())
        {
            std::string path = g_titleDir + "/title_";
            for (const char *c = title; *c; ++c)
                path.push_back(static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c));
            path += ".png";
            b.title(b.add(path.data(), path.size()));
        }
        else
            b.text(title, std::strlen(title), b.labelFont, 0.5f * b.space.area, kTitleY, kTitleCap, kNewLabel.scaleX,
                   kNewLabel.tracking, kLabelOn, kLabelOnStroke, 0.0f, PCSX2GS_ALIGN_CENTRE);

        const float pageRight = std::min(b.space.area - kLabelX, kBracketX + kPageWidth);
        if (open && !split)
        {
            // Narrow: the page replaces the list.
            b.page(*page, view.pageScroll, view.pageCursor, kCursorX, b.space.area - kLabelX);
            b.help(view.pageCursor >= 0 && view.pageCursor < page->rowCount ? page->rows[view.pageCursor].help : "");
            return;
        }

        // The pause's rows fill the original's span; the title's (fewer) keep its spacing.
        const float step = view.pause && view.rowCount > 1 ? std::min(kRowStep, (kLastRowY - kFirstRowY) / static_cast<float>(view.rowCount - 1))
                                                           : kRowStep;
        const int cursor = std::clamp(view.cursor, 0, view.rowCount - 1);
        for (int i = 0; i < view.rowCount; ++i)
            if (i != cursor)
                b.label(view.rows[i].name, kFirstRowY + step * static_cast<float>(i), false);
        const float centre = kFirstRowY + step * static_cast<float>(cursor);
        b.label(view.rows[cursor].name, centre, true); // last, so the bracket starts at its end
        if (!open)
            b.cursor(centre);

        if (split && page && open)
        {
            const auto [first, last] = pageExtent(*page, view.pageScroll);
            b.bracket(kBracketX, centre, std::min(first, centre) - kBracketReach, std::max(last, centre) + kBracketReach);
            b.page(*page, view.pageScroll, view.pageCursor, kBracketX + kPageCursorGap, pageRight);
        }
        else if (split && page)
        {
            // Before it is opened: the page's names only, at half strength (K, Nov 13).
            b.dim = 0.5f;
            b.page(*page, 0, -1, kBracketX + kPageCursorGap, pageRight, true);
            b.dim = 1.0f;
        }

        if (view.confirmRow >= 0 && view.confirmRow < view.rowCount)
        {
            const float at = kFirstRowY + step * static_cast<float>(view.confirmRow);
            b.value("YES", kValueX, at, view.confirmYes);
            b.value("NO", kValueX + kValueGap, at, !view.confirmYes);
            b.help(view.question);
        }
        else if (open)
            b.help(view.pageCursor >= 0 && view.pageCursor < page->rowCount ? page->rows[view.pageCursor].help : "");
        else
            b.help(view.rows[cursor].help);
    }
#endif
}
