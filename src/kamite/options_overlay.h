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

    // A category's page: its options under headers (the launcher's category names, which the cursor skips).
    struct PageRow
    {
        const char *name = "";
        const char *help = "";                // two lines at most
        bool header = false;
        bool sub = false;                     // a sub-option (shown only while its parent is on): indented
        const char *const *choices = nullptr; // up to 3 are shown in a row; more as < value >
        int choiceCount = 0;
        int choice = 0;
    };

    struct Page
    {
        const char *name = ""; // the category (its title replaces OPTIONS while the page is open)
        const PageRow *rows = nullptr;
        int rowCount = 0;
    };

    struct View
    {
        float aspect = 16.0f / 9.0f; // the picture's
        float fade = 1.0f;           // the screen's fades (its backdrop's alpha, the game's fade to and from black)
        bool pause = true;           // the pause menu's screen (the title's has fewer rows, spaced wider)
        const Row *rows = nullptr;
        int rowCount = 0;
        int cursor = 0;
        int confirmRow = -1;         // a Yes / No beside this row (-1 = none)
        bool confirmYes = false;
        const char *question = "";   // the help box while asking
        // The category under the list's cursor: on 16:10 and wider it is shown beside the list, joined to it by a
        // bracket while open; narrower, an open page replaces the list.
        const Page *page = nullptr;
        bool inPage = false; // the cursor is in the page
        int pageCursor = 0;  // its row
        int pageScroll = 0;  // the first row shown
    };

    // Tunable in F2 (Display, "Options text") while K judges them in game.
    struct Look
    {
        float blur = 0.5f;         // softening, in lines (PS2 pixels)
        float shadowAlpha = 0.18f; // a barely visible drop shadow (the original has none)
        float shadowOffset = 1.0f; // lines
        // Mockup switches while K picks (temporary): the bracket's look (0 the Techniques connector, 1 the cursor's
        // cream and red, 2 flat tan), and the page beside the list before it is opened (0 none, 1 as is, 2 dimmed).
        int bracket = 0;
        int preview = 1;
    };
    Look &look();

    // The fonts (resources/fonts) and the OPTIONS title (resources/options), set once at start.
    void setAssets(const std::string &labelFont, const std::string &helpFont, const std::string &titleImage);
    // The scroll bar's pieces (resources/options/scroll_track.png, scroll_thumb.png: tools/options/make_scrollbar.py).
    void setScrollBar(const std::string &track, const std::string &thumb);
    // The rows a page shows at once (scrolling keeps the cursor among them).
    int pageRowsShown(const Page &page, int scroll);

#if defined(KAMITE_HAS_SCREEN_OVERLAY)
    void build(const View &view, std::vector<Pcsx2GsScreenItem> &items, std::string &strings);
#endif
}
