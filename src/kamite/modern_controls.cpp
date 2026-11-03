// Control Style: Classic (the PS2's) or Modern.
//
// Modern: the right stick turns a free camera round Gene instead of dodging, and the left stick moves him relative to
// the camera (he faces where he walks). The right stick press (R3) switches between that and the original controls
// (tank movement, right stick = dodges, the camera behind Gene) (K, Nov 3: on the camera's own stick; was Select). The
// switch is on the release, and only when the stick stayed near the centre while pressed (a hard dodge flick that
// clicks the stick does not switch). In Modern the game never sees R3: Gene's shared cancel check func_00123938 sends
// an R3 press in a move's cancel window (seqFlags 0x10) to mode 0 state 4 (func_0010CB10, a short motion), which a
// switch would otherwise fire. The choice is kept across rooms, doors and loads; a launch starts in the free camera.
// With Auto, contact starts brawl mode: Gene's attack (or something he threw) hurting an enemy, or anything hurting
// Gene (the two damage steps, debug_tools' hooks call noteContact). Not the battle camera: it also comes on for
// crates and throws (K, Nov 3); not distance or enemy intent (K: free camera must still get you out of a fight).
// Nothing switches back: R3 returns to the free camera. L1 is always the game's own quick turn; in the free camera the camera swings back
// behind Gene with him. Whenever
// the game takes the camera (a close-up for a grab or a special move, a cutscene, a menu: no player camera update for
// a moment), everything is Classic for that moment and the choice comes back after it.
//
// Input side (window thread, the pad filter ps2_host_input calls for player 1): L1 and the right stick are taken from
// what the game sees; the left stick is turned into the stick the original controls would need for the same
// direction (relative to where Gene faces). Camera side (game thread, camera.cpp's yaw hook): the camera's yaw round
// Gene and an extra pitch, eased back to the original camera when the original controls are in effect; while Gene
// stands or walks, his yaw (+0x104) is also turned toward the stick's direction directly (up to 900 degrees a
// second), so he does not have to wait for the original turning speed.
//
// Mouse ([Game] MouseCamera, on by default): the runtime's mouse camera captures the mouse on a click in the game
// window (that click is not passed on; Esc or losing the focus frees it, and so does the F2 menu); its movement turns
// the free camera at MouseSensitivity degrees per pixel. The mouse never reaches the game as a right stick, so it
// cannot dodge; while the original controls are in effect it does nothing.
//
// Directions: the camera's yaw is in Gene's convention (the original camera's yaw is his + the offset at +0x414),
// "right" on the stick is the direction a camera turned right by the stick would face (yaw decreasing).
#include "kamite/modern_controls.h"

#include "kamite/hotkeys.h"
#include "kamite/button_prompts.h"
#include "kamite/jukebox.h"
#include "kamite/frame_patches.h"
#include "kamite/prompt_hd.h"
#include "kamite/camera.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"
#include "kamite/menu_buttons.h"
#include "kamite/style_presets.h"
#include "kamite/test_mode.h"
#include "kamite/game_layout.h"
#include "ps2_host_input.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_input_options.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

namespace kamite::modern
{
    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kDegToRad = kPi / 180.0f;
        constexpr int64_t kNoCameraMs = 150;   // no player camera update for this long: the game has the camera
        constexpr float kStickDeadzone = 0.18f;
        constexpr float kSwitchMaxStick = 0.5f; // an R3 press with the right stick further out than this: a flick, no switch
        constexpr float kPitchMin = -1.2f, kPitchMax = 0.9f; // extra pitch, radians (the floor is camera.cpp's: above the ground)
        constexpr int64_t kQuickTurnFollowMs = 650;            // the camera stays behind Gene this long after one
        constexpr float kTurnRate = 900.0f * (3.14159265f / 180.0f); // Gene turned toward the stick, radians a second
        constexpr uint32_t kPlayerPointer = game::GameWork::address(game::GameWork::kPlayerObj); // GameWork.playerObj (Getplayer)

        // Settings (F2 menu on the GS thread, read by the window and game threads).
        std::atomic<bool> g_modern{false};
        std::atomic<bool> g_auto{true};
        std::atomic<float> g_yawSpeed{200.0f};  // degrees a second at full stick
        std::atomic<float> g_pitchSpeed{80.0f};
        std::atomic<bool> g_invertX{false};
        std::atomic<bool> g_invertY{false};
        std::atomic<bool> g_mouseCamera{true};
        std::atomic<float> g_mouseSensitivity{0.1f}; // degrees per pixel

        // Camera side -> input side.
        std::atomic<bool> g_battleCamera{false};
        std::atomic<bool> g_gameCamera{false};
        std::atomic<int64_t> g_lastCameraMs{0};
        std::atomic<bool> g_free{false};        // the free camera is on (not easing back to the original)
        std::atomic<float> g_cameraYaw{0.0f};   // the free camera's yaw
        std::atomic<float> g_geneYaw{0.0f};

        // Input side -> camera side.
        std::atomic<bool> g_classic{false};     // the player's choice (R3, or Auto at a fight's start): original controls
        std::atomic<bool> g_classicByAuto{false}; // ... set by Auto (for the F2 status line)
        std::atomic<float> g_stickX{0.0f}, g_stickY{0.0f};
        std::atomic<bool> g_moving{false};      // the left stick asks for a direction (Modern movement)
        std::atomic<float> g_moveYaw{0.0f};     // that direction (world yaw)
        std::atomic<uint32_t> g_recenter{0};    // bumped by each quick turn
        std::atomic<float> g_pitchFloor{-1.2f}; // the lowest extra pitch that keeps the camera above the ground

        // Input side only (window thread).
        bool g_l1Was = false;
        bool g_r3Was = false;
        bool g_r3Stray = false; // the right stick left the centre during this press: not a switch

        int64_t nowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        bool gameHasCamera()
        {
            return g_gameCamera.load(std::memory_order_relaxed) ||
                   nowMs() - g_lastCameraMs.load(std::memory_order_relaxed) > kNoCameraMs;
        }

        // The original controls are in effect (besides the game having the camera).
        bool classicByChoice()
        {
            return g_classic.load(std::memory_order_relaxed);
        }

        float stickAxis(uint8_t value)
        {
            float axis = (static_cast<float>(value) - 128.0f) / 127.0f;
            axis = std::fmax(-1.0f, std::fmin(1.0f, axis));
            const float magnitude = std::fabs(axis);
            if (magnitude < kStickDeadzone)
                return 0.0f;
            return std::copysign((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), axis);
        }

        float wrapAngle(float angle)
        {
            while (angle > kPi)
                angle -= 2.0f * kPi;
            while (angle < -kPi)
                angle += 2.0f * kPi;
            return angle;
        }

        // ps2_host_input's pad filter (window thread), player 1 only.
        void padFilter(uint32_t player, ps2x::iop::PadInputState &state)
        {
            style_presets::padFilter(player, state); // the d-pad style swaps (ps2_host_input has one filter)
            if (player != 0u)
                return;
            if (!g_modern.load(std::memory_order_relaxed))
                return;
            const bool r3 = (state.buttons & ps2x::iop::PadButton::R3) != 0u;
            state.buttons = static_cast<uint16_t>(state.buttons & ~ps2x::iop::PadButton::R3); // the switch's, never the game's
            const float rx = (static_cast<float>(state.rightX) - 128.0f) / 127.0f;
            const float ry = (static_cast<float>(state.rightY) - 128.0f) / 127.0f;
            const bool l1 = (state.buttons & ps2x::iop::PadButton::L1) != 0u;
            if (gameHasCamera())
            {
                // The game's own moment: everything as on the PS2; an R3 held through it does not switch after.
                g_l1Was = l1;
                g_r3Was = r3;
                g_r3Stray = r3;
                g_stickX = 0.0f;
                g_stickY = 0.0f;
                return;
            }
            // R3 switches on its release, unless the stick was pushed out during the press (a dodge flick that clicked).
            if (r3 && std::fmax(std::fabs(rx), std::fabs(ry)) > kSwitchMaxStick)
                g_r3Stray = true;
            if (!r3 && g_r3Was)
            {
                if (!g_r3Stray)
                {
                    g_classic = !g_classic.load(std::memory_order_relaxed);
                    g_classicByAuto = false;
                }
                g_r3Stray = false;
            }
            g_r3Was = r3;
            // L1 is the game's quick turn; in the free camera the camera swings back behind Gene with him.
            if (l1 && !g_l1Was && !classicByChoice())
                g_recenter.fetch_add(1u, std::memory_order_relaxed);
            g_l1Was = l1;

            if (classicByChoice() || !g_free.load(std::memory_order_relaxed))
            {
                // The original controls (or the camera still easing back to them).
                g_stickX = 0.0f;
                g_stickY = 0.0f;
                g_moving = false;
                if (!classicByChoice())
                {
                    state.rightX = 0x80u; // easing out of a quick turn: no dodges from the camera stick
                    state.rightY = 0x80u;
                }
                return;
            }
            // The free camera has the right stick; the game sees it centred (no dodges).
            g_stickX = stickAxis(state.rightX);
            g_stickY = stickAxis(state.rightY);
            state.rightX = 0x80u;
            state.rightY = 0x80u;

            // The left stick, relative to the camera: the direction it asks for, as the stick the original controls
            // would need for it (relative to where Gene faces).
            const float lx = (static_cast<float>(state.leftX) - 128.0f) / 127.0f;
            const float ly = (static_cast<float>(state.leftY) - 128.0f) / 127.0f;
            const float magnitude = std::fmin(std::sqrt(lx * lx + ly * ly), 1.0f);
            if (magnitude < kStickDeadzone)
            {
                g_moving = false;
                return;
            }
            const float desired = wrapAngle(g_cameraYaw.load(std::memory_order_relaxed) - std::atan2(lx, -ly));
            g_moveYaw = desired;
            g_moving = true;
            const float relative = wrapAngle(desired - g_geneYaw.load(std::memory_order_relaxed));
            const float x = -std::sin(relative) * magnitude, up = std::cos(relative) * magnitude;
            const auto axis = [](float value) { return static_cast<uint8_t>(std::lround(std::fmin(std::fmax(128.0f + value * 127.0f, 0.0f), 255.0f))); };
            state.leftX = axis(x);
            state.leftY = axis(-up);
        }

        // Camera side (game thread).
        float g_yaw = 0.0f;
        float g_pitch = 0.0f;
        bool g_tracking = true;      // the free camera is on the original one (eased in, or just reset)
        bool g_following = false;   // a quick turn: easing back behind Gene before it is free again
        int64_t g_followUntilMs = 0; // ... and staying there at least until then (his turn takes a moment)
        uint32_t g_recenterSeen = 0;
        int g_mouseState = -1; // the mouse camera as last set (-1 = not yet)

        // The runtime's mouse camera on while the Modern style and the mouse option are; its movement since the last
        // frame (always taken, so nothing piles up while it is not used).
        ps2_host_input::MouseCameraInput takeMouse()
        {
            const int wanted = (g_modern.load(std::memory_order_relaxed) && g_mouseCamera.load(std::memory_order_relaxed)) ? 1 : 0;
            if (wanted != g_mouseState)
            {
                g_mouseState = wanted;
                ps2_host_input::setMouseStickEnabled(false); // the mouse is never the game's right stick
                ps2_host_input::setMouseCameraEnabled(wanted != 0); // the window thread frees the cursor when off
            }
            return ps2_host_input::consumeMouseCameraInput();
        }

        uint32_t ram32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        float ramFloat(const uint8_t *rdram, uint32_t address)
        {
            float value;
            std::memcpy(&value, rdram + (address & 0x1FFFFFFu), sizeof(value));
            return value;
        }

        bool validPointer(uint32_t pointer) { return pointer >= 0x100000u && pointer < 0x2000000u; }

        // Gene stands or walks: major step 0, sub-step 0 / 10 (standing) or 1 / 2 (walking).
        bool geneMoves(const uint8_t *rdram, uint32_t player)
        {
            const uint8_t major = rdram[(player + 0x2F4u) & 0x1FFFFFFu], sub = rdram[(player + 0x2F5u) & 0x1FFFFFFu];
            return major == 0u && (sub == 0u || sub == 1u || sub == 2u || sub == 10u);
        }

        std::filesystem::path g_iniPath;
        std::mutex g_statusMutex;
        std::string g_status;

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        std::string number(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", std::round(value * 10.0f) / 10.0f);
            return text;
        }

        std::string number2(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", std::round(value * 1000.0f) / 1000.0f);
            return text;
        }

        void saveToIni()
        {
            IniFile ini;
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                setStatus("Could not read kamite.ini.");
                return;
            }
            ini.set("Game", "ControlStyle", g_modern.load() ? "modern" : "classic");
            ini.setBool("Game", "ModernAuto", g_auto.load());
            ini.set("Game", "CameraSpeed", number(g_yawSpeed.load()));
            ini.set("Game", "CameraPitchSpeed", number(g_pitchSpeed.load()));
            ini.setBool("Game", "CameraInvertX", g_invertX.load());
            ini.setBool("Game", "CameraInvertY", g_invertY.load());
            ini.setBool("Game", "MouseCamera", g_mouseCamera.load());
            ini.set("Game", "MouseSensitivity", number2(g_mouseSensitivity.load()));
            ini.set("Game", "MenuButtons", menu_buttons::styleName(menu_buttons::style()));
            ini.set("Game", "ButtonPrompts", prompt_hd::choiceName(prompt_hd::choice()));
            setStatus(ini.save() ? "Saved to kamite.ini." : "Could not write kamite.ini.");
        }

#if defined(KAMITE_HAS_GS_MENU)
        void slider(const Pcsx2GsUi *ui, const char *label, std::atomic<float> &setting, float low, float high, const char *format)
        {
            float value = setting.load(std::memory_order_relaxed);
            ui->set_next_width(340.0f);
            if (ui->slider_float(label, &value, low, high, format))
                setting.store(value, std::memory_order_relaxed);
        }

        void checkbox(const Pcsx2GsUi *ui, const char *label, std::atomic<bool> &setting)
        {
            int value = setting.load() ? 1 : 0;
            if (ui->checkbox(label, &value))
                setting = value != 0;
        }

        void drawControlsPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 3u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again to get these controls.");
                return;
            }
            ui->separator_text("Control style");
            static const char *const kStyles[] = {"Classic: as on the PS2", "Modern: free camera on the right stick"};
            int style = g_modern.load() ? 1 : 0;
            ui->set_next_width(340.0f);
            if (ui->combo("##style", &style, kStyles, 2))
                g_modern = style == 1;
            // The Modern settings show only with Modern selected (K, Oct 27).
            if (style == 1)
            {
                ui->text_wrapped("The right stick press switches between the free camera and brawl mode (classic controls); the choice "
                                 "stays across rooms. L1 is the game's quick turn.");
                checkbox(ui, "Auto: fighting starts brawl mode (the right stick press switches back)##auto", g_auto);
                ui->separator_text("Free camera");
                slider(ui, "Turn speed##yaw", g_yawSpeed, 40.0f, 400.0f, "%.0f deg/s");
                slider(ui, "Up/down speed##pitch", g_pitchSpeed, 20.0f, 250.0f, "%.0f deg/s");
                checkbox(ui, "Invert left/right##invx", g_invertX);
                checkbox(ui, "Invert up/down##invy", g_invertY);
                ui->separator_text("Mouse");
                checkbox(ui, "The mouse turns the free camera##mouse", g_mouseCamera);
                ui->begin_disabled(!g_mouseCamera.load());
                slider(ui, "Sensitivity##mousesens", g_mouseSensitivity, 0.02f, 0.5f, "%.3f deg/pixel");
                ui->end_disabled();
                ui->text_disabled("Click in the game to capture the mouse; Esc frees it (so does this menu).");
            }

            ui->separator_text("Now");
            const char *now = !g_modern.load()    ? "Classic"
                              : gameHasCamera()   ? "The game has the camera: brawl mode"
                              : classicByChoice() ? (g_classicByAuto.load() ? "Brawl mode (Auto: a hit landed)"
                                                                            : "Brawl mode (right stick press)")
                                                  : "Free camera, camera-relative movement";
            ui->text(now);

            ui->separator_text("Menu controls");
            // In the order of the launcher's list; the index is menu_buttons::Style's order 1, 2, 0.
            static const char *const kMenuButtons[] = {"Western: the bottom button confirms, the right one cancels",
                                                       "Japanese: the right button confirms, the bottom one cancels",
                                                       "Classic: circle and cross confirm, triangle cancels"};
            static constexpr menu_buttons::Style kMenuStyles[] = {menu_buttons::Style::Western, menu_buttons::Style::Japanese,
                                                                  menu_buttons::Style::Classic};
            const menu_buttons::Style menuStyle = menu_buttons::style();
            int menus = menuStyle == menu_buttons::Style::Japanese ? 1 : menuStyle == menu_buttons::Style::Classic ? 2 : 0;
            ui->set_next_width(340.0f);
            if (ui->combo("##menubuttons", &menus, kMenuButtons, 3) && menus >= 0 && menus < 3)
                menu_buttons::setStyle(kMenuStyles[menus]);
            ui->text_disabled("Menus, dialogs and the roulette; the moves keep their buttons. The save menu's own wording "
                              "follows at the next start.");

            ui->separator_text("kamite.ini");
            if (ui->button("Save##controls"))
                saveToIni();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                {
                    ui->same_line();
                    ui->text(g_status.c_str());
                }
            }
            ui->separator_text("Button prompts");
            static const char *const kPrompts[] = {"Automatic: from the controller in use", "Xbox", "PlayStation",
                                                   "Nintendo", "Steam Deck"};
            int prompts = static_cast<int>(prompt_hd::choice());
            ui->set_next_width(340.0f);
            if (ui->combo("##prompts", &prompts, kPrompts, 5))
            {
                prompt_hd::setChoice(static_cast<prompt_hd::Choice>(prompts));
                button_prompts::requestRebuild();
            }
            ui->text_disabled("The buttons the game shows for a controller; keys and mouse always show keys. Redrawn at once.");
            ui->separator_text("Bindings");
            ui->text_disabled("The game's buttons: F1 (or Back + Start on a controller) opens the Controls screen; the "
                              "launcher's Controls tab edits the same controls.ini.");
            ui->spacing();
            hotkeys::drawSection(ui);
        }
#endif

        float envFloat(const char *name, float fallback, float low, float high)
        {
            const char *text = std::getenv(name);
            const float value = (text && *text) ? static_cast<float>(std::atof(text)) : fallback;
            return std::fmin(std::fmax(value, low), high);
        }

        bool envIs(const char *name, const char *value)
        {
            const char *text = std::getenv(name);
            if (!text)
                return false;
            std::string lower(text);
            for (char &c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lower == value;
        }

        bool envBool(const char *name, bool fallback)
        {
            const char *text = std::getenv(name);
            return (text && *text) ? *text != '0' : fallback;
        }
    }

    void noteCamera(const uint8_t *rdram, bool battle, bool gameCamera)
    {
        const int64_t now = nowMs();
        g_battleCamera.store(battle, std::memory_order_relaxed);
        // The shop's jukebox: the game's camera stands still on the radio, as on the PS2 (jukebox.h). The roulette:
        // the game swings its camera round to the roulette angle while the wheel is up; Modern hands it back too.
        g_gameCamera.store(gameCamera || jukebox::open(rdram) || rouletteUp(), std::memory_order_relaxed);
        g_lastCameraMs.store(now, std::memory_order_relaxed);
    }

    namespace
    {
        bool g_turned = false;     // updateCameraLive turned Gene this call...
        float g_turnedYaw = 0.0f;  // ... to this yaw
    }

    FreeCamera updateCameraLive(uint8_t *rdram, float gameYaw, float geneYaw, float dt);

    // A case replay plays the recorded camera and turns back (not in the save state: test_mode.h HostCamera); a case
    // being recorded keeps them.
    FreeCamera updateCamera(uint8_t *rdram, float gameYaw, float geneYaw, float dt)
    {
        test_mode::HostCamera host;
        if (test_mode::replayHost(host))
        {
            takeMouse();
            float yaw = geneYaw;
            const uint32_t player = ram32(rdram, kPlayerPointer);
            if (host.turned && validPointer(player))
            {
                yaw = host.geneYaw;
                std::memcpy(rdram + ((player + 0x104u) & 0x1FFFFFFu), &yaw, sizeof(yaw));
            }
            g_geneYaw.store(yaw, std::memory_order_relaxed);
            g_cameraYaw.store(host.yaw, std::memory_order_relaxed);
            g_free = host.free;
            FreeCamera out;
            out.free = host.free;
            out.yaw = host.yaw;
            out.pitch = host.pitch;
            return out;
        }
        g_turned = false;
        const FreeCamera out = updateCameraLive(rdram, gameYaw, geneYaw, dt);
        host.free = out.free;
        host.yaw = out.yaw;
        host.pitch = out.pitch;
        host.turned = g_turned;
        host.geneYaw = g_turnedYaw;
        test_mode::recordHost(host);
        return out;
    }

    FreeCamera updateCameraLive(uint8_t *rdram, float gameYaw, float geneYaw, float dt)
    {
        FreeCamera out;
        g_geneYaw.store(geneYaw, std::memory_order_relaxed);
        const ps2_host_input::MouseCameraInput mouse = takeMouse();
        if (!g_modern.load(std::memory_order_relaxed))
        {
            g_tracking = true;
            g_free = false;
            return out;
        }
        dt = std::fmin(std::fmax(dt, 0.0f), 0.1f);
        const int64_t now = nowMs();
        const uint32_t recenter = g_recenter.load(std::memory_order_relaxed);
        if (recenter != g_recenterSeen)
        {
            g_recenterSeen = recenter;
            g_following = true;
            g_followUntilMs = now + kQuickTurnFollowMs;
        }
        const bool original = gameHasCamera() || classicByChoice();
        if (g_tracking && !original && !g_following)
        {
            g_yaw = gameYaw; // starts free from where the original camera is
            g_pitch = 0.0f;
            g_tracking = false;
        }
        if (original || g_following)
        {
            // Back to the original camera (faster while following a quick turn); once there, it is the original
            // camera exactly. A quick turn keeps it behind Gene until his turn is over. The L1 glide was 7.0 (K,
            // Oct 27: "a touch too slow", 30% faster).
            const float ease = 1.0f - std::exp(-dt * (g_following ? 12.0f : 9.1f));
            g_yaw = wrapAngle(g_yaw + wrapAngle(gameYaw - g_yaw) * ease);
            g_pitch += (0.0f - g_pitch) * ease;
            const bool there = std::fabs(wrapAngle(gameYaw - g_yaw)) < 0.01f && std::fabs(g_pitch) < 0.005f;
            if (there && now >= g_followUntilMs)
            {
                g_following = false;
                if (original)
                    g_tracking = true;
            }
            g_free = false;
            if (g_tracking)
                return out;
        }
        else
        {
            const float x = g_stickX.load(std::memory_order_relaxed) * (g_invertX.load() ? -1.0f : 1.0f);
            const float y = g_stickY.load(std::memory_order_relaxed) * (g_invertY.load() ? -1.0f : 1.0f);
            float yawStep = x * g_yawSpeed.load(std::memory_order_relaxed) * kDegToRad * dt;
            float pitchStep = y * g_pitchSpeed.load(std::memory_order_relaxed) * kDegToRad * dt;
            if (mouse.active)
            {
                // Mouse right = turn right (yaw decreasing), mouse down = look down (pitch up), as the stick.
                const float perPixel = g_mouseSensitivity.load(std::memory_order_relaxed) * kDegToRad;
                yawStep += mouse.x * perPixel * (g_invertX.load() ? -1.0f : 1.0f);
                pitchStep += mouse.y * perPixel * (g_invertY.load() ? -1.0f : 1.0f);
            }
            g_yaw = wrapAngle(g_yaw - yawStep);
            const float floor = std::fmax(kPitchMin, std::fmin(g_pitchFloor.load(std::memory_order_relaxed), 0.0f));
            g_pitch = std::fmin(std::fmax(g_pitch + pitchStep, floor), kPitchMax);
            g_free = true;

            // Gene faces where the left stick points while he stands or walks (elsewhere the stick the filter made
            // steers him the original way).
            const uint32_t player = ram32(rdram, kPlayerPointer);
            if (g_moving.load(std::memory_order_relaxed) && validPointer(player) && geneMoves(rdram, player))
            {
                const float target = g_moveYaw.load(std::memory_order_relaxed);
                const float step = std::fmin(std::fmax(wrapAngle(target - geneYaw), -kTurnRate * dt), kTurnRate * dt);
                const float turned = wrapAngle(geneYaw + step);
                std::memcpy(rdram + ((player + 0x104u) & 0x1FFFFFFu), &turned, sizeof(turned));
                g_geneYaw.store(turned, std::memory_order_relaxed);
                g_turned = true;
                g_turnedYaw = turned;
            }
        }
        g_cameraYaw.store(g_yaw, std::memory_order_relaxed);
        out.free = true;
        out.yaw = g_yaw;
        out.pitch = g_pitch;
        return out;
    }

    void setPitchFloor(float floor)
    {
        g_pitchFloor.store(floor, std::memory_order_relaxed);
    }

    void resetCamera()
    {
        g_tracking = true;
        g_following = false;
    }

    bool enabled()
    {
        return g_modern.load(std::memory_order_relaxed);
    }

    bool autoEnabled() { return g_auto.load(std::memory_order_relaxed); }

    void noteContact()
    {
        if (g_auto.load(std::memory_order_relaxed) && g_modern.load(std::memory_order_relaxed) &&
            !g_classic.load(std::memory_order_relaxed))
        {
            g_classic = true;
            g_classicByAuto = true;
        }
    }

    bool freeCameraNow()
    {
        return g_modern.load(std::memory_order_relaxed) && !gameHasCamera() && !classicByChoice();
    }

    void install()
    {
        g_modern = envIs("KAMITE_CONTROL_STYLE", "modern");
        g_auto = envBool("KAMITE_MODERN_AUTO", true);
        g_yawSpeed = envFloat("KAMITE_CAMERA_SPEED", 200.0f, 40.0f, 400.0f);
        g_pitchSpeed = envFloat("KAMITE_CAMERA_PITCH_SPEED", 80.0f, 20.0f, 250.0f);
        g_invertX = envBool("KAMITE_CAMERA_INVERT_X", false);
        g_invertY = envBool("KAMITE_CAMERA_INVERT_Y", false);
        g_mouseCamera = envBool("KAMITE_MOUSE_CAMERA", true);
        g_mouseSensitivity = envFloat("KAMITE_MOUSE_SENSITIVITY", 0.1f, 0.01f, 1.0f);
        g_iniPath = cameraIniPath();
        ps2_host_input::setPadFilter(&padFilter);
        KM_INFO(Game, "control style: " << (g_modern.load() ? "modern" : "classic")
                                        << (g_modern.load() ? (g_auto.load() ? " (R3 switches; auto in battle)" : " (R3 switches)") : ""));
#if defined(KAMITE_HAS_GS_MENU)
        static bool s_panel = false;
        if (!s_panel)
        {
            s_panel = true;
            ps2x::menu::addPanel("Controls", &drawControlsPanel);
        }
#endif
    }
}
