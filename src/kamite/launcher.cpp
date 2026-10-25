#include "kamite/launcher.h"

#include "kamite/camera_presets.h"
#include "kamite/character_mods.h"
#include "kamite/game_disc.h"
#include "kamite/ini_file.h"
#include "kamite/launcher_platform.h"
#include "kamite/log.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/ps2_display_options.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "raylib.h"
#include "rlImGui.h"
#include "rlgl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace kamite::launcher
{
    namespace
    {
        namespace fs = std::filesystem;

        // ---- choices ------------------------------------------------------------------------------------------------
        struct Choice
        {
            const char *label;
            const char *value;
        };

        constexpr Choice kAspects[] = {{"4:3 (the PS2's)", "4:3"}, {"16:9", "16:9"},  {"16:10", "16:10"},
                                       {"21:9", "21:9"},          {"32:9", "32:9"},  {"Match the screen", "auto"}};
        constexpr Choice kHudLayouts[] = {{"16:9: HUD at the screen edges", "16:9"}, {"4:3: HUD where the PS2 had it", "4:3"}};
        constexpr Choice kMenuLayouts[] = {{"4:3: as on the PS2, with side bars", "4:3"}, {"Stretched to the screen", "stretch"}};
        constexpr Choice kRouletteTimers[] = {{"Normal", "normal"}, {"Double time", "double"}};
        constexpr Choice kPostEffects[] = {{"PS2 resolution (PCSX2's fix)", "ps2"}, {"Upscaled", "upscaled"}, {"No fix", "off"}};
        constexpr Choice kWindowModes[] = {{"Window", "window"}, {"Fullscreen (borderless)", "fullscreen"}, {"Maximized window", "maximized"}};
        constexpr Choice kFovs[] = {{"100%: as on the PS2", "100"}, {"110%", "110"}, {"120%", "120"}, {"130%", "130"},
                                    {"140%", "140"}, {"150%", "150"}};
        constexpr Choice kCameraDistances[] = {{"Stays: wider view looks zoomed out", "0"}, {"Halfway in", "50"},
                                               {"Comes in: Gene keeps his size", "100"}};
        constexpr Choice kControlStyles[] = {{"Classic", "classic"}, {"Modern", "modern"}};
        constexpr Choice kModernL1[] = {{"Hold", "hold"}, {"Toggle", "toggle"}};
        constexpr Choice kMenuButtons[] = {{"Classic", "classic"}, {"Modern", "modern"}};
        constexpr Choice kButtonPrompts[] = {{"Automatic (follows your controller)", "auto"}, {"Xbox", "xbox"},
                                             {"PlayStation (the game's own)", "playstation"}, {"Nintendo Switch", "nintendo"},
                                             {"Steam Deck / Steam Controller", "steam"}};
        constexpr Choice kCameraTilts[] = {{"None", "0"}, {"5 degrees", "5"}, {"10 degrees", "10"}, {"15 degrees", "15"}};
        constexpr Choice kWindowSizes[] = {{"Automatic (follows internal resolution)", "auto"}, {"PS2 size", "native"}};
        constexpr Choice kResolutions[] = {{"1x  native (512x448)", "1"}, {"2x  1024x896", "2"}, {"3x  1536x1344", "3"},
                                           {"4x  2048x1792", "4"},        {"5x  2560x2240", "5"}, {"6x  3072x2688", "6"},
                                           {"8x  4096x3584", "8"}};

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // A combo over fixed choices; a value that is none of them (set by hand) is shown as one more entry and kept
        // unless another is picked. Returns true when changed (value updated).
        template <size_t N>
        bool choiceCombo(const char *id, const Choice (&choices)[N], std::string &value)
        {
            int current = -1;
            for (size_t i = 0; i < N; ++i)
                if (lower(value) == lower(choices[i].value))
                    current = static_cast<int>(i);
            std::vector<const char *> labels;
            for (const Choice &choice : choices)
                labels.push_back(choice.label);
            const std::string custom = "Custom: " + value;
            if (current < 0)
            {
                labels.push_back(custom.c_str());
                current = static_cast<int>(N);
            }
            int picked = current;
            ImGui::SetNextItemWidth(-1.0f);
            if (!ImGui::Combo(id, &picked, labels.data(), static_cast<int>(labels.size())) || picked == current || picked >= static_cast<int>(N))
                return false;
            value = choices[picked].value;
            return true;
        }

        // ---- look ---------------------------------------------------------------------------------------------------
        const ImVec4 kAccent(0.91f, 0.38f, 0.16f, 1.0f);      // The game's fire orange
        const ImVec4 kAccentHover(1.00f, 0.50f, 0.22f, 1.0f);
        const ImVec4 kAccentActive(0.78f, 0.28f, 0.10f, 1.0f);
        const ImVec4 kPanel(0.10f, 0.09f, 0.09f, 0.92f);
        const ImVec4 kTextDim(0.66f, 0.62f, 0.58f, 1.0f);

        void applyStyle()
        {
            ImGuiStyle &style = ImGui::GetStyle();
            ImGui::StyleColorsDark(&style);
            style.WindowRounding = 0.0f;
            style.FrameRounding = 6.0f;
            style.PopupRounding = 6.0f;
            style.GrabRounding = 6.0f;
            style.TabRounding = 6.0f;
            style.FramePadding = ImVec2(12.0f, 8.0f);
            style.ItemSpacing = ImVec2(12.0f, 12.0f);
            style.WindowPadding = ImVec2(0.0f, 0.0f);
            ImVec4 *colors = style.Colors;
            colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
            colors[ImGuiCol_ChildBg] = kPanel;
            colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.11f, 0.10f, 0.98f);
            colors[ImGuiCol_Border] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.16f, 0.15f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.20f, 0.17f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.22f, 0.17f, 1.0f);
            colors[ImGuiCol_Button] = ImVec4(0.20f, 0.17f, 0.15f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = kAccentHover;
            colors[ImGuiCol_ButtonActive] = kAccentActive;
            colors[ImGuiCol_Header] = ImVec4(0.30f, 0.20f, 0.15f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = kAccentHover;
            colors[ImGuiCol_HeaderActive] = kAccentActive;
            colors[ImGuiCol_CheckMark] = kAccent;
            colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.15f, 0.13f, 1.0f);
            colors[ImGuiCol_TabHovered] = kAccentHover;
            colors[ImGuiCol_TabSelected] = kAccent;
            colors[ImGuiCol_NavCursor] = kAccentHover;
            colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_Text] = ImVec4(0.95f, 0.92f, 0.88f, 1.0f);
            colors[ImGuiCol_TextDisabled] = kTextDim;
        }

        void drawBackground()
        {
            const int width = GetScreenWidth(), height = GetScreenHeight();
            DrawRectangleGradientV(0, 0, width, height, Color{20, 15, 13, 255}, Color{48, 20, 10, 255});
            // A low glow behind the title.
            DrawCircleGradient(width / 2, height / 4, static_cast<float>(std::max(width, height)) * 0.45f, Color{120, 45, 15, 70},
                               Color{0, 0, 0, 0});
        }

        // Every controller, as ImGui's gamepad keys (SDL3 through ps2_host_gamepads: the same devices the game uses).
        void feedGamepads(ImGuiIO &io)
        {
            using namespace ps2_host_gamepads;
            update();
            bool held[16] = {};
            float lx = 0.0f, ly = 0.0f;
            for (int device = 0; device < kMaxDevices; ++device)
            {
                if (!connected(device))
                    continue;
                held[0] |= button(device, DPadUp);
                held[1] |= button(device, DPadDown);
                held[2] |= button(device, DPadLeft);
                held[3] |= button(device, DPadRight);
                held[4] |= button(device, South);
                held[5] |= button(device, East);
                held[6] |= button(device, West);
                held[7] |= button(device, North);
                held[8] |= button(device, LeftShoulder);
                held[9] |= button(device, RightShoulder);
                held[10] |= button(device, Start);
                held[11] |= button(device, Back);
                if (std::fabs(axis(device, LeftX)) > std::fabs(lx))
                    lx = axis(device, LeftX);
                if (std::fabs(axis(device, LeftY)) > std::fabs(ly))
                    ly = axis(device, LeftY);
            }
            const ImGuiKey keys[] = {ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                                     ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp,
                                     ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,       ImGuiKey_GamepadStart,    ImGuiKey_GamepadBack};
            for (int i = 0; i < 12; ++i)
                io.AddKeyEvent(keys[i], held[i]);
            auto stick = [&](ImGuiKey negative, ImGuiKey positive, float value)
            {
                const float dead = 0.35f;
                const float n = value < -dead ? std::min(1.0f, (-value - dead) / (1.0f - dead)) : 0.0f;
                const float p = value > dead ? std::min(1.0f, (value - dead) / (1.0f - dead)) : 0.0f;
                io.AddKeyAnalogEvent(negative, n > 0.0f, n);
                io.AddKeyAnalogEvent(positive, p > 0.0f, p);
            };
            stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
            stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ly);
        }

        // ---- the screens --------------------------------------------------------------------------------------------
        enum class Screen
        {
            Home,
            Install,
            Options,
        };

        struct State
        {
            const Context *context = nullptr;
            IniFile ini;
            Screen screen = Screen::Home;
            bool installed = false;
            std::string installMessage;
            bool installError = false;
            char isoText[1024] = {};
            bool focusFirst = true; // put the gamepad/keyboard cursor on the screen's first button
            bool done = false;
            Result result = Result::Quit;
            ImFont *font = nullptr;
            float unit = 1.0f; // UI scale: window height / 720
            Texture2D logo{};  // resources/images/logo.png when present (the project's own logo)
            Texture2D burst{}; // resources/images/burst.png: the speed lines behind the logo
            // The title's motion: the cursor (eased, plus a slow drift of its own) shifts the burst layers and the
            // logo by different amounts, the bursts turn slowly and pulse.
            float time = 0.0f;
            ImVec2 parallax{0.0f, 0.0f}; // -1..1 each way
            float burstShown = 0.0f;     // fades the burst in on the screens with the logo, out on the others
            bool logoPlaced = false;     // logoMin/logoMax hold last frame's logo rectangle (without the parallax shift)
            ImVec2 logoMin{}, logoMax{};
        };

        // The wordmark's centre inside logo.png (tools/logo/build_logo.py prints it); the burst is centred there.
        constexpr float kLogoCentreX = 0.480f, kLogoCentreY = 0.737f;

        void updateMotion(State &state, Screen screen)
        {
            const float dt = std::min(GetFrameTime(), 0.1f);
            state.time += dt;
            const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
            ImVec2 target(0.0f, 0.0f);
            if (IsCursorOnScreen() && width > 0.0f && height > 0.0f)
            {
                const Vector2 mouse = GetMousePosition();
                target.x = std::clamp((mouse.x - width * 0.5f) / (width * 0.5f), -1.0f, 1.0f);
                target.y = std::clamp((mouse.y - height * 0.5f) / (height * 0.5f), -1.0f, 1.0f);
            }
            target.x += 0.15f * std::sin(state.time * 0.23f);
            target.y += 0.15f * std::cos(state.time * 0.31f);
            const float ease = 1.0f - std::exp(-dt * 3.0f);
            state.parallax.x += (target.x - state.parallax.x) * ease;
            state.parallax.y += (target.y - state.parallax.y) * ease;
            const float shown = screen == Screen::Options ? 0.0f : 1.0f;
            state.burstShown += (shown - state.burstShown) * (1.0f - std::exp(-dt * 5.0f));
        }

        // One layer of the burst: the texture centred at (x, y), radius in pixels, squashed vertically, turned.
        void drawBurstLayer(const Texture2D &burst, float x, float y, float radius, float angle, unsigned char alpha)
        {
            const float size = radius * 2.0f;
            rlPushMatrix();
            rlTranslatef(x, y, 0.0f);
            rlScalef(1.0f, 0.75f, 1.0f);
            rlRotatef(angle, 0.0f, 0.0f, 1.0f);
            DrawTexturePro(burst, Rectangle{0.0f, 0.0f, static_cast<float>(burst.width), static_cast<float>(burst.height)},
                           Rectangle{0.0f, 0.0f, size, size}, Vector2{radius, radius}, 0.0f, Color{255, 255, 255, alpha});
            rlPopMatrix();
        }

        void drawBurst(const State &state)
        {
            if (state.burst.id == 0u || state.burstShown < 0.01f)
                return;
            const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
            const float diagonal = std::hypot(width, height);
            float x = width * 0.5f, y = height * 0.3f;
            if (state.logoPlaced)
            {
                x = state.logoMin.x + (state.logoMax.x - state.logoMin.x) * kLogoCentreX;
                y = state.logoMin.y + (state.logoMax.y - state.logoMin.y) * kLogoCentreY;
            }
            const float unit = state.unit, t = state.time, shown = state.burstShown;
            const ImVec2 p = state.parallax;
            // Back layer: bigger, dimmer, turns one way and shifts the most; front layer: the other way, less.
            drawBurstLayer(state.burst, x - p.x * 36.0f * unit, y - p.y * 36.0f * unit,
                           diagonal * 0.80f * (1.0f + 0.02f * std::sin(t * 1.1f + 1.0f)), t * 1.2f,
                           static_cast<unsigned char>(45.0f * shown));
            drawBurstLayer(state.burst, x - p.x * 18.0f * unit, y - p.y * 18.0f * unit,
                           diagonal * 0.62f * (1.0f + 0.03f * std::sin(t * 1.6f)), 40.0f - t * 2.0f,
                           static_cast<unsigned char>(70.0f * shown));
        }

        float fontSize(const State &state, float base) { return std::round(base * state.unit); }

        void pushSize(const State &state, float base) { ImGui::PushFont(state.font, fontSize(state, base)); }

        void centeredText(const char *text, const ImVec4 &color)
        {
            const float width = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
            ImGui::TextColored(color, "%s", text);
        }

        bool bigButton(const State &state, const char *label, float width, bool accent)
        {
            if (accent)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
            }
            pushSize(state, 30.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            const bool clicked = ImGui::Button(label, ImVec2(width, fontSize(state, 64.0f)));
            ImGui::PopFont();
            if (accent)
                ImGui::PopStyleColor(2);
            return clicked;
        }

        void title(State &state)
        {
            if (state.logo.id != 0u && state.logo.height > 0)
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
                // Big: as wide as fits, at most 44% of the window's height.
                const float aspect = static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width);
                const float width = std::min({ImGui::GetWindowWidth() * 0.92f, fontSize(state, 1150.0f),
                                              ImGui::GetWindowHeight() * 0.44f / aspect});
                const float height = width * aspect;
                const ImVec2 start((ImGui::GetWindowWidth() - width) * 0.5f, ImGui::GetCursorPosY());
                const ImVec2 shift(state.parallax.x * fontSize(state, 8.0f), state.parallax.y * fontSize(state, 8.0f));
                ImGui::SetCursorPos(start);
                const ImVec2 screenStart = ImGui::GetCursorScreenPos();
                ImGui::SetCursorPos(ImVec2(start.x + shift.x, start.y + shift.y));
                rlImGuiImageSize(&state.logo, static_cast<int>(width), static_cast<int>(height));
                state.logoMin = screenStart;
                state.logoMax = ImVec2(screenStart.x + width, screenStart.y + height);
                state.logoPlaced = true;
                ImGui::SetCursorPos(ImVec2(0.0f, start.y + height));
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            }
            else
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 70.0f)));
                pushSize(state, 92.0f);
                centeredText("K A M I T E", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
                ImGui::PopFont();
            }
            pushSize(state, 22.0f);
            centeredText("God Hand\u2122 for PS2 Recompiled, Refined, Rearmed.", kTextDim);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 40.0f)));
        }

        bool checkImage(State &state, const fs::path &image)
        {
            if (image.empty())
            {
                state.installMessage = "Choose your disc image (USA, SLUS-21503).";
                state.installError = true;
                return false;
            }
            std::error_code error;
            if (!fs::is_regular_file(image, error))
            {
                state.installMessage = "There is no file at " + utf8(image) + ".";
                state.installError = true;
                return false;
            }
            const DiscCheckResult check = verifyDiscAndPrepare(image, state.context->cacheDir);
            if (!check.ok)
            {
                state.installMessage = check.error;
                state.installError = true;
                return false;
            }
            state.ini.set("Game", "Iso", utf8(image));
            state.ini.save();
            state.installed = true;
            state.installMessage.clear();
            state.installError = false;
            KM_INFO(Boot, "launcher: disc image installed: " << utf8(image));
            return true;
        }

        void drawHome(State &state)
        {
            title(state);
            const float buttonWidth = fontSize(state, 420.0f);
            if (!state.installed)
            {
                pushSize(state, 22.0f);
                centeredText("To play, point the port to your own copy of the game.", kTextDim);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (bigButton(state, "Install", buttonWidth, true))
                {
                    state.screen = Screen::Install;
                    state.focusFirst = true;
                }
                if (state.focusFirst)
                    ImGui::SetItemDefaultFocus();
            }
            else
            {
                if (bigButton(state, "Play", buttonWidth, true))
                {
                    state.result = Result::Play;
                    state.done = true;
                }
                if (state.focusFirst)
                    ImGui::SetItemDefaultFocus();
                if (bigButton(state, "Options", buttonWidth, false))
                {
                    state.screen = Screen::Options;
                    state.focusFirst = true;
                }
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
                pushSize(state, 20.0f);
                bool hide = !state.ini.getBool("Launcher", "Show", true);
                const char *label = "Don't show this again (hold Shift while starting to see it)";
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(label).x - ImGui::GetFrameHeight() -
                                      ImGui::GetStyle().ItemInnerSpacing.x) *
                                     0.5f);
                if (ImGui::Checkbox(label, &hide))
                {
                    state.ini.setBool("Launcher", "Show", !hide);
                    state.ini.save();
                }
                ImGui::PopFont();
            }
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
            pushSize(state, 22.0f);
            const float quitWidth = fontSize(state, 160.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - quitWidth) * 0.5f);
            if (ImGui::Button("Quit", ImVec2(quitWidth, 0.0f)))
            {
                state.result = Result::Quit;
                state.done = true;
            }
            ImGui::PopFont();
            // The rights notice at the foot of the title screen.
            {
                pushSize(state, 16.0f);
                const char *lines[] = {"God Hand\u2122 is a trademark of CAPCOM CO., LTD. All rights to the game belong to Capcom.",
                                       "Kamite is a fan-made project, not affiliated with or endorsed by Capcom or Clover Studio. "
                                       "It contains no game data: play your own copy."};
                const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                const float y = ImGui::GetWindowHeight() - lineHeight * 2.0f - fontSize(state, 18.0f);
                if (ImGui::GetCursorPosY() < y)
                    ImGui::SetCursorPosY(y);
                for (const char *line : lines)
                    centeredText(line, kTextDim);
                ImGui::PopFont();
            }
            state.focusFirst = false;
        }

        void drawInstall(State &state)
        {
            title(state);
            const float width = fontSize(state, 760.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 22.0f);
            ImGui::BeginChild("install", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders |
                                                                  ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::TextWrapped("The port plays from your own disc image (USA, SLUS-21503, the .iso of the disc). "
                               "Nothing is copied: the game reads it while you play, so keep it where it is.");
            ImGui::Spacing();
            if (ImGui::Button("Choose disc image..."))
            {
                const fs::path picked = platform::pick(GetWindowHandle(), false);
                if (!picked.empty())
                {
                    const std::string text = utf8(picked);
                    std::snprintf(state.isoText, sizeof(state.isoText), "%s", text.c_str());
                    checkImage(state, picked);
                }
            }
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            ImGui::TextColored(kTextDim, "or type its path:");
            ImGui::SetNextItemWidth(-1.0f);
            const bool entered = ImGui::InputText("##iso", state.isoText, sizeof(state.isoText), ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Check and install") || entered)
            {
                std::string text = state.isoText;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                checkImage(state, fromUtf8(text));
            }
            if (!state.installMessage.empty())
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, state.installError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : kTextDim);
                ImGui::TextWrapped("%s", state.installMessage.c_str());
                ImGui::PopStyleColor();
            }
            if (state.installed)
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.45f, 1.0f), "Installed. Your disc image checks out.");
            }
            ImGui::Spacing();
            if (ImGui::Button(state.installed ? "Continue" : "Back"))
            {
                state.screen = Screen::Home;
                state.focusFirst = true;
            }
            ImGui::EndChild();
            ImGui::PopFont();
            state.focusFirst = false;
        }

        // ---- settings tables --------------------------------------------------------------------------------------
        // Every setting is a row (name and help on the left, the control on the right) with a line above it; a
        // setting's own sub-options (subSetting) share its section, with no line between them.
        int g_settingRows = 0;

        void dividerLine()
        {
            ImGuiWindow *window = ImGui::GetCurrentWindow();
            const float y = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
            const float x0 = window->InnerRect.Min.x + ImGui::GetStyle().WindowPadding.x;
            const float x1 = window->InnerRect.Max.x - ImGui::GetStyle().WindowPadding.x;
            ImGui::PushClipRect(window->InnerRect.Min, window->InnerRect.Max, false);
            window->DrawList->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(ImVec4(0.35f, 0.22f, 0.15f, 0.55f)), 1.0f);
            ImGui::PopClipRect();
        }

        void describe(const char *name, const char *help, const char *note)
        {
            ImGui::TextUnformatted(name);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            if (help && *help)
                ImGui::TextWrapped("%s", help);
            if (note && *note)
                ImGui::TextWrapped("NOTE: %s", note);
            ImGui::PopStyleColor();
        }

        // A setting in a section of its own: a line with some room above and below it, then the name and help on the
        // left (`note`: a line of its own at the bottom of the help, "NOTE: ..."; `below`: more controls under the help,
        // such as Camera's "Show advanced settings"), the control on the right.
        void settingLabel(const char *name, const char *help, const char *note = nullptr, const std::function<void()> &below = {})
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool divided = g_settingRows++ > 0;
            const float gap = std::round(ImGui::GetFontSize() * 0.45f), spacing = ImGui::GetStyle().ItemSpacing.y;
            if (divided)
            {
                ImGui::Dummy(ImVec2(0.0f, gap));
                dividerLine();
                ImGui::Dummy(ImVec2(0.0f, gap));
            }
            describe(name, help, note);
            if (below)
                below();
            ImGui::TableSetColumnIndex(1);
            if (divided)
                ImGui::Dummy(ImVec2(0.0f, 2.0f * gap + spacing)); // the control lines up with the name
        }

        // A sub-option in the section of the setting above it.
        void subSetting(const char *name, const char *help, const char *note = nullptr)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(ImGui::GetFontSize());
            describe(name, help, note);
            ImGui::Unindent(ImGui::GetFontSize());
            ImGui::TableSetColumnIndex(1);
        }

        bool beginSettings(const char *id)
        {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
                return false;
            g_settingRows = 0;
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.58f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.42f);
            return true;
        }

        void iniCombo(State &state, const char *id, const char *section, const char *key, const char *fallback,
                      bool (*combo)(const char *, std::string &))
        {
            std::string value = state.ini.get(section, key, fallback);
            if (value.empty())
                value = fallback;
            if (combo(id, value))
                state.ini.set(section, key, value);
        }

        void iniCheckbox(State &state, const char *id, const char *section, const char *key, bool fallback)
        {
            bool value = state.ini.getBool(section, key, fallback);
            if (ImGui::Checkbox(id, &value))
                state.ini.setBool(section, key, value);
        }

        // Display > Camera: the presets of camera_presets.h; "Custom" when the four values match none of them.
        void cameraPresetCombo(State &state)
        {
            const auto value = [&](const char *key, float fallback)
            {
                const std::string text = state.ini.get("Video", key, "");
                return text.empty() ? fallback : static_cast<float>(std::atof(text.c_str()));
            };
            const float fov = value("FieldOfView", 100.0f), fight = value("FightFieldOfView", 100.0f);
            const float distance = value("CameraDistance", 100.0f), tilt = value("CameraTilt", 0.0f);
            const auto same = [](float a, float b) { return std::fabs(a - b) < 0.05f; };
            int current = -1;
            for (size_t i = 0; i < std::size(kCameraPresets); ++i)
            {
                const CameraPreset &preset = kCameraPresets[i];
                if (same(fov, preset.fieldOfView) && same(fight, preset.fightFieldOfView) &&
                    same(distance, preset.cameraDistance) && same(tilt, preset.cameraTilt))
                    current = static_cast<int>(i);
            }
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::BeginCombo("##camera", current >= 0 ? kCameraPresets[current].name : "Custom"))
            {
                for (size_t i = 0; i < std::size(kCameraPresets); ++i)
                {
                    const CameraPreset &preset = kCameraPresets[i];
                    if (ImGui::Selectable(preset.name, static_cast<int>(i) == current))
                    {
                        const auto text = [](float number)
                        {
                            char buffer[32];
                            std::snprintf(buffer, sizeof(buffer), "%g", number);
                            return std::string(buffer);
                        };
                        state.ini.set("Video", "FieldOfView", text(preset.fieldOfView));
                        state.ini.set("Video", "FightFieldOfView", text(preset.fightFieldOfView));
                        state.ini.set("Video", "CameraDistance", text(preset.cameraDistance));
                        state.ini.set("Video", "CameraTilt", text(preset.cameraTilt));
                    }
                }
                ImGui::EndCombo();
            }
        }

        // ---- Controls: player 1's bindings (controls.ini through ps2_host_input's model, as the F1 screen) ----------
        namespace pc = ps2_host_input::controls;
        namespace gp = ps2_host_gamepads;

        struct ControlRow
        {
            const char *key;    // controls.ini [Pad1] key
            const char *action; // what it does in the game ("" = nothing known)
            const char *button; // the PS2 button
        };
        // The F1 screen's rows (ps2_controls_screen.cpp kRows).
        constexpr ControlRow kControlRows[] = {
            {"Cross", "Attack / OK", "Cross"},
            {"Circle", "Action / prompt", "Circle"},
            {"Square", "Combo", "Square"},
            {"Triangle", "Attack / cancel", "Triangle"},
            {"L1", "Turn around", "L1"},
            {"R1", "God Reel", "R1"},
            {"L2", "Taunt", "L2"},
            {"R2", "Unleash", "R2"},
            {"L3", "", "L3 (left stick click)"},
            {"R3", "", "R3 (right stick click)"},
            {"Start", "Pause menu", "Start"},
            {"Select", "", "Select"},
            {"Up", "Menu up", "D-pad up"},
            {"Down", "Menu down", "D-pad down"},
            {"Left", "Menu left", "D-pad left"},
            {"Right", "Menu right", "D-pad right"},
            {"LUp", "Move forward", "Left stick up"},
            {"LDown", "Step back", "Left stick down"},
            {"LLeft", "Move left", "Left stick left"},
            {"LRight", "Move right", "Left stick right"},
            {"RUp", "Duck", "Right stick up"},
            {"RDown", "Back flip", "Right stick down"},
            {"RLeft", "Sidestep left", "Right stick left"},
            {"RRight", "Sidestep right", "Right stick right"},
        };
        constexpr int kControlRowCount = static_cast<int>(std::size(kControlRows));
        constexpr int kKeyColumns = 2; // Key 1, Key 2 (the F1 screen also has a third; it is kept as it is)
        constexpr uint32_t kPlayer = 0u;

        struct Capture
        {
            bool active = false;
            bool controller = false; // false: keys and mouse
            int row = 0;
            int column = 0;          // keys: 0..kKeyColumns-1
            bool waitRelease = true; // the click or button press that started it must end first
            double started = 0.0;
        };
        Capture g_capture;
        bool g_openCapture = false;   // open the popup at drawControls' ID level (cells push their own IDs)
        bool g_captureFrame = false;  // a capture ran this frame: its Esc / B must not also leave Options
        std::string g_controlsStatus;
        bool g_resetArmed = false;

        struct Split
        {
            std::vector<std::string> keys, pads, other;
        };

        Split splitRow(int row)
        {
            Split split;
            for (std::string &binding : pc::bindings(kPlayer, kControlRows[row].key))
            {
                switch (pc::kindOf(binding))
                {
                case pc::Kind::Keys: split.keys.push_back(std::move(binding)); break;
                case pc::Kind::Controller: split.pads.push_back(std::move(binding)); break;
                default: split.other.push_back(std::move(binding)); break;
                }
            }
            return split;
        }

        void storeRow(int row, const Split &split)
        {
            std::vector<std::string> list = split.keys;
            list.insert(list.end(), split.pads.begin(), split.pads.end());
            list.insert(list.end(), split.other.begin(), split.other.end());
            pc::setBindings(kPlayer, kControlRows[row].key, std::move(list));
        }

        std::string rowName(int row)
        {
            const ControlRow &r = kControlRows[row];
            return *r.action ? std::string(r.action) + " (" + r.button + ")" : std::string(r.button);
        }

        // As the F1 screen: the binding goes into the cell; within one action it appears once; another action may
        // share it (the status line says so).
        void bindCell(int row, bool controller, int column, const std::string &binding)
        {
            Split split = splitRow(row);
            std::vector<std::string> &list = controller ? split.pads : split.keys;
            size_t at = controller ? 0u : static_cast<size_t>(column);
            if (at < list.size())
                list[at] = binding;
            else
            {
                list.push_back(binding);
                at = list.size() - 1u;
            }
            for (size_t i = list.size(); i-- > 0u;)
                if (i != at && pc::sameBinding(list[i], binding))
                {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                    if (i < at)
                        --at;
                }
            storeRow(row, split);
            g_controlsStatus = rowName(row) + ": " + pc::displayName(binding);
            for (int other = 0; other < kControlRowCount; ++other)
            {
                if (other == row)
                    continue;
                bool shared = false;
                for (const std::string &existing : pc::bindings(kPlayer, kControlRows[other].key))
                    shared = shared || pc::sameBinding(existing, binding);
                if (shared)
                {
                    g_controlsStatus += "   (also on " + rowName(other) + ")";
                    break;
                }
            }
            KM_INFO(Boot, "launcher: controls " << kControlRows[row].key << " = " << binding);
        }

        void clearCell(int row, bool controller, int column)
        {
            Split split = splitRow(row);
            std::vector<std::string> &list = controller ? split.pads : split.keys;
            const size_t at = controller ? 0u : static_cast<size_t>(column);
            if (at >= list.size())
                return;
            const std::string removed = list[at];
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(at));
            storeRow(row, split);
            g_controlsStatus = rowName(row) + ": " + pc::displayName(removed) + " removed";
        }

        bool anyPadInput()
        {
            for (int device = 0; device < gp::kMaxDevices; ++device)
            {
                if (!gp::connected(device))
                    continue;
                for (int b = 0; b < gp::ButtonCount; ++b)
                    if (gp::button(device, static_cast<gp::Button>(b)))
                        return true;
                for (int a = 0; a < gp::AxisCount; ++a)
                    if (std::fabs(gp::axis(device, static_cast<gp::Axis>(a))) > 0.3f)
                        return true;
            }
            return false;
        }

        // The capture in progress (every frame while its popup is open): true when it ended.
        bool captureTick()
        {
            if (IsKeyPressed(KEY_ESCAPE) || GetTime() - g_capture.started > 10.0)
            {
                g_controlsStatus = IsKeyPressed(KEY_ESCAPE) ? "Cancelled." : "Nothing pressed for 10 s: cancelled.";
                return true;
            }
            if (g_capture.waitRelease)
            {
                const bool held = g_capture.controller ? anyPadInput()
                                                       : (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
                                                          IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) || IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_SPACE));
                if (!held)
                    g_capture.waitRelease = false;
                return false;
            }
            if (!g_capture.controller)
            {
                for (int key : pc::bindableKeys())
                    if (IsKeyPressed(key))
                    {
                        const std::string binding = pc::keyBinding(key);
                        if (!binding.empty())
                        {
                            bindCell(g_capture.row, false, g_capture.column, binding);
                            return true;
                        }
                    }
                for (int mouse : {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE})
                    if (IsMouseButtonPressed(mouse))
                    {
                        bindCell(g_capture.row, false, g_capture.column, pc::mouseBinding(mouse));
                        return true;
                    }
                return false;
            }
            for (int device = 0; device < gp::kMaxDevices; ++device)
            {
                if (!gp::connected(device))
                    continue;
                for (int b = 0; b < gp::ButtonCount; ++b)
                    if (gp::button(device, static_cast<gp::Button>(b)))
                    {
                        bindCell(g_capture.row, true, 0, pc::padButtonBinding(device, b));
                        return true;
                    }
                for (int a = 0; a < gp::AxisCount; ++a)
                {
                    const float v = gp::axis(device, static_cast<gp::Axis>(a));
                    if (std::fabs(v) > 0.6f)
                    {
                        bindCell(g_capture.row, true, 0, pc::padAxisBinding(device, a, v < 0.0f));
                        return true;
                    }
                }
            }
            return false;
        }

        void startCapture(int row, bool controller, int column)
        {
            g_capture = Capture{true, controller, row, column, true, GetTime()};
            g_openCapture = true;
        }

        // One cell: click (or A/Enter) to bind, right-click (or X / Delete when focused) to clear.
        void bindingCell(int row, bool controller, int column, const std::string &binding, float width)
        {
            ImGui::PushID(row * 8 + (controller ? 7 : column));
            const std::string label = (binding.empty() ? std::string("-") : pc::displayName(binding)) + "##cell";
            if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f)))
                startCapture(row, controller, column);
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                clearCell(row, controller, column);
            if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                                           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
                clearCell(row, controller, column);
            ImGui::PopID();
        }

        void drawControls(State &state)
        {
            if (beginSettings("controls"))
            {
                settingLabel("Button prompts", "The buttons the game shows for a controller. Automatic picks them from the "
                                               "controller you use; keys and mouse always show keys. Prompt art: Xelu's "
                                               "free prompts (CC0).");
                iniCombo(state, "##buttonprompts", "Game", "ButtonPrompts", "auto",
                         [](const char *id, std::string &value) { return choiceCombo(id, kButtonPrompts, value); });
                ImGui::EndTable();
            }
            ImGui::Spacing();
            static int device = 0; // 0 keyboard & mouse, 1 controller
            ImGui::TextUnformatted("Bindings for");
            ImGui::SameLine();
            ImGui::RadioButton("Keyboard & mouse", &device, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Controller", &device, 1);
            const bool controller = device == 1;
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            ImGui::TextWrapped("%s", controller ? "Click a button, then press the controller button or move the stick or "
                                                  "trigger it should be. Right-click (or X) clears it. Esc cancels."
                                                : "Click a box, then press the key or click the mouse button it should be. "
                                                  "Right-click (or Delete) clears it. Esc cancels; F-keys stay the port's "
                                                  "shortcuts.");
            if (pc::envOverride(kPlayer))
                ImGui::TextWrapped("PS2X_PAD1 is set in the environment: it wins over these bindings while it is set.");
            ImGui::PopStyleColor();
            if (ImGui::BeginTable("bindings", controller ? 2 : 1 + kKeyColumns,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("PS2 button", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                if (controller)
                    ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                else
                {
                    ImGui::TableSetupColumn("Key 1", ImGuiTableColumnFlags_WidthStretch, 0.27f);
                    ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch, 0.27f);
                }
                ImGui::TableHeadersRow();
                for (int row = 0; row < kControlRowCount; ++row)
                {
                    const Split split = splitRow(row);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(kControlRows[row].button);
                    if (*kControlRows[row].action)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(kTextDim, "%s", kControlRows[row].action);
                    }
                    if (controller)
                    {
                        ImGui::TableSetColumnIndex(1);
                        bindingCell(row, true, 0, split.pads.empty() ? std::string() : split.pads[0], -1.0f);
                    }
                    else
                    {
                        for (int column = 0; column < kKeyColumns; ++column)
                        {
                            ImGui::TableSetColumnIndex(1 + column);
                            bindingCell(row, false, column, column < static_cast<int>(split.keys.size()) ? split.keys[column] : std::string(),
                                        -1.0f);
                        }
                    }
                }
                ImGui::EndTable();
            }
            if (!g_controlsStatus.empty())
                ImGui::TextColored(kTextDim, "%s", g_controlsStatus.c_str());
            if (ImGui::Button(g_resetArmed ? "Click again to reset every binding" : "Reset all bindings to the defaults"))
            {
                if (g_resetArmed)
                {
                    pc::resetDefaults(kPlayer);
                    g_controlsStatus = "Default bindings restored.";
                }
                g_resetArmed = !g_resetArmed;
            }
            ImGui::SameLine();
            if (ImGui::Button("Open controls.ini"))
            {
                std::string message;
                pc::save(message);
                platform::open(state.context->exeDir / "controls.ini");
            }

            // The capture popup.
            if (g_openCapture)
            {
                g_openCapture = false;
                ImGui::OpenPopup("##capture");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("##capture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
            {
                g_captureFrame = true;
                ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 0.5f));
                ImGui::TextUnformatted(rowName(g_capture.row).c_str());
                ImGui::TextColored(kTextDim, "%s", g_capture.controller ? "Press a controller button or move a stick or trigger..."
                                                                       : "Press a key or click a mouse button...");
                ImGui::TextColored(kTextDim, "Esc cancels (%d s)", std::max(0, 10 - static_cast<int>(GetTime() - g_capture.started)));
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
                if (captureTick())
                {
                    g_capture.active = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        void saveControls()
        {
            if (!pc::dirty())
                return;
            std::string message;
            if (!pc::save(message))
                KM_WARN(Boot, "launcher: controls not saved: " << message);
        }

        void drawOptions(State &state)
        {
            g_captureFrame = false;
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 30.0f)));
            pushSize(state, 46.0f);
            centeredText("Options", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
            ImGui::PopFont();
            const float width = std::min(ImGui::GetWindowWidth() - fontSize(state, 60.0f), fontSize(state, 1040.0f));
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 21.0f);
            const float footer = ImGui::GetFrameHeightWithSpacing() + fontSize(state, 36.0f);
            ImGui::BeginChild("options", ImVec2(width, ImGui::GetContentRegionAvail().y - footer),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            const bool advanced = state.ini.getBool("Launcher", "ShowAdvanced", false);
            if (ImGui::BeginTabBar("tabs"))
            {
                const std::string aspect = state.ini.get("Video", "AspectRatio", "16:9");
                if (ImGui::BeginTabItem("Display"))
                {
                    if (beginSettings("display"))
                    {
                        settingLabel("Aspect ratio", "The picture's shape. Wider than 4:3 shows more of the scene to the "
                                                     "sides; menus keep the PS2's proportions.");
                        iniCombo(state, "##aspect", "Video", "AspectRatio", "16:9",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kAspects, value); });
                        settingLabel("HUD layout", "Where the health bar, radar and the rest of the in-game HUD go on a "
                                                   "wide screen. Wider than 16:9 keeps them at the edges of a 16:9 area.",
                                     "F7 switches it while playing.");
                        ImGui::BeginDisabled(lower(aspect) == "4:3");
                        iniCombo(state, "##hud", "Video", "HudLayout", "16:9",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kHudLayouts, value); });
                        ImGui::EndDisabled();
                        settingLabel("Menu width", "Menus and screens (pause, options, roulette setup) are drawn for 4:3: "
                                                   "shown 4:3 with side bars, or stretched to the screen.");
                        ImGui::BeginDisabled(lower(aspect) == "4:3");
                        iniCombo(state, "##menus", "Video", "MenuLayout", "4:3",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kMenuLayouts, value); });
                        ImGui::EndDisabled();
                        settingLabel("Camera", "How much the gameplay camera shows and how it frames Gene (widescreen "
                                               "already widens it sideways). Cutscenes keep their framing.",
                                     "F2 while playing tunes it live.",
                                     [&] { iniCheckbox(state, "Show advanced settings##advanced", "Launcher", "ShowAdvanced", false); });
                        cameraPresetCombo(state);
                        if (advanced)
                        {
                            subSetting("Field of view", "How much the camera shows, both ways, outside fights.");
                            iniCombo(state, "##fov", "Video", "FieldOfView", "100",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kFovs, value); });
                            subSetting("Field of view in fights", "The same while the game's battle camera is on (Gene "
                                                                  "has an enemy targeted). 100% frames fights as on the PS2.");
                            iniCombo(state, "##fightfov", "Video", "FightFieldOfView", "100",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kFovs, value); });
                            subSetting("Camera distance", "At a wider field of view, how far the camera comes in toward "
                                                          "Gene. Coming all the way in keeps him the size he is at 100%.");
                            iniCombo(state, "##camdist", "Video", "CameraDistance", "100",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kCameraDistances, value); });
                            subSetting("Camera tilt", "At a wider field of view, the camera also rises and looks down at "
                                                      "Gene by this much: more of the floor ahead, less sky.");
                            iniCombo(state, "##camtilt", "Video", "CameraTilt", "0",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kCameraTilts, value); });
                            subSetting("Enemies follow the view", "The angles round Gene in which enemies close in and "
                                                                  "lunge widen with the picture, so the ones you can see at "
                                                                  "the edges join in as on the PS2. Off: the original angles.");
                            iniCheckbox(state, "On##enemyview", "Game", "EnemiesFollowView", true);
                        }
                        settingLabel("Window", "How the game opens.", "F11 switches between window and fullscreen while "
                                                                      "playing.");
                        {
                            std::string mode = state.ini.getBool("Video", "StartFullscreen", false)  ? "fullscreen"
                                               : state.ini.getBool("Video", "StartMaximized", false) ? "maximized"
                                                                                                     : "window";
                            if (choiceCombo("##window", kWindowModes, mode))
                            {
                                state.ini.setBool("Video", "StartFullscreen", mode == "fullscreen");
                                state.ini.setBool("Video", "StartMaximized", mode == "maximized");
                            }
                        }
                        subSetting("Window size", "The window's size when it opens (not maximized or fullscreen).");
                        iniCombo(state, "##size", "Video", "WindowSize", "auto",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kWindowSizes, value); });
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Graphics"))
                {
                    if (beginSettings("graphics"))
                    {
                        settingLabel("Internal resolution", "How sharp the 3D is drawn. 3x suits a 1440p screen, 4x and "
                                                            "up a 4K one; higher costs more GPU time.");
                        iniCombo(state, "##scale", "Video", "InternalResolution", "3",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kResolutions, value); });
                        settingLabel("Post effects", "The blur effects (depth of field, heat haze) are drawn from small "
                                                     "copies of the frame. PS2 resolution keeps them aligned but grainy "
                                                     "at high internal resolutions; Upscaled draws them sharp.");
                        iniCombo(state, "##posteffects", "Video", "PostEffects", "ps2",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kPostEffects, value); });
                        settingLabel("Depth of field", "The game's blur of the distance. At high internal resolutions "
                                                       "its edges can show dark, shimmering fringes.",
                                     "F2 switches it while playing.");
                        iniCheckbox(state, "On##dof", "Video", "DepthOfField", true);
                        settingLabel("Replacement textures", "HD texture packs in PCSX2's format.");
                        iniCheckbox(state, "Use replacement textures##tex", "Runtime", "PS2X_PCSX2_TEXTURES", true);
                        subSetting("Texture pack folder", "The folder that holds replacements/. Empty: textures/SLUS-21503 "
                                                          "next to the game.");
                        {
                            const std::string folder = state.ini.get("Runtime", "PS2X_PCSX2_TEXTURE_DIR");
                            ImGui::TextWrapped("%s", folder.empty() ? "(next to the game)" : folder.c_str());
                            if (ImGui::Button("Choose folder..."))
                            {
                                const fs::path picked = platform::pick(GetWindowHandle(), true);
                                if (!picked.empty())
                                    state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", utf8(picked));
                            }
                            if (!folder.empty())
                            {
                                ImGui::SameLine();
                                if (ImGui::Button("Use default"))
                                    state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", "");
                            }
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Gameplay"))
                {
                    if (beginSettings("gameplay"))
                    {
                        settingLabel("60 FPS", "Runs the game at 60 frames a second instead of 30, with its timers kept "
                                               "at the PS2's speed.");
                        iniCheckbox(state, "On##fps60", "Game", "Fps60", false);
                        settingLabel("L3 dash", "Clicking the left stick dashes (a quick double tap forward).");
                        iniCheckbox(state, "On##l3", "Game", "L3Dash", false);
                        settingLabel("Roulette timer", "How long you have to pick a roulette move. Double time gives "
                                                       "twice as long (the dial runs half as fast), at 30 and 60 FPS.");
                        iniCombo(state, "##roulette", "Game", "RouletteTimer", "normal",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kRouletteTimers, value); });
                        settingLabel("Unlock Hard", "Hard can be picked from the start instead of after a clear. Your "
                                                    "save is not changed.");
                        iniCheckbox(state, "On##hard", "Game", "UnlockHard", false);
                        settingLabel("Control style", "Classic: as on the PS2. Modern: the right stick turns the camera "
                                                      "and Gene moves where you point; hold or toggle L1 for the "
                                                      "original controls, double-tap L1 for the quick turn.",
                                     "F2 > Controls changes it while playing.");
                        iniCombo(state, "##controlstyle", "Game", "ControlStyle", "classic",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kControlStyles, value); });
                        if (lower(state.ini.get("Game", "ControlStyle", "classic")) == "modern")
                        {
                            subSetting("Modern: auto", "The original controls in fights: from the battle camera coming on "
                                                       "until no enemy is near.");
                            iniCheckbox(state, "On##modernauto", "Game", "ModernAuto", true);
                            subSetting("Modern: L1", "Hold L1 for the original controls, or press it to switch to them "
                                                     "and back.");
                            iniCombo(state, "##modernl1", "Game", "ModernL1", "hold",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kModernL1, value); });
                            subSetting("Modern: mouse camera", "The mouse turns the free camera. Click in the game to "
                                                               "capture the mouse, Esc frees it.",
                                       "F2 > Controls sets the sensitivity.");
                            iniCheckbox(state, "On##mousecamera", "Game", "MouseCamera", true);
                        }
                        settingLabel("Menu buttons", "Classic: as on the PS2, circle and cross confirm and triangle "
                                                     "cancels (B/A confirm, Y cancels on an Xbox pad). Modern: the "
                                                     "bottom button confirms and the right one cancels (A/B, or "
                                                     "cross/circle on a PlayStation pad). Menus and dialogs only; "
                                                     "the moves and the roulette keep their buttons.");
                        iniCombo(state, "##menubuttons", "Game", "MenuButtons", "classic",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kMenuButtons, value); });
                        settingLabel("Mute music", "Stage, boss, menu and minigame music plays silent. Cutscenes keep "
                                                   "their sound (their music and voices are one track).");
                        iniCheckbox(state, "On##music", "Sound", "MuteMusic", false);
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Mods"))
                {
                    if (beginSettings("mods"))
                    {
                        settingLabel("Character", "Who you play as. Other characters use Gene's moves and his gauntlet "
                                                  "arm.");
                        static std::vector<characters::Character> list;
                        if (list.empty())
                            list = characters::list(state.context->exeDir);
                        const std::string current = lower(state.ini.get("Mods", "Character", "gene"));
                        int picked = 0;
                        std::vector<const char *> names;
                        for (size_t i = 0; i < list.size(); ++i)
                        {
                            names.push_back(list[i].name.c_str());
                            if (lower(list[i].id) == current)
                                picked = static_cast<int>(i);
                        }
                        ImGui::SetNextItemWidth(-1.0f);
                        if (ImGui::Combo("##character", &picked, names.data(), static_cast<int>(names.size())))
                            state.ini.set("Mods", "Character", list[static_cast<size_t>(picked)].id);
                        if (!list[static_cast<size_t>(picked)].credit.empty())
                            ImGui::TextColored(kTextDim, "%s", list[static_cast<size_t>(picked)].credit.c_str());
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controls"))
                {
                    drawControls(state);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Experimental"))
                {
                    if (beginSettings("experimental"))
                    {
                        settingLabel("See-through", "Walls and pillars between the camera and Gene dissolve in an oval "
                                                    "round him, so he and the enemies about him stay in view. Walls next "
                                                    "to the camera, which the PS2 drops, are drawn too. Known: a banded "
                                                    "blur where hazy distance shows through (the depth of field reads "
                                                    "the dissolve).",
                                     "F2 > Camera sizes it while playing.");
                        iniCheckbox(state, "On##seethrough", "Video", "SeeThrough", true);
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            const float backWidth = fontSize(state, 200.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - backWidth) * 0.5f);
            const bool back = ImGui::Button("Back", ImVec2(backWidth, 0.0f));
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            const bool cancel = !g_captureFrame && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
            ImGui::PopFont();
            if (back || cancel)
            {
                state.ini.save();
                saveControls();
                g_resetArmed = false;
                state.screen = Screen::Home;
                state.focusFirst = true;
                return;
            }
            state.focusFirst = false;
        }
    }

    bool wanted(bool haveImage)
    {
        if (!haveImage || platform::shiftHeld())
            return true;
        const char *value = std::getenv("KAMITE_LAUNCHER");
        return !(value && *value == '0');
    }

    Result run(const Context &context)
    {
        State state;
        state.context = &context;
        if (!state.ini.load(context.iniPath))
            KM_WARN(Boot, "launcher: cannot read " << utf8(context.iniPath));
        std::error_code error;
        state.installed = !context.iso.empty() && fs::is_regular_file(context.iso, error) && context.message.empty();
        if (!context.iso.empty())
            std::snprintf(state.isoText, sizeof(state.isoText), "%s", utf8(context.iso).c_str());
        if (!context.message.empty())
        {
            state.installMessage = context.message;
            state.installError = true;
            state.screen = Screen::Install;
        }

        ps2_host_display::openHostWindow("Kamite");
        if (!ps2_host_display::vsyncEnabled())
            SetTargetFPS(60);
        rlImGuiSetup(true);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        const fs::path fontPath = context.exeDir / "resources" / "fonts" / "Roboto-Regular.ttf";
        if (fs::is_regular_file(fontPath, error))
            state.font = io.Fonts->AddFontFromFileTTF(utf8(fontPath).c_str(), 20.0f);
        applyStyle();
        const fs::path logoPath = context.exeDir / "resources" / "images" / "logo.png";
        if (fs::is_regular_file(logoPath, error))
        {
            state.logo = LoadTexture(utf8(logoPath).c_str());
            if (state.logo.id != 0u)
            {
                GenTextureMipmaps(&state.logo);
                SetTextureFilter(state.logo, TEXTURE_FILTER_TRILINEAR);
            }
        }
        const fs::path burstPath = context.exeDir / "resources" / "images" / "burst.png";
        if (fs::is_regular_file(burstPath, error))
        {
            state.burst = LoadTexture(utf8(burstPath).c_str());
            if (state.burst.id != 0u)
            {
                GenTextureMipmaps(&state.burst);
                SetTextureFilter(state.burst, TEXTURE_FILTER_TRILINEAR);
            }
        }
        KM_INFO(Boot, "launcher: shown (" << (state.installed ? "installed" : "not installed") << ")");

        while (!state.done)
        {
            if (WindowShouldClose())
            {
                state.result = Result::Quit;
                break;
            }
            state.unit = std::clamp(static_cast<float>(GetScreenHeight()) / 900.0f, 0.55f, 3.0f);
            feedGamepads(io);
            updateMotion(state, state.screen);
            BeginDrawing();
            drawBackground();
            drawBurst(state);
            rlImGuiBegin();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            if (ImGui::Begin("##launcher", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus))
            {
                switch (state.screen)
                {
                case Screen::Home:
                    drawHome(state);
                    break;
                case Screen::Install:
                    drawInstall(state);
                    break;
                case Screen::Options:
                    drawOptions(state);
                    break;
                }
            }
            ImGui::End();
            rlImGuiEnd();
            EndDrawing();
        }
        state.ini.save();
        saveControls();
        if (state.logo.id != 0u)
            UnloadTexture(state.logo);
        if (state.burst.id != 0u)
            UnloadTexture(state.burst);
        rlImGuiShutdown(); // the F12 debugger sets ImGui up again later
        // One empty frame, so the window does not keep showing the launcher until the game's first picture.
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
        KM_INFO(Boot, "launcher: " << (state.result == Result::Play ? "play" : "quit"));
        return state.result;
    }
}
