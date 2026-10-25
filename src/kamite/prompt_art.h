#pragma once

// PC key-cap art for the PS2 button prompts: labels for the bound keys and controller buttons, text drawn as coverage,
// and small key caps. Port of Tokyo Jungle's pad_prompt_art (cr/work/candidate/ps3recomp/libs/input/pad_prompt_art.c),
// re-keyed from Windows virtual keys to the raylib KEY_* / MOUSE_BUTTON_* codes of controls.ini (ps2_host_input.cpp
// kKeyNames) and to ps2_host_gamepads' SDL3 button/axis numbers. button_prompt_sheets.cpp turns this art into the
// game's own button sheets.
//
// GDI only (Windows fonts, no assets). Every call creates and frees its own GDI objects and keeps no state, so any
// thread may call it (the sheets are built on a worker thread). Shapes and text are drawn white-on-black into a 4x
// supersampled DIB and box-filtered to coverage; colours are composited in software (premultiplied "over").

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kamite::prompt_art
{
    // UTF-8 label pair. `compact` has at most 4 characters and is what a 32-texel prompt cell shows ("Spc", "Ctrl",
    // "Bksp", "M1", "F12"). The arrow keys are U+2190..U+2193 (drawn as vector arrows), unbound is U+2014.
    struct Label
    {
        std::string full;
        std::string compact;
    };

    inline constexpr std::string_view kUnbound = "\xE2\x80\x94";
    inline constexpr std::string_view kArrowLeft = "\xE2\x86\x90";
    inline constexpr std::string_view kArrowUp = "\xE2\x86\x91";
    inline constexpr std::string_view kArrowRight = "\xE2\x86\x92";
    inline constexpr std::string_view kArrowDown = "\xE2\x86\x93";

    // ps2_host_input's either-side modifier codes ("Keyboard/Shift", "Control", "Alt" as PCSX2 writes them).
    inline constexpr int kAnyShift = -1;
    inline constexpr int kAnyControl = -2;
    inline constexpr int kAnyAlt = -3;

    // raylib KeyboardKey (raylib.h), or kAnyShift/kAnyControl/kAnyAlt. 0 = unbound.
    [[nodiscard]] Label keyLabel(int key);
    // raylib MouseButton: 0 left, 1 right, 2 middle, 3 side, 4 extra, 5 forward, 6 back -> "M1".."M7".
    [[nodiscard]] Label mouseLabel(int button);
    // Xbox-style names (GLFW and SDL report every controller with that layout; ps2_host_input.cpp kPadButtonDisplay).
    // button = ps2_host_gamepads::Button, axis = ps2_host_gamepads::Axis, sign -1/+1.
    [[nodiscard]] Label padButtonLabel(int button);
    [[nodiscard]] Label padAxisLabel(int axis, int sign);

    // ---- drawing ----

    enum class Weight : uint8_t
    {
        Semibold,
        Bold
    };

    struct TextBox
    {
        float cx = 0.0f;        // centre, output pixels
        float cy = 0.0f;
        float width = 0.0f;     // widest the ink may be
        float capHeight = 0.0f; // tallest the capitals may be
        Weight weight = Weight::Semibold;
        // Whole-pixel cap height, baseline and left ink edge: crisp horizontal strokes when the texture is upscaled.
        bool snap = true;
    };

    // Coverage (0..1, row-major w*h, overwritten) of `text` centred in `box`, as large as fits. Any label uses
    // Bahnschrift SemiCondensed when that draws it at least 4% larger than Segoe UI (in practice labels of three or
    // more characters and wide letters such as W or M). Arrows are vector arrows;
    // "M1".."M3" become a mouse with that button filled when the box is at least 10 px tall. Returns the cap height used
    // in pixels, 0 when nothing was drawn (empty text, or GDI could not create its surfaces).
    float textCoverage(std::string_view text, const TextBox &box, int w, int h, float *coverage);
    // The cap height textCoverage would use, without drawing (to give several labels one size).
    [[nodiscard]] float textCapHeight(std::string_view text, const TextBox &box);

    // Premultiplied RGBA, 0..1, row-major.
    struct Image
    {
        int w = 0;
        int h = 0;
        std::vector<float> px; // w*h*4

        Image() = default;
        Image(int width, int height) : w(width), h(height), px(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0.0f) {}
    };

    struct Rgb
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
    };

    // "Over" with a coverage mask: dst = colour * cov * alpha + dst * (1 - cov * alpha).
    void composite(Image &dst, const float *coverage, Rgb colour, float alpha);

    // Key caps in the look of the game's button sheet (texture 0x18 of core_usa.bin): black outline, grey face lit
    // from the top, white label.
    struct CapLook
    {
        Rgb outline{0.0f, 0.0f, 0.0f};
        Rgb faceTop{0.62f, 0.62f, 0.62f};
        Rgb faceBottom{0.40f, 0.40f, 0.40f};
        Rgb rim{0.80f, 0.80f, 0.80f}; // the bright inner edge along the top and left
        Rgb label{1.0f, 1.0f, 1.0f};
        float outlineWidth = 1.0f;
        Weight labelWeight = Weight::Semibold;
    };

    struct Cap
    {
        float x0 = 0.0f; // outer rect including the outline, output pixels
        float y0 = 0.0f;
        float x1 = 0.0f;
        float y1 = 0.0f;
        float radius = 2.0f;
        std::string text;        // compact label
        float labelWidth = 0.9f; // label box as fractions of the face (inside the outline)
        float labelCap = 0.62f;
        float labelCapPx = 0.0f; // > 0: this cap height in pixels instead of labelCap
    };

    // Draws the caps over `dst`. False when GDI failed (dst may be partly drawn).
    bool drawCaps(const Cap *caps, int count, const CapLook &look, Image &dst);

    // Four caps as the keyboard's inverted T (up on top; left, down, right below) inside x0..x1, y0..y1, all four labels
    // at one size (the largest the widest of them allows). labels: up, right, down, left (controls.ini's LUp LRight
    // LDown LLeft order).
    bool drawInvertedT(const std::string (&labels)[4], float x0, float y0, float x1, float y1, const CapLook &look, Image &dst);
}
