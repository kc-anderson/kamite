#include "kamite/prompt_art.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#if defined(_MSC_VER)
#pragma comment(lib, "gdi32.lib")
#endif
#endif

namespace kamite::prompt_art
{
    namespace
    {
        // ---- raylib codes (raylib.h KeyboardKey). raylib.h cannot be included next to windows.h (Rectangle,
        // CloseWindow, DrawText...), so the values are spelled out. ----
        constexpr int kKeyApostrophe = 39, kKeyComma = 44, kKeyMinus = 45, kKeyPeriod = 46, kKeySlash = 47;
        constexpr int kKeyZero = 48, kKeyNine = 57, kKeySemicolon = 59, kKeyEqual = 61, kKeyA = 65, kKeyZ = 90;
        constexpr int kKeyLeftBracket = 91, kKeyBackslash = 92, kKeyRightBracket = 93, kKeyGrave = 96, kKeySpace = 32;
        constexpr int kKeyEscape = 256, kKeyEnter = 257, kKeyTab = 258, kKeyBackspace = 259, kKeyInsert = 260;
        constexpr int kKeyDelete = 261, kKeyRight = 262, kKeyLeft = 263, kKeyDown = 264, kKeyUp = 265;
        constexpr int kKeyPageUp = 266, kKeyPageDown = 267, kKeyHome = 268, kKeyEnd = 269, kKeyCapsLock = 280;
        constexpr int kKeyScrollLock = 281, kKeyNumLock = 282, kKeyPrintScreen = 283, kKeyPause = 284;
        constexpr int kKeyF1 = 290, kKeyF12 = 301, kKeyLeftShift = 340, kKeyLeftControl = 341, kKeyLeftAlt = 342;
        constexpr int kKeyLeftSuper = 343, kKeyRightShift = 344, kKeyRightControl = 345, kKeyRightAlt = 346;
        constexpr int kKeyRightSuper = 347, kKeyKbMenu = 348, kKeyKp0 = 320, kKeyKp9 = 329, kKeyKpDecimal = 330;
        constexpr int kKeyKpDivide = 331, kKeyKpMultiply = 332, kKeyKpSubtract = 333, kKeyKpAdd = 334;
        constexpr int kKeyKpEnter = 335, kKeyKpEqual = 336;

        struct KeyName
        {
            int key;
            const char *full;
            const char *compact; // <= 4 characters: what a 32-texel cell shows
        };

        // Names follow pad_prompt_art.c's k_key_names (Tokyo Jungle) where the key exists there.
        constexpr KeyName kKeyNames[] = {
            {kKeySpace, "Space", "Spc"}, {kKeyEnter, "Enter", "Ent"}, {kKeyKpEnter, "NumEnter", "NEnt"},
            {kKeyBackspace, "Bksp", "Bksp"}, {kKeyTab, "Tab", "Tab"}, {kKeyEscape, "Esc", "Esc"},
            {kKeyCapsLock, "Caps", "Caps"}, {kAnyShift, "Shift", "Shft"}, {kKeyLeftShift, "Shift", "Shft"},
            {kKeyRightShift, "RShift", "RShf"}, {kAnyControl, "Ctrl", "Ctrl"}, {kKeyLeftControl, "Ctrl", "Ctrl"},
            {kKeyRightControl, "RCtrl", "RCtl"}, {kAnyAlt, "Alt", "Alt"}, {kKeyLeftAlt, "Alt", "Alt"},
            {kKeyRightAlt, "RAlt", "RAlt"}, {kKeyLeftSuper, "Win", "Win"}, {kKeyRightSuper, "RWin", "RWin"},
            {kKeyKbMenu, "Menu", "Menu"}, {kKeyInsert, "Ins", "Ins"}, {kKeyDelete, "Del", "Del"},
            {kKeyHome, "Home", "Home"}, {kKeyEnd, "End", "End"}, {kKeyPageUp, "PgUp", "PgUp"},
            {kKeyPageDown, "PgDn", "PgDn"}, {kKeyNumLock, "NumLock", "NmLk"}, {kKeyScrollLock, "ScrLock", "ScLk"},
            {kKeyPrintScreen, "PrtSc", "PrSc"}, {kKeyPause, "Pause", "Paus"}, {kKeyKpMultiply, "Num*", "N*"},
            {kKeyKpAdd, "Num+", "N+"}, {kKeyKpSubtract, "Num-", "N-"}, {kKeyKpDecimal, "Num.", "N."},
            {kKeyKpDivide, "Num/", "N/"}, {kKeyKpEqual, "Num=", "N="}, {kKeySemicolon, ";", ";"},
            {kKeyEqual, "=", "="}, {kKeyComma, ",", ","}, {kKeyMinus, "-", "-"}, {kKeyPeriod, ".", "."},
            {kKeySlash, "/", "/"}, {kKeyGrave, "`", "`"}, {kKeyLeftBracket, "[", "["}, {kKeyBackslash, "\\", "\\"},
            {kKeyRightBracket, "]", "]"}, {kKeyApostrophe, "'", "'"},
        };

        // ps2_host_gamepads::Button (SDL3 SDL_GamepadButton order), Xbox names.
        constexpr KeyName kPadButtons[] = {
            {0, "A", "A"}, {1, "B", "B"}, {2, "X", "X"}, {3, "Y", "Y"}, {4, "View", "View"}, {5, "Guide", "Gd"},
            {6, "Menu", "Menu"}, {7, "LS", "LS"}, {8, "RS", "RS"}, {9, "LB", "LB"}, {10, "RB", "RB"},
            {11, "D-pad up", "\xE2\x86\x91"}, {12, "D-pad down", "\xE2\x86\x93"}, {13, "D-pad left", "\xE2\x86\x90"},
            {14, "D-pad right", "\xE2\x86\x92"}, {15, "Misc", "Misc"}, {16, "Paddle 1", "P1"}, {17, "Paddle 2", "P2"},
            {18, "Paddle 3", "P3"}, {19, "Paddle 4", "P4"}, {20, "Touchpad", "TP"},
        };

        // ---- look ----
        constexpr int kSs = 4;                // supersampling per axis
        constexpr float kCondensedGain = 1.04f; // Bahnschrift only when it draws at least 4% larger
        constexpr float kHintedBelow = 8.0f;    // cap height (px) under which text is drawn hinted at 1x

        constexpr wchar_t kFaceSemibold[] = L"Segoe UI Semibold";
        constexpr wchar_t kFaceBold[] = L"Segoe UI";
        constexpr wchar_t kFaceCondensed[] = L"Bahnschrift SemiBold SemiConden"; // GDI's 31-character family name
        constexpr wchar_t kFaceFallback[] = L"Arial"; // when Segoe UI is missing (Wine)

        int arrowDir(std::string_view s)
        {
            if (s == kArrowUp)
                return 1;
            if (s == kArrowDown)
                return 2;
            if (s == kArrowLeft)
                return 3;
            if (s == kArrowRight)
                return 4;
            return 0;
        }

        int mouseButton(std::string_view s)
        {
            if (s == "M1")
                return 1;
            if (s == "M2")
                return 2;
            if (s == "M3")
                return 3;
            return 0;
        }

#if defined(_WIN32)
        // ---- supersampled mask canvas (pad_prompt_art.c PpaCanvas) ----
        class Canvas
        {
        public:
            Canvas(int w, int h, int scale = kSs) : m_s(scale), m_w(w), m_h(h), m_W(w * scale), m_H(h * scale)
            {
                m_dc = CreateCompatibleDC(nullptr);
                if (!m_dc)
                    return;
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = m_W;
                bi.bmiHeader.biHeight = -m_H; // top-down
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                void *bits = nullptr;
                m_bmp = CreateDIBSection(m_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                if (!m_bmp || !bits)
                    return;
                m_px = static_cast<uint32_t *>(bits);
                m_old = SelectObject(m_dc, m_bmp);
                SetBkMode(m_dc, TRANSPARENT);
                SetPolyFillMode(m_dc, WINDING);
            }

            ~Canvas()
            {
                if (m_dc)
                {
                    if (m_old)
                        SelectObject(m_dc, m_old);
                    if (m_bmp)
                        DeleteObject(m_bmp);
                    DeleteDC(m_dc);
                }
            }

            Canvas(const Canvas &) = delete;
            Canvas &operator=(const Canvas &) = delete;

            [[nodiscard]] bool ok() const { return m_px != nullptr; }
            [[nodiscard]] HDC dc() const { return m_dc; }
            [[nodiscard]] int W() const { return m_W; }
            [[nodiscard]] int H() const { return m_H; }

            void clear()
            {
                GdiFlush();
                std::memset(m_px, 0, static_cast<size_t>(m_W) * static_cast<size_t>(m_H) * 4u);
            }

            // Box filter of the green channel into cov[w*h], 0..1.
            void take(float *cov) const
            {
                GdiFlush();
                const float norm = 1.0f / (255.0f * static_cast<float>(m_s * m_s));
                for (int y = 0; y < m_h; ++y)
                {
                    for (int x = 0; x < m_w; ++x)
                    {
                        unsigned sum = 0;
                        for (int sy = 0; sy < m_s; ++sy)
                        {
                            const uint32_t *row = m_px + static_cast<size_t>(y * m_s + sy) * static_cast<size_t>(m_W) + static_cast<size_t>(x) * m_s;
                            for (int sx = 0; sx < m_s; ++sx)
                                sum += (row[sx] >> 8) & 0xFFu;
                        }
                        cov[y * m_w + x] = static_cast<float>(sum) * norm;
                    }
                }
            }

            // Supersampled columns that hold ink (any green), -1/-1 when empty.
            void inkColumns(int &left, int &right) const
            {
                GdiFlush();
                left = -1;
                right = -1;
                for (int x = 0; x < m_W; ++x)
                {
                    for (int y = 0; y < m_H; ++y)
                    {
                        if ((m_px[static_cast<size_t>(y) * m_W + x] >> 8) & 0xFFu)
                        {
                            if (left < 0)
                                left = x;
                            right = x;
                            break;
                        }
                    }
                }
            }

        private:
            int m_s;
            int m_w;
            int m_h;
            int m_W;
            int m_H;
            HDC m_dc = nullptr;
            HBITMAP m_bmp = nullptr;
            HGDIOBJ m_old = nullptr;
            uint32_t *m_px = nullptr;
        };

        int ss(float v)
        {
            return static_cast<int>(std::lround(v * static_cast<float>(kSs)));
        }

        void fillRoundRect(Canvas &c, float x0, float y0, float x1, float y1, float r)
        {
            const HGDIOBJ brush = SelectObject(c.dc(), GetStockObject(WHITE_BRUSH));
            const HGDIOBJ pen = SelectObject(c.dc(), GetStockObject(NULL_PEN));
            const int d = std::max(1, ss(2.0f * r));
            // With a null pen GDI leaves out the right and bottom edge: add one.
            RoundRect(c.dc(), ss(x0), ss(y0), ss(x1) + 1, ss(y1) + 1, d, d);
            SelectObject(c.dc(), pen);
            SelectObject(c.dc(), brush);
        }

        void fillPoly(Canvas &c, const float *xy, int n)
        {
            POINT p[16];
            n = std::min(n, 16);
            for (int i = 0; i < n; ++i)
                p[i] = POINT{ss(xy[2 * i]), ss(xy[2 * i + 1])};
            const HGDIOBJ brush = SelectObject(c.dc(), GetStockObject(WHITE_BRUSH));
            const HGDIOBJ pen = SelectObject(c.dc(), GetStockObject(NULL_PEN));
            Polygon(c.dc(), p, n);
            SelectObject(c.dc(), pen);
            SelectObject(c.dc(), brush);
        }

        // pad_prompt_art.c ppa_draw_arrow: a block arrow pointing right, rotated.
        void drawArrow(Canvas &c, int dir, float cx, float cy, float len)
        {
            const float sh = 0.13f * len, hw = 0.40f * len, hl = 0.50f * len, h = 0.5f * len;
            const float base[7][2] = {{-h, -sh}, {h - hl, -sh}, {h - hl, -hw}, {h, 0.0f}, {h - hl, hw}, {h - hl, sh}, {-h, sh}};
            float xy[14];
            for (int i = 0; i < 7; ++i)
            {
                const float x = base[i][0], y = base[i][1];
                float rx = x, ry = y;
                switch (dir)
                {
                case 1: rx = y; ry = -x; break;  // up
                case 2: rx = -y; ry = x; break;  // down
                case 3: rx = -x; ry = -y; break; // left
                default: break;                  // right
                }
                xy[2 * i] = cx + rx;
                xy[2 * i + 1] = cy + ry;
            }
            fillPoly(c, xy, 7);
        }

        void fillRect(Canvas &c, float x0, float y0, float x1, float y1)
        {
            const HGDIOBJ brush = SelectObject(c.dc(), GetStockObject(WHITE_BRUSH));
            const HGDIOBJ pen = SelectObject(c.dc(), GetStockObject(NULL_PEN));
            Rectangle(c.dc(), ss(x0), ss(y0), ss(x1) + 1, ss(y1) + 1);
            SelectObject(c.dc(), pen);
            SelectObject(c.dc(), brush);
        }

        // A mouse seen from above: outline, the line between the buttons and the one under them, the pressed button
        // filled (1 left, 2 right, 3 the wheel). Height h, centred at (cx, cy). Adds into cov.
        void drawMouse(Canvas &c, int button, float cx, float cy, float h, float *cov, float *tmp, int n)
        {
            const float w = std::round(0.68f * h);
            const float x0 = std::round(cx - 0.5f * w), y0 = std::round(cy - 0.5f * h);
            const float x1 = x0 + w, y1 = y0 + h, r = 0.48f * w, t = std::max(1.0f, std::round(0.07f * h));
            const float split = std::round(y0 + 0.42f * h);
            const auto add = [&](const float *src, const float *mask) {
                for (int i = 0; i < n; ++i)
                    cov[i] = std::min(1.0f, cov[i] + src[i] * (mask ? mask[i] : 1.0f));
            };
            std::fill(cov, cov + n, 0.0f);
            std::vector<float> inner(static_cast<size_t>(n));
            // outline ring = body minus its inside
            c.clear();
            fillRoundRect(c, x0, y0, x1, y1, r);
            c.take(tmp);
            c.clear();
            fillRoundRect(c, x0 + t, y0 + t, x1 - t, y1 - t, std::max(0.5f, r - t));
            c.take(inner.data());
            for (int i = 0; i < n; ++i)
                cov[i] = std::max(0.0f, tmp[i] - inner[static_cast<size_t>(i)]);
            // the pressed button: the inside above the split, clipped to its half; or the wheel
            c.clear();
            if (button == 3)
            {
                const float ww = std::max(1.0f, std::round(0.16f * w));
                fillRoundRect(c, cx - 0.5f * ww, y0 + t + 1.0f, cx + 0.5f * ww, split - 0.5f, 0.5f * ww);
                c.take(tmp);
                add(tmp, nullptr);
            }
            else
            {
                if (button == 1)
                    fillRect(c, x0, y0, cx - 0.5f * t, split - 0.5f * t);
                else
                    fillRect(c, cx + 0.5f * t, y0, x1, split - 0.5f * t);
                c.take(tmp);
                add(tmp, inner.data());
                c.clear();
                fillRect(c, cx - 0.5f * t, y0, cx + 0.5f * t, split);
                c.take(tmp);
                add(tmp, inner.data());
            }
            c.clear();
            fillRect(c, x0, split - 0.5f * t, x1, split + 0.5f * t);
            c.take(tmp);
            add(tmp, inner.data());
        }

        // ---- text ----
        struct Face
        {
            const wchar_t *name;
            int weight;
        };

        HFONT makeFont(const Face &face, float px, int scale = kSs)
        {
            return CreateFontW(-static_cast<int>(std::lround(px * static_cast<float>(scale))), 0, 0, 0, face.weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                               DEFAULT_PITCH | FF_SWISS, face.name);
        }

        struct Metrics
        {
            float ink = 0.0f;  // ink width (advance minus the outer side bearings), output px
            float cap = 0.0f;  // cap height, output px
            bool faceOk = false;
        };

        Metrics measure(HDC dc, const Face &face, const std::wstring &s, float px)
        {
            Metrics m;
            const HFONT f = makeFont(face, px);
            if (!f)
                return m;
            const HGDIOBJ old = SelectObject(dc, f);
            wchar_t got[LF_FACESIZE]{};
            GetTextFaceW(dc, LF_FACESIZE, got);
            m.faceOk = _wcsicmp(got, face.name) == 0;
            SIZE sz{};
            GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
            float ink = static_cast<float>(sz.cx);
            ABC first{}, last{};
            if (GetCharABCWidthsW(dc, s.front(), s.front(), &first) && GetCharABCWidthsW(dc, s.back(), s.back(), &last))
                ink -= static_cast<float>(first.abcA + last.abcC);
            // Cap height = the black box of "H" (OUTLINETEXTMETRIC's otmsCapEmHeight reads ~0.7x low for Segoe UI
            // here, which made labels 1.4x the box).
            GLYPHMETRICS gm{};
            const MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
            float cap = 0.70f * px * kSs;
            if (GetGlyphOutlineW(dc, L'H', GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR && gm.gmBlackBoxY > 0)
                cap = static_cast<float>(gm.gmBlackBoxY);
            SelectObject(dc, old);
            DeleteObject(f);
            m.ink = ink / kSs;
            m.cap = cap / kSs;
            return m;
        }

        // Largest size (font px) whose ink fits width and whose caps fit capHeight; 0 when the face is missing.
        float fit(HDC dc, const Face &face, const std::wstring &s, float width, float capHeight, float &cap)
        {
            const float ref = 40.0f;
            Metrics m = measure(dc, face, s, ref);
            if (!m.faceOk)
                return 0.0f;
            float px = 400.0f;
            if (m.ink > 0.0f)
                px = std::min(px, width * ref / m.ink);
            if (m.cap > 0.0f)
                px = std::min(px, capHeight * ref / m.cap);
            for (int i = 0; i < 60 && px > 2.0f; ++i)
            {
                m = measure(dc, face, s, px);
                if (m.ink <= width + 0.01f && m.cap <= capHeight + 0.01f)
                    break;
                px *= 0.97f;
            }
            cap = m.cap;
            return px;
        }

        std::wstring widen(std::string_view text)
        {
            if (text.empty())
                return {};
            wchar_t buffer[64];
            const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), buffer, 63);
            return n > 0 ? std::wstring(buffer, static_cast<size_t>(n)) : std::wstring();
        }
#endif
    }

    Label keyLabel(int key)
    {
        char buffer[16];
        if (key == 0)
            return {std::string(kUnbound), std::string(kUnbound)};
        if (key >= kKeyA && key <= kKeyZ)
            return {std::string(1, static_cast<char>('A' + key - kKeyA)), std::string(1, static_cast<char>('A' + key - kKeyA))};
        if (key >= kKeyZero && key <= kKeyNine)
            return {std::string(1, static_cast<char>('0' + key - kKeyZero)), std::string(1, static_cast<char>('0' + key - kKeyZero))};
        if (key >= kKeyF1 && key <= kKeyF12)
        {
            std::snprintf(buffer, sizeof(buffer), "F%d", key - kKeyF1 + 1);
            return {buffer, buffer};
        }
        if (key >= kKeyKp0 && key <= kKeyKp9)
        {
            std::snprintf(buffer, sizeof(buffer), "Num%d", key - kKeyKp0);
            return {buffer, buffer}; // "Num8" fits a cell (4 characters)
        }
        switch (key)
        {
        case kKeyUp: return {std::string(kArrowUp), std::string(kArrowUp)};
        case kKeyDown: return {std::string(kArrowDown), std::string(kArrowDown)};
        case kKeyLeft: return {std::string(kArrowLeft), std::string(kArrowLeft)};
        case kKeyRight: return {std::string(kArrowRight), std::string(kArrowRight)};
        default: break;
        }
        for (const KeyName &name : kKeyNames)
            if (name.key == key)
                return {name.full, name.compact};
        return {"?", "?"};
    }

    Label mouseLabel(int button)
    {
        if (button < 0 || button > 6)
            return {"?", "?"};
        const std::string text = "M" + std::to_string(button + 1);
        return {text, text};
    }

    Label padButtonLabel(int button)
    {
        for (const KeyName &name : kPadButtons)
            if (name.key == button)
                return {name.full, name.compact};
        return {"?", "?"};
    }

    Label padAxisLabel(int axis, int sign)
    {
        switch (axis)
        {
        case 0: return {sign < 0 ? "LS left" : "LS right", "LS"};
        case 1: return {sign < 0 ? "LS up" : "LS down", "LS"};
        case 2: return {sign < 0 ? "RS left" : "RS right", "RS"};
        case 3: return {sign < 0 ? "RS up" : "RS down", "RS"};
        case 4: return {"LT", "LT"};
        case 5: return {"RT", "RT"};
        default: return {"?", "?"};
        }
    }

    void composite(Image &dst, const float *coverage, Rgb colour, float alpha)
    {
        const size_t n = static_cast<size_t>(dst.w) * static_cast<size_t>(dst.h);
        for (size_t i = 0; i < n; ++i)
        {
            const float a = std::clamp(coverage[i] * alpha, 0.0f, 1.0f);
            if (a <= 0.0f)
                continue;
            float *d = &dst.px[i * 4u];
            d[0] = colour.r * a + d[0] * (1.0f - a);
            d[1] = colour.g * a + d[1] * (1.0f - a);
            d[2] = colour.b * a + d[2] * (1.0f - a);
            d[3] = a + d[3] * (1.0f - a);
        }
    }

#if defined(_WIN32)
    namespace
    {
        // What textCoverage draws: an arrow, a mouse or text (face, size, cap height in output pixels).
        struct Plan
        {
            int arrow = 0;
            int mouse = 0;
            float size = 0.0f; // arrow length / mouse height
            Face face{kFaceSemibold, FW_SEMIBOLD};
            float px = 0.0f;
            float cap = 0.0f;
        };

        bool plan(HDC dc, std::string_view text, const std::wstring &ws, const TextBox &box, Plan &p)
        {
            if (text.empty() || box.width <= 0.5f || box.capHeight <= 0.5f)
                return false;
            if ((p.arrow = arrowDir(text)) != 0)
            {
                p.size = std::min(box.width, box.capHeight * 1.25f);
                if (box.snap)
                {
                    p.size = std::floor(p.size);
                    // an odd length puts the shaft and the tip on whole pixels around a pixel centre
                    if (static_cast<int>(p.size) % 2 == 0)
                        p.size -= 1.0f;
                }
                p.cap = p.size;
                return p.size >= 3.0f;
            }
            if ((p.mouse = mouseButton(text)) != 0 && box.capHeight >= 10.0f)
            {
                p.size = std::floor(std::min(box.capHeight * 1.5f, box.width * 1.4f));
                p.cap = p.size;
                return true;
            }
            p.mouse = 0;
            if (ws.empty())
                return false;
            const Face primary = box.weight == Weight::Bold ? Face{kFaceBold, FW_BOLD} : Face{kFaceSemibold, FW_SEMIBOLD};
            const Face condensed{kFaceCondensed, FW_SEMIBOLD};
            p.face = primary;
            p.px = fit(dc, primary, ws, box.width, box.capHeight, p.cap);
            if (p.px <= 0.0f)
            {
                p.face = Face{kFaceFallback, box.weight == Weight::Bold ? FW_BOLD : FW_SEMIBOLD};
                p.px = fit(dc, p.face, ws, box.width, box.capHeight, p.cap);
            }
            // Bahnschrift SemiCondensed when it draws the label larger (long labels, wide letters like W or M).
            float capC = 0.0f;
            const float pxC = fit(dc, condensed, ws, box.width, box.capHeight, capC);
            if (pxC > 0.0f && (p.px <= 0.0f || capC > p.cap * kCondensedGain))
            {
                p.face = condensed;
                p.px = pxC;
                p.cap = capC;
            }
            if (p.px <= 0.0f || p.cap <= 0.0f)
                return false;
            if (box.snap && p.cap >= 1.0f)
            {
                const float whole = std::floor(p.cap + 0.1f);
                p.px *= whole / p.cap;
                p.cap = measure(dc, p.face, ws, p.px).cap;
            }
            return true;
        }
    }

    float textCapHeight(std::string_view text, const TextBox &box)
    {
        Canvas c(1, 1);
        Plan p;
        if (!c.ok() || !plan(c.dc(), text, widen(text), box, p))
            return 0.0f;
        return p.cap;
    }

    float textCoverage(std::string_view text, const TextBox &box, int w, int h, float *coverage)
    {
        // size check first: a negative w or h cast to size_t would make the fill below run off the buffer
        if (w <= 0 || h <= 0)
            return 0.0f;
        const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
        std::fill(coverage, coverage + n, 0.0f);
        Canvas c(w, h);
        if (!c.ok())
            return 0.0f;
        const std::wstring ws = widen(text);
        Plan p;
        if (!plan(c.dc(), text, ws, box, p))
            return 0.0f;
        c.clear();

        if (p.arrow)
        {
            const float cx = box.snap ? std::floor(box.cx) + 0.5f : box.cx;
            const float cy = box.snap ? std::floor(box.cy) + 0.5f : box.cy;
            drawArrow(c, p.arrow, cx, cy, p.size);
            c.take(coverage);
            return p.cap;
        }
        if (p.mouse)
        {
            std::vector<float> tmp(n);
            drawMouse(c, p.mouse, box.cx, box.cy, p.size, coverage, tmp.data(), static_cast<int>(n));
            return p.cap;
        }

        const float baseline = box.snap ? std::round(box.cy + 0.5f * p.cap) : box.cy + 0.5f * p.cap;
        if (p.cap < kHintedBelow)
        {
            // Small letters: GDI's own hinted greyscale text at 1x keeps the stems on whole pixels, where the box
            // filter of the 4x text would smear them over two.
            Canvas one(w, h, 1);
            const HFONT f1 = one.ok() ? makeFont(p.face, p.px, 1) : nullptr;
            if (f1)
            {
                const HGDIOBJ old1 = SelectObject(one.dc(), f1);
                SetTextColor(one.dc(), RGB(255, 255, 255));
                SetTextAlign(one.dc(), TA_CENTER | TA_BASELINE);
                one.clear();
                int x1 = static_cast<int>(std::lround(box.cx));
                const int y1 = static_cast<int>(std::lround(baseline));
                TextOutW(one.dc(), x1, y1, ws.c_str(), static_cast<int>(ws.size()));
                int left = 0, right = 0;
                one.inkColumns(left, right);
                if (left >= 0)
                {
                    const int shift = static_cast<int>(std::lround(box.cx - 0.5f * static_cast<float>(right + 1 - left))) - left;
                    if (shift != 0)
                    {
                        one.clear();
                        x1 += shift;
                        TextOutW(one.dc(), x1, y1, ws.c_str(), static_cast<int>(ws.size()));
                    }
                }
                SelectObject(one.dc(), old1);
                DeleteObject(f1);
                one.take(coverage);
                return p.cap;
            }
        }
        const HFONT f = makeFont(p.face, p.px);
        if (!f)
            return 0.0f;
        const HGDIOBJ old = SelectObject(c.dc(), f);
        SetTextColor(c.dc(), RGB(255, 255, 255));
        SetTextAlign(c.dc(), TA_CENTER | TA_BASELINE);
        int x = ss(box.cx);
        TextOutW(c.dc(), x, ss(baseline), ws.c_str(), static_cast<int>(ws.size()));
        // Centre the ink (not the advance) and start it on a whole pixel.
        int left = 0, right = 0;
        c.inkColumns(left, right);
        if (left >= 0)
        {
            const float inkLeft = static_cast<float>(left) / kSs;
            const float inkWidth = static_cast<float>(right + 1 - left) / kSs;
            float want = box.cx - 0.5f * inkWidth;
            if (box.snap)
                want = std::round(want);
            const int shift = static_cast<int>(std::lround((want - inkLeft) * kSs));
            if (shift != 0)
            {
                c.clear();
                x += shift;
                TextOutW(c.dc(), x, ss(baseline), ws.c_str(), static_cast<int>(ws.size()));
            }
        }
        SelectObject(c.dc(), old);
        DeleteObject(f);
        c.take(coverage);
        return p.cap;
    }

    bool drawCaps(const Cap *caps, int count, const CapLook &look, Image &dst)
    {
        if (dst.w <= 0 || dst.h <= 0)
            return false;
        Canvas c(dst.w, dst.h);
        if (!c.ok())
            return false;
        const int n = dst.w * dst.h;
        std::vector<float> outer(static_cast<size_t>(n)), face(static_cast<size_t>(n)), tmp(static_cast<size_t>(n));
        for (int k = 0; k < count; ++k)
        {
            const Cap &cap = caps[k];
            const float t = look.outlineWidth;
            const float fx0 = cap.x0 + t, fy0 = cap.y0 + t, fx1 = cap.x1 - t, fy1 = cap.y1 - t;
            // outline = the whole plate in the outline colour, then the face over it
            c.clear();
            fillRoundRect(c, cap.x0, cap.y0, cap.x1, cap.y1, cap.radius);
            c.take(outer.data());
            composite(dst, outer.data(), look.outline, 1.0f);
            c.clear();
            fillRoundRect(c, fx0, fy0, fx1, fy1, std::max(0.5f, cap.radius - t));
            c.take(face.data());
            // face: top-to-bottom gradient
            for (int y = 0; y < dst.h; ++y)
            {
                const float v = std::clamp((static_cast<float>(y) + 0.5f - fy0) / std::max(1.0f, fy1 - fy0), 0.0f, 1.0f);
                const Rgb col{look.faceTop.r + (look.faceBottom.r - look.faceTop.r) * v,
                              look.faceTop.g + (look.faceBottom.g - look.faceTop.g) * v,
                              look.faceTop.b + (look.faceBottom.b - look.faceTop.b) * v};
                for (int x = 0; x < dst.w; ++x)
                {
                    const size_t i = static_cast<size_t>(y) * dst.w + x;
                    const float a = std::clamp(face[i], 0.0f, 1.0f);
                    float *d = &dst.px[i * 4u];
                    d[0] = col.r * a + d[0] * (1.0f - a);
                    d[1] = col.g * a + d[1] * (1.0f - a);
                    d[2] = col.b * a + d[2] * (1.0f - a);
                    d[3] = a + d[3] * (1.0f - a);
                }
            }
            // rim: the face minus itself moved one pixel down-right = a bright edge along the top and left
            c.clear();
            fillRoundRect(c, fx0 + 1.0f, fy0 + 1.0f, fx1 + 1.0f, fy1 + 1.0f, std::max(0.5f, cap.radius - t));
            c.take(tmp.data());
            for (int i = 0; i < n; ++i)
                tmp[static_cast<size_t>(i)] = std::max(0.0f, face[static_cast<size_t>(i)] - tmp[static_cast<size_t>(i)]);
            composite(dst, tmp.data(), look.rim, 0.85f);
            // label
            if (!cap.text.empty())
            {
                TextBox box;
                box.cx = 0.5f * (fx0 + fx1);
                box.cy = 0.5f * (fy0 + fy1);
                box.width = cap.labelWidth * (fx1 - fx0);
                box.capHeight = cap.labelCapPx > 0.0f ? cap.labelCapPx : cap.labelCap * (fy1 - fy0);
                box.weight = look.labelWeight;
                if (textCoverage(cap.text, box, dst.w, dst.h, tmp.data()) > 0.0f)
                    composite(dst, tmp.data(), look.label, 1.0f);
            }
        }
        return true;
    }
#else
    float textCapHeight(std::string_view, const TextBox &)
    {
        return 0.0f;
    }

    float textCoverage(std::string_view, const TextBox &, int w, int h, float *coverage)
    {
        if (w > 0 && h > 0)
            std::fill(coverage, coverage + static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
        return 0.0f;
    }

    bool drawCaps(const Cap *, int, const CapLook &, Image &)
    {
        return false;
    }
#endif

    bool drawInvertedT(const std::string (&labels)[4], float x0, float y0, float x1, float y1, const CapLook &look, Image &dst)
    {
        // Whole-pixel keys: three across, two down; a key may be a little taller than wide.
        const float ux = std::floor((x1 - x0) / 3.0f);
        const float uy = std::min(std::floor((y1 - y0) / 2.0f), ux + 1.0f);
        if (ux < 5.0f || uy < 5.0f)
            return false;
        const float ox = x0 + std::floor(0.5f * ((x1 - x0) - 3.0f * ux));
        const float oy = y0 + std::floor(0.5f * ((y1 - y0) - 2.0f * uy));
        // up, right, down, left -> grid (col, row)
        const int cell[4][2] = {{1, 0}, {2, 1}, {1, 1}, {0, 1}};
        Cap caps[4];
        for (int i = 0; i < 4; ++i)
        {
            Cap &k = caps[i];
            k.x0 = ox + static_cast<float>(cell[i][0]) * ux;
            k.y0 = oy + static_cast<float>(cell[i][1]) * uy;
            k.x1 = k.x0 + ux;
            k.y1 = k.y0 + uy;
            k.radius = std::max(1.5f, 0.22f * ux);
            k.text = labels[i];
            k.labelWidth = 0.92f;
            k.labelCap = 0.70f;
        }
        // One size for all four: the smallest of their own best fits (a wide "W" sets it for "A S D").
        float common = 0.0f;
        for (const Cap &k : caps)
        {
            if (k.text.empty() || arrowDir(k.text))
                continue;
            const float t = look.outlineWidth;
            TextBox box;
            box.width = k.labelWidth * (k.x1 - k.x0 - 2.0f * t);
            box.capHeight = k.labelCap * (k.y1 - k.y0 - 2.0f * t);
            box.weight = look.labelWeight;
            const float cap = textCapHeight(k.text, box);
            if (cap > 0.0f)
                common = common > 0.0f ? std::min(common, cap) : cap;
        }
        for (Cap &k : caps)
            if (!arrowDir(k.text))
                k.labelCapPx = common;
        return drawCaps(caps, 4, look, dst);
    }
}
