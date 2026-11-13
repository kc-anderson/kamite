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
        bool sub = false;                     // a sub-option, joined to the option above by an elbow. The caller
                                              // lists a sub-option only while its parent is on (K)
        const char *const *choices = nullptr; // up to 3 are shown in a row; more as < value >
        int choiceCount = 0;
        int choice = 0;
        bool list = false;                    // < value > even with 3 or fewer (long words: Display Mode)
        // A slider instead (valueCount > 0): choice indexes these numbers, shown with the unit beside a bar drawn like
        // the enemies' health bar: the number's share of the last (the largest).
        const int *values = nullptr;
        int valueCount = 0;
        const char *unit = "%";
    };

    struct Page
    {
        const char *name = ""; // the category (its title replaces OPTIONS while the page is open)
        const PageRow *rows = nullptr;
        int rowCount = 0;
    };

    // A button hint in the strip under the help band ("[A] Confirm"): the button's images (PNG paths: one, or two for
    // Classic's two confirm buttons; null = none) and the word.
    struct Hint
    {
        const char *art[2] = {nullptr, nullptr};
        const char *word = "";
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
        // The menu buttons' hints, laid out from the strip's right end: confirm, then cancel (K, Nov 13: as the game's
        // load menu words them, in the help text's look, on the list, in pages and while asking Yes / No).
        Hint confirmHint, cancelHint;
        // While a page is open: L1 / R1 beside its title, each with the neighbouring category's name outside it
        // (K, Nov 13). A side with no neighbour (word "") shows nothing.
        Hint prevHint, nextHint;
    };

    // The fonts (resources/fonts) and the titles' folder (resources/options: title_options.png, title_<category>.png),
    // set once at start.
    void setAssets(const std::string &labelFont, const std::string &helpFont, const std::string &titleDir);
    // The scroll bar's pieces (resources/options/scroll_track.png, scroll_thumb.png: tools/options/make_scrollbar.py).
    void setScrollBar(const std::string &track, const std::string &thumb);
    // The sliders' fill (resources/options/slider_fill.png: tools/options/make_slider.py).
    void setSliderFill(const std::string &fill);
    // A title image's letters (title_<name>.png, name in lower case): their left and right pixel columns in its canvas,
    // measured once at start (the glow left out), so the L1 / R1 hints sit beside the letters whatever their width.
    void setTitleLetters(const std::string &name, float left, float right);
    // The rows a page shows at once (scrolling keeps the cursor among them).
    int pageRowsShown(const Page &page, int scroll);

#if defined(KAMITE_HAS_SCREEN_OVERLAY)
    void build(const View &view, std::vector<Pcsx2GsScreenItem> &items, std::string &strings);
    // Black over the whole picture at this strength (0..1): the fade behind an OPTIONS change.
    void appendBlackout(float alpha, std::vector<Pcsx2GsScreenItem> &items);
#endif
}
