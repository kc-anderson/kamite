#pragma once

// CONTROLS > BUTTON MAPPING: what the page shows (options_overlay::MappingView), without game state, so the preview tool
// (tools/options/overlay_preview.py --page mapping) builds the page the game shows. The design, the original
// controller page's measurements and the stages: claude/button-mapping.md.
//
// The page is positional: each line is a physical button of the controller (its place on the default layout: South is
// where the PS2 cross is). Its label is the play action of the PS2 button bound there (controls.ini, as the prompts
// read it: the first controller binding), then the menus' word for that place (Menu Controls: the menus are pinned
// to physical buttons and never move with the mapping).

#include "kamite/options_overlay.h"

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace kamite
{
    class IniFile;
}

namespace kamite::mapping_page
{
    // The places. The first ten are the remappable ones, in ps2_host_input::prompts::Input's order (place i is where
    // that PS2 button sits on the default layout).
    enum Place : uint8_t
    {
        South, East, West, North, L1, R1, L2, R2, L3, R3, // remappable
        Start, Select, DPad, LStick, RStick,
        PlaceCount
    };
    constexpr int kRemappable = 10;
    constexpr uint8_t kNone = 0xFFu;
    const char *placeName(Place place); // "south", "l1"... (the data files' names)

    // A pad's picture and where its buttons are (resources/options/mapping/pad_<name>.txt, tools/options/make_pad_art.py).
    struct PadArt
    {
        std::string image, flames; // file names beside the data file
        float width = 0.0f, height = 0.0f, flamesMargin = 0.0f; // the render's pixels
        struct Point
        {
            float x = 0.0f, y = 0.0f, reach = 0.0f;
            float viaX = 0.0f, viaY = 0.0f; // a bend on the line's way (via), when hasVia
            bool set = false, hasVia = false;
        };
        std::array<Point, PlaceCount> points{};
        std::vector<uint8_t> left, right; // the columns' places, top to bottom
        std::vector<float> silhouette;    // left, right per band
        bool ok = false;
    };
    bool loadArt(const std::string &path, PadArt &out, std::string &error);
    // The picture a prompt set's pad gets (prompt_hd's folders: "xbox360", "ps3", "switch", "steamdeck"): "xbox",
    // "playstation", "nintendo" (the Steam sets get the Xbox pad until their own renders: stage 4).
    const char *padFor(const std::string &setFolder);

    // What the labels depend on besides the mapping.
    struct Options
    {
        bool clickStickDash = false; // [Game] L3Dash: L3 dashes
        bool modern = false;         // [Game] ControlStyle=modern: R3 switches brawling / free camera, the right stick
        bool presets = false;        // [Game] StyleSwitching: the d-pad switches styles
        int menuStyle = 1;           // 0 Classic, 1 Western, 2 Japanese (menu_buttons::Style)
    };
    Options optionsFrom(const IniFile &ini);

    struct State
    {
        // The PS2 button (prompts::Input 0..9) whose first controller binding is each remappable place; kNone: none.
        std::array<uint8_t, kRemappable> onPlace{};
        std::array<std::string, kRemappable> hot{}; // a hotkey's name on that button ("" none)
        bool readOnly = false; // a layout the page cannot edit (set in the launcher)
    };
    State defaultState();

    // The page for the overlay; the strings and lines live here.
    struct PageData
    {
        std::deque<std::string> text;
        std::vector<options_overlay::MappingLine> lines;
        std::vector<uint8_t> places; // each line's place
        std::vector<float> silhouette;
        options_overlay::MappingView view;

        const char *keep(std::string value)
        {
            text.push_back(std::move(value));
            return text.back().c_str();
        }
    };
    // root: the folder resources/ is in (the exe's). setFolder: the prompt set's folder (prompt_hd). cursor: a place,
    // kNone for none.
    void build(PageData &d, const PadArt &art, const std::string &artDir, const std::string &root, const std::string &setFolder,
               const State &state, const Options &options, uint8_t cursor);
    // The cursor's next place among the remappable lines: up / down within its column, left / right across to the line
    // nearest its height. The same place when there is none that way.
    uint8_t move(const PageData &d, uint8_t cursor, int dx, int dy);
    // The first remappable line (top of the left column).
    uint8_t firstLine(const PageData &d);

    constexpr const char *kReadOnlyNotice = "This layout was set in the launcher. Change it there, or press Reset.";
}
