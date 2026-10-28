// Button prompts at full resolution from Xelu's prompt pack. See prompt_hd.h.
#include "kamite/prompt_hd.h"

#include "kamite/log.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace kamite::prompt_hd
{
    namespace
    {
        namespace sheets = kamite::prompt_sheets;

        // Premultiplied RGBA, 0..1, row-major.
        struct Canvas
        {
            int w = 0, h = 0;
            std::vector<float> px;
            Canvas() = default;
            Canvas(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * height * 4u, 0.0f) {}
            float *at(int x, int y) { return &px[(static_cast<size_t>(y) * w + x) * 4u]; }
            const float *at(int x, int y) const { return &px[(static_cast<size_t>(y) * w + x) * 4u]; }
        };

        // ---- the sheets' cells (button_prompt_sheets.cpp kCells / kShapes) ----
        struct Rect
        {
            float x0, y0, x1, y1;
        };
        constexpr int kCell[sheets::IconCount][2] = {
            {0, 0},   {32, 0},  {64, 0},  {96, 0},  // Circle Triangle Square Cross
            {64, 32}, {0, 32},  {96, 32}, {32, 32}, // L1 R1 L2 R2
            {32, 64}, {0, 64},                      // L3 R3
            {96, 64}, {64, 64},                     // LeftStick RightStick
            {32, 96}, {0, 96},                      // Start Select
        };
        // The action prompt's four shapes (texture 0x0F's bottom row): the sprite rect the game draws and the texels
        // its shape covers.
        struct Shape
        {
            int icon;
            Rect sprite, clear;
        };
        constexpr Shape kShapes[4] = {
            {sheets::Triangle, {0.0f, 101.0f, 29.0f, 128.0f}, {0, 100, 30, 128}},
            {sheets::Circle, {33.0f, 98.0f, 64.0f, 128.0f}, {32, 97, 66, 128}},
            {sheets::Square, {70.0f, 102.0f, 96.0f, 128.0f}, {68, 100, 98, 128}},
            {sheets::Cross, {100.0f, 102.0f, 128.0f, 128.0f}, {99, 100, 128, 128}},
        };

        // ---- the pack's files ----
        const char *folder(Set set)
        {
            switch (set)
            {
            case Set::Xbox360: return "xbox360";
            case Set::SteamDeck: return "steamdeck";
            case Set::Switch: return "switch";
            case Set::Keyboard: return "keyboard";
            default: return "";
            }
        }

        struct Pair
        {
            std::string_view label, file;
        };
        // Controller labels are SDL's positional (Xbox) names: "A" is the bottom face button on every pad.
        constexpr Pair kXbox360[] = {
            {"A", "360_A"},       {"B", "360_B"},       {"X", "360_X"},           {"Y", "360_Y"},
            {"LB", "360_LB"},     {"RB", "360_RB"},     {"LT", "360_LT"},         {"RT", "360_RT"},
            {"View", "360_Back_Alt"}, {"Menu", "360_Start_Alt"}, {"LS", "360_Left_Stick_Click"}, {"RS", "360_Right_Stick_Click"},
            {"\xE2\x86\x91", "360_Dpad_Up"}, {"\xE2\x86\x93", "360_Dpad_Down"}, {"\xE2\x86\x90", "360_Dpad_Left"}, {"\xE2\x86\x92", "360_Dpad_Right"},
        };
        constexpr Pair kSteamDeck[] = {
            {"A", "SteamDeck_A"},    {"B", "SteamDeck_B"},    {"X", "SteamDeck_X"},    {"Y", "SteamDeck_Y"},
            {"LB", "SteamDeck_L1"},  {"RB", "SteamDeck_R1"},  {"LT", "SteamDeck_L2"},  {"RT", "SteamDeck_R2"},
            {"View", "SteamDeck_Inventory"}, {"Menu", "SteamDeck_Menu"}, {"Gd", "SteamDeck_Steam"},
            {"LS", "SteamDeck_Left_Stick_Click"}, {"RS", "SteamDeck_Right_Stick_Click"},
            {"P1", "SteamDeck_R4"},  {"P2", "SteamDeck_L4"},  {"P3", "SteamDeck_R5"},  {"P4", "SteamDeck_L5"},
            {"\xE2\x86\x91", "SteamDeck_Dpad_Up"}, {"\xE2\x86\x93", "SteamDeck_Dpad_Down"}, {"\xE2\x86\x90", "SteamDeck_Dpad_Left"},
            {"\xE2\x86\x92", "SteamDeck_Dpad_Right"},
        };
        // Nintendo's letters sit the other way round: the bottom button is B, the right one A, the left one Y.
        constexpr Pair kSwitch[] = {
            {"A", "Switch_B"},       {"B", "Switch_A"},       {"X", "Switch_Y"},       {"Y", "Switch_X"},
            {"LB", "Switch_LB"},     {"RB", "Switch_RB"},     {"LT", "Switch_LT"},     {"RT", "Switch_RT"},
            {"View", "Switch_Minus"}, {"Menu", "Switch_Plus"}, {"Gd", "Switch_Home"}, {"Misc", "Switch_Square"},
            {"LS", "Switch_Left_Stick_Click"}, {"RS", "Switch_Right_Stick_Click"},
            {"\xE2\x86\x91", "Switch_Dpad_Up"}, {"\xE2\x86\x93", "Switch_Dpad_Down"}, {"\xE2\x86\x90", "Switch_Dpad_Left"},
            {"\xE2\x86\x92", "Switch_Dpad_Right"},
        };
        // prompt_art::keyLabel's compact names -> the pack's key (<name>_Key_Light.png).
        constexpr Pair kKeys[] = {
            {"Spc", "Space"},     {"Ent", "Enter"},       {"NEnt", "Enter"},     {"Bksp", "Backspace"}, {"Tab", "Tab"},
            {"Esc", "Esc"},       {"Caps", "Caps_Lock"},  {"Shft", "Shift"},     {"RShf", "Shift"},     {"Ctrl", "Ctrl"},
            {"RCtl", "Ctrl"},     {"Alt", "Alt"},         {"RAlt", "Alt"},       {"Win", "Win"},        {"RWin", "Win"},
            {"Ins", "Insert"},    {"Del", "Del"},         {"Home", "Home"},      {"End", "End"},        {"PgUp", "Page_Up"},
            {"PgDn", "Page_Down"}, {"NmLk", "Num_Lock"},  {"PrSc", "Print_Screen"}, {";", "Semicolon"}, {"-", "Minus"},
            {"N-", "Minus"},      {"N+", "Plus"},         {"N*", "Asterisk"},    {"/", "Slash"},        {"N/", "Slash"},
            {"`", "Tilda"},       {"[", "Bracket_Left"},  {"]", "Bracket_Right"}, {"'", "Quote"},       {"M1", "Mouse_Left"},
            {"M2", "Mouse_Right"}, {"M3", "Mouse_Middle"},
            {"\xE2\x86\x91", "Arrow_Up"}, {"\xE2\x86\x93", "Arrow_Down"}, {"\xE2\x86\x90", "Arrow_Left"}, {"\xE2\x86\x92", "Arrow_Right"},
        };

        template <size_t N>
        std::string lookup(const Pair (&table)[N], std::string_view label)
        {
            for (const Pair &p : table)
                if (p.label == label)
                    return std::string(p.file);
            return {};
        }

        std::string keyFile(const std::string &label)
        {
            std::string base = lookup(kKeys, label);
            if (base.empty())
            {
                if (label.size() == 1 && std::isalnum(static_cast<unsigned char>(label[0])))
                    base = std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(label[0]))));
                else if (label.size() >= 2 && label.size() <= 3 && label[0] == 'F' && std::isdigit(static_cast<unsigned char>(label[1])))
                    base = label; // F1..F12
                else if (label.size() == 4 && label.compare(0, 3, "Num") == 0 && std::isdigit(static_cast<unsigned char>(label[3])))
                    base = label.substr(3); // the keypad's digits as the digits
            }
            return base.empty() ? std::string() : base + "_Key_Light";
        }

        // The file for a cell's label, "" when the pack has none (the cell keeps the PS2-resolution drawing).
        std::string fileFor(Set set, const std::string &label)
        {
            switch (set)
            {
            case Set::Xbox360: return lookup(kXbox360, label);
            case Set::SteamDeck: return lookup(kSteamDeck, label);
            case Set::Switch: return lookup(kSwitch, label);
            case Set::Keyboard: return keyFile(label);
            default: return {};
            }
        }

        std::string stickFile(Set set, bool left)
        {
            switch (set)
            {
            case Set::Xbox360: return left ? "360_Left_Stick" : "360_Right_Stick";
            case Set::SteamDeck: return left ? "SteamDeck_Left_Stick" : "SteamDeck_Right_Stick";
            case Set::Switch: return left ? "Switch_Left_Stick" : "Switch_Right_Stick";
            default: return {};
            }
        }

        // A pack image, premultiplied and cropped to its visible part. Loaded once (builder thread).
        struct Art
        {
            Canvas image;
            bool ok = false;
        };
        const Art &art(Set set, const std::string &file)
        {
            static std::mutex mutex;
            static std::map<std::string, Art> cache;
            std::lock_guard<std::mutex> lock(mutex);
            const std::string key = std::string(folder(set)) + "/" + file;
            auto it = cache.find(key);
            if (it != cache.end())
                return it->second;
            Art &entry = cache[key];
            const fs::path path = hd_textures::exeDirectory() / "resources" / "prompts" / "xelu" / folder(set) / (file + ".png");
            hd_textures::Image raw;
            if (!hd_textures::loadPng(path, raw))
            {
                KM_WARN(Game, "button prompts: missing prompt image " << key << ".png");
                return entry;
            }
            int x0 = raw.width, y0 = raw.height, x1 = -1, y1 = -1;
            for (int y = 0; y < raw.height; ++y)
                for (int x = 0; x < raw.width; ++x)
                    if (raw.rgba[(static_cast<size_t>(y) * raw.width + x) * 4u + 3u] > 4u)
                    {
                        x0 = std::min(x0, x);
                        y0 = std::min(y0, y);
                        x1 = std::max(x1, x);
                        y1 = std::max(y1, y);
                    }
            if (x1 < x0)
                return entry;
            entry.image = Canvas(x1 - x0 + 1, y1 - y0 + 1);
            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    const uint8_t *s = &raw.rgba[(static_cast<size_t>(y) * raw.width + x) * 4u];
                    float *d = entry.image.at(x - x0, y - y0);
                    const float a = s[3] / 255.0f;
                    d[0] = s[0] / 255.0f * a;
                    d[1] = s[1] / 255.0f * a;
                    d[2] = s[2] / 255.0f * a;
                    d[3] = a;
                }
            }
            entry.ok = true;
            return entry;
        }

        // Bilinear sample with the edges clamped (premultiplied).
        void sample(const Canvas &src, float x, float y, float *out)
        {
            x = std::clamp(x, 0.0f, static_cast<float>(src.w - 1));
            y = std::clamp(y, 0.0f, static_cast<float>(src.h - 1));
            const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
            const int x1 = std::min(x0 + 1, src.w - 1), y1 = std::min(y0 + 1, src.h - 1);
            const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
            const float *a = src.at(x0, y0), *b = src.at(x1, y0), *c = src.at(x0, y1), *d = src.at(x1, y1);
            for (int i = 0; i < 4; ++i)
                out[i] = (a[i] * (1.0f - fx) + b[i] * fx) * (1.0f - fy) + (c[i] * (1.0f - fx) + d[i] * fx) * fy;
        }

        // `src` scaled to fit the box (aspect kept), centred, drawn "over" dst; 4x4 bilinear taps per pixel so
        // shrinking stays smooth.
        void drawFitted(Canvas &dst, const Canvas &src, Rect box)
        {
            const float bw = box.x1 - box.x0, bh = box.y1 - box.y0;
            const float scale = std::min(bw / static_cast<float>(src.w), bh / static_cast<float>(src.h));
            const float w = static_cast<float>(src.w) * scale, h = static_cast<float>(src.h) * scale;
            const float ox = box.x0 + (bw - w) * 0.5f, oy = box.y0 + (bh - h) * 0.5f;
            const int px0 = std::max(0, static_cast<int>(std::floor(ox))), py0 = std::max(0, static_cast<int>(std::floor(oy)));
            const int px1 = std::min(dst.w, static_cast<int>(std::ceil(ox + w))), py1 = std::min(dst.h, static_cast<int>(std::ceil(oy + h)));
            for (int y = py0; y < py1; ++y)
            {
                for (int x = px0; x < px1; ++x)
                {
                    float acc[4] = {};
                    int taps = 0;
                    for (int sy = 0; sy < 4; ++sy)
                    {
                        for (int sx = 0; sx < 4; ++sx)
                        {
                            const float dx = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 4.0f;
                            const float dy = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 4.0f;
                            if (dx < ox || dy < oy || dx >= ox + w || dy >= oy + h)
                                continue;
                            float s[4];
                            sample(src, (dx - ox) / scale - 0.5f, (dy - oy) / scale - 0.5f, s);
                            for (int i = 0; i < 4; ++i)
                                acc[i] += s[i];
                            ++taps;
                        }
                    }
                    if (taps == 0)
                        continue;
                    float *d = dst.at(x, y);
                    const float inv = 1.0f / 16.0f; // taps outside the image count as transparent
                    const float a = acc[3] * inv;
                    for (int i = 0; i < 4; ++i)
                        d[i] = acc[i] * inv + d[i] * (1.0f - a);
                }
            }
        }

        void clear(Canvas &dst, Rect r)
        {
            for (int y = std::max(0, static_cast<int>(r.y0)); y < std::min(dst.h, static_cast<int>(r.y1)); ++y)
                for (int x = std::max(0, static_cast<int>(r.x0)); x < std::min(dst.w, static_cast<int>(r.x1)); ++x)
                    std::fill_n(dst.at(x, y), 4, 0.0f);
        }

        Rect scaled(Rect r)
        {
            const float s = static_cast<float>(kScale);
            return {r.x0 * s, r.y0 * s, r.x1 * s, r.y1 * s};
        }

        // The PS2 sheet as the GS draws it (bilinear), kScale x.
        Canvas baseOf(const uint8_t *payload, const uint8_t *clut)
        {
            std::vector<uint8_t> indices(static_cast<size_t>(sheets::kSheetSize) * sheets::kSheetSize);
            std::array<uint8_t, 1024> pal{};
            sheets::unswizzle(payload, indices.data());
            sheets::paletteRgba(clut, pal.data());
            Canvas native(sheets::kSheetSize, sheets::kSheetSize);
            for (size_t i = 0; i < indices.size(); ++i)
            {
                const uint8_t *e = &pal[static_cast<size_t>(indices[i]) * 4u];
                const float a = std::min(1.0f, e[3] / 128.0f);
                float *d = &native.px[i * 4u];
                d[0] = e[0] / 255.0f * a;
                d[1] = e[1] / 255.0f * a;
                d[2] = e[2] / 255.0f * a;
                d[3] = a;
            }
            Canvas out(sheets::kSheetSize * kScale, sheets::kSheetSize * kScale);
            for (int y = 0; y < out.h; ++y)
                for (int x = 0; x < out.w; ++x)
                    sample(native, (static_cast<float>(x) + 0.5f) / kScale - 0.5f, (static_cast<float>(y) + 0.5f) / kScale - 0.5f, out.at(x, y));
            return out;
        }

        // One cell from the pack: true when drawn.
        bool drawCell(Canvas &canvas, Set set, const std::string &file, Rect clearRect, Rect box)
        {
            if (file.empty())
                return false;
            const Art &a = art(set, file);
            if (!a.ok)
                return false;
            clear(canvas, scaled(clearRect));
            drawFitted(canvas, a.image, scaled(box));
            return true;
        }
    }

    namespace
    {
        std::atomic<int> g_choiceOverride{-1}; // the F2 menu's choice (-1 = the environment's)
    }

    void setChoice(Choice choice)
    {
        g_choiceOverride.store(static_cast<int>(choice), std::memory_order_relaxed);
    }

    const char *choiceName(Choice choice)
    {
        switch (choice)
        {
        case Choice::Xbox: return "xbox";
        case Choice::PlayStation: return "playstation";
        case Choice::Nintendo: return "nintendo";
        case Choice::Steam: return "steam";
        default: return "auto";
        }
    }

    Choice choice()
    {
        if (const int chosen = g_choiceOverride.load(std::memory_order_relaxed); chosen >= 0)
            return static_cast<Choice>(chosen);
        const char *value = std::getenv("KAMITE_BUTTON_PROMPTS");
        std::string text = value ? value : "auto";
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (text == "xbox")
            return Choice::Xbox;
        if (text == "playstation" || text == "ps2")
            return Choice::PlayStation;
        if (text == "nintendo" || text == "switch")
            return Choice::Nintendo;
        if (text == "steam" || text == "steamdeck")
            return Choice::Steam;
        return Choice::Auto;
    }

    const char *setName(Set set)
    {
        switch (set)
        {
        case Set::PlayStation: return "PlayStation (the game's own art)";
        case Set::Xbox360: return "Xbox 360";
        case Set::SteamDeck: return "Steam Deck";
        case Set::Switch: return "Nintendo Switch";
        case Set::Keyboard: return "keyboard";
        }
        return "?";
    }

    bool compose(Set set, int sheet, const uint8_t *payload, const uint8_t *clut, const prompt_sheets::PromptLabels &labels,
                 hd_textures::Image &out)
    {
        if (set == Set::PlayStation || !payload || !clut)
            return false;
        Canvas canvas = baseOf(payload, clut);
        int drawn = 0;
        if (sheet == 0)
        {
            for (int icon = 0; icon < sheets::IconCount; ++icon)
            {
                const float cx = static_cast<float>(kCell[icon][0]), cy = static_cast<float>(kCell[icon][1]);
                const Rect cellRect{cx, cy, cx + 32.0f, cy + 32.0f};
                // One texel of clear border: the GS's bilinear filter reads half a texel past a cell.
                const Rect box{cx + 1.0f, cy + 1.0f, cx + 31.0f, cy + 31.0f};
                if (icon == sheets::LeftStick || icon == sheets::RightStick)
                {
                    const std::string(&keys)[4] = icon == sheets::LeftStick ? labels.leftStick : labels.rightStick;
                    if (set != Set::Keyboard)
                    {
                        drawn += drawCell(canvas, set, stickFile(set, icon == sheets::LeftStick), cellRect, box) ? 1 : 0;
                        continue;
                    }
                    // Keys: the inverted T (up on top; left, down, right below), when the pack has all four.
                    std::string files[4];
                    bool all = true;
                    for (int d = 0; d < 4; ++d)
                    {
                        files[d] = keyFile(keys[d]);
                        all = all && !files[d].empty() && art(set, files[d]).ok;
                    }
                    if (!all)
                        continue;
                    clear(canvas, scaled(cellRect));
                    const float k = 10.0f, top = cy + 5.0f, bottom = cy + 16.0f;
                    const Rect where[4] = {{cx + 11.0f, top, cx + 11.0f + k, top + k},          // up
                                           {cx + 21.0f, bottom, cx + 21.0f + k, bottom + k},    // right
                                           {cx + 11.0f, bottom, cx + 11.0f + k, bottom + k},    // down
                                           {cx + 1.0f, bottom, cx + 1.0f + k, bottom + k}};     // left
                    for (int d = 0; d < 4; ++d)
                        drawFitted(canvas, art(set, files[d]).image, scaled(where[d]));
                    ++drawn;
                    continue;
                }
                drawn += drawCell(canvas, set, fileFor(set, labels.icon[icon]), cellRect, box) ? 1 : 0;
            }
            // The style list's arrows (button_prompt_sheets.h) at full resolution: the same shapes, smooth edges
            // (4 x 4 samples a pixel) instead of the PS2 sheet's texels scaled up.
            for (int piece = 0; piece < sheets::ArrowPieceCount; ++piece)
            {
                const sheets::ArrowRect r = sheets::arrowRect(piece);
                const Rect area = scaled(Rect{r.x0, r.y0, r.x1, r.y1});
                for (int y = static_cast<int>(area.y0); y < static_cast<int>(area.y1) && y < canvas.h; ++y)
                    for (int x = static_cast<int>(area.x0); x < static_cast<int>(area.x1) && x < canvas.w; ++x)
                    {
                        float sum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                        for (int sy = 0; sy < 4; ++sy)
                            for (int sx = 0; sx < 4; ++sx)
                            {
                                float c[4];
                                sheets::arrowColour((x + (sx + 0.5f) / 4.0f) / kScale, (y + (sy + 0.5f) / 4.0f) / kScale, c);
                                for (int k = 0; k < 4; ++k)
                                    sum[k] += c[k];
                            }
                        float *d = canvas.at(x, y);
                        for (int k = 0; k < 4; ++k)
                            d[k] = sum[k] / 16.0f;
                    }
            }
        }
        else
        {
            // The game draws the shape's quad about one screen pixel left of and above the grey button under it
            // (gif_31864, the tutorial's prompt: shape quad centre 214.75,331, button 215.5,332; the PS2 symbols sit off
            // centre too, a thin ring hides it, a whole button shows it). One texel right and down puts the pack's
            // button on the game's.
            constexpr float kShift = 1.0f;
            for (const Shape &shape : kShapes)
            {
                const Rect box{shape.sprite.x0 + kShift, shape.sprite.y0 + kShift, std::min(shape.sprite.x1 + kShift, 128.0f),
                               std::min(shape.sprite.y1 + kShift, 128.0f)};
                drawn += drawCell(canvas, set, fileFor(set, labels.icon[shape.icon]), shape.clear, box) ? 1 : 0;
            }
        }
        if (drawn == 0)
            return false;
        out.width = canvas.w;
        out.height = canvas.h;
        out.rgba.assign(static_cast<size_t>(canvas.w) * canvas.h * 4u, 0u);
        for (size_t i = 0; i < static_cast<size_t>(canvas.w) * canvas.h; ++i)
        {
            const float *p = &canvas.px[i * 4u];
            const float a = std::clamp(p[3], 0.0f, 1.0f);
            uint8_t *o = &out.rgba[i * 4u];
            if (a > 0.0f)
                for (int c = 0; c < 3; ++c)
                    o[c] = static_cast<uint8_t>(std::lround(std::clamp(p[c] / a, 0.0f, 1.0f) * 255.0f));
            o[3] = static_cast<uint8_t>(std::lround(a * 128.0f)); // the PS2's scale: 0x80 = opaque
        }
        return true;
    }
}
