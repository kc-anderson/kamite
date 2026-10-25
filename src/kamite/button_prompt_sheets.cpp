#include "kamite/button_prompt_sheets.h"

#include "kamite/prompt_art.h"
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
        namespace art = kamite::prompt_art;

        constexpr int kCellTexels = kCellSize * kCellSize;

        // ---- the icon sheet's cells (button_sheet_codes_x4.png; layout from switch table D_0044B150) ----
        enum class CellKind : uint8_t
        {
            RoundCap,   // grey sphere cap: black outline r~15.5, bright rim r~13-14.5, face r<=12.5, symbol/letters
            RoundedCap, // rounded-rectangle cap: outline at columns 0/31 and rows 1/30, rim 1-3, face 4..28 x 5..28
            Stick,      // "LEFT"/"RIGHT" over a stick ball, edge to edge
            Plate       // "START"/"SELECT" lettering with a small tab, edge to edge
        };

        struct Cell
        {
            int x;
            int y;
            CellKind kind;
        };

        constexpr Cell kCells[IconCount] = {
            {0, 0, CellKind::RoundCap},     // Circle
            {32, 0, CellKind::RoundCap},    // Triangle
            {64, 0, CellKind::RoundCap},    // Square
            {96, 0, CellKind::RoundCap},    // Cross
            {64, 32, CellKind::RoundedCap}, // L1
            {0, 32, CellKind::RoundedCap},  // R1
            {96, 32, CellKind::RoundedCap}, // L2
            {32, 32, CellKind::RoundedCap}, // R2
            {32, 64, CellKind::RoundCap},   // L3
            {0, 64, CellKind::RoundCap},    // R3
            {96, 64, CellKind::Stick},      // LeftStick
            {64, 64, CellKind::Stick},      // RightStick
            {32, 96, CellKind::Plate},      // Start
            {0, 96, CellKind::Plate},       // Select
        };

        // What gets cleared (and refilled from the face around it) before the label goes on:
        // - round caps: every texel within 12.4 of the cell centre (the ○△□ symbols reach 12.35, "R3"/"L3" 12.35), and
        //   coloured texels out to 13.9 (the × arms end at 13.5, on the rim);
        // - rounded caps: the letters, columns 7..26 x rows 7..25 ("R1" 7..24, "L2"/"R2" 7..26; rows 7..25 with the
        //   emboss), leaving face columns 4-6 / 27-28 and rows 5-6 / 26-28 to fill from.
        constexpr float kRoundClearRadius = 12.4f;
        constexpr float kRoundColourRadius = 13.9f;
        constexpr int kChromaSymbol = 12; // max-min channel spread of the symbol texels (the face is neutral grey)
        constexpr int kRoundedClear[4] = {7, 7, 26, 25}; // x0, y0, x1, y1 inclusive

        // START/SELECT have no cap of their own: they get the R1 cell's cap (cleared), the other wide button.
        constexpr int kPlateTemplate = R1;

        // Label boxes (cell pixels; continuous coordinates, the cell spans 0..32): one character as tall as the
        // original letters (R1/L3: 14-15 px caps inside a 1-px black outline), two a little lower, three or four
        // (Spc, Ctrl, Bksp) wider and lower. Round caps: the face chord narrows away from the middle.
        struct LabelBox
        {
            float cx, cy, width, cap;
        };
        constexpr LabelBox kRoundBoxes[3] = {{16.0f, 16.0f, 17.0f, 14.0f}, {16.0f, 16.0f, 20.0f, 12.0f}, {16.0f, 16.0f, 21.0f, 10.0f}};
        constexpr LabelBox kRoundedBoxes[3] = {{16.0f, 16.5f, 20.0f, 15.0f}, {16.0f, 16.5f, 21.0f, 13.0f}, {16.0f, 16.5f, 23.0f, 11.0f}};
        // A coloured letter in the look of the PS2 symbols (A/B/X/Y on a controller): as big as the symbols (the
        // circle is 22 px across, the triangle 24 wide), bold, no outline, lit from the top left inside its strokes.
        constexpr LabelBox kSymbolBoxes[3] = {{16.0f, 16.0f, 19.0f, 18.0f}, {16.0f, 16.0f, 21.0f, 15.0f}, {16.0f, 16.0f, 22.0f, 12.0f}};

        // The dark edge around the white label: the original R1/L3 letters have a black outline.
        constexpr float kHaloAlpha = 0.80f;
        // The symbol look's bevel: the stroke's top-left inside edge is lit, its bottom-right one in shadow (the
        // original rings and crosses have this), as a share of the ink's brightness.
        constexpr float kBevelLight = 0.45f, kBevelShadow = 0.40f;

        // The stick cells' inverted T inside the 1-texel clear border (bilinear sampling of the 32x32 cell reads
        // half a texel outside it).
        constexpr float kStickArea[4] = {1.0f, 1.0f, 31.0f, 31.0f};

        // Mini caps for the inverted T, in the R1 cap's colours (face 0x8B-0x93 at the top to 0x5E-0x6A at the bottom,
        // rim ~0xAF along the top and left, black outline).
        art::CapLook stickLook()
        {
            art::CapLook look;
            look.outline = {0.0f, 0.0f, 0.0f};
            look.faceTop = {0.55f, 0.55f, 0.55f};
            look.faceBottom = {0.40f, 0.40f, 0.40f};
            look.rim = {0.69f, 0.69f, 0.69f};
            look.label = {1.0f, 1.0f, 1.0f};
            look.outlineWidth = 1.0f;
            look.labelWeight = art::Weight::Bold; // ~6 px letters, drawn hinted: bold keeps whole-pixel stems
            return look;
        }

        // ---- texture 0x0F's bottom row: the four outline shapes (UV rects of layout #5 works 1-4, design notes),
        // and the texels to clear (the × starts one row above its rect, at y 101). ----
        struct Shape
        {
            int icon;
            float u0, v0, u1, v1;  // sprite rect, half-open
            int cx0, cy0, cx1, cy1; // clear rect, half-open
        };
        constexpr Shape kShapes[4] = {
            {Triangle, 0.0f, 101.0f, 29.0f, 128.0f, 0, 100, 30, 128},
            {Circle, 33.0f, 98.0f, 64.0f, 128.0f, 32, 97, 66, 128},
            {Square, 70.0f, 102.0f, 96.0f, 128.0f, 68, 100, 98, 128},
            {Cross, 100.0f, 102.0f, 128.0f, 128.0f, 99, 100, 128, 128},
        };

        // ---- palette ----
        struct Rgba
        {
            float r, g, b, a; // premultiplied, 0..1
        };

        std::array<Rgba, 256> premultiplied(const uint8_t *clut)
        {
            std::array<uint8_t, 1024> raw{};
            paletteRgba(clut, raw.data());
            std::array<Rgba, 256> pal{};
            for (int i = 0; i < 256; ++i)
            {
                const uint8_t *e = &raw[static_cast<size_t>(i) * 4u];
                const float a = std::min(1.0f, static_cast<float>(e[3]) / 128.0f);
                pal[static_cast<size_t>(i)] = {e[0] / 255.0f * a, e[1] / 255.0f * a, e[2] / 255.0f * a, a};
            }
            return pal;
        }

        using IndexSet = std::array<bool, 256>;

        // ---- the colour ramps (Tint) ----
        void rgbToHsv(float r, float g, float b, float &h, float &sat, float &v)
        {
            const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
            v = mx;
            sat = mx > 0.0f ? d / mx : 0.0f;
            if (d <= 0.0f)
                h = 0.0f;
            else if (mx == r)
                h = 60.0f * std::fmod((g - b) / d + 6.0f, 6.0f);
            else if (mx == g)
                h = 60.0f * ((b - r) / d + 2.0f);
            else
                h = 60.0f * ((r - g) / d + 4.0f);
        }

        void hsvToRgb(float h, float sat, float v, float &r, float &g, float &b)
        {
            const float c = v * sat, hh = std::fmod(h, 360.0f) / 60.0f, x = c * (1.0f - std::fabs(std::fmod(hh, 2.0f) - 1.0f));
            float rr = 0, gg = 0, bb = 0;
            if (hh < 1) { rr = c; gg = x; }
            else if (hh < 2) { rr = x; gg = c; }
            else if (hh < 3) { gg = c; bb = x; }
            else if (hh < 4) { gg = x; bb = c; }
            else if (hh < 5) { rr = x; bb = c; }
            else { rr = c; bb = x; }
            const float m = v - c;
            r = rr + m; g = gg + m; b = bb + m;
        }

        enum class Hue : uint8_t { None, Red, Yellow, Green, Blue, Pink };

        Hue hueOf(const uint8_t *e) // raw RGBA
        {
            if (e[3] < 0x80u)
                return Hue::None;
            float h, sat, v;
            rgbToHsv(e[0] / 255.0f, e[1] / 255.0f, e[2] / 255.0f, h, sat, v);
            if (sat < 0.25f)
                return Hue::None;
            if (h < 35.0f) return Hue::Red;
            if (h < 75.0f) return Hue::Yellow;
            if (h < 180.0f) return Hue::Green;
            if (h < 275.0f) return Hue::Blue;
            return Hue::Pink;
        }

        Hue hueOf(Tint tint)
        {
            switch (tint)
            {
            case Tint::Red: return Hue::Red;
            case Tint::Green: return Hue::Green;
            case Tint::Blue: return Hue::Blue;
            case Tint::Yellow: return Hue::Yellow;
            default: return Hue::None;
            }
        }

        // The CLUT to draw with: the original, or with its pink entries (the square's ramp, used by nothing else on
        // either sheet) turned a saturated yellow of the same brightness, for a yellow Y.
        // keep: entries (by index) that other art of the sheet uses too, left as they are.
        void makeClut(const uint8_t *clut, bool yellow, const IndexSet &keep, uint8_t *outClut)
        {
            std::memcpy(outClut, clut, 1024u);
            if (!yellow)
                return;
            // The pink entries are all about as bright (the square's edge blends); the yellow ramp made of them is
            // spread in brightness too, so the Y gets its bevel (the lightest keeps its brightness, the others step down).
            std::vector<std::pair<float, int>> pinks; // brightness, entry
            for (int c = 0; c < 256; ++c)
            {
                if (keep[static_cast<size_t>(c)])
                    continue;
                const int j = (c & 0xE7) | ((c & 0x08) << 1) | ((c & 0x10) >> 1); // CSM1 storage order (paletteRgba)
                const uint8_t *e = outClut + j * 4;
                if (hueOf(e) == Hue::Pink)
                    pinks.emplace_back(static_cast<float>(std::max({e[0], e[1], e[2]})) / 255.0f, j);
            }
            std::sort(pinks.begin(), pinks.end());
            for (size_t k = 0; k < pinks.size(); ++k)
            {
                uint8_t *e = outClut + pinks[k].second * 4;
                float h, sat, v, r, g, b;
                rgbToHsv(e[0] / 255.0f, e[1] / 255.0f, e[2] / 255.0f, h, sat, v);
                const float step = pinks.size() > 1 ? static_cast<float>(pinks.size() - 1u - k) / static_cast<float>(pinks.size() - 1u) : 0.0f;
                v = std::max(0.45f, v - 0.5f * step); // the brightest stays, the darkest ends near half
                hsvToRgb(50.0f, std::clamp(sat * 1.8f, 0.6f, 0.95f), v, r, g, b);
                e[0] = static_cast<uint8_t>(std::lround(r * 255.0f));
                e[1] = static_cast<uint8_t>(std::lround(g * 255.0f));
                e[2] = static_cast<uint8_t>(std::lround(b * 255.0f));
            }
        }

        // The palette entries of one hue, and the brightest of them as the ink to draw with.
        void addHue(IndexSet &allowed, const uint8_t *rawPal, Hue hue, art::Rgb &ink)
        {
            float best = -1.0f;
            for (int i = 0; i < 256; ++i)
            {
                const uint8_t *e = rawPal + i * 4;
                if (hueOf(e) != hue)
                    continue;
                allowed[static_cast<size_t>(i)] = true;
                const float v = static_cast<float>(std::max({e[0], e[1], e[2]}));
                if (v > best)
                {
                    best = v;
                    ink = {e[0] / 255.0f, e[1] / 255.0f, e[2] / 255.0f};
                }
            }
        }

        int chroma(const uint8_t *rgbaRaw, int index)
        {
            const uint8_t *e = rgbaRaw + index * 4;
            return std::max({e[0], e[1], e[2]}) - std::min({e[0], e[1], e[2]});
        }

        int nearest(const std::array<Rgba, 256> &pal, const IndexSet &allowed, const float *px)
        {
            int best = 0;
            float bestD = 1e30f;
            for (int i = 0; i < 256; ++i)
            {
                if (!allowed[static_cast<size_t>(i)])
                    continue;
                const Rgba &e = pal[static_cast<size_t>(i)];
                const float dr = e.r - px[0], dg = e.g - px[1], db = e.b - px[2], da = e.a - px[3];
                const float d = dr * dr + dg * dg + db * db + da * da;
                if (d < bestD)
                {
                    bestD = d;
                    best = i;
                }
            }
            return best;
        }

        // ---- one cell being edited ----
        struct CellWork
        {
            art::Image img{kCellSize, kCellSize};
            std::array<uint8_t, kCellTexels> index{};
            std::array<bool, kCellTexels> dirty{}; // re-quantize
            IndexSet allowed{};
        };

        void loadCell(CellWork &w, const uint8_t *sheet, int cx, int cy, const std::array<Rgba, 256> &pal)
        {
            w.allowed.fill(false);
            w.allowed[0] = true;
            for (int y = 0; y < kCellSize; ++y)
            {
                for (int x = 0; x < kCellSize; ++x)
                {
                    const uint8_t v = sheet[(cy + y) * kSheetSize + cx + x];
                    const size_t i = static_cast<size_t>(y * kCellSize + x);
                    w.index[i] = v;
                    w.allowed[v] = true;
                    const Rgba &e = pal[v];
                    w.img.px[i * 4u + 0u] = e.r;
                    w.img.px[i * 4u + 1u] = e.g;
                    w.img.px[i * 4u + 2u] = e.b;
                    w.img.px[i * 4u + 3u] = e.a;
                }
            }
            w.dirty.fill(false);
        }

        void storeCell(const CellWork &w, uint8_t *sheet, int cx, int cy, const std::array<Rgba, 256> &pal)
        {
            for (int y = 0; y < kCellSize; ++y)
            {
                for (int x = 0; x < kCellSize; ++x)
                {
                    const size_t i = static_cast<size_t>(y * kCellSize + x);
                    sheet[(cy + y) * kSheetSize + cx + x] = w.dirty[i] ? static_cast<uint8_t>(nearest(pal, w.allowed, &w.img.px[i * 4u])) : w.index[i];
                }
            }
        }

        // Harmonic fill of the masked texels from the unmasked ones around them (SOR on the 4-neighbour Laplacian):
        // the cap face is a smooth top-left-lit gradient, so this rebuilds it under the old symbol.
        void inpaint(art::Image &img, const std::array<bool, kCellTexels> &mask)
        {
            const int n = kCellSize;
            // start every masked texel at the mean of the known texels next to the hole, peeling inwards
            std::array<bool, kCellTexels> known{};
            for (int i = 0; i < kCellTexels; ++i)
                known[static_cast<size_t>(i)] = !mask[static_cast<size_t>(i)];
            for (bool grew = true; grew;)
            {
                grew = false;
                std::array<bool, kCellTexels> next = known;
                for (int y = 0; y < n; ++y)
                {
                    for (int x = 0; x < n; ++x)
                    {
                        const int i = y * n + x;
                        if (known[static_cast<size_t>(i)])
                            continue;
                        float sum[4] = {0, 0, 0, 0};
                        int count = 0;
                        for (int dy = -1; dy <= 1; ++dy)
                        {
                            for (int dx = -1; dx <= 1; ++dx)
                            {
                                const int xx = x + dx, yy = y + dy;
                                if ((dx || dy) && xx >= 0 && yy >= 0 && xx < n && yy < n && known[static_cast<size_t>(yy * n + xx)])
                                {
                                    for (int c = 0; c < 4; ++c)
                                        sum[c] += img.px[static_cast<size_t>(yy * n + xx) * 4u + static_cast<size_t>(c)];
                                    ++count;
                                }
                            }
                        }
                        if (count)
                        {
                            for (int c = 0; c < 4; ++c)
                                img.px[static_cast<size_t>(i) * 4u + static_cast<size_t>(c)] = sum[c] / static_cast<float>(count);
                            next[static_cast<size_t>(i)] = true;
                            grew = true;
                        }
                    }
                }
                known = next;
            }
            const float omega = 1.8f;
            for (int iter = 0; iter < 300; ++iter)
            {
                for (int y = 0; y < n; ++y)
                {
                    for (int x = 0; x < n; ++x)
                    {
                        const int i = y * n + x;
                        if (!mask[static_cast<size_t>(i)])
                            continue;
                        const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                        float sum[4] = {0, 0, 0, 0};
                        int count = 0;
                        for (const auto &p : nb)
                        {
                            if (p[0] < 0 || p[1] < 0 || p[0] >= n || p[1] >= n)
                                continue;
                            for (int c = 0; c < 4; ++c)
                                sum[c] += img.px[static_cast<size_t>(p[1] * n + p[0]) * 4u + static_cast<size_t>(c)];
                            ++count;
                        }
                        for (int c = 0; c < 4; ++c)
                        {
                            float &v = img.px[static_cast<size_t>(i) * 4u + static_cast<size_t>(c)];
                            v += omega * (sum[c] / static_cast<float>(count) - v);
                        }
                    }
                }
            }
        }

        size_t glyphCount(const std::string &text)
        {
            size_t count = 0;
            for (const char ch : text)
                count += (static_cast<unsigned char>(ch) & 0xC0u) != 0x80u;
            return count;
        }

        // White label with a dark halo over the cap face, or (symbol look, with `ink` a colour) a big bold bevelled
        // letter without one; marks the touched texels dirty. False when nothing could be drawn (GDI failure, no
        // usable font).
        bool drawLabel(CellWork &w, const std::string &text, const LabelBox (&boxes)[3], art::Rgb ink = {1.0f, 1.0f, 1.0f},
                       bool symbolLook = false)
        {
            const LabelBox &b = boxes[std::clamp<size_t>(glyphCount(text), 1u, 3u) - 1u];
            art::TextBox box;
            box.cx = b.cx;
            box.cy = b.cy;
            box.width = b.width;
            box.capHeight = b.cap;
            if (symbolLook)
                box.weight = art::Weight::Bold;
            std::array<float, kCellTexels> cov{};
            if (art::textCoverage(text, box, kCellSize, kCellSize, cov.data()) <= 0.0f)
                return false;
            if (symbolLook)
            {
                const auto at = [&](int x, int y) {
                    return x < 0 || y < 0 || x >= kCellSize || y >= kCellSize ? 0.0f : cov[static_cast<size_t>(y * kCellSize + x)];
                };
                std::array<float, kCellTexels> light{}, shadow{};
                for (int y = 0; y < kCellSize; ++y)
                {
                    for (int x = 0; x < kCellSize; ++x)
                    {
                        const float c = at(x, y);
                        if (c <= 0.0f)
                            continue;
                        // Inside edges: where the texel up-left (down-right) is outside the stroke.
                        light[static_cast<size_t>(y * kCellSize + x)] = std::max(0.0f, c - std::min({at(x - 1, y), at(x, y - 1), at(x - 1, y - 1)}));
                        shadow[static_cast<size_t>(y * kCellSize + x)] = std::max(0.0f, c - std::min({at(x + 1, y), at(x, y + 1), at(x + 1, y + 1)}));
                    }
                }
                // The base ink a step below the ramp's top, so the lit edge has somewhere to go.
                const art::Rgb base{ink.r * 0.82f, ink.g * 0.82f, ink.b * 0.82f};
                const art::Rgb lit{std::min(1.0f, ink.r + (1.0f - ink.r) * kBevelLight), std::min(1.0f, ink.g + (1.0f - ink.g) * kBevelLight),
                                   std::min(1.0f, ink.b + (1.0f - ink.b) * kBevelLight)};
                const art::Rgb dark{base.r * (1.0f - kBevelShadow), base.g * (1.0f - kBevelShadow), base.b * (1.0f - kBevelShadow)};
                art::composite(w.img, cov.data(), base, 1.0f);
                art::composite(w.img, shadow.data(), dark, 1.0f);
                art::composite(w.img, light.data(), lit, 1.0f);
                for (int i = 0; i < kCellTexels; ++i)
                    if (cov[static_cast<size_t>(i)] > 0.0f)
                        w.dirty[static_cast<size_t>(i)] = true;
                return true;
            }
            std::array<float, kCellTexels> halo{};
            for (int y = 0; y < kCellSize; ++y)
            {
                for (int x = 0; x < kCellSize; ++x)
                {
                    float m = 0.0f;
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int xx = x + dx, yy = y + dy;
                            if (xx < 0 || yy < 0 || xx >= kCellSize || yy >= kCellSize)
                                continue;
                            const float weight = (dx && dy) ? 0.6f : 1.0f;
                            m = std::max(m, cov[static_cast<size_t>(yy * kCellSize + xx)] * weight);
                        }
                    }
                    halo[static_cast<size_t>(y * kCellSize + x)] = m;
                }
            }
            art::composite(w.img, halo.data(), {0.0f, 0.0f, 0.0f}, kHaloAlpha);
            art::composite(w.img, cov.data(), ink, 1.0f);
            for (int i = 0; i < kCellTexels; ++i)
                if (halo[static_cast<size_t>(i)] > 0.0f || cov[static_cast<size_t>(i)] > 0.0f)
                    w.dirty[static_cast<size_t>(i)] = true;
            return true;
        }

        void clearRoundCap(CellWork &w, const uint8_t *rawPal)
        {
            std::array<bool, kCellTexels> mask{};
            for (int y = 0; y < kCellSize; ++y)
            {
                for (int x = 0; x < kCellSize; ++x)
                {
                    const size_t i = static_cast<size_t>(y * kCellSize + x);
                    const float r = std::hypot(static_cast<float>(x) - 15.5f, static_cast<float>(y) - 15.5f);
                    mask[i] = r <= kRoundClearRadius || (r <= kRoundColourRadius && chroma(rawPal, w.index[i]) >= kChromaSymbol);
                }
            }
            inpaint(w.img, mask);
            for (int i = 0; i < kCellTexels; ++i)
                w.dirty[static_cast<size_t>(i)] = w.dirty[static_cast<size_t>(i)] || mask[static_cast<size_t>(i)];
        }

        void clearRoundedCap(CellWork &w)
        {
            std::array<bool, kCellTexels> mask{};
            for (int y = kRoundedClear[1]; y <= kRoundedClear[3]; ++y)
                for (int x = kRoundedClear[0]; x <= kRoundedClear[2]; ++x)
                    mask[static_cast<size_t>(y * kCellSize + x)] = true;
            inpaint(w.img, mask);
            for (int i = 0; i < kCellTexels; ++i)
                w.dirty[static_cast<size_t>(i)] = w.dirty[static_cast<size_t>(i)] || mask[static_cast<size_t>(i)];
        }

        // The icon sheet's own white lettering (the START/SELECT words): opaque, neutral, >= 0xC8. The cap cells top
        // out at 0xAD (face buttons) / 0xC3 (R1...): limited to their own entries a "white" label came out mid grey, and
        // on the □ and × caps it matched the symbol's light pink / light blue (offline preview, 2026-10-18).
        void addInk(IndexSet &allowed, const uint8_t *sheet, const uint8_t *rawPal)
        {
            for (const Icon icon : {Start, Select})
            {
                const Cell &c = kCells[icon];
                for (int y = 0; y < kCellSize; ++y)
                {
                    for (int x = 0; x < kCellSize; ++x)
                    {
                        const int v = sheet[(c.y + y) * kSheetSize + c.x + x];
                        const uint8_t *e = rawPal + v * 4;
                        if (e[3] >= 0x80u && chroma(rawPal, v) < 16 && std::min({e[0], e[1], e[2]}) >= 0xC8u)
                            allowed[static_cast<size_t>(v)] = true;
                    }
                }
            }
        }

        bool looksLikeIconSheet(const uint8_t *sheet)
        {
            // Every cap cell has transparent corners (index 0) and the rounded caps a black outline (index 1) on
            // their left edge; the stick and START/SELECT cells reach the edges.
            for (int icon = Circle; icon <= R3; ++icon)
            {
                const Cell &c = kCells[icon];
                for (const int corner : {0, kCellSize - 1})
                    if (sheet[(c.y + corner) * kSheetSize + c.x] != 0u || sheet[(c.y + corner) * kSheetSize + c.x + kCellSize - 1] != 0u)
                        return false;
                if (c.kind == CellKind::RoundedCap && sheet[(c.y + 16) * kSheetSize + c.x] != 1u)
                    return false;
            }
            return true;
        }

        bool emptyStick(const std::string (&labels)[4])
        {
            return labels[0].empty() && labels[1].empty() && labels[2].empty() && labels[3].empty();
        }

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

    bool buildIconSheet(const uint8_t *origPayload, const uint8_t *clut, const PromptLabels &labels, uint8_t *outPayload,
                        uint8_t *outClut)
    {
        if (!origPayload || !clut || !outPayload || !outClut)
            return false;
        std::vector<uint8_t> sheet(kPayloadBytes);
        unswizzle(origPayload, sheet.data());
        if (!looksLikeIconSheet(sheet.data()))
            return false;
        bool yellow = false;
        for (int icon = 0; icon < IconCount; ++icon)
            yellow = yellow || (labels.tint[icon] == Tint::Yellow && !labels.icon[icon].empty());
        IndexSet keep{};
        for (int y = 0; y < kSheetSize; ++y)
            for (int x = 0; x < kSheetSize; ++x)
                if (x / kCellSize != kCells[Square].x / kCellSize || y / kCellSize != kCells[Square].y / kCellSize)
                    keep[sheet[y * kSheetSize + x]] = true;
        makeClut(clut, yellow, keep, outClut);
        std::array<uint8_t, 1024> rawPal{};
        paletteRgba(outClut, rawPal.data());
        const std::array<Rgba, 256> pal = premultiplied(outClut);
        const std::vector<uint8_t> original = sheet;

        IndexSet ink{};
        addInk(ink, original.data(), rawPal.data());

        // The cleared R1 cap, for START/SELECT.
        CellWork plate;
        loadCell(plate, original.data(), kCells[kPlateTemplate].x, kCells[kPlateTemplate].y, pal);
        clearRoundedCap(plate);

        for (int icon = 0; icon < IconCount; ++icon)
        {
            const Cell &cell = kCells[icon];
            CellWork w;
            switch (cell.kind)
            {
            case CellKind::RoundCap:
            case CellKind::RoundedCap:
            {
                if (labels.icon[icon].empty())
                    continue;
                loadCell(w, original.data(), cell.x, cell.y, pal);
                art::Rgb inkColour{1.0f, 1.0f, 1.0f};
                if (labels.tint[icon] != Tint::White)
                    addHue(w.allowed, rawPal.data(), hueOf(labels.tint[icon]), inkColour); // a coloured letter: its ramp
                else
                    for (int i = 0; i < 256; ++i)
                        w.allowed[static_cast<size_t>(i)] = w.allowed[static_cast<size_t>(i)] || ink[static_cast<size_t>(i)];
                if (cell.kind == CellKind::RoundCap)
                    clearRoundCap(w, rawPal.data());
                else
                    clearRoundedCap(w);
                const bool symbolLook = cell.kind == CellKind::RoundCap && labels.tint[icon] != Tint::White;
                if (!drawLabel(w, labels.icon[icon], symbolLook ? kSymbolBoxes : cell.kind == CellKind::RoundCap ? kRoundBoxes : kRoundedBoxes,
                               inkColour, symbolLook))
                    return false;
                break;
            }
            case CellKind::Plate:
            {
                if (labels.icon[icon].empty())
                    continue;
                loadCell(w, original.data(), cell.x, cell.y, pal);
                // the R1 cap (as cleared) replaces the lettering; its texels keep R1's quantized indices. Like R1 it
                // touches the cell edges (outline at columns 0/31), as the START/SELECT words did.
                for (int i = 0; i < 256; ++i)
                    w.allowed[static_cast<size_t>(i)] = w.allowed[static_cast<size_t>(i)] || plate.allowed[static_cast<size_t>(i)] || ink[static_cast<size_t>(i)];
                w.img = plate.img;
                for (size_t i = 0; i < static_cast<size_t>(kCellTexels); ++i)
                {
                    w.index[i] = plate.dirty[i] ? static_cast<uint8_t>(nearest(pal, plate.allowed, &plate.img.px[i * 4u])) : plate.index[i];
                    w.dirty[i] = false;
                }
                if (!drawLabel(w, labels.icon[icon], kRoundedBoxes))
                    return false;
                break;
            }
            case CellKind::Stick:
            {
                const auto &dirs = icon == LeftStick ? labels.leftStick : labels.rightStick;
                if (emptyStick(dirs))
                    continue;
                loadCell(w, original.data(), cell.x, cell.y, pal);
                std::fill(w.img.px.begin(), w.img.px.end(), 0.0f);
                if (!art::drawInvertedT(dirs, kStickArea[0], kStickArea[1], kStickArea[2], kStickArea[3], stickLook(), w.img))
                    return false;
                w.dirty.fill(true);
                break;
            }
            }
            storeCell(w, sheet.data(), cell.x, cell.y, pal);
        }

        swizzle(sheet.data(), outPayload);
        return true;
    }

    bool buildActionSheet(const uint8_t *origPayload, const uint8_t *clut, const PromptLabels &labels, uint8_t *outPayload,
                          uint8_t *outClut)
    {
        if (!origPayload || !clut || !outPayload || !outClut)
            return false;
        std::vector<uint8_t> sheet(kPayloadBytes);
        unswizzle(origPayload, sheet.data());
        bool yellow = false;
        for (const Shape &sh : kShapes)
            yellow = yellow || (labels.tint[sh.icon] == Tint::Yellow && !labels.icon[sh.icon].empty());
        IndexSet keep{};
        const Shape &square = kShapes[2];
        for (int y = 0; y < kSheetSize; ++y)
            for (int x = 0; x < kSheetSize; ++x)
                if (!(x >= square.cx0 && x < square.cx1 && y >= square.cy0 && y < square.cy1))
                    keep[sheet[y * kSheetSize + x]] = true;
        makeClut(clut, yellow, keep, outClut);
        std::array<uint8_t, 1024> rawPal{};
        paletteRgba(outClut, rawPal.data());
        const std::array<Rgba, 256> pal = premultiplied(outClut);
        // Sanity: each clear rect holds its shape (>= 60 opaque, saturated texels; the US sheet has 136-276).
        for (const Shape &s : kShapes)
        {
            int inked = 0;
            for (int y = s.cy0; y < s.cy1; ++y)
                for (int x = s.cx0; x < s.cx1; ++x)
                    inked += rawPal[sheet[y * kSheetSize + x] * 4u + 3u] >= 0x80u && chroma(rawPal.data(), sheet[y * kSheetSize + x]) >= 40;
            if (inked < 60)
                return false;
        }

        for (const Shape &s : kShapes)
        {
            const std::string &text = labels.icon[s.icon];
            if (text.empty())
                continue;
            const int w = s.cx1 - s.cx0, h = s.cy1 - s.cy0;
            // the shape's own colours (and transparent); the ink is the mean of its opaque texels
            IndexSet allowed{};
            allowed[0] = true;
            double sum[3] = {0, 0, 0};
            int count = 0;
            for (int y = s.cy0; y < s.cy1; ++y)
            {
                for (int x = s.cx0; x < s.cx1; ++x)
                {
                    const uint8_t v = sheet[y * kSheetSize + x];
                    allowed[v] = true;
                    const uint8_t *e = &rawPal[static_cast<size_t>(v) * 4u];
                    if (e[3] >= 0x80u)
                    {
                        for (int c = 0; c < 3; ++c)
                            sum[c] += e[c];
                        ++count;
                    }
                }
            }
            if (!count)
                continue;
            art::Rgb inkColour{static_cast<float>(sum[0] / count / 255.0), static_cast<float>(sum[1] / count / 255.0),
                               static_cast<float>(sum[2] / count / 255.0)};
            if (labels.tint[s.icon] != Tint::White)
                addHue(allowed, rawPal.data(), hueOf(labels.tint[s.icon]), inkColour); // an Xbox colour: that ramp instead
            art::TextBox box;
            box.cx = 0.5f * (s.u0 + s.u1) - static_cast<float>(s.cx0);
            box.cy = 0.5f * (s.v0 + s.v1) - static_cast<float>(s.cy0);
            // one character ~20 px caps (the shapes are 26-30 px outlines), lower for more
            const size_t glyphs = glyphCount(text);
            box.width = (s.u1 - s.u0) - (glyphs == 1u ? 6.0f : 3.0f);
            box.capHeight = (glyphs == 1u ? 0.72f : glyphs == 2u ? 0.55f : 0.42f) * (s.v1 - s.v0);
            box.weight = art::Weight::Bold;
            art::Image img(w, h);
            std::vector<float> cov(static_cast<size_t>(w) * static_cast<size_t>(h));
            if (art::textCoverage(text, box, w, h, cov.data()) <= 0.0f)
                return false;
            art::composite(img, cov.data(), inkColour, 1.0f);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    sheet[(s.cy0 + y) * kSheetSize + s.cx0 + x] =
                        static_cast<uint8_t>(nearest(pal, allowed, &img.px[static_cast<size_t>(y * w + x) * 4u]));
        }

        swizzle(sheet.data(), outPayload);
        return true;
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
