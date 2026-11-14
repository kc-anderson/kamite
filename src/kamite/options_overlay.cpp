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
        // An L1 / R1 switch (titleSwitch): the titles slide kTitleSlide units while streaked; the blurred twins
        // (title_<name>_blur.png, make_title_blur.py) are kTitleBlurPad of the title's width wider on each side.
        constexpr float kTitleSlide = 46.0f, kTitleBlurPad = 0.15f;

        // The page beside the list (16:10 and wider): the bracket's upright just past the list's longest name, the
        // page's cursor and names after it as the list's are after the screen's edge; its right end at the text area's
        // margin, at most kPageWidth on. Values sit in a block at the right, choices centred in equal slots.
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

        // BUTTON MAPPING (claude/button-mapping.md, the original controller page measured from the disc). The title at
        // the top left as the original's (its letters from kMapTitleX, kMapTitleScale of the other pages' size). The
        // pad's picture kMapPadHeight lines tall (at most kMapPadShare of the text area wide) centred at kMapPadY, as
        // much of the page as the original's. A column either side: every panel's inner end kMapKnee past the widest
        // pad's outline (the lines turn there: a straight edge, the same for every pad), kMapPanel units wide (kMapEdge in from the text area's
        // edges at most). The right column's rows stacked evenly over kMapTop..kMapBottom (each as tall as its label
        // needs, at least kMapRow), kMapRowGap..kMapRowGapMax apart, all squeezed together when they do not fit; the
        // left column's rows level with their mirrors in the right column (L2 by R2, L1 by R1, the d-pad by the middle
        // face row, L3 by R3, the left stick by the right), below the title (kMapTitleClear).
        constexpr float kMapTitleX = 20.0f, kMapTitleScale = 0.8f, kMapTitleClear = 62.0f;
        constexpr float kMapPadY = 230.0f, kMapPadHeight = 190.0f, kMapPadShare = 0.46f;
        constexpr float kMapKnee = 4.0f, kMapPanel = 160.0f, kMapEdge = 10.0f;
        constexpr float kMapPadWidest = 262.0f; // the widest pad's picture at kMapPadHeight (the PlayStation one), units
        constexpr float kMapTop = 40.0f, kMapBottom = 362.0f, kMapRow = 29.0f, kMapRowGap = 4.0f, kMapRowGapMax = 12.0f;
        // Text, measured from the original's cross row (ATTACK / THROW ITEM / CONFIRM): every line's underline touches
        // its letters' stroke (baseline to the line's centre: kMapAbove for a thin divider, kMapAboveLast for the
        // thicker underscore, so both look the same), the next function's letters kMapBelow under it (letter tops); a
        // wrapped line follows kMapWrap below the letters above it; kMapRowTop over the top letters. One line at kMapCap, more at kMapCapN (a HOT line at
        // kMapHotCap). All left-aligned in a column after the button's icon (kMapIcon lines square, at the panel's left;
        // the pack's buttons fill about 84 % of theirs), kMapTextGap after it, kMapTextMargin short of the panel's end.
        // The letters: the list's shape, tracked out kMapTracking of the cap (the original spreads its words).
        constexpr float kMapCap = 15.5f, kMapCapN = 11.5f, kMapHotCap = 9.5f;
        constexpr float kMapAbove = 2.2f, kMapAboveLast = 2.7f, kMapBelow = 3.8f, kMapWrap = 3.5f, kMapRowTop = 2.5f;
        constexpr float kMapIcon = 25.0f, kMapTextGap = 4.0f, kMapTextMargin = 6.0f, kMapTracking = 0.13f;
        // The lines: grungy brush strokes laid along them (leader.png tapers to a point at the button; underscore.png
        // runs along the panel's bottom), kMapLine lines thick (the stroke's core about half of it); a label's inner
        // lines kMapSubLine thick at kMapSubAlpha (K: thinner, fainter).
        constexpr float kMapLine = 6.0f, kMapSubLine = 3.6f, kMapSubAlpha = 0.55f;
        // The drum (BUTTON MAPPING, A on a line): items kMapDrumPitch lines apart at full size, each step out
        // kMapDrumShrink smaller and kMapDrumFade fainter, curving kMapDrumLean units toward the pad per step squared;
        // shown to kMapDrumReach steps out. While it is open the other labels are at kMapDrumDim, those under its items
        // at kMapDrumUnder (K: less noise).
        constexpr float kMapDrumPitch = 27.0f, kMapDrumShrink = 0.17f, kMapDrumFade = 0.3f, kMapDrumLean = 7.0f;
        constexpr float kMapDrumReach = 2.6f, kMapDrumDim = 0.5f, kMapDrumUnder = 0.08f;
        // A soft dark band behind the drum (over the column, kMapDrumShadeSoft lines feathered), kMapDrumShade strong.
        constexpr float kMapDrumShadeSoft = 7.0f;
        constexpr uint32_t kMapDrumShade = rgba(0, 0, 0, 120);
        // The hints' second line (Reset) this far above the first.
        constexpr float kHintLineStep = 27.0f;
        // The panels' wash (panel.png: opaque at the outer end, nothing at the pad): the original's tan, 0.40 at the
        // outer end; a fixed line's panel, lines and text muted (K: tell the remappable ones apart).
        constexpr uint32_t kMapPanelTint = rgba(147, 115, 88, 102), kMapPanelOn = rgba(160, 125, 92, 150);
        constexpr float kMapFixedAlpha = 0.6f;
        constexpr uint32_t kMapText = rgba(0xDE, 0xD3, 0xB9), kMapStroke = rgba(0x01, 0x01, 0x00); // cream, black stroke
        constexpr uint32_t kMapFixedText = rgba(0x9A, 0x8C, 0x7E);                                 // the values' grey, lifted

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
            void hintWord(const char *word, float right, float gap, float y = kHintY)
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
                item.y = Space::y(y + 0.5f * kHelpCap);
                item.size = Space::y(kHelpCap);
                item.scale_x = Space::scale(kHelpScaleX);
                item.shadow = faded(kHelpShadow);
                item.shadow_x = Space::y(kHelpShadowX);
                item.shadow_y = Space::y(kHelpShadowY);
                items.push_back(item);
            }

            // A button image gap units before the last text or image drawn.
            void hintArt(const char *path, float gap, float y = kHintY)
            {
                Pcsx2GsScreenItem item{};
                item.kind = PCSX2GS_SCREEN_IMAGE;
                item.align = PCSX2GS_ALIGN_BEFORE_LAST;
                item.text = add(path, std::strlen(path));
                item.fill = faded(rgba(255, 255, 255));
                item.x = Space::h(gap);
                item.x1 = Space::y(kHintArt);
                item.y = Space::y(y - 0.5f * kHintArt);
                item.y1 = Space::y(y + 0.5f * kHintArt);
                items.push_back(item);
            }

            // "[A] Confirm  [B] Cancel", laid out from the right end.
            void hints(const Hint &confirm, const Hint &cancel, float y = kHintY)
            {
                float gap = -1.0f; // the first word: at the edge
                for (const Hint *h : {&cancel, &confirm})
                {
                    if (!h->word || !*h->word)
                        continue;
                    hintWord(h->word, gap < 0.0f ? space.area - kHintRight : -1.0f, gap, y);
                    float artGap = kHintArtGap;
                    for (int i = 1; i >= 0; --i) // drawn right to left: art[1] beside the word
                        if (h->art[i] && *h->art[i])
                        {
                            hintArt(h->art[i], artGap, y);
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

            // A title image (its path without ".png") moved dx units, its blurred twin over it by blur (0..1), at alpha.
            void titleMoving(const std::string &stem, float dx, float blur, float alpha)
            {
                const float width = kTitleWidth / kTitlePixelsPerUnit, centre = 0.5f * space.area + dx;
                const auto draw = [&](const std::string &path, float pad, float a)
                {
                    if (a <= 0.0f)
                        return;
                    Pcsx2GsScreenItem item = base();
                    item.kind = PCSX2GS_SCREEN_IMAGE;
                    item.text = add(path.data(), path.size());
                    item.fill = faded(rgba(255, 255, 255), std::min(a, 1.0f));
                    item.x = space.x(centre - (0.5f + pad) * width);
                    item.x1 = space.x(centre + (0.5f + pad) * width);
                    item.y = Space::y(kTitleTop);
                    item.y1 = Space::y(kTitleTop + kTitleHeight / kTitlePixelsPerUnit);
                    items.push_back(item);
                };
                // Both at full strength through the middle of the change: the streak over the sharp letters.
                draw(stem + ".png", 0.0f, alpha * std::min(1.0f, 2.0f * (1.0f - blur)));
                draw(stem + "_blur.png", kTitleBlurPad, alpha * std::min(1.0f, 2.0f * blur));
            }

            // L1 / R1 switched the category (the title screen's menu words, K, Nov 14): the old title streaks off the
            // way the list moved and fades, the new one streaks in from the other side, slowing, and sharpens.
            void titleSwitch(const std::string &from, const std::string &to, int dir, float t)
            {
                const float d = dir > 0 ? 1.0f : -1.0f;
                const float out = std::clamp(t / 0.4f, 0.0f, 1.0f);
                titleMoving(from, -d * kTitleSlide * out * out, 1.0f, 1.0f - out);
                const float in = std::clamp((t - 0.15f) / 0.85f, 0.0f, 1.0f);
                const float ease = 1.0f - (1.0f - in) * (1.0f - in) * (1.0f - in);
                const float sharp = std::clamp((in - 0.35f) / 0.65f, 0.0f, 1.0f);
                titleMoving(to, d * kTitleSlide * (1.0f - ease), 1.0f - sharp * sharp * (3.0f - 2.0f * sharp),
                            std::min(1.0f, 4.0f * in));
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

            // A brush stroke from (x0, y0) to (x1, y1) (units, lines), thickness lines across, the whole image along it.
            void stroke(const char *brush, float x0, float y0, float x1, float y1, float thickness, uint32_t fill)
            {
                const float dx = (x1 - x0) * 7.0f / 6.0f, dy = y1 - y0;
                if (dx * dx + dy * dy < 0.25f)
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
                items.push_back(item);
            }

            // The title image with its letters' left at x (units), scale of its usual size, its top where it always is.
            void titleAt(uint32_t path, const char *name, float x, float scale)
            {
                std::string key;
                for (const char *c = name; *c; ++c)
                    key.push_back(static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c));
                const auto found = g_titleLetters.find(key);
                const float l = found != g_titleLetters.end() ? found->second.first : kLettersLeft * kTitleWidth;
                Pcsx2GsScreenItem item = base();
                item.kind = PCSX2GS_SCREEN_IMAGE;
                item.text = path;
                item.fill = faded(rgba(255, 255, 255));
                const float ppu = kTitlePixelsPerUnit / scale, left = x - l / ppu;
                item.x = space.x(left);
                item.x1 = space.x(left + kTitleWidth / ppu);
                item.y = Space::y(kTitleTop);
                item.y1 = Space::y(kTitleTop + kTitleHeight / ppu); // its pixels per unit are per line too
                items.push_back(item);
            }

            // CONTROLS > BUTTON MAPPING: panels, the pad in its flames, the lines, the icons, the labels.
            void mapping(const MappingView &m)
            {
                const float aspect = std::max(m.padAspect, 0.1f);
                const float padH = std::min(kMapPadHeight, kMapPadShare * space.area / (aspect * kLinesToUnits));
                const float padW = padH * aspect * kLinesToUnits; // units
                const float cx = 0.5f * space.area, padL = cx - 0.5f * padW, padR = cx + 0.5f * padW;
                const float padT = kMapPadY - 0.5f * padH;
                // The columns turn at the same place for every pad (K: the labels never move with the pad), just past
                // the widest pad's outline at this size; a narrower pad's lines are a little longer.
                const float widest = std::min(kMapPadWidest, kMapPadShare * space.area);
                const float kneeL = cx - 0.5f * widest - kMapKnee, kneeR = cx + 0.5f * widest + kMapKnee;
                const float outerL = std::max(kMapEdge, kneeL - kMapPanel), outerR = std::min(space.area - kMapEdge, kneeR + kMapPanel);

                struct Row
                {
                    int line = -1;
                    float top = 0.0f, bottom = 0.0f;
                };
                // A label's shape at full size, from the row's bottom (its underscore) up: each line's baseline and cap,
                // the underline under each line that ends a function (the last one's is the underscore), the height.
                struct Shape
                {
                    int n = 0;
                    float base[5] = {}, cap[5] = {}, under[5] = {};
                    bool divider[5] = {};
                    float height = 0.0f;
                };
                const auto shapeOf = [&](const MappingLine &l) {
                    Shape t;
                    t.n = std::min(std::max(l.count, 1) + (*l.hot ? 1 : 0), 5);
                    const bool single = t.n == 1;
                    float top = 0.0f, base = 0.0f;
                    for (int i = t.n - 1; i >= 0; --i)
                    {
                        const bool hot = *l.hot && i == t.n - 1;
                        const float cap = hot ? kMapHotCap : single ? kMapCap : kMapCapN;
                        const bool ends = hot || i == t.n - 1 || ((l.ends >> i) & 1u) != 0u || (*l.hot && i == t.n - 2);
                        if (i == t.n - 1)
                            base = -kMapAboveLast;
                        else if (ends)
                        {
                            t.divider[i] = true;
                            t.under[i] = top - kMapBelow;
                            base = t.under[i] - kMapAbove;
                        }
                        else
                            base = top - kMapWrap;
                        t.base[i] = base;
                        t.cap[i] = cap;
                        top = base - cap;
                    }
                    t.height = std::max(kMapRow, -top + kMapRowTop);
                    // One function (no divider, no HOT: COMBO, UNLEASH / THE GODHAND): centred in its row instead (K);
                    // only labels with several functions sit on their lines from the bottom.
                    bool several = *l.hot;
                    for (int i = 0; i + 1 < t.n; ++i)
                        several = several || t.divider[i];
                    if (!several)
                    {
                        const float shift = 0.5f * (-t.height - (t.base[0] - t.cap[0])) - 0.5f * t.base[t.n - 1];
                        for (int i = 0; i < t.n; ++i)
                            t.base[i] += shift;
                    }
                    return t;
                };
                const auto height = [&](const MappingLine &l) { return shapeOf(l).height; };
                std::vector<Row> right, left;
                for (int i = 0; i < m.lineCount; ++i)
                    (m.lines[i].right ? right : left).push_back(Row{i});
                // The right column: stacked evenly over the band (squeezed when they do not fit).
                float total = 0.0f;
                for (const Row &r : right)
                    total += height(m.lines[r.line]);
                const float span = kMapBottom - kMapTop;
                const float gap = right.size() > 1 ? std::clamp((span - total) / static_cast<float>(right.size() - 1), kMapRowGap, kMapRowGapMax) : 0.0f;
                const float used = total + gap * static_cast<float>(right.empty() ? 0 : right.size() - 1);
                const float squeeze = used > span ? span / used : 1.0f;
                float y = kMapTop + (used < span ? 0.5f * (span - used) : 0.0f);
                for (Row &r : right)
                {
                    r.top = y;
                    r.bottom = y + height(m.lines[r.line]) * squeeze;
                    y = r.bottom + gap * squeeze;
                }
                // The left column: each row level with its mirror (the same rows for every pad); one without a mirror
                // (never with the shipped layout) stacked under the one above it.
                {
                    float below = kMapTitleClear;
                    for (Row &r : left)
                    {
                        const MappingLine &l = m.lines[r.line];
                        const float h = height(l) * squeeze;
                        r.bottom = below + h;
                        for (const Row &o : right)
                            if (o.line == l.mirror)
                                r.bottom = o.bottom;
                        r.top = r.bottom - h;
                        below = r.bottom + kMapRowGap * squeeze;
                    }
                }

                const auto each = [&](auto &&fn) {
                    for (const Row &r : left)
                        fn(r, false);
                    for (const Row &r : right)
                        fn(r, true);
                };
                // The panels: the wash opaque at the outer end, nothing at the pad (mirrored on the right).
                each([&](const Row &r, bool side) {
                    if (!m.lines[r.line].fixed) // a fixed line has none (K: it reads as inactive)
                        picture(m.panel, side ? outerR : outerL, r.top, side ? kneeR : kneeL, r.bottom,
                                r.line == m.cursor ? kMapPanelOn : kMapPanelTint);
                });
                // The pad in its flames.
                const float margin = m.flamesMargin * padH, marginU = margin * kLinesToUnits;
                picture(m.flames, padL - marginU, padT - margin, padR + marginU, padT + padH + margin, rgba(255, 255, 255));
                picture(m.pad, padL, padT, padR, padT + padH, rgba(255, 255, 255));
                // The lines: from the button (tapering to it) up or down to the panel's corner by the pad, then along the
                // panel's bottom; each upper text line its own thin, faint underscore.
                each([&](const Row &r, bool side) {
                    const MappingLine &l = m.lines[r.line];
                    const bool on = r.line == m.cursor;
                    const float alpha = l.fixed ? kMapFixedAlpha : 1.0f;
                    const uint32_t tint = rgba(255, 255, 255, static_cast<uint32_t>(255.0f * alpha));
                    const char *leader = on && *m.leaderLit ? m.leaderLit : m.leader;
                    const char *under = on && *m.underscoreLit ? m.underscoreLit : m.underscore;
                    const float knee = side ? kneeR : kneeL, outer = side ? outerR : outerL;
                    const float bx = padL + l.px * padW, by = padT + l.py * padH;
                    // A bend on the way (via): the leader to it, then on to the corner (drawn from the corner, the
                    // underscore's slight thinning at the bend, under the leader's full end).
                    const float tx = l.via ? padL + l.vx * padW : knee, ty = l.via ? padT + l.vy * padH : r.bottom;
                    const float vx = (tx - bx) * 7.0f / 6.0f, vy = ty - by, len = std::sqrt(vx * vx + vy * vy);
                    const float reach = l.reach * padH;
                    if (len > reach + 1.0f)
                        stroke(leader, bx + vx / len * reach * kLinesToUnits, by + vy / len * reach, tx, ty, kMapLine, tint);
                    if (l.via)
                        stroke(under, knee, r.bottom, tx, ty, kMapLine, tint);
                    const float back = side ? -1.5f : 1.5f; // the underscore starts under the leader's end
                    stroke(under, knee + back, r.bottom, outer, r.bottom, kMapLine, tint);
                    const Shape t = shapeOf(l);
                    const float s = (r.bottom - r.top) / t.height;
                    const float textX = (side ? knee : outer) + 1.0f + kMapIcon * kLinesToUnits + kMapTextGap;
                    const uint32_t faint = rgba(255, 255, 255, static_cast<uint32_t>(255.0f * alpha * kMapSubAlpha));
                    for (int i = 0; i + 1 < t.n; ++i)
                        if (t.divider[i])
                        {
                            const float uy = r.bottom + t.under[i] * s;
                            if (side)
                                stroke(under, textX - 2.0f, uy, outer, uy, kMapSubLine, faint);
                            else
                                stroke(under, knee, uy, outer, uy, kMapSubLine, faint);
                        }
                });
                // The drum's span (it covers the cursor's label): rows it passes over are drawn fainter.
                const bool drum = m.drum && m.drumCount > 0 && m.cursor >= 0;
                Row drumRow;
                bool drumSide = false;
                each([&](const Row &r, bool side) {
                    if (r.line == m.cursor)
                        drumRow = r, drumSide = side;
                });
                const float drumMid = 0.5f * (drumRow.top + drumRow.bottom);
                const float drumHalf = kMapDrumPitch * std::min(kMapDrumReach, 0.5f * static_cast<float>(m.drumCount)) + 0.5f * kMapDrumPitch;
                // The icons (at each label's left, centred on its letters) and the labels, left-aligned in a column.
                each([&](const Row &r, bool side) {
                    const MappingLine &l = m.lines[r.line];
                    const bool on = r.line == m.cursor;
                    if (drum && on)
                        return; // the drum is drawn in its place
                    const float keep = dim;
                    if (drum)
                        dim = keep * (side == drumSide && r.bottom > drumMid - drumHalf && r.top < drumMid + drumHalf ? kMapDrumUnder : kMapDrumDim);
                    const float knee = side ? kneeR : kneeL, outer = side ? outerR : outerL;
                    const float left = side ? knee : outer, right = side ? outer : knee;
                    const Shape t = shapeOf(l);
                    const float s = (r.bottom - r.top) / t.height;
                    const float lettersTop = r.bottom + (t.base[0] - t.cap[0]) * s, lettersBottom = r.bottom + t.base[t.n - 1] * s;
                    const float icon = std::min(kMapIcon * s, r.bottom - r.top - 2.0f), mid = 0.5f * (lettersTop + lettersBottom);
                    picture(l.icon, left + 1.0f, mid - 0.5f * icon, left + 1.0f + icon * kLinesToUnits, mid + 0.5f * icon,
                            rgba(255, 255, 255, l.fixed ? static_cast<uint32_t>(255.0f * 0.85f) : 255u));
                    const float x0 = left + 1.0f + kMapIcon * kLinesToUnits + kMapTextGap, room = std::max(right - kMapTextMargin - x0, 8.0f);
                    const uint32_t fill = on ? kLabelOn : l.fixed ? kMapFixedText : kMapText, edge = on ? kLabelOnStroke : kMapStroke;
                    for (int i = 0; i < t.n; ++i)
                    {
                        const bool hot = *l.hot && i == t.n - 1;
                        const char *line = hot ? l.hot : l.text[i];
                        const float cap = t.cap[i] * s;
                        text(line, std::strlen(line), labelFont, x0, r.bottom + t.base[i] * s, cap, kNewLabel.scaleX, kMapTracking * cap,
                             hot ? kLabel : fill, hot ? kMapStroke : edge, room);
                    }
                    dim = keep;
                });
                if (!drum)
                    return;
                // The drum: each item where the turn puts it, the nearest ones last (on top).
                const float knee = drumSide ? kneeR : kneeL, outer = drumSide ? outerR : outerL;
                const float colL = drumSide ? knee : outer, colR = drumSide ? outer : knee;
                const float inward = drumSide ? -1.0f : 1.0f; // toward the pad
                const int n = m.drumCount;
                {
                    Pcsx2GsScreenItem shade{};
                    shade.kind = PCSX2GS_SCREEN_RECT;
                    shade.fill = faded(kMapDrumShade);
                    shade.blur = Space::y(kMapDrumShadeSoft);
                    shade.x = space.x(colL - 2.0f);
                    shade.x1 = space.x(colR + 2.0f);
                    shade.y = Space::y(drumMid - drumHalf);
                    shade.y1 = Space::y(drumMid + drumHalf);
                    items.push_back(shade);
                }
                std::vector<std::pair<float, int>> order;
                for (int k = 0; k < n; ++k)
                {
                    float d = std::fmod(static_cast<float>(k) - m.drumPos, static_cast<float>(n));
                    if (d < -0.5f * static_cast<float>(n))
                        d += static_cast<float>(n);
                    else if (d >= 0.5f * static_cast<float>(n))
                        d -= static_cast<float>(n);
                    if (std::fabs(d) <= kMapDrumReach)
                        order.emplace_back(d, k);
                }
                std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return std::fabs(a.first) > std::fabs(b.first); });
                for (const auto &[d, k] : order)
                {
                    const float a = std::fabs(d);
                    const float scale = 1.0f - kMapDrumShrink * a;
                    const float alpha = std::clamp(1.0f - kMapDrumFade * a, 0.0f, 1.0f) * std::clamp((kMapDrumReach - a) / 0.6f, 0.0f, 1.0f);
                    const float y = drumMid + kMapDrumPitch * d * (1.0f - 0.06f * a);
                    const float lean = inward * kMapDrumLean * d * d;
                    const bool hovered = a < 0.5f;
                    const float cap = kMapCap * scale, icon = kMapIcon * scale;
                    const float x0 = colL + 1.0f + lean;
                    const float keep = dim;
                    dim = keep * alpha;
                    picture(m.drum[k].icon, x0, y - 0.5f * icon, x0 + icon * kLinesToUnits, y + 0.5f * icon, rgba(255, 255, 255));
                    const float tx = x0 + icon * kLinesToUnits + kMapTextGap * scale;
                    const char *line = m.drum[k].text;
                    text(line, std::strlen(line), labelFont, tx, y + 0.5f * cap, cap, kNewLabel.scaleX, kMapTracking * cap,
                         hovered ? kLabelOn : kMapText, hovered ? kLabelOnStroke : kMapStroke,
                         std::max(colR + lean - kMapTextMargin - tx, 8.0f));
                    dim = keep;
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
            if (!g_titleDir.empty()) // at the top left, as the original controller page's
            {
                const std::string path = g_titleDir + "/title_controls.png";
                b.titleAt(b.add(path.data(), path.size()), "CONTROLS", kMapTitleX, kMapTitleScale);
            }
            b.help(view.mapping->notice);
            const MappingView &m = *view.mapping;
            Hint confirm = view.confirmHint, cancel = view.cancelHint, reset;
            confirm.word = m.confirmWord;
            cancel.word = m.cancelWord;
            reset.art[0] = m.resetArt;
            reset.word = m.resetWord;
            b.hints(confirm, cancel);
            b.hints(reset, Hint{}, kHintY - kHintLineStep); // its own line above, right-aligned (K, Nov 14)
            return;
        }
        const bool split = view.aspect >= kSplitAspect;
        const Page *page = view.page && view.page->rowCount > 0 ? view.page : nullptr;
        const bool open = page && view.inPage;

        // The title: OPTIONS, or the open category's (K, Nov 13: it replaces OPTIONS until the page is left).
        const char *title = open ? page->name : "OPTIONS";
        const auto stem = [](const char *name)
        {
            std::string path = g_titleDir + "/title_";
            for (const char *c = name; *c; ++c)
                path.push_back(static_cast<char>(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c));
            return path;
        };
        if (!g_titleDir.empty() && open && view.titleDir != 0 && view.titleFrom && *view.titleFrom && view.titleT < 1.0f)
            b.titleSwitch(stem(view.titleFrom), stem(title), view.titleDir, std::max(view.titleT, 0.0f));
        else if (!g_titleDir.empty())
        {
            const std::string path = stem(title) + ".png";
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
