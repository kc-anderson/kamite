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
// What cannot (shown with "set in the launcher"): the renderer and graphics API, the texture pack (loaded at start),
// the character (a disc overlay). The aspect ratio, display mode, internal resolution and post effects change in the
// OPTIONS screen (Nov 13). The Gameplay enhancements are all live (Nov 13; Wild Curry Funk from the next room).
#include "kamite/dev_menu.h"
#include "kamite/buck_breaker.h"
#include "kamite/frame_patches.h"
#include "kamite/game_options.h"
#include "kamite/bone_tweaks.h"
#include "kamite/hud_layout.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"
#include "kamite/npc_poses.h"
#include "kamite/sprint_traversal.h"
#include "kamite/style_presets.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_hotkeys.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <cstdlib>
#include <cstring>
#include <cmath>
#include <mutex>
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
                status = "Could not read GHR.ini.";
                return false;
            }
            return true;
        }

        void saveIni(IniFile &ini, std::string &status)
        {
            status = ini.save() ? "Saved to GHR.ini." : "Could not write GHR.ini.";
        }

#if defined(KAMITE_HAS_GS_MENU)
        void launcherOnly(const Pcsx2GsUi *ui, const char *label, const std::string &value)
        {
            const std::string line = std::string(label) + ": " + value + "  (set in the launcher; a restart applies it)";
            ui->text_disabled(line.c_str());
        }

        // Live in the OPTIONS screen (Nov 13): shown here, changed there.
        void inOptions(const Pcsx2GsUi *ui, const char *label, const std::string &value)
        {
            const std::string line = std::string(label) + ": " + value + "  (OPTIONS > Display changes it while playing)";
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
            inOptions(ui, "Aspect ratio", env("KAMITE_ASPECT", "16:9"));
            inOptions(ui, "Display mode", envOn("KAMITE_START_FULLSCREEN", false) ? "full screen"
                                          : envOn("KAMITE_START_MAXIMIZED", false) ? "maximized"
                                                                                     : "window, " + env("PS2X_WINDOW_SIZE", "auto"));

            ui->separator_text("GHR.ini");
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
            launcherOnly(ui, "Graphics API", env("PS2X_PCSX2_API", "vulkan") != "dx11" ? "Vulkan" : "Direct3D 11");
            inOptions(ui, "Internal resolution", env("PS2X_GS_SCALE", "3") + "x");
            launcherOnly(ui, "Post effects", env("PS2X_PCSX2_NATIVE_SCALING", "upscaled"));
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
            if (ui->checkbox("Click-Stick Dash: holding L3 pushes the left stick fully forward##l3dash", &dash))
                setL3Dash(dash != 0);
            static const char *const kTimer[] = {"Normal: the PS2's roulette time", "Double: twice the time to choose"};
            int timer = rouletteTimerDouble() ? 1 : 0;
            ui->set_next_width(340.0f);
            if (ui->combo("Roulette timer##roulette", &timer, kTimer, 2))
                setRouletteTimerDouble(timer == 1);
            int cycle = rouletteTargetCycle() ? 1 : 0;
            if (ui->checkbox("Target Select: the right stick moves the wheel's target##roulettecycle", &cycle))
                setRouletteTargetCycle(cycle != 0);

            ui->separator_text("Fightin' Excitement");
            int placable = tensionPlacable() ? 1 : 0;
            if (ui->checkbox("Tension is Placable: R2 again ends the God Hand, the tension left stays##placable", &placable))
                setTensionPlacable(placable != 0);
            if (placable)
            {
                int cuff = offTheCuff() ? 1 : 0;
                if (ui->checkbox("Off the Cuff: unleash with any tension##offthecuff", &cuff))
                    setOffTheCuff(cuff != 0);
            }
            int punter = ballBusterWomen() ? 1 : 0;
            if (ui->checkbox("Cunt Punter: Ball Buster works on women##ballbusterwomen", &punter))
                setBallBusterWomen(punter != 0);
            int sway = swayBar() ? 1 : 0;
            if (ui->checkbox("Sway Bar: a stun gauge under the enemies' health bars##swaybar", &sway))
                setSwayBar(sway != 0);

            int chairs = liftChairs() ? 1 : 0;
            if (ui->checkbox("Wild Curry Funk: chairs can be lifted (from the next room)##liftchairs", &chairs))
                setLiftChairs(chairs != 0);

            ui->separator_text("Style Shuffle");
            int presets = style_presets::enabled() ? 1 : 0;
            if (ui->checkbox("Moveset Presets: the styles S, E, X, Y (off: None, the style on kept)##styles", &presets))
                style_presets::setEnabled(presets != 0);
            if (presets)
            {
                int signal = style_presets::signal() ? 1 : 0;
                if (ui->checkbox("Signal Style Switch: Gene's line and the arm flash##stylesignal", &signal))
                    style_presets::setSignal(signal != 0);
            }
            int buck = buck_breaker::enabled() ? 1 : 0;
            if (ui->checkbox("Buck Breaker: the punish slot (off: the game's punishes)##buckbreaker", &buck))
                buck_breaker::setEnabled(buck != 0);

            ui->separator_text("More");
            int jive = sprint_traversal::enabled() ? 1 : 0;
            if (ui->checkbox("Busta-Chan Jive: sprint attacks over / under the marked railings##sprinttraversal", &jive))
                sprint_traversal::setEnabled(jive != 0);
            int dry = rouletteNoOrbs() ? 1 : 0;
            if (ui->checkbox("Dry Spinning: R1 opens the roulette with no orbs##roulettenoorbs", &dry))
                setRouletteNoOrbs(dry != 0);
            int poses = npc_poses::enabled() ? 1 : 0;
            if (ui->checkbox("Better Shop & Casino Poses##npcposes", &poses))
                npc_poses::setEnabled(poses != 0);

            ui->separator_text("GHR.ini");
            if (ui->button("Save##gameplay"))
            {
                IniFile ini;
                if (loadIni(ini, g_gameplayStatus))
                {
                    ini.setBool("Game", "Fps60", sixty != 0);
                    ini.setBool("Game", "L3Dash", l3Dash());
                    ini.set("Game", "RouletteTimer", rouletteTimerDouble() ? "double" : "normal");
                    ini.setBool("Game", "RouletteTargetCycle", rouletteTargetCycle());
                    ini.setBool("Game", "TensionPlacable", tensionPlacable());
                    ini.setBool("Game", "OffTheCuff", offTheCuff());
                    ini.setBool("Game", "BallBusterWomen", ballBusterWomen());
                    ini.setBool("Game", "SwayBar", swayBar());
                    ini.setBool("Game", "LiftChairs", liftChairs());
                    ini.setBool("Game", "StyleSwitching", style_presets::enabled());
                    ini.setBool("Game", "StyleSignal", style_presets::signal());
                    ini.setBool("Game", "BuckBreaker", buck_breaker::enabled());
                    ini.setBool("Game", "SprintTraversal", sprint_traversal::enabled());
                    ini.setBool("Game", "RouletteNoOrbs", rouletteNoOrbs());
                    ini.setBool("Game", "NpcPoses", npc_poses::enabled());
                    ini.set("Game", "Money", std::to_string(static_cast<int>(std::lround(bone_tweaks::money() * 100.0f))));
                    saveIni(ini, g_gameplayStatus);
                }
            }
            status(ui, g_gameplayStatus);
        }
#endif
    }

    bool saveIniValue(const char *section, const char *key, const std::string &value)
    {
        static std::mutex lock;
        std::lock_guard<std::mutex> guard(lock);
        IniFile ini;
        if (g_iniPath.empty() || !ini.load(g_iniPath))
            return false;
        ini.set(section, key, value);
        return ini.save();
    }

    void install(const std::filesystem::path &iniPath)
    {
        g_iniPath = iniPath;
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::setTitle("God Hand: Rearmed dev menu", "F2 (or the Dev menu hotkey) closes this. The game keeps running behind it.");
        ps2x::menu::setTabOrder({"Display", "Graphics", "Gameplay", "Camera", "Controls", "NPCs", "Debug"});
        ps2x::menu::addPanel("Display", &drawDisplay);
        ps2x::menu::addPanel("Graphics", &drawGraphicsExtras);
        ps2x::menu::addPanel("Gameplay", &drawGameplay);
#endif
        KM_INFO(Game, "dev menu: Display, Gameplay tabs registered");
    }
}
