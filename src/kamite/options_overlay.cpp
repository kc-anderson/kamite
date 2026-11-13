// The OPTIONS screen's overlay list. See options_overlay.h; measurements: claude/rearmed-options.md.
#include "kamite/options_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <utility>

namespace kamite::options_overlay
{
    namespace
    {
        std::string g_labelFont, g_helpFont, g_titleDir, g_scrollTrack, g_scrollThumb, g_sliderFill;
        std::map<std::string, std::pair<float, float>> g_titleLetters; // setTitleLetters

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
        // A header never ends the page with its options below the edge.
        const int first = std::max(scroll, 0);
        if (shown > 1 && first + shown < page.rowCount && page.rows[first + shown - 1].header)
            --shown;
        return shown;
    }

    void setScrollBar(const std::string &track, const std::string &thumb)
    {
        g_scrollTrack = track;
        g_scrollThumb = thumb;
    }

    void setSliderFill(const std::string &fill) { g_sliderFill = fill; }

    void setTitleLetters(const std::string &name, float left, float right) { g_titleLetters[name] = {left, right}; }

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
        // The confirm / cancel hints: the help's text in the darker strip under its band (K, Nov 13: half-way up from
        // the strip's centre toward the help's last line), right-aligned kHintRight units in from the text area's edge; each button image kHintArt lines square (the
        // pack's 100 x 100 buttons fill about 84 % of theirs), kHintArtGap units before its word, the two hints
        // kHintGap apart.
        constexpr float kHintY = 428.0f, kHintRight = 16.0f, kHintArt = 24.0f, kHintArtGap = 1.5f, kHintGap = 14.0f;
        // The L1 / R1 hints beside an open page's title (K, Nov 13): each button kTitleHintGap units out from the
        // letters, centred on them (kTitleHintY), then the neighbouring category's name kTitleNameGap further out, in
        // the list's unselected tan at kTitleNameCap (squeezed to stay kTitleNameMargin in from the text area's edge).
        constexpr float kTitleHintY = 43.0f, kTitleHintArt = 36.0f, kTitleHintGap = 6.0f, kTitleNameGap = 3.0f;
        constexpr float kTitleNameCap = 17.0f, kTitleNameMargin = 8.0f;
        // The titles (title_<name>.png: K's painted OPTIONS and the categories made to match by
        // tools/options/make_splash.py), placed as the original's painted one (its letters x 146..363, y 20..66 of the
        // 512 x 448 screen): one canvas for all, the letters centred across it at one height, 8.72 of its pixels per
        // unit and per line (K's lettering at half size), so a page's title swaps in exactly where OPTIONS was.
        constexpr float kTitlePixelsPerUnit = 8.72f, kTitleTop = 12.32f, kTitleWidth = 2467.0f, kTitleHeight = 535.0f;
        constexpr float kTitleY = 70.0f, kTitleCap = 30.0f; // the text stand-in when the image is missing
        constexpr float kLettersLeft = 0.12f, kLettersRight = 0.88f; // of the canvas, for a title never measured
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

        // BUTTON MAPPING (claude/button-mapping.md, the original controller page measured from the disc). The pad: its
        // picture kMapPadHeight lines tall (at most kMapPadWidth units wide), centred at kMapPadY. The columns: their
        // outer ends kMapReach units out from the picture's sides (at least kMapEdge in from the text area's), each
        // row's inner end kMapGap past the pad's outline at its height (the original's panels stop at the pad), the
        // panel kMapPanelMin..kMapPanelMax wide (kMapPanelFree clear of the pad). Rows between kMapTop and kMapBottom, as near their buttons' height as
        // they fit, kMapRowGap apart; all squeezed together when they do not fit.
        constexpr float kMapPadY = 210.0f, kMapPadHeight = 232.0f, kMapPadWidth = 290.0f;
        constexpr float kMapReach = 172.0f, kMapEdge = 10.0f, kMapGap = 5.0f, kMapPanelMin = 96.0f, kMapPanelMax = 178.0f;
        constexpr float kMapPanelFree = 128.0f; // a row above or below the pad (the original's MENU panel)
        constexpr float kMapTop = 50.0f, kMapBottom = 356.0f, kMapRowGap = 4.5f, kMapTitleClear = 70.0f;
        // A row: one line at kMapCap, more at kMapCapN kMapPitch apart, kMapRowPad round them, a HOT line kMapHotLine
        // more (kMapHotCap). The button's icon kMapIcon lines square at the label's left (the pack's buttons fill about
        // 84 % of theirs); the text kMapTextGap after it, kMapTextMargin from the panel's end; a line pushed in kMapIndent.
        // The letters: the list's shape, tracked out kMapTracking of the cap (the original spreads its short words).
        constexpr float kMapCap = 15.5f, kMapCapN = 11.5f, kMapPitch = 14.0f, kMapRowPad = 10.5f, kMapHotCap = 9.5f, kMapHotLine = 12.0f;
        constexpr float kMapIcon = 27.0f, kMapTextGap = 4.0f, kMapTextMargin = 6.0f, kMapIndent = 12.0f, kMapTracking = 0.13f;
        // The lines (brush.png stretched along them, its core about half the thickness): the leader from the button to
        // the panel's lower inner corner, then along the panel's bottom (the underscore); each upper line of a label
        // gets its own, thinner.
        constexpr float kMapLine = 5.5f, kMapSubLine = 4.5f, kMapBrushSpan = 300.0f;
        // The panels' wash (panel.png: opaque at the outer end): the original's tan, 0.40 at the outer end.
        constexpr uint32_t kMapPanel = rgba(147, 115, 88, 102), kMapPanelOn = rgba(160, 125, 92, 150);
        constexpr uint32_t kMapText = rgba(0xDE, 0xD3, 0xB9), kMapStroke = rgba(0x01, 0x01, 0x00); // cream, black stroke

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

            // A shape in the text's look: the same softening and drop shadow (base()).
            void textLook(Pcsx2GsScreenItem &item) const
            {
                const Pcsx2GsScreenItem look = base();
                item.blur = look.blur;
                item.shadow = look.shadow;
                item.shadow_x = look.shadow_x;
                item.shadow_y = look.shadow_y;
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
                // The bar's length is the number's share of the largest (from 0, not from the smallest stop: a field
                // of view of 100 % of 150 shows two thirds, not an empty bar).
                const float x1 = x + kValueBlock - kSliderNumber;
                const float top = static_cast<float>(std::max(row.values[row.valueCount - 1], 1));
                const float f = std::clamp(static_cast<float>(row.values[c]) / top, 0.0f, 1.0f);
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
                    text(line, length, helpFont, kHelpX, baseline, kHelpCap, kHelpScaleX, 0.0f, kHelp, 0u, space.area - 2.0f * kHelpX);
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
                        // In the text's look (K): its softening and drop shadow.
                        const float t = 0.5f * kElbowThickness, tu = t * kLinesToUnits, ex = names + 5.0f;
                        rect(ex - tu, parent + 0.5f * kLabelCap + 3.0f, ex + tu, centre + t, kLabel);
                        textLook(items.back());
                        rect(ex - tu, centre - t, x - 5.0f, centre + t, kLabel);
                        textLook(items.back());
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
                    if (row.choiceCount <= 3 && !row.list)
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

            // One help-look line, from the right: the word ends at right (units) or, with right < 0, gap units before
            // the last text or image drawn.
            void hintWord(const char *word, float right, float gap)
            {
                const size_t length = std::strlen(word);
                if (length == 0u)
                    return;
                Pcsx2GsScreenItem item = base();
                item.kind = PCSX2GS_SCREEN_TEXT;
                item.align = right < 0.0f ? PCSX2GS_ALIGN_BEFORE_LAST : PCSX2GS_ALIGN_RIGHT;
                item.text = add(word, length);
                item.font = helpFont;
                item.fill = faded(kHelp);
                item.stroke = 0u;
                item.x = right < 0.0f ? Space::h(gap) : space.x(right);
                item.y = Space::y(kHintY + 0.5f * kHelpCap);
                item.size = Space::y(kHelpCap);
                item.scale_x = Space::scale(kHelpScaleX);
                item.shadow = faded(kHelpShadow);
                item.shadow_x = Space::y(kHelpShadowX);
                item.shadow_y = Space::y(kHelpShadowY);
                items.push_back(item);
            }

            // A button image gap units before the last text or image drawn.
            void hintArt(const char *path, float gap)
            {
                Pcsx2GsScreenItem item{};
                item.kind = PCSX2GS_SCREEN_IMAGE;
                item.align = PCSX2GS_ALIGN_BEFORE_LAST;
                item.text = add(path, std::strlen(path));
                item.fill = faded(rgba(255, 255, 255));
                item.x = Space::h(gap);
                item.x1 = Space::y(kHintArt);
                item.y = Space::y(kHintY - 0.5f * kHintArt);
                item.y1 = Space::y(kHintY + 0.5f * kHintArt);
                items.push_back(item);
            }

            // "[A] Confirm  [B] Cancel", laid out from the right end.
            void hints(const Hint &confirm, const Hint &cancel)
            {
                float gap = -1.0f; // the first word: at the edge
                for (const Hint *h : {&cancel, &confirm})
                {
                    if (!*h->word)
                        continue;
                    hintWord(h->word, gap < 0.0f ? space.area - kHintRight : -1.0f, gap);
                    float artGap = kHintArtGap;
                    for (int i = 1; i >= 0; --i) // drawn right to left: art[1] beside the word
                        if (h->art[i] && *h->art[i])
                        {
                            hintArt(h->art[i], artGap);
                            artGap = 0.0f;
                        }
                    gap = kHintGap;
                }
            }

            // L1 / R1 beside the open page's title, the neighbours' names outside them.
            void titleHints(const char *name, const Hint &prev, const Hint &next)
            {
                std::string key;
                for (const char *c = name; *c; ++c)
                    key.push_back(static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c));
                const auto found = g_titleLetters.find(key);
                const float l = found != g_titleLetters.end() ? found->second.first : kLettersLeft * kTitleWidth;
                const float r = found != g_titleLetters.end() ? found->second.second : kLettersRight * kTitleWidth;
                const float canvas = 0.5f * space.area - 0.5f * kTitleWidth / kTitlePixelsPerUnit;
                const float lettersLeft = canvas + l / kTitlePixelsPerUnit, lettersRight = canvas + r / kTitlePixelsPerUnit;
                const float art = kTitleHintArt * kLinesToUnits;
                const auto image = [&](const char *path, float x0)
                {
                    Pcsx2GsScreenItem item{};
                    item.kind = PCSX2GS_SCREEN_IMAGE;
                    item.text = add(path, std::strlen(path));
                    item.fill = faded(rgba(255, 255, 255));
                    item.x = space.x(x0);
                    item.x1 = space.x(x0 + art);
                    item.y = Space::y(kTitleHintY - 0.5f * kTitleHintArt);
                    item.y1 = Space::y(kTitleHintY + 0.5f * kTitleHintArt);
                    items.push_back(item);
                };
                const auto label = [&](const char *word, float x, uint32_t align, float room)
                {
                    if (room <= 4.0f)
                        return;
                    text(word, std::strlen(word), labelFont, x, kTitleHintY + 0.5f * kTitleNameCap, kTitleNameCap, kNewLabel.scaleX,
                         kNewLabel.tracking, kLabel, 0u, room, align);
                };
                if (*prev.word)
                {
                    const float x0 = lettersLeft - kTitleHintGap - art;
                    if (prev.art[0] && *prev.art[0])
                        image(prev.art[0], x0);
                    label(prev.word, x0 - kTitleNameGap, PCSX2GS_ALIGN_RIGHT, x0 - kTitleNameGap - kTitleNameMargin);
                }
                if (*next.word)
                {
                    const float x0 = lettersRight + kTitleHintGap;
                    if (next.art[0] && *next.art[0])
                        image(next.art[0], x0);
                    label(next.word, x0 + art + kTitleNameGap, PCSX2GS_ALIGN_LEFT,
                          space.area - kTitleNameMargin - (x0 + art + kTitleNameGap));
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

            // A title's letters in units (setTitleLetters), left and right.
            std::pair<float, float> titleLetters(const char *name) const
            {
                std::string key;
                for (const char *c = name; *c; ++c)
                    key.push_back(static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c));
                const auto found = g_titleLetters.find(key);
                const float l = found != g_titleLetters.end() ? found->second.first : kLettersLeft * kTitleWidth;
                const float r = found != g_titleLetters.end() ? found->second.second : kLettersRight * kTitleWidth;
                const float canvas = 0.5f * space.area - 0.5f * kTitleWidth / kTitlePixelsPerUnit;
                return {canvas + l / kTitlePixelsPerUnit, canvas + r / kTitlePixelsPerUnit};
            }

            // An image over x0..x1 (units; x1 < x0 mirrors it) and y0..y1 (lines).
            void picture(const char *path, float x0, float y0, float x1, float y1, uint32_t fill)
            {
                if (!path || !*path)
                    return;
                Pcsx2GsScreenItem item{};
                item.kind = PCSX2GS_SCREEN_IMAGE;
                item.text = add(path, std::strlen(path));
                item.fill = faded(fill);
                item.x = space.x(x0);
                item.x1 = space.x(x1);
                item.y = Space::y(y0);
                item.y1 = Space::y(y1);
                items.push_back(item);
            }

            // A brushed line from (x0, y0) to (x1, y1) (units, lines), thickness lines across, the brush's u range
            // picked by seed so neighbouring lines do not show the same wobble.
            void stroke(const char *brush, float x0, float y0, float x1, float y1, float thickness, int seed, uint32_t fill)
            {
                const float dx = (x1 - x0) * 7.0f / 6.0f, dy = y1 - y0, length = std::sqrt(dx * dx + dy * dy);
                if (length < 0.5f)
                    return;
                Pcsx2GsScreenItem item{};
                item.kind = PCSX2GS_SCREEN_LINE;
                item.text = add(brush, std::strlen(brush));
                item.fill = faded(fill);
                item.x = space.x(x0);
                item.y = Space::y(y0);
                item.x1 = space.x(x1);
                item.y1 = Space::y(y1);
                item.stroke_width = Space::y(thickness);
                const float span = std::clamp(length / kMapBrushSpan, 0.2f, 1.0f);
                const float at = std::fmod(0.6180339f * static_cast<float>(seed + 1), 1.0f) * (1.0f - span);
                item.x2 = at;
                item.y2 = at + span;
                items.push_back(item);
            }

            // CONTROLS > BUTTON MAPPING: panels, the pad in its flames, the lines, the icons, the labels.
            void mapping(const MappingView &m)
            {
                const float aspect = std::max(m.padAspect, 0.1f);
                const float padH = std::min(kMapPadHeight, kMapPadWidth / (aspect * kLinesToUnits));
                const float padW = padH * aspect * kLinesToUnits; // units
                const float cx = 0.5f * space.area, padL = cx - 0.5f * padW, padR = cx + 0.5f * padW;
                const float padT = kMapPadY - 0.5f * padH;
                const float outerL = std::max(kMapEdge, padL - kMapReach), outerR = std::min(space.area - kMapEdge, padR + kMapReach);
                const auto [titleL, titleR] = titleLetters("CONTROLS");

                struct Row
                {
                    int line;
                    float height, centre, top, bottom, inner, scale;
                };
                std::vector<Row> rows[2];
                for (int i = 0; i < m.lineCount; ++i)
                {
                    const MappingLine &l = m.lines[i];
                    const int n = std::max(l.count, 1);
                    const float h = (n == 1 ? kMapCap : kMapCapN + kMapPitch * static_cast<float>(n - 1)) + kMapRowPad +
                                    (*l.hot ? kMapHotLine : 0.0f);
                    rows[l.right ? 1 : 0].push_back(Row{i, h, padT + l.py * padH, 0, 0, 0, 1.0f});
                }
                for (int side = 0; side < 2; ++side)
                {
                    std::vector<Row> &col = rows[side];
                    if (col.empty())
                        continue;
                    // Squeezed together when they do not fit.
                    float total = kMapRowGap * static_cast<float>(col.size() - 1);
                    for (const Row &r : col)
                        total += r.height;
                    const float s = std::min(1.0f, (kMapBottom - kMapTop) / std::max(total, 1.0f));
                    const float gap = kMapRowGap * s;
                    for (Row &r : col)
                    {
                        r.height *= s;
                        r.scale = s;
                    }
                    // As near their buttons' height as they fit, in order: c = centre - (its least distance from the
                    // first row's), made non-decreasing (pool adjacent violators) and kept inside the band.
                    std::vector<float> offset(col.size()), c(col.size()), weight(col.size(), 1.0f);
                    for (size_t i = 0; i < col.size(); ++i)
                    {
                        offset[i] = i == 0 ? 0.0f : offset[i - 1] + 0.5f * (col[i - 1].height + col[i].height) + gap;
                        c[i] = col[i].centre - offset[i];
                    }
                    std::vector<std::pair<float, float>> blocks; // (mean, weight)
                    std::vector<size_t> sizes;
                    for (size_t i = 0; i < c.size(); ++i)
                    {
                        blocks.emplace_back(c[i], 1.0f);
                        sizes.push_back(1);
                        while (blocks.size() > 1 && blocks[blocks.size() - 2].first > blocks.back().first)
                        {
                            const auto b = blocks.back();
                            const size_t n = sizes.back();
                            blocks.pop_back();
                            sizes.pop_back();
                            auto &a = blocks.back();
                            a.first = (a.first * a.second + b.first * b.second) / (a.second + b.second);
                            a.second += b.second;
                            sizes.back() += n;
                        }
                    }
                    const float lo = kMapTop + 0.5f * col.front().height;
                    const float hi = kMapBottom - 0.5f * col.back().height - offset.back();
                    size_t k = 0;
                    for (size_t b = 0; b < blocks.size(); ++b)
                        for (size_t j = 0; j < sizes[b]; ++j, ++k)
                            c[k] = std::clamp(blocks[b].first, lo, std::max(lo, hi));
                    for (size_t i = 0; i < col.size(); ++i)
                    {
                        Row &r = col[i];
                        r.centre = c[i] + offset[i];
                        r.top = r.centre - 0.5f * r.height;
                        r.bottom = r.centre + 0.5f * r.height;
                        // The inner end: just past the pad's outline over the row's height (the original's panels stop at
                        // the pad), clear of the title's letters near the top.
                        float edge = side == 1 ? -1e9f : 1e9f;
                        for (int band = 0; m.silhouette && band < m.bands; ++band)
                        {
                            const float b0 = padT + padH * static_cast<float>(band) / static_cast<float>(m.bands);
                            const float b1 = padT + padH * static_cast<float>(band + 1) / static_cast<float>(m.bands);
                            const float l = m.silhouette[2 * band], rr = m.silhouette[2 * band + 1];
                            if (b1 < r.top || b0 > r.bottom || l < 0.0f)
                                continue;
                            edge = side == 1 ? std::max(edge, padL + rr * padW) : std::min(edge, padL + l * padW);
                        }
                        if (side == 1)
                        {
                            float inner = edge > -1e8f ? edge + kMapGap : outerR - kMapPanelFree;
                            if (r.top < kMapTitleClear)
                                inner = std::max(inner, titleR + 8.0f);
                            r.inner = std::clamp(inner, outerR - kMapPanelMax, outerR - kMapPanelMin);
                        }
                        else
                        {
                            float inner = edge < 1e8f ? edge - kMapGap : outerL + kMapPanelFree;
                            if (r.top < kMapTitleClear)
                                inner = std::min(inner, titleL - 8.0f);
                            r.inner = std::clamp(inner, outerL + kMapPanelMin, outerL + kMapPanelMax);
                        }
                    }
                }

                // The panels: the wash opaque at the outer end (panel.png, mirrored on the right).
                for (int side = 0; side < 2; ++side)
                    for (const Row &r : rows[side])
                    {
                        const float outer = side == 1 ? outerR : outerL;
                        picture(m.panel, outer, r.top, r.inner, r.bottom, r.line == m.cursor ? kMapPanelOn : kMapPanel);
                    }
                // The pad in its flames.
                const float margin = m.flamesMargin * padH; // lines
                const float marginU = margin * kLinesToUnits;
                picture(m.flames, padL - marginU, padT - margin, padR + marginU, padT + padH + margin, rgba(255, 255, 255));
                picture(m.pad, padL, padT, padR, padT + padH, rgba(255, 255, 255));
                // The lines: leader and underscore, then each upper text line's own underscore.
                const auto textTop = [&](const Row &r, const MappingLine &l, int i) {
                    // The baseline of text line i (the last sits kMapRowPad / 2 above the bottom, a HOT line under it).
                    const float pitch = kMapPitch * r.scale, pad = 0.5f * kMapRowPad * r.scale;
                    const float last = r.bottom - pad - (*l.hot ? kMapHotLine * r.scale : 0.0f);
                    return last - pitch * static_cast<float>(std::max(l.count, 1) - 1 - i);
                };
                for (int side = 0; side < 2; ++side)
                    for (const Row &r : rows[side])
                    {
                        const MappingLine &l = m.lines[r.line];
                        const bool on = r.line == m.cursor;
                        const char *brush = on && *m.brushLit ? m.brushLit : m.brush;
                        const uint32_t tint = rgba(255, 255, 255);
                        const float outer = side == 1 ? outerR : outerL;
                        const float bx = padL + l.px * padW, by = padT + l.py * padH;
                        // The leader stops reach short of the button, toward the knee.
                        const float kx = r.inner, ky = r.bottom;
                        const float vx = (kx - bx) * 7.0f / 6.0f, vy = ky - by, len = std::sqrt(vx * vx + vy * vy);
                        const float reach = l.reach * padH;
                        if (len > reach + 1.0f)
                        {
                            const float sx = bx + vx / len * reach * kLinesToUnits, sy = by + vy / len * reach;
                            stroke(brush, sx, sy, kx, ky, kMapLine, 2 * r.line, tint);
                        }
                        stroke(brush, kx, ky, outer, ky, kMapLine, 2 * r.line + 1, tint);
                        for (int i = 0; i + 1 < l.count; ++i)
                        {
                            const float y = textTop(r, l, i) + 3.0f * r.scale;
                            const float from = side == 1 ? r.inner + kMapIcon * kLinesToUnits : r.inner - kMapTextMargin;
                            stroke(brush, from, y, outer, y, kMapSubLine, 7 * r.line + i, tint);
                        }
                    }
                // The icons (at each label's left) and the labels.
                for (int side = 0; side < 2; ++side)
                    for (const Row &r : rows[side])
                    {
                        const MappingLine &l = m.lines[r.line];
                        const bool on = r.line == m.cursor;
                        const float left = side == 1 ? r.inner : outerL, right = side == 1 ? outerR : r.inner;
                        const float icon = kMapIcon * r.scale, iconU = icon * kLinesToUnits;
                        const float first = textTop(r, l, 0), last = textTop(r, l, std::max(l.count, 1) - 1);
                        const float cap = (l.count <= 1 ? kMapCap : kMapCapN) * r.scale;
                        const float mid = 0.5f * (first - cap + last);
                        picture(l.icon, left + 1.0f, mid - 0.5f * icon, left + 1.0f + iconU, mid + 0.5f * icon, rgba(255, 255, 255));
                        const float x0 = left + 1.0f + iconU + kMapTextGap, x1 = right - kMapTextMargin, room = std::max(x1 - x0, 8.0f);
                        for (int i = 0; i < l.count; ++i)
                        {
                            const char *t = l.text[i];
                            const float base = textTop(r, l, i);
                            const uint32_t fill = on ? kLabelOn : kMapText, edge = on ? kLabelOnStroke : kMapStroke;
                            const float tracking = kMapTracking * cap;
                            if (l.count == 1) // one line: centred in its room, as the original's
                                text(t, std::strlen(t), labelFont, 0.5f * (x0 + x1), base, cap, kNewLabel.scaleX, tracking, fill,
                                     edge, room, PCSX2GS_ALIGN_CENTRE);
                            else if (l.align[i] == 2)
                                text(t, std::strlen(t), labelFont, x1, base, cap, kNewLabel.scaleX, tracking, fill, edge, room,
                                     PCSX2GS_ALIGN_RIGHT);
                            else
                            {
                                const float in = l.align[i] == 1 ? kMapIndent : 0.0f;
                                text(t, std::strlen(t), labelFont, x0 + in, base, cap, kNewLabel.scaleX, tracking, fill, edge,
                                     room - in);
                            }
                        }
                        if (*l.hot)
                        {
                            const float base = r.bottom - 0.5f * kMapRowPad * r.scale;
                            text(l.hot, std::strlen(l.hot), labelFont, x1, base, kMapHotCap * r.scale, kNewLabel.scaleX,
                                 kMapTracking * kMapHotCap * r.scale, kLabel, kMapStroke, room, PCSX2GS_ALIGN_RIGHT);
                        }
                    }
            }
        };
    }

    void appendBlackout(float alpha, std::vector<Pcsx2GsScreenItem> &items)
    {
        Pcsx2GsScreenItem item{};
        item.kind = PCSX2GS_SCREEN_RECT;
        item.align = PCSX2GS_ALIGN_LEFT;
        item.fill = rgba(0, 0, 0, static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f));
        item.x = -0.01f;
        item.x1 = 1.01f;
        item.y = -0.01f;
        item.y1 = 1.01f;
        items.push_back(item);
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
        if (view.mapping)
        {
            // BUTTON MAPPING: the whole picture, under the CONTROLS title (a sub-screen: no list, no L1 / R1).
            b.mapping(*view.mapping);
            if (!g_titleDir.empty())
            {
                const std::string path = g_titleDir + "/title_controls.png";
                b.title(b.add(path.data(), path.size()));
            }
            b.help(view.mapping->notice);
            b.hints(view.confirmHint, view.cancelHint);
            return;
        }
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
        if (open)
            b.titleHints(title, view.prevHint, view.nextHint);

        const float pageRight = std::min(b.space.area - kLabelX, kBracketX + kPageWidth);
        if (open && !split)
        {
            // Narrow: the page replaces the list.
            b.page(*page, view.pageScroll, view.pageCursor, kCursorX, b.space.area - kLabelX);
            b.help(view.pageCursor >= 0 && view.pageCursor < page->rowCount ? page->rows[view.pageCursor].help : "");
            b.hints(view.confirmHint, view.cancelHint);
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
        b.hints(view.confirmHint, view.cancelHint);
    }
#endif
}
