#pragma once

// The in-game OPTIONS screen's category pages, built from the shared settings list (settings_table.cpp) and GHR.ini:
// what options_overlay draws. No game state, so the preview tool (tools/options/overlay_preview.py) builds the same
// pages the game shows.

#include "kamite/options_overlay.h"
#include "kamite/settings_table.h"

#include <deque>
#include <string>
#include <vector>

namespace kamite
{
    class IniFile;
}

namespace kamite::options_pages
{
    // A category's page as the overlay draws it, rebuilt each frame from GHR.ini (sub-options come and go with
    // their parents). Strings live in deques so the rows' pointers stay put.
    struct PageData
    {
        std::deque<std::string> text;
        std::deque<std::vector<const char *>> choiceLists;
        std::deque<std::vector<int>> valueLists;
        std::vector<options_overlay::PageRow> rows;
        std::vector<const settings::Setting *> setting; // nullptr: a header (or the BUTTON MAPPING row)
        options_overlay::Page page;
        std::string name;
        int mappingRow = -1; // CONTROLS: the BUTTON MAPPING row (opens its page; no value)

        const char *keep(std::string value)
        {
            text.push_back(std::move(value));
            return text.back().c_str();
        }
    };

    // The live settings of a tab under their categories' headers (upper case), sub-options only while their parent
    // allows them, the 4:3-only ones left out at 4:3.
    void buildPage(PageData &d, settings::Tab tab, const IniFile &ini, float aspect);
    // The page's first option / the next one up (step -1) or down (+1), headers skipped; -1 when there is none.
    int firstOption(const PageData &d);
    int nextOption(const PageData &d, int from, int step);

    std::string upper(const char *text);
    // CONTROLS > BUTTON MAPPING: the row's help (in game only: the launcher keeps its own bindings table).
    constexpr const char *kMappingName = "BUTTON MAPPING";
    constexpr const char *kMappingHelp = "Swap what the buttons do in play.\nMenus keep their buttons (Menu Controls).";

    // A description as the help box shows it: two lines at most at the text area's width ("..." past that).
    std::string wrapHelp(const char *text, float aspect);
}
