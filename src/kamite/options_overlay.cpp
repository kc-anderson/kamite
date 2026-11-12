// The OPTIONS screen's overlay list. See options_overlay.h; measurements: claude/rearmed-options.md.
#include "kamite/options_overlay.h"

#include <algorithm>
#include <cstring>

namespace kamite::options_overlay
{
    namespace
    {
        Look g_look;
        std::string g_labelFont, g_helpFont, g_titleImage;
    }

    Look &look() { return g_look; }

    void setAssets(const std::string &labelFont, const std::string &helpFont, const std::string &titleImage)
    {
        g_labelFont = labelFont;
        g_helpFont = helpFont;
        g_titleImage = titleImage;
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
        // K's painted title, placed as the original's painted one (its letters x 146..363, y 20..66 of the 512 x 448
        // screen; K's image is drawn in the same texel shape: 17.44 of its pixels per unit and per line), centred.
        constexpr float kTitlePixelsPerUnit = 17.44f, kTitleTop = 13.7f, kTitleWidth = 3947.0f, kTitleHeight = 1064.0f;
        constexpr float kTitleY = 70.0f, kTitleCap = 30.0f; // the text stand-in when the image is missing
        constexpr float kMaxLabel = 190.0f;                  // the longest original (RESTORE DEFAULTS)

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

            uint32_t add(const char *text, size_t length)
            {
                const uint32_t offset = static_cast<uint32_t>(strings.size());
                strings.append(text, length);
                strings.push_back('\0');
                return offset;
            }
            uint32_t faded(uint32_t colour, float alpha = 1.0f) const
            {
                const float a = static_cast<float>(colour >> 24) * std::clamp(fade * alpha, 0.0f, 1.0f);
                return (colour & 0x00FFFFFFu) | (static_cast<uint32_t>(a + 0.5f) << 24);
            }
            Pcsx2GsScreenItem base() const
            {
                Pcsx2GsScreenItem item{};
                item.blur = Space::y(g_look.blur);
                item.shadow = faded(rgba(0, 0, 0), g_look.shadowAlpha);
                item.shadow_x = item.shadow_y = Space::y(g_look.shadowOffset);
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

            void label(const char *name, float centre, bool on)
            {
                const Fit &f = fitFor(name);
                text(name, std::strlen(name), labelFont, kLabelX, centre + 0.5f * kLabelCap, kLabelCap, f.scaleX, f.tracking,
                     on ? kLabelOn : kLabel, on ? kLabelOnStroke : 0u, kMaxLabel);
            }

            void value(const char *name, float x, float centre, bool on)
            {
                text(name, std::strlen(name), labelFont, x, centre + 0.5f * kValueCap, kValueCap, kNewLabel.scaleX,
                     kNewLabel.tracking, on ? kValueOn : kValue, on ? kValueOnStroke : 0u);
            }

            void cursor(float centre)
            {
                Pcsx2GsScreenItem item = base();
                item.kind = PCSX2GS_SCREEN_TRIANGLE;
                item.fill = faded(kLabelOn);
                item.stroke = faded(kLabelOnStroke);
                item.stroke_width = Space::y(kStroke);
                item.x = space.x(kCursorX - kCursorHalfW);
                item.y = Space::y(centre - kCursorHalfH);
                item.x1 = space.x(kCursorX + kCursorHalfW);
                item.y1 = Space::y(centre);
                item.x2 = space.x(kCursorX - kCursorHalfW);
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

        if (!g_titleImage.empty())
            b.title(b.add(g_titleImage.data(), g_titleImage.size()));
        else
            b.text("OPTIONS", 7u, b.labelFont, kLabelX, kTitleY, kTitleCap, kNewLabel.scaleX, kNewLabel.tracking, kLabelOn, kLabelOnStroke);
        // The pause's rows fill the original's span; the title's (fewer) keep its spacing.
        const float step = view.pause && view.rowCount > 1 ? std::min(kRowStep, (kLastRowY - kFirstRowY) / static_cast<float>(view.rowCount - 1))
                                                           : kRowStep;
        const int cursor = std::clamp(view.cursor, 0, view.rowCount - 1);
        for (int i = 0; i < view.rowCount; ++i)
            b.label(view.rows[i].name, kFirstRowY + step * static_cast<float>(i), i == cursor);
        const float centre = kFirstRowY + step * static_cast<float>(cursor);
        b.cursor(centre);
        if (view.confirmRow >= 0 && view.confirmRow < view.rowCount)
        {
            const float at = kFirstRowY + step * static_cast<float>(view.confirmRow);
            b.value("YES", kValueX, at, view.confirmYes);
            b.value("NO", kValueX + kValueGap, at, !view.confirmYes);
            b.help(view.question);
        }
        else
            b.help(view.rows[cursor].help);
    }
#endif
}
