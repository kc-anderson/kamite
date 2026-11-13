#include "kamite/settings_table.h"

#include "kamite/camera_presets.h"
#include "kamite/ini_file.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace kamite::settings
{
    namespace
    {
        // ---- choices --------------------------------------------------------------------------------------------
        constexpr Choice kMenuButtons[] = {{"western", "Western", "WESTERN"}, {"japanese", "Japanese", "JAPANESE"},
                                           {"classic", "Classic", "CLASSIC"}};
        constexpr Choice kButtonPrompts[] = {{"auto", "Automatic (follows your controller)", "AUTOMATIC"},
                                             {"xbox", "Xbox", "XBOX"},
                                             {"playstation", "PlayStation (the game's own)", "PLAYSTATION"},
                                             {"nintendo", "Nintendo Switch", "NINTENDO"},
                                             {"steam", "Steam Deck / Steam Controller", "STEAM DECK"}};
        constexpr Choice kDisplayModes[] = {{"fullscreen", "Fullscreen (Borderless)", "FULLSCREEN"},
                                            {"maximized", "Maximized Window", "MAXIMIZED"},
                                            {"window", "Window", "WINDOW"}};
        constexpr Choice kWindowSizes[] = {{"auto", "Automatic (follows internal resolution)", "AUTOMATIC"},
                                           {"native", "PS2 size", "PS2 SIZE"}};
        constexpr Choice kAspects[] = {{"4:3", "4:3 (the PS2's)", "4:3"},    {"16:9", "16:9", "16:9"},
                                       {"16:10", "16:10", "16:10"},         {"21:9", "21:9", "21:9"},
                                       {"32:9", "32:9", "32:9"},            {"auto", "Match the screen", "MATCH SCREEN"}};
        // Camera & FOV: camera_presets.h's presets in their order, then Custom (the four values as set).
        constexpr Choice kCameraPresetChoices[] = {{"ps2", "As on the PS2", "AS ON THE PS2"}, {"wider", "Wider", "WIDER"},
                                                   {"widest", "Widest", "WIDEST"},              {"action", "Action", "ACTION"},
                                                   {"custom", "Custom", "CUSTOM"}};
        static_assert(std::size(kCameraPresetChoices) == std::size(kCameraPresets) + 1);
        constexpr Choice kFovs[] = {{"100", "100%: as on the PS2", "100"}, {"110", "110%", "110"}, {"120", "120%", "120"},
                                    {"130", "130%", "130"},                {"140", "140%", "140"}, {"150", "150%", "150"}};
        constexpr Choice kCameraDistances[] = {{"0", "0%: Gene keeps his size", "0"},
                                               {"50", "50%: halfway out", "50"},
                                               {"100", "100%: wider view looks zoomed out", "100"}};
        constexpr Choice kCameraTilts[] = {{"0", "None", "0"}, {"5", "5 degrees", "5"}, {"10", "10 degrees", "10"},
                                           {"15", "15 degrees", "15"}};
        constexpr Choice kHudLayouts[] = {{"16:9", "16:9: HUD at the screen edges", "16:9"},
                                          {"4:3", "4:3: HUD where the PS2 had it", "4:3"}};
        constexpr Choice kMenuLayouts[] = {{"4:3", "4:3: as on the PS2, with side bars", "4:3"},
                                           {"stretch", "Stretched to the screen", "STRETCHED"}};
        constexpr Choice kGraphicsApis[] = {{"vulkan", "Vulkan", "VULKAN"}, {"dx11", "Direct3D 11", "DIRECT3D 11"}};
        constexpr Choice kResolutions[] = {{"1", "1x  native (512x448)", "1X"}, {"2", "2x  1024x896", "2X"},
                                           {"3", "3x  1536x1344", "3X"},        {"4", "4x  2048x1792", "4X"},
                                           {"5", "5x  2560x2240", "5X"},        {"6", "6x  3072x2688", "6X"},
                                           {"8", "8x  4096x3584", "8X"}};
        constexpr Choice kPostEffects[] = {{"ps2", "PS2 resolution (PCSX2's fix)", "PS2 RESOLUTION"},
                                           {"upscaled", "Upscaled", "UPSCALED"},
                                           {"off", "No fix", "NO FIX"}};
        constexpr Choice kControlStyles[] = {{"classic", "Classic", "CLASSIC"}, {"modern", "Modern", "MODERN"}};
        constexpr Choice kRouletteTimers[] = {{"normal", "Normal", "NORMAL"}, {"double", "Double time", "DOUBLE"}};
        constexpr Choice kSoundModes[] = {{"mono", "Mono", "MONO"}, {"stereo", "Stereo", "STEREO"},
                                          {"prologic2", "Dolby Pro Logic II", "PRO LOGIC II"}};
        // The volumes: 5 % steps, except 65 -> 69 -> 70 (K).
        constexpr int kVolumes[] = {0, 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60, 65, 69, 70, 75, 80, 85, 90, 95, 100};

        // ---- builders -------------------------------------------------------------------------------------------
        constexpr Setting base(const char *id, Tab tab, const char *category, const char *name, const char *description,
                               const char *section, const char *key, const char *fallback, Kind kind)
        {
            Setting s;
            s.id = id, s.tab = tab, s.category = category, s.name = name, s.description = description;
            s.section = section, s.key = key, s.fallback = fallback, s.kind = kind;
            return s;
        }
        constexpr Setting toggle(Tab tab, const char *category, const char *name, const char *description,
                                 const char *section, const char *key, bool on)
        {
            return base(key, tab, category, name, description, section, key, on ? "1" : "0", Kind::Toggle);
        }
        template <size_t N>
        constexpr Setting choice(Tab tab, const char *category, const char *name, const char *description,
                                 const char *section, const char *key, const char *fallback, const Choice (&choices)[N])
        {
            Setting s = base(key, tab, category, name, description, section, key, fallback, Kind::Choice);
            s.choices = choices, s.choiceCount = static_cast<int>(N);
            return s;
        }
        constexpr Setting volume(const char *name, const char *description, const char *key)
        {
            Setting s = base(key, Tab::Sound, "Volume", name, description, "Sound", key, "100", Kind::Slider);
            s.values = kVolumes, s.valueCount = static_cast<int>(std::size(kVolumes)), s.unit = "%";
            return s;
        }
        constexpr Setting custom(const char *id, Tab tab, const char *category, const char *name, const char *description)
        {
            return base(id, tab, category, name, description, "", "", "", Kind::Custom);
        }
        // Modifiers.
        constexpr Setting live(Setting s) { return s.live = true, s; }
        constexpr Setting note(Setting s, const char *text) { return s.note = text, s; }
        constexpr Setting gameNote(Setting s, const char *text) { return s.gameNote = text, s; }
        constexpr Setting sub(Setting s, const char *parent, const char *value = nullptr)
        {
            return s.parent = parent, s.parentValue = value, s;
        }
        constexpr Setting greyed(Setting s) { return s.greyed = true, s; }
        constexpr Setting onLeave(Setting s) { return s.applyOnLeave = true, s; }
        constexpr Setting withChoices(Setting s, const Choice *choices, int count, const char *fallback)
        {
            return s.choices = choices, s.choiceCount = count, s.fallback = fallback, s;
        }
        constexpr Setting wideOnly(Setting s) { return s.wideOnly = true, s; }
        constexpr Setting slider(Setting s, const char *unit) { return s.gameSlider = true, s.unit = unit, s; }

        constexpr const char *kDegrees = "\xC2\xB0";
    }

    // The order is the launcher's and the game's.
    const Setting kSettings[] = {
        // ---- Controls -------------------------------------------------------------------------------------------
        // Camera first (K, Nov 13: the camera's behaviour sits with the controls).
        live(note(choice(Tab::Controls, "Camera", "Camera & FOV",
                         "How far you see to the side.\nWorks with Aspect Ratio; enemies adapt.",
                         "Video", "CameraPreset", "ps2", kCameraPresetChoices),
                  "F2 while playing tunes it live.")),
        live(sub(slider(choice(Tab::Controls, "Camera", "Field of View",
                               "How much the camera shows, both ways, outside fights.", "Video", "FieldOfView", "100", kFovs),
                        "%"),
                 "CameraPreset", "custom")),
        live(sub(slider(choice(Tab::Controls, "Camera", "Field of View in Fights",
                               "The same while the game's battle camera is on (Gene has an enemy targeted). 100% frames "
                               "fights as on the PS2.",
                               "Video", "FightFieldOfView", "100", kFovs),
                        "%"),
                 "CameraPreset", "custom")),
        live(sub(slider(choice(Tab::Controls, "Camera", "Camera Distance",
                               "At a wider field of view, how far back the camera stays. 0% keeps Gene the size he is at "
                               "100%; 100% shows more round him.",
                               "Video", "CameraZoomOut", "0", kCameraDistances),
                        "%"),
                 "CameraPreset", "custom")),
        live(sub(slider(choice(Tab::Controls, "Camera", "Camera Tilt",
                               "At a wider field of view, the camera also rises and looks down at Gene by this much: more "
                               "of the floor ahead, less sky.",
                               "Video", "CameraTilt", "0", kCameraTilts),
                        kDegrees),
                 "CameraPreset", "custom")),
        live(note(toggle(Tab::Controls, "Camera", "Steady Idle Cam",
                         "Whether the camera bobs with Gene.", "Video",
                         "SteadyIdleCamera", false),
                  "F2 > Camera switches it while playing.")),
        live(note(toggle(Tab::Controls, "Camera", "See Through",
                         "Instead of removing walls, the camera sees\nthrough them around Gene and enemies.",
                         "Video", "SeeThrough", true),
                  "F2 > Camera sizes it while playing.")),
        live(choice(Tab::Controls, "Controller", "Menu Controls",
                    "Western: bottom button confirms; right cancels.\nJapanese: right button confirms; bottom cancels.",
                    "Game", "MenuButtons", "western", kMenuButtons)),
        live(toggle(Tab::Controls, "Controller", "Vibration", "Controller rumble. Girls know what it's for.", "Game",
                    "Vibration", true)),

        // ---- Display --------------------------------------------------------------------------------------------
        live(onLeave(choice(Tab::Display, "Renderer", "Graphics API",
                            "Vulkan is the default. If Vulkan fails to start, the game uses Direct3D 11 instead.", "Video",
                            "GraphicsAPI", "vulkan", kGraphicsApis))),
        live(onLeave(choice(Tab::Display, "Renderer", "Internal Resolution",
                            "How many pixels the game renders.\n1x: 448p | 2x: 896p | 3x: 1344p | 4x: 1792p", "Video",
                            "InternalResolution", "3", kResolutions))),
        // Launcher-only (K, Nov 13: Upscaled has caused no issues; a dev option, clutter in game).
        live(withChoices(custom("DisplayMode", Tab::Display, "Game Window", "Display Mode",
                                "Window, max window, or borderless fullscreen.\nYou can also use F11, if you're into that."),
                         kDisplayModes, static_cast<int>(std::size(kDisplayModes)), "fullscreen")),
        greyed(sub(choice(Tab::Display, "Game Window", "Window Size",
                          "The window's size when it opens (not maximized or fullscreen).", "Video", "WindowSize", "auto",
                          kWindowSizes),
                   "DisplayMode", "window")),
        live(onLeave(choice(Tab::Display, "Game Window", "Aspect Ratio",
                            "Select screen width.\nGoes from 4:3 to wider than yo momma!", "Video", "AspectRatio",
                            "16:9", kAspects))),
        choice(Tab::Display, "Effects", "Post Effects",
               "The blur effects (depth of field, heat haze) are drawn from small copies of the frame. PS2 resolution "
               "keeps them aligned but grainy at high internal resolutions; Upscaled draws them sharp.",
               "Video", "PostEffects", "upscaled", kPostEffects),
        live(note(toggle(Tab::Display, "Effects", "Depth of Field",
                         "Blur in the distance.\nIt can't hurt you.",
                         "Video", "DepthOfField", true),
                  "F2 switches it while playing.")),

        // ---- Interface ------------------------------------------------------------------------------------------
        live(toggle(Tab::Interface, "HUD", "Enemy Life Gauge", "Select whether to display\nan enemy's life gauge.", "Game",
                    "EnemyLifeGauge", true)), // the game's own help
        live(wideOnly(note(choice(Tab::Interface, "HUD", "HUD Layout",
                                  "Where the health bar, radar, and the rest of the in-game HUD go on a wide screen. Wider "
                                  "than 16:9 keeps them at the edges of a 16:9 area.",
                                  "Video", "HudLayout", "16:9", kHudLayouts),
                           "F7 switches it while playing."))),
        live(toggle(Tab::Interface, "HUD", "Sway Bar",
                    "Stun gauge for enemies and bosses.\nFills up till they're stunned, then drains.",
                    "Game", "SwayBar", false)),
        live(wideOnly(choice(Tab::Interface, "Menus & Prompts", "Menu Width",
                             "Menus and screens (pause, options, roulette setup) are drawn for 4:3: shown 4:3 with side "
                             "bars, or stretched to the screen.",
                             "Video", "MenuLayout", "4:3", kMenuLayouts))),
        live(choice(Tab::Interface, "Menus & Prompts", "Button Prompts",
                    "Controller button hint style.\nThis choice reflects your value as a person.",
                    "Game", "ButtonPrompts", "auto", kButtonPrompts)),

        // ---- Gameplay -------------------------------------------------------------------------------------------
        live(toggle(Tab::Gameplay, "Speed & Feed", "60 FPS",
                    "Play at a smooth sexy silky 60FPS.\nLeave OFF if you want the OG experience.", "Game",
                    "Fps60", false)),
        live(note(choice(Tab::Gameplay, "Move & Groove", "Control Style",
                         "Classic: PS2's OG fixed facing brawling.\nModern: Toggle third-person freecam.",
                         "Game", "ControlStyle", "classic", kControlStyles),
                  "F2 > Controls changes it while playing.")),
        live(sub(toggle(Tab::Gameplay, "Move & Groove", "Modern: Auto",
                        "Brawling starts when dealing or taking damage.\nPress the stick (rebindable) to go back to freecam.",
                        "Game", "ModernAuto", true),
                 "ControlStyle", "modern")),
        live(note(sub(toggle(Tab::Gameplay, "Move & Groove", "Modern: Mouse Look",
                             "Freak...",
                             "Game", "MouseCamera", true),
                      "ControlStyle", "modern"),
                  "F2 > Controls sets the sensitivity.")),
        live(toggle(Tab::Gameplay, "Move & Groove", "Click-Stick Dash",
                    "Press and hold the stick to dash.\nRebindable in Controls.", "Game", "L3Dash", false)),
        live(note(toggle(Tab::Gameplay, "Move & Groove", "Busta-Chan Jive",
                         "Sprint attacks go through 99% of obstacles.\nSlide right through, swooce right over.",
                         "Game", "SprintTraversal", false),
                  "F2 > Debug > Stage collision marks them.")),
        live(toggle(Tab::Gameplay, "Style Shuffle", "Moveset Presets",
                    "Configure up to 4 styles; switch with the d-pad.\nLetters around the radar show your sweet sexy styles.",
                    "Game", "StyleSwitching", false)),
        live(sub(toggle(Tab::Gameplay, "Style Shuffle", "Signal Style Switch",
                        "Gene calls out each style as you switch to it, and his God Hand flashes.", "Game", "StyleSignal",
                        false),
                 "StyleSwitching")),
        live(toggle(Tab::Gameplay, "Style Shuffle", "Buck Breaker",
                    "Pick how to punish stunned enemies.\nWorks with Moveset Presets. Some restrictions apply.",
                    "Game", "BuckBreaker", false)),
        live(toggle(Tab::Gameplay, "Fightin' Excitement", "Cunt Punter",
                    "Ball Buster works on women. Name changes on target.\nThis also works IRL, though your mileage may vary.", "Game", "BallBusterWomen", false)),
        live(toggle(Tab::Gameplay, "Fightin' Excitement", "Tension is Placable",
                    "After unleashing the God Hand, pull the trigger again\nto leash it early and keep your remaining Tension.",
                    "Game", "TensionPlacable", false)),
        live(sub(toggle(Tab::Gameplay, "Fightin' Excitement", "Off the Cuff",
                        "Unleash the God Hand with any Tension, not just full.\nGene is invulnerable only when unleashing at full Tension.",
                        "Game", "OffTheCuff", false),
                 "TensionPlacable")),
        live(toggle(Tab::Gameplay, "Fightin' Excitement", "Wild Curry Funk",
                    "Carry and throw more types of objects. Mostly chairs.\nIf it's not obvious, this makes the game "
                    "easier.",
                    "Game", "LiftChairs", false)),
        live(choice(Tab::Gameplay, "Roulette Wheel & Reel", "Roulette Timer",
                    "Time limit to select a move after opening the Roulette.\nTechniques fire off automatically at the end of the timer.",
                    "Game", "RouletteTimer", "normal", kRouletteTimers)),
        live(toggle(Tab::Gameplay, "Roulette Wheel & Reel", "Target Select",
                    "Switch Roulette targets with the right stick and the d-pad.\nHINT: You can also use the d-pad to cycle Roulette moves.",
                    "Game", "RouletteTargetCycle", false)),
        live(toggle(Tab::Gameplay, "Roulette Wheel & Reel", "Dry Spinning",
                    "You can open the Roulette even when you don't have balls.\nHINT: You can use the Roulette camera for targeting.",
                    "Game", "RouletteNoOrbs", false)),
        toggle(Tab::Gameplay, "Skip Poker", "Intro Skipper",
               "Start at the main menu: no Capcom and Clover logos and no \"Press Start\" screen.", "Game", "SkipIntro",
               false),
        toggle(Tab::Gameplay, "Skip Poker", "Hard-On",
               "Hard can be picked from the start instead of after a clear. Your save is not changed.", "Game",
               "UnlockHard", false),
        live(note(toggle(Tab::Gameplay, "Misc. & Risk", "Better Shop & Casino Poses",
                         "Give the shop and casino people better poses.\nNot sure what else you were expecting...",
                         "Game", "NpcPoses", false),
                  "F2 > NPCs sets the poses.")),

        // ---- Sound ----------------------------------------------------------------------------------------------
        live(choice(Tab::Sound, "Sound", "Mode", "Select between monaural,\nstereo, or surround sound.", "Sound", "Mode",
                    "stereo", kSoundModes)), // the game's own help (its SOUND row)
        live(volume("Music", "Set stage, boss, menu, and minigame music volume.", "MusicVolume")),
        live(volume("Sound Effects", "Set the volume of ingame sound effects.", "EffectsVolume")),
        live(volume("Voices",
                    "Set the volume of ingame voice lines.\nCutscene music and voices are one track.",
                    "VoicesVolume")),

        // ---- Mods (the launcher only) ---------------------------------------------------------------------------
        custom("Character", Tab::Mods, "Character", "Character",
               "Who you play as. Currently, other characters use Gene's moves and his God Hand."),
        custom("Textures", Tab::Mods, "Texture Packs", "Replacement Textures", "HD texture packs in PCSX2's format."),
    };
    const size_t kSettingCount = std::size(kSettings);

    const char *tabName(Tab tab)
    {
        switch (tab)
        {
        case Tab::Controls:
            return "Controls";
        case Tab::Display:
            return "Display";
        case Tab::Interface:
            return "Interface";
        case Tab::Gameplay:
            return "Gameplay";
        case Tab::Sound:
            return "Sound";
        case Tab::Mods:
            return "Mods";
        }
        return "";
    }

    const Setting *find(const char *id)
    {
        for (const Setting &s : kSettings)
            if (std::strcmp(s.id, id) == 0)
                return &s;
        return nullptr;
    }

    namespace
    {
        bool same(const std::string &a, const char *b)
        {
            if (a.size() != std::strlen(b))
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }
    }

    std::string value(const IniFile &ini, const Setting &setting)
    {
        if (std::strcmp(setting.id, "DisplayMode") == 0) // two keys
            return ini.getBool("Video", "StartFullscreen", true)    ? "fullscreen"
                   : ini.getBool("Video", "StartMaximized", false) ? "maximized"
                                                                   : "window";
        if (!*setting.key)
            return {};
        std::string text = ini.get(setting.section, setting.key, "");
        if (std::strcmp(setting.key, "MenuButtons") == 0 && same(text, "modern"))
            return "western"; // the name before Oct 31
        if (!text.empty())
            return text;
        if (std::strcmp(setting.key, "MusicVolume") == 0 && ini.getBool("Sound", "MuteMusic", false))
            return "0"; // Music 0 replaces Mute Music (K, Nov 12)
        if (std::strcmp(setting.key, "CameraPreset") == 0) // before Nov 13 the preset was the four values' match
        {
            const auto number = [&](const char *key, float fallback) {
                const std::string v = ini.get("Video", key, "");
                return v.empty() ? fallback : static_cast<float>(std::atof(v.c_str()));
            };
            return cameraPresetId(number("FieldOfView", 100.0f), number("FightFieldOfView", 100.0f),
                                  number("CameraZoomOut", 0.0f), number("CameraTilt", 0.0f));
        }
        return setting.fallback;
    }

    bool on(const IniFile &ini, const Setting &setting)
    {
        const std::string text = value(ini, setting);
        return !text.empty() && text != "0";
    }

    int choiceIndex(const Setting &setting, const std::string &text)
    {
        for (int i = 0; i < setting.choiceCount; ++i)
            if (same(text, setting.choices[i].value))
                return i;
        return -1;
    }

    bool parentAllows(const IniFile &ini, const Setting &setting)
    {
        if (!setting.parent)
            return true;
        if (std::strcmp(setting.parent, "DisplayMode") == 0) // two keys in GHR.ini (StartFullscreen, StartMaximized)
            return !ini.getBool("Video", "StartFullscreen", true) && !ini.getBool("Video", "StartMaximized", false);
        const Setting *parent = find(setting.parent);
        if (!parent)
            return true;
        if (setting.parentValue)
            return same(value(ini, *parent), setting.parentValue);
        return on(ini, *parent);
    }

    const char *cameraPresetId(float fov, float fight, float zoomOut, float tilt)
    {
        const auto same = [](float a, float b) { return std::fabs(a - b) < 0.05f; };
        for (size_t i = 0; i < std::size(kCameraPresets); ++i)
        {
            const CameraPreset &p = kCameraPresets[i];
            if (same(fov, p.fieldOfView) && same(fight, p.fightFieldOfView) && same(zoomOut, p.cameraZoomOut) &&
                same(tilt, p.cameraTilt))
                return kCameraPresetChoices[i].value;
        }
        return "custom";
    }

    void writeDisplayMode(IniFile &ini, const std::string &mode)
    {
        ini.setBool("Video", "StartFullscreen", mode == "fullscreen");
        ini.setBool("Video", "StartMaximized", mode == "maximized");
    }

    void writeCameraPreset(IniFile &ini, const std::string &id)
    {
        // A preset writes its four values; Custom starts from Action's (K, Nov 13).
        size_t index = std::size(kCameraPresets) - 1; // Action
        for (size_t i = 0; i < std::size(kCameraPresets); ++i)
            if (same(id, kCameraPresetChoices[i].value))
                index = i;
        const CameraPreset &p = kCameraPresets[index];
        const auto text = [](float v) {
            char out[32];
            std::snprintf(out, sizeof(out), "%g", std::round(v * 100.0f) / 100.0f);
            return std::string(out);
        };
        ini.set("Video", "CameraPreset", id);
        ini.set("Video", "FieldOfView", text(p.fieldOfView));
        ini.set("Video", "FightFieldOfView", text(p.fightFieldOfView));
        ini.set("Video", "CameraZoomOut", text(p.cameraZoomOut));
        ini.set("Video", "CameraTilt", text(p.cameraTilt));
    }
}
