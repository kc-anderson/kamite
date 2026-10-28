// The F2 dev menu: the Display, Graphics and Gameplay sections, and the tab order. See dev_menu.h.
//
// What can change while the game runs (and how):
//   HUD layout, menu width         hud_layout: applied at the next game frame
//   full screen                    the window thread's F11 handling (ps2_runtime), asked for with the hotkey action
//   depth of field and the other   camera.cpp's Graphics section (pcsx2gs_set_option)
//   renderer switches
//   60 FPS                         frame_patches: the patch bits, VU1 timing, roulette period and frame divider,
//                                  switched between two game frames
//   L3 dash, roulette timer        frame_patches: atomics read by the frame hook
//   mute music                     game_options: the patch flag, at the music's next volume call
// What cannot (shown with "set in the launcher"): the aspect ratio (the window's shape and the widescreen hooks),
// the internal resolution and post effects (the renderer opens with them), the texture pack (loaded at start), the
// character (a disc overlay), style switching and NPC poses (their hooks are installed at start).
#include "kamite/dev_menu.h"
#include "kamite/frame_patches.h"
#include "kamite/game_options.h"
#include "kamite/hud_layout.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_hotkeys.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <cstdlib>
#include <cstring>
#include <string>

namespace kamite::dev_menu
{
    namespace
    {
        std::filesystem::path g_iniPath;
        std::string g_displayStatus, g_gameplayStatus;

        std::string env(const char *name, const char *fallback)
        {
            const char *value = std::getenv(name);
            return value && *value ? value : fallback;
        }

        bool envOn(const char *name, bool fallback)
        {
            const char *value = std::getenv(name);
            return value && *value ? *value != '0' : fallback;
        }

        bool loadIni(IniFile &ini, std::string &status)
        {
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                status = "Could not read kamite.ini.";
                return false;
            }
            return true;
        }

        void saveIni(IniFile &ini, std::string &status)
        {
            status = ini.save() ? "Saved to kamite.ini." : "Could not write kamite.ini.";
        }

#if defined(KAMITE_HAS_GS_MENU)
        void launcherOnly(const Pcsx2GsUi *ui, const char *label, const std::string &value)
        {
            const std::string line = std::string(label) + ": " + value + "  (set in the launcher; a restart applies it)";
            ui->text_disabled(line.c_str());
        }

        void status(const Pcsx2GsUi *ui, const std::string &text)
        {
            if (!text.empty())
            {
                ui->same_line();
                ui->text(text.c_str());
            }
        }

        void drawDisplay(const Pcsx2GsUi *ui)
        {
            ui->separator_text("HUD");
            static const char *const kHud[] = {"16:9: the HUD at the screen's edges", "4:3: the HUD where the PS2 had it"};
            int hud = hud::layoutIndex();
            ui->set_next_width(340.0f);
            if (ui->combo("HUD layout##hud", &hud, kHud, 2))
                hud::requestLayout(hud);
            static const char *const kMenus[] = {"4:3 with side bars", "Stretched"};
            int menus = hud::menuModeIndex();
            ui->set_next_width(340.0f);
            if (ui->combo("Menus and screens##menus", &menus, kMenus, 2))
                hud::requestMenuMode(menus);
            ui->text_disabled("The pause menu, the status and shop screens: drawn for 4:3, shown with side bars or "
                              "stretched to the picture.");

            ui->separator_text("Window");
            if (ui->button("Switch full screen / window##fullscreen"))
                ps2x::hotkeys::fire(ps2x::hotkeys::Action::Fullscreen);
            ui->same_line();
            const std::string key = ps2x::hotkeys::text(ps2x::hotkeys::binding(ps2x::hotkeys::Action::Fullscreen));
            ui->text_disabled(key.empty() ? "(no key bound)" : ("(" + key + ")").c_str());
            launcherOnly(ui, "Aspect ratio", env("KAMITE_ASPECT", "16:9"));
            launcherOnly(ui, "Window at start", envOn("KAMITE_START_FULLSCREEN", false) ? "full screen"
                                                : envOn("KAMITE_START_MAXIMIZED", false) ? "maximized"
                                                                                           : "window, " + env("PS2X_WINDOW_SIZE", "auto"));

            ui->separator_text("kamite.ini");
            if (ui->button("Save##display"))
            {
                IniFile ini;
                if (loadIni(ini, g_displayStatus))
                {
                    ini.set("Video", "HudLayout", hud::layoutIndex() ? "4:3" : "16:9");
                    ini.set("Video", "MenuLayout", hud::menuModeIndex() ? "stretch" : "4:3");
                    saveIni(ini, g_displayStatus);
                }
            }
            status(ui, g_displayStatus);
            ui->text_disabled("The HUD layout and the menu width. The camera and the renderer switches save from their own tabs.");
        }

        void drawGraphicsExtras(const Pcsx2GsUi *ui)
        {
            ui->separator_text("At start");
            launcherOnly(ui, "Renderer", env("PS2X_GS_RENDERER", "pcsx2"));
            launcherOnly(ui, "Internal resolution", env("PS2X_GS_SCALE", "3") + "x");
            launcherOnly(ui, "Post effects", env("PS2X_PCSX2_NATIVE_SCALING", "ps2"));
            launcherOnly(ui, "HD textures", envOn("PS2X_PCSX2_TEXTURES", true) ? "on, " + env("PS2X_PCSX2_TEXTURE_DIR", "textures/SLUS-21503") : "off");
            launcherOnly(ui, "Character", env("KAMITE_CHARACTER", "gene"));
        }

        void drawGameplay(const Pcsx2GsUi *ui)
        {
            ui->separator_text("Frame rate");
            int sixty = fps60() ? 1 : 0;
            if (ui->checkbox("60 FPS mode##fps60", &sixty))
                requestFps60(sixty != 0);
            ui->text_disabled("Switches between two game frames, with every fix of the mode (the patch bits, the VU1 "
                              "timing, the roulette period, the frame divider). A save state made in either mode loads "
                              "into the mode set here. Known differences at 60: enemy backflips are faster, texture "
                              "animations run 2x.");
            int dash = l3Dash() ? 1 : 0;
            if (ui->checkbox("L3 dash: holding L3 pushes the left stick fully forward##l3dash", &dash))
                setL3Dash(dash != 0);
            static const char *const kTimer[] = {"Normal: the PS2's roulette time", "Double: twice the time to choose"};
            int timer = rouletteTimerDouble() ? 1 : 0;
            ui->set_next_width(340.0f);
            if (ui->combo("Roulette timer##roulette", &timer, kTimer, 2))
                setRouletteTimerDouble(timer == 1);

            ui->separator_text("Sound");
            int mute = muteMusic() ? 1 : 0;
            if (ui->checkbox("Mute music##mutemusic", &mute))
                setMuteMusic(mute != 0);
            ui->text_disabled("Takes effect at the music's next volume change (a room or a track change). Cutscene "
                              "streams keep playing: their voices are in the same stream.");

            ui->separator_text("At start");
            launcherOnly(ui, "Style switching", envOn("KAMITE_STYLE_SWITCHING", false) ? "on" : "off");
            launcherOnly(ui, "Better shop & casino poses", envOn("KAMITE_NPC_POSES", false) ? "on" : "off");
            launcherOnly(ui, "Hard unlocked", envOn("KAMITE_UNLOCK_HARD", false) ? "yes" : "as saved");

            ui->separator_text("kamite.ini");
            if (ui->button("Save##gameplay"))
            {
                IniFile ini;
                if (loadIni(ini, g_gameplayStatus))
                {
                    ini.setBool("Game", "Fps60", sixty != 0);
                    ini.setBool("Game", "L3Dash", l3Dash());
                    ini.set("Game", "RouletteTimer", rouletteTimerDouble() ? "double" : "normal");
                    ini.setBool("Sound", "MuteMusic", muteMusic());
                    saveIni(ini, g_gameplayStatus);
                }
            }
            status(ui, g_gameplayStatus);
        }
#endif
    }

    void install(const std::filesystem::path &iniPath)
    {
        g_iniPath = iniPath;
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::setTitle("Kamite dev menu", "F2 (or the Dev menu hotkey) closes this. The game keeps running behind it.");
        ps2x::menu::setTabOrder({"Display", "Graphics", "Gameplay", "Camera", "Controls", "NPCs", "Debug"});
        ps2x::menu::addPanel("Display", &drawDisplay);
        ps2x::menu::addPanel("Graphics", &drawGraphicsExtras);
        ps2x::menu::addPanel("Gameplay", &drawGameplay);
#endif
        KM_INFO(Game, "dev menu: Display, Gameplay tabs registered");
    }
}
