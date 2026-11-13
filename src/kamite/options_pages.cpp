#include "kamite/options_pages.h"

#include "kamite/ini_file.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace kamite::options_pages
{
    std::string upper(const char *text)
    {
        std::string out = text ? text : "";
        for (char &c : out)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return out;
    }

    // The shared description as the help box shows it: two lines at most, wrapped to the text area's width (the
    // help font at its size takes about 7.9 layout units a letter, measured on a preview; 8.3 leaves room). Longer ones end in "..." until K's pass over
    // the texts (build step 6).
    std::string wrapHelp(const char *text, float aspect)
    {
        const float area = 384.0f * std::min(aspect, 21.0f / 9.0f);
        const size_t width = static_cast<size_t>(std::max(30.0f, (area - 58.0f) / 8.3f));
        std::vector<std::string> lines(1);
        std::string word;
        const auto place = [&]() {
            if (word.empty())
                return;
            std::string &line = lines.back();
            if (!line.empty() && line.size() + 1u + word.size() > width)
                lines.emplace_back();
            if (!lines.back().empty())
                lines.back() += ' ';
            lines.back() += word;
            word.clear();
        };
        for (const char *c = text; c && *c; ++c)
        {
            if (*c == ' ')
                place();
            else if (*c == '\n')
            {
                place();
                lines.emplace_back();
            }
            else
                word += *c;
        }
        place();
        if (lines.size() > 2u)
        {
            lines.resize(2u);
            std::string &last = lines[1];
            while (last.size() + 3u > width && last.find(' ') != std::string::npos)
                last.erase(last.rfind(' '));
            last += "...";
        }
        return lines.size() > 1u ? lines[0] + "\n" + lines[1] : lines[0];
    }

    // Numbers in a choice's values (the camera's), and the value as GHR.ini holds it.
    float number(const std::string &text) { return static_cast<float>(std::atof(text.c_str())); }
    void buildPage(PageData &d, settings::Tab tab, const IniFile &ini, float aspect)
    {
        d = PageData{};
        d.name = upper(settings::tabName(tab));
        const char *category = nullptr;
        for (size_t i = 0; i < settings::kSettingCount; ++i)
        {
            const settings::Setting &s = settings::kSettings[i];
            if (s.tab != tab || !s.live || (s.wideOnly && aspect < 1.34f) || !settings::parentAllows(ini, s))
                continue;
            if (!category || std::strcmp(category, s.category) != 0)
            {
                category = s.category;
                options_overlay::PageRow header;
                header.name = d.keep(upper(category));
                header.header = true;
                d.rows.push_back(header);
                d.setting.push_back(nullptr);
            }
            options_overlay::PageRow row;
            row.name = d.keep(upper(s.name));
            row.help = d.keep(wrapHelp(s.description, aspect));
            row.sub = s.parent != nullptr;
            const std::string value = settings::value(ini, s);
            std::vector<const char *> choices;
            switch (s.kind)
            {
            case settings::Kind::Toggle:
                choices = {"ON", "OFF"};
                row.choice = settings::on(ini, s) ? 0 : 1;
                break;
            case settings::Kind::Choice:
                if (s.gameSlider)
                {
                    // The stops, with the value itself among them when it is between (a camera preset's 153 %).
                    std::vector<int> stops;
                    const int now = static_cast<int>(std::lround(number(value)));
                    for (int c = 0; c < s.choiceCount; ++c)
                        stops.push_back(std::atoi(s.choices[c].value));
                    if (std::find(stops.begin(), stops.end(), now) == stops.end())
                        stops.push_back(now);
                    std::sort(stops.begin(), stops.end());
                    d.valueLists.push_back(stops);
                    row.values = d.valueLists.back().data();
                    row.valueCount = static_cast<int>(stops.size());
                    row.choice = static_cast<int>(std::find(stops.begin(), stops.end(), now) - stops.begin());
                    row.unit = s.unit;
                    break;
                }
                for (int c = 0; c < s.choiceCount; ++c)
                    choices.push_back(s.choices[c].game);
                row.choice = settings::choiceIndex(s, value);
                if (row.choice < 0) // set by hand to something else: shown as it is
                {
                    choices.push_back(d.keep(upper(value.c_str())));
                    row.choice = static_cast<int>(choices.size()) - 1;
                }
                break;
            case settings::Kind::Slider:
            {
                const int now = std::atoi(value.c_str());
                int best = 0;
                for (int v = 1; v < s.valueCount; ++v)
                    if (std::abs(s.values[v] - now) < std::abs(s.values[best] - now))
                        best = v;
                row.values = s.values;
                row.valueCount = s.valueCount;
                row.choice = best;
                row.unit = s.unit;
                break;
            }
            case settings::Kind::Custom: // the launcher's own (never live)
                break;
            }
            if (!choices.empty())
            {
                d.choiceLists.push_back(std::move(choices));
                row.choices = d.choiceLists.back().data();
                row.choiceCount = static_cast<int>(d.choiceLists.back().size());
            }
            d.rows.push_back(row);
            d.setting.push_back(&s);
        }
        d.page.name = d.name.c_str();
        d.page.rows = d.rows.data();
        d.page.rowCount = static_cast<int>(d.rows.size());
    }

    // The page's first / next / previous option (headers are skipped); -1 when there is none that way.
    int firstOption(const PageData &d)
    {
        for (size_t i = 0; i < d.rows.size(); ++i)
            if (!d.rows[i].header)
                return static_cast<int>(i);
        return -1;
    }
    int nextOption(const PageData &d, int from, int step)
    {
        for (int i = from + step; i >= 0 && i < static_cast<int>(d.rows.size()); i += step)
            if (!d.rows[static_cast<size_t>(i)].header)
                return i;
        return -1;
    }
}
