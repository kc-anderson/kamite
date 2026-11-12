#pragma once

// The OPTIONS screen's text and cursor as a list for the renderer's overlay (pcsx2gs_set_screen). No game state here:
// options_screen.cpp fills a View each frame and sends what build() makes. The look of the game's own Options screen,
// measured from the disc (claude/rearmed-options.md "Style of the original"), in the layout's units: x in 512ths of a
// 4:3 picture (one unit = 7/6 of a line), y in its 448 lines.

#include <cstdint>
#include <string>
#include <vector>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_SCREEN_OVERLAY 1
#endif

namespace kamite::options_overlay
{
    struct Row
    {
        const char *name;
        const char *help; // two lines at most ('\n')
    };

    struct View
    {
        float aspect = 16.0f / 9.0f; // the picture's
        float fade = 1.0f;           // the screen's own fade (its backdrop's alpha)
        bool pause = true;           // the pause menu's screen (the title's has fewer rows, spaced wider)
        const Row *rows = nullptr;
        int rowCount = 0;
        int cursor = 0;
        int confirmRow = -1;         // a Yes / No beside this row (-1 = none)
        bool confirmYes = false;
        const char *question = "";   // the help box while asking
    };

    // Tunable in F2 (Display, "Options text") while K judges them in game.
    struct Look
    {
        float blur = 0.5f;         // softening, in lines (PS2 pixels)
        float shadowAlpha = 0.18f; // a barely visible drop shadow (the original has none)
        float shadowOffset = 1.0f; // lines
    };
    Look &look();

    // The fonts (resources/fonts), set once at start.
    void setFonts(const std::string &labelFont, const std::string &helpFont);

#if defined(KAMITE_HAS_SCREEN_OVERLAY)
    void build(const View &view, std::vector<Pcsx2GsScreenItem> &items, std::string &strings);
#endif
}
