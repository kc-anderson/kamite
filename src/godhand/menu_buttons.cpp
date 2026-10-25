// Menu buttons: Classic (PS2) or Modern (south confirms, east cancels). See menu_buttons.h.
//
// How the game reads buttons (SLUS_215.03): func_0031AFE0 turns the pad's 16 PS2 button bits into a 64-bit command
// word (D_007474A0+0x1A0 held, +0x1B0 pressed) through a 64-entry table at 0x3C3D50 (0x3C3E50 for the second
// button configuration): bit 24 = triangle, 25 = cross, 26 = circle, 27 = square, 28-35 the d-pad, 36-43 the rest.
// Menus and dialogs test those bits (MenuInput_stepPad, the choice strings "[circle] Yes [triangle] No"), and in
// play circle and triangle are two of the four attack slots. So the swap is made once, at the pad, for everything:
// the game's own prompts stay right once the sheets show the swapped cells, and a move assigned to "triangle" is
// shown with and thrown by the same button.
#include "godhand/menu_buttons.h"
#include "godhand/log.h"
#include "ps2x/iop/iop_pad.h"

#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

namespace godhand::menu_buttons
{
    namespace
    {
        std::atomic<bool> g_modern{false};

        // Glyph codes of the game's text (core_usa.bin, 16-bit).
        constexpr uint16_t kEnd = 0x8001u, kSpace = 0x10u;
        constexpr uint16_t kCircle = 0u, kTriangle = 1u, kCross = 3u, kR1 = 5u;
        uint16_t glyph(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return static_cast<uint16_t>(0x40 + (c - 'A'));
            if (c >= 'a' && c <= 'z')
                return static_cast<uint16_t>(0x5A + (c - 'a'));
            if (c >= '0' && c <= '9')
                return static_cast<uint16_t>(0x20 + (c - '0'));
            return kSpace;
        }
        std::vector<uint16_t> text(std::initializer_list<uint16_t> icons, const char *word = nullptr)
        {
            std::vector<uint16_t> out(icons);
            for (const char *c = word; c && *c; ++c)
                out.push_back(glyph(*c));
            return out;
        }
        std::vector<uint16_t> cat(std::initializer_list<std::vector<uint16_t>> parts)
        {
            std::vector<uint16_t> out;
            for (const auto &p : parts)
                out.insert(out.end(), p.begin(), p.end());
            return out;
        }
    }

    void install()
    {
        const char *value = std::getenv("GODHAND_MENU_BUTTONS");
        std::string text = value ? value : "classic";
        for (char &c : text)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        g_modern.store(text == "modern" || text == "1", std::memory_order_relaxed);
        GH_INFO(Game, "menu buttons: " << (g_modern.load() ? "modern (the south button confirms, the east one cancels: circle and triangle traded)"
                                                            : "classic (circle and cross confirm, triangle cancels, as on the PS2)"));
    }

    bool modern()
    {
        return g_modern.load(std::memory_order_relaxed);
    }

    void swapPad(ps2x::iop::PadInputState &state)
    {
        if (!g_modern.load(std::memory_order_relaxed))
            return;
        namespace PadButton = ps2x::iop::PadButton;
        using ps2x::iop::PadPressure;
        const uint16_t circle = state.buttons & PadButton::Circle, triangle = state.buttons & PadButton::Triangle;
        state.buttons = static_cast<uint16_t>((state.buttons & ~(PadButton::Circle | PadButton::Triangle)) |
                                              (circle ? PadButton::Triangle : 0u) | (triangle ? PadButton::Circle : 0u));
        std::swap(state.pressure[static_cast<size_t>(PadPressure::Circle)], state.pressure[static_cast<size_t>(PadPressure::Triangle)]);
    }

    const std::vector<TextPatch> &textPatches()
    {
        // Offsets in core_usa.bin (US), found by their icon codes (Oct 2026); each patch keeps its length.
        static const std::vector<TextPatch> patches = {
            // The roulette's bar: "[R1]Select [cross][circle]OK [triangle]Cancel" (20 codes) ->
            // "[R1]Cycle  [cross]OK  [triangle]Cancel": one button per action, the gaps where the second icon was.
            {0x591CCu,
             cat({text({kR1}, "Select"), text({kSpace, kCross, kCircle}, "OK"), text({kSpace, kTriangle}, "Cancel")}),
             cat({text({kR1}, "Cycle"), text({kSpace, kSpace, kCross}, "OK"), text({kSpace, kSpace, kTriangle}, "Cancel")})},
            // "[cross][circle]Change" and "[cross][circle]Select": the circle becomes a space.
            {0x591F8u, text({kCross, kCircle}, "Change"), text({kCross, kSpace}, "Change")},
            {0x59238u, text({kCross, kCircle}, "Select"), text({kCross, kSpace}, "Select")},
            // The dialog choices "[circle] Of course / Yes" (the "[triangle] No" stays): the circle becomes the cross.
            {0x577A8u, {kCircle, 0xFFFFu, 0xFFFFu}, {kCross, 0xFFFFu, 0xFFFFu}},
            {0x57820u, {kCircle, 0xFFFFu, 0xFFFFu}, {kCross, 0xFFFFu, 0xFFFFu}},
            {0x57866u, {kCircle, 0xFFFFu, 0xFFFFu}, {kCross, 0xFFFFu, 0xFFFFu}},
        };
        return patches;
    }
}
