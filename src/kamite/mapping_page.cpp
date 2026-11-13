#include "kamite/mapping_page.h"

#include "kamite/ini_file.h"
#include "kamite/settings_table.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace kamite::mapping_page
{
    namespace
    {
        constexpr const char *kPlaceNames[PlaceCount] = {"south", "east", "west", "north", "l1", "r1", "l2", "r2",
                                                         "l3", "r3", "start", "select", "dpad", "lstick", "rstick"};

        // The original page's wording (claude/button-mapping.md), per PS2 button (prompts::Input 0..9).
        struct Words
        {
            const char *line[2];
        };
        constexpr Words kActions[kRemappable] = {
            {{"ATTACK", "THROW ITEM"}},   // cross
            {{"ACTION BUTTON", nullptr}}, // circle
            {{"COMBO", nullptr}},         // square
            {{"ATTACK", nullptr}},        // triangle
            {{"180\xC2\xB0 TURN", nullptr}},
            {{"OPEN", "ROULETTE WHEEL"}},
            {{"TAUNT", nullptr}},
            {{"UNLEASH", "THE GODHAND"}},
            {{"DASH", nullptr}},          // L3: with Click-Stick Dash
            {{"BRAWL / FREECAM", nullptr}}, // R3: with Modern
        };
        constexpr const char *kEmpty = "\xE2\x80\x94"; // an em dash: nothing on this button

        // Xelu's pack per set (resources/prompts/xelu/<folder>), by place; the sticks are ours (no click arrow).
        struct SetFiles
        {
            const char *folder;
            const char *file[13]; // South .. DPad
        };
        constexpr SetFiles kSets[] = {
            {"xbox360", {"360_A", "360_B", "360_X", "360_Y", "360_LB", "360_RB", "360_LT", "360_RT", "360_Left_Stick_Click",
                         "360_Right_Stick_Click", "360_Start_Alt", "360_Back_Alt", "360_Dpad"}},
            {"ps3", {"PS3_Cross", "PS3_Circle", "PS3_Square", "PS3_Triangle", "PS3_L1", "PS3_R1", "PS3_L2", "PS3_R2",
                     "PS3_Left_Stick_Click", "PS3_Right_Stick_Click", "PS3_Start", "PS3_Select", "PS3_Dpad"}},
            // Nintendo's letters by place: B at the bottom, A on the right, Y on the left, X at the top.
            {"switch", {"Switch_B", "Switch_A", "Switch_Y", "Switch_X", "Switch_LB", "Switch_RB", "Switch_LT", "Switch_RT",
                        "Switch_Left_Stick_Click", "Switch_Right_Stick_Click", "Switch_Plus", "Switch_Minus", "Switch_Dpad"}},
            {"steamdeck", {"SteamDeck_A", "SteamDeck_B", "SteamDeck_X", "SteamDeck_Y", "SteamDeck_L1", "SteamDeck_R1",
                           "SteamDeck_L2", "SteamDeck_R2", "SteamDeck_Left_Stick_Click", "SteamDeck_Right_Stick_Click",
                           "SteamDeck_Menu", "SteamDeck_Inventory", "SteamDeck_Dpad"}},
        };
        const SetFiles &setFiles(const std::string &folder)
        {
            for (const SetFiles &s : kSets)
                if (folder == s.folder)
                    return s;
            return kSets[0];
        }

        int placeOf(const std::string &name)
        {
            for (int i = 0; i < PlaceCount; ++i)
                if (name == kPlaceNames[i])
                    return i;
            return -1;
        }
    }

    const char *placeName(Place place) { return place < PlaceCount ? kPlaceNames[place] : ""; }

    const char *padFor(const std::string &setFolder)
    {
        if (setFolder == "ps3")
            return "playstation";
        if (setFolder == "switch")
            return "nintendo";
        return "xbox";
    }

    bool loadArt(const std::string &path, PadArt &out, std::string &error)
    {
        out = PadArt{};
        std::ifstream file(path);
        if (!file)
        {
            error = "cannot open " + path;
            return false;
        }
        std::string line;
        while (std::getline(file, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            std::istringstream in(line);
            std::string word;
            if (!(in >> word) || word[0] == '#')
                continue;
            if (word == "image")
                in >> out.image >> out.width >> out.height;
            else if (word == "flames")
                in >> out.flames >> out.flamesMargin;
            else if (word == "point")
            {
                std::string name;
                PadArt::Point p;
                in >> name >> p.x >> p.y >> p.reach;
                const int place = placeOf(name);
                if (place < 0 || in.fail())
                {
                    error = "bad point line: " + line;
                    return false;
                }
                p.set = true;
                out.points[static_cast<size_t>(place)] = p;
            }
            else if (word == "via")
            {
                std::string name;
                float x = 0.0f, y = 0.0f;
                in >> name >> x >> y;
                const int place = placeOf(name);
                if (place < 0 || in.fail())
                {
                    error = "bad via line: " + line;
                    return false;
                }
                PadArt::Point &p = out.points[static_cast<size_t>(place)];
                p.viaX = x;
                p.viaY = y;
                p.hasVia = true;
            }
            else if (word == "left" || word == "right")
            {
                std::vector<uint8_t> &column = word == "left" ? out.left : out.right;
                std::string name;
                while (in >> name)
                {
                    const int place = placeOf(name);
                    if (place < 0)
                    {
                        error = "unknown button " + name;
                        return false;
                    }
                    column.push_back(static_cast<uint8_t>(place));
                }
            }
            else if (word == "silhouette")
            {
                int bands = 0;
                in >> bands;
                float v = 0.0f;
                while (in >> v)
                    out.silhouette.push_back(v);
                if (bands <= 0 || out.silhouette.size() != static_cast<size_t>(bands) * 2u)
                    out.silhouette.clear();
            }
        }
        if (out.image.empty() || out.width <= 0.0f || out.height <= 0.0f)
        {
            error = "no image line in " + path;
            return false;
        }
        out.ok = true;
        return true;
    }

    Options optionsFrom(const IniFile &ini)
    {
        Options o;
        const auto on = [&](const char *id) {
            const settings::Setting *s = settings::find(id);
            return s && settings::on(ini, *s);
        };
        o.clickStickDash = on("L3Dash");
        o.presets = on("StyleSwitching");
        if (const settings::Setting *s = settings::find("ControlStyle"))
            o.modern = settings::value(ini, *s) == "modern";
        if (const settings::Setting *s = settings::find("MenuButtons"))
        {
            const std::string v = settings::value(ini, *s);
            o.menuStyle = v == "classic" ? 0 : v == "japanese" ? 2 : 1;
        }
        return o;
    }

    State defaultState()
    {
        State s;
        for (int i = 0; i < kRemappable; ++i)
            s.onPlace[static_cast<size_t>(i)] = static_cast<uint8_t>(i);
        return s;
    }

    void build(PageData &d, const PadArt &art, const std::string &artDir, const std::string &root, const std::string &setFolder,
               const State &state, const Options &options, uint8_t cursor)
    {
        d.text.clear();
        d.lines.clear();
        d.places.clear();
        d.view = options_overlay::MappingView{};
        if (!art.ok)
            return;
        const SetFiles &set = setFiles(setFolder);
        const std::string icons = root + "/resources/prompts/xelu/" + set.folder + "/";
        d.view.pad = d.keep(artDir + "/" + art.image);
        d.view.flames = art.flames.empty() ? "" : d.keep(artDir + "/" + art.flames);
        d.view.leader = d.keep(artDir + "/leader.png");
        d.view.leaderLit = d.keep(artDir + "/leader_lit.png");
        d.view.underscore = d.keep(artDir + "/underscore.png");
        d.view.underscoreLit = d.keep(artDir + "/underscore_lit.png");
        d.view.panel = d.keep(artDir + "/panel.png");
        d.view.padAspect = art.width / art.height;
        d.view.flamesMargin = art.flamesMargin / art.height;
        d.silhouette = art.silhouette;
        d.view.silhouette = d.silhouette.empty() ? nullptr : d.silhouette.data();
        d.view.bands = static_cast<int>(d.silhouette.size() / 2u);

        // The menus' words by place (menu_buttons: Western south confirms, east cancels; Japanese the other way round;
        // Classic as the US release: south and east confirm, north cancels).
        const auto menuWord = [&](uint8_t place) -> const char * {
            switch (options.menuStyle)
            {
            case 0: return place == South || place == East ? "CONFIRM" : place == North ? "CANCEL" : nullptr;
            case 2: return place == East ? "CONFIRM" : place == South ? "CANCEL" : nullptr;
            default: return place == South ? "CONFIRM" : place == East ? "CANCEL" : nullptr;
            }
        };

        for (int side = 0; side < 2; ++side)
            for (const uint8_t place : side == 0 ? art.left : art.right)
            {
                const PadArt::Point &p = art.points[place];
                if (!p.set)
                    continue;
                options_overlay::MappingLine line;
                line.px = p.x / art.width;
                line.py = p.y / art.height;
                line.reach = p.reach / art.height;
                if (p.hasVia)
                {
                    line.via = true;
                    line.vx = p.viaX / art.width;
                    line.vy = p.viaY / art.height;
                }
                line.right = side == 1;
                const auto add = [&](const char *text) {
                    if (text && line.count < 3)
                        line.text[line.count++] = text;
                };
                if (place < kRemappable)
                {
                    const uint8_t input = state.onPlace[place];
                    const bool shown = input < kRemappable && !(input == L3 && !options.clickStickDash) && !(input == R3 && !options.modern);
                    if (shown)
                    {
                        add(kActions[input].line[0]);
                        add(kActions[input].line[1]);
                    }
                    else
                        add(kEmpty);
                    if (const char *word = menuWord(place))
                        add(word);
                    if (!state.hot[place].empty())
                        line.hot = d.keep("HOT: " + state.hot[place]);
                }
                else if (place == Start)
                    add("MENU");
                else if (place == LStick)
                    add("MOVE");
                else if (place == RStick)
                {
                    if (options.modern)
                    {
                        add("EVADE (BRAWLING)");
                        add("CAMERA (FREECAM)");
                    }
                    else
                        add("EVADE");
                }
                else if (place == DPad && options.presets)
                    add("STYLE SWITCH");
                line.fixed = place >= kRemappable;
                if (line.count == 0)
                    continue; // nothing in play (Select; the d-pad without Moveset Presets): no line, as the original
                if (place == LStick || place == RStick)
                    line.icon = d.keep(artDir + "/stick_" + set.folder + (place == LStick ? "_left.png" : "_right.png"));
                else
                    line.icon = d.keep(icons + set.file[place] + ".png");
                d.lines.push_back(line);
                d.places.push_back(place);
            }
        // The left column lines up with the right where the pad is symmetric (the original's L2 / R2, L1 / R1 and
        // left / right stick rows share their heights).
        for (size_t i = 0; i < d.lines.size(); ++i)
        {
            if (d.lines[i].right)
                continue;
            const uint8_t p = d.places[i];
            const uint8_t want = p == L2 ? uint8_t{R2} : p == L1 ? uint8_t{R1} : p == L3 ? uint8_t{R3} : p == LStick ? uint8_t{RStick} : kNone;
            for (size_t j = 0; j < d.lines.size(); ++j)
                if (d.lines[j].right && d.places[j] == want)
                    d.lines[i].mirror = static_cast<int>(j);
        }
        d.view.lines = d.lines.data();
        d.view.lineCount = static_cast<int>(d.lines.size());
        d.view.cursor = -1;
        if (!state.readOnly)
            for (size_t i = 0; i < d.places.size(); ++i)
                if (d.places[i] == cursor)
                    d.view.cursor = static_cast<int>(i);
        d.view.notice = state.readOnly ? kReadOnlyNotice : "";
    }

    uint8_t firstLine(const PageData &d)
    {
        for (const uint8_t place : d.places)
            if (place < kRemappable)
                return place;
        return kNone;
    }

    uint8_t move(const PageData &d, uint8_t cursor, int dx, int dy)
    {
        int at = -1;
        for (size_t i = 0; i < d.places.size(); ++i)
            if (d.places[i] == cursor)
                at = static_cast<int>(i);
        if (at < 0)
            return firstLine(d);
        const options_overlay::MappingLine &from = d.lines[static_cast<size_t>(at)];
        if (dy != 0)
        {
            for (int i = at + dy; i >= 0 && i < static_cast<int>(d.places.size()); i += dy)
                if (d.lines[static_cast<size_t>(i)].right == from.right && d.places[static_cast<size_t>(i)] < kRemappable)
                    return d.places[static_cast<size_t>(i)];
            return cursor;
        }
        if (dx != 0 && (dx > 0) != from.right)
        {
            // Across: the remappable line on the other side whose button is nearest in height.
            int best = -1;
            float bestDistance = 0.0f;
            for (size_t i = 0; i < d.places.size(); ++i)
            {
                if (d.lines[i].right == from.right || d.places[i] >= kRemappable)
                    continue;
                const float distance = std::fabs(d.lines[i].py - from.py);
                if (best < 0 || distance < bestDistance)
                {
                    best = static_cast<int>(i);
                    bestDistance = distance;
                }
            }
            if (best >= 0)
                return d.places[static_cast<size_t>(best)];
        }
        return cursor;
    }
}
