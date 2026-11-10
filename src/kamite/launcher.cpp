#include "kamite/launcher.h"
#include "kamite/launcher_bark.h"

#include "kamite/camera_presets.h"
#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/game_disc.h"
#include "kamite/ini_file.h"
#include "kamite/launcher_platform.h"
#include "kamite/log.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_hotkeys.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "raylib.h"
#include "rlImGui.h"
#include "rlgl.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace kamite::launcher
{
    namespace
    {
        namespace fs = std::filesystem;

        // Items whose activation makes the cursor's tick, not the confirm sound (boxes, dropdowns, radio buttons, folds:
        // K), and the buttons that play their own sound when let go (Back: cancel, Play: Play's; a confirm on the press
        // made both sound). Refilled each frame by the widgets right after they are drawn; read by frameSounds.
        std::vector<ImGuiID> g_tickItems, g_ownSoundItems;
        void tickItem() { g_tickItems.push_back(ImGui::GetItemID()); }
        void ownSoundItem() { g_ownSoundItems.push_back(ImGui::GetItemID()); }

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
        constexpr Choice kGraphicsApis[] = {{"Vulkan", "vulkan"}, {"Direct3D 11", "dx11"}};
        constexpr Choice kPostEffects[] = {{"PS2 resolution (PCSX2's fix)", "ps2"}, {"Upscaled", "upscaled"}, {"No fix", "off"}};
        constexpr Choice kWindowModes[] = {{"Fullscreen (Borderless)", "fullscreen"}, {"Maximized Window", "maximized"}, {"Window", "window"}};
        constexpr Choice kFovs[] = {{"100%: as on the PS2", "100"}, {"110%", "110"}, {"120%", "120"}, {"130%", "130"},
                                    {"140%", "140"}, {"150%", "150"}};
        constexpr Choice kCameraDistances[] = {{"0%: Gene keeps his size", "0"}, {"50%: halfway out", "50"},
                                               {"100%: wider view looks zoomed out", "100"}};
        constexpr Choice kControlStyles[] = {{"Classic", "classic"}, {"Modern", "modern"}};
        constexpr Choice kMenuButtons[] = {{"Western", "western"}, {"Japanese", "japanese"}, {"Classic", "classic"}};
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
            const bool changed = ImGui::Combo(id, &picked, labels.data(), static_cast<int>(labels.size()));
            tickItem();
            if (!changed || picked == current || picked >= static_cast<int>(N))
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
        const ImVec4 kTextSoft(0.80f, 0.77f, 0.73f, 1.0f); // dimmer text straight on the background
        const ImVec4 kText(0.95f, 0.92f, 0.88f, 1.0f);
        const ImVec4 kHeading(1.0f, 0.62f, 0.30f, 1.0f); // the Options title and the categories' headings
        bool g_hotkeysOpen = false;                       // the Controls tab's Hotkeys: folded each time Options opens

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
            // The logo's layers (tools/logo/export_launcher_logo.py), all the same canvas, drawn into one rectangle:
            Texture2D logo{};        // resources/images/logo.png: GOD HAND and its shadow (it stays still)
            Texture2D kana{};        // resources/images/kana.png: the flat kana behind it (moves a little with the cursor)
            Texture2D rearmed{};     // resources/images/rearmed.png: the REARMED plate without its letters
            Texture2D rearmedText{}; // resources/images/rearmed_text.png: its letters, white (tinted gold / hot)
            Texture2D rearmedGlow{}; // resources/images/rearmed_glow.png: the letters blurred, for the hot glow
            Texture2D burst{}; // resources/images/burst.png: the speed lines behind the logo
            Texture2D background{};   // resources/images/background.png: the desert (the gradient without it)
            Shader backgroundShader{}; // kBackgroundShader: heat haze, depth of field, clouds (none: the plain image)
            int backgroundTexel = -1, backgroundTime = -1, backgroundView = -1;
            Font splashFont{}; // resources/splash/splash.fnt, the Dominican font baked as an atlas
            std::string splash; // a random line of resources/splash/splash.txt, "" when there is none
            // The title's motion: the cursor (eased) shifts the burst layers and the kana by different amounts,
            // the bursts turn slowly and pulse; the wordmark does not move.
            float time = 0.0f;
            ImVec2 parallax{0.0f, 0.0f}; // -1..1 each way; 0 while the cursor is away
            float burstShown = 0.0f;     // fades the burst in on the screens with the logo, out on the others
            bool logoPlaced = false;     // logoMin/logoMax hold last frame's logo rectangle (without the parallax shift)
            bool logoThisFrame = false;  // title() laid the logo out this frame (drawLogo draws it after ImGui)
            ImVec2 logoMin{}, logoMax{};
            float playTime = -1.0f;      // seconds since Play was pressed (the plate's animation, the fade); -1: not yet
            bool audio = false;          // the audio device is open (closed again before the game opens its own)
            Music music{};               // resources/music/launcher.ogg, looped; fades out with Play
            bool hasMusic = false;
            // The game's menu sounds (read from the player's disc image): kMenuSoundIds' order.
            Sound menuSounds[4]{};
            float menuSoundBase[4]{};    // each sound's own volume in the game's bank
            bool hasMenuSound[4]{};
            // The start bark (launcher_bark.h, K Nov 10): one line, dealt from [Launcher] BarkDeck, rendered in the
            // background from the disc image; the music's tail is rendered when Play is pressed.
            std::thread barkWorker;
            bool barkStarted = false;
            int barkLine = -1;
            launcher_bark::Stereo barkPcm;   // empty: no voice (not read)
            launcher_bark::Wash wash;
            std::vector<int16_t> musicPcm;   // resources/music/launcher.ogg decoded, for its tail
            int musicRate = 0;
            fs::path musicPath;
            bool musicHandedOff = false;     // Play replaced the music stream with its tail
            float volume = 0.8f;         // [Launcher] Volume: music and sounds (the corner slider)
            bool muted = false;          // [Launcher] Mute
            ImGuiID volumeSliderId = 0;  // makes no sound when grabbed
            ImGuiID lastNavId = 0;       // the keyboard / gamepad cursor's item last frame
            bool popupWasOpen = false;
            bool soundThisFrame = false; // a confirm / cancel / Play sound already played this frame
            int quietFrames = 0;         // no cursor tick for a frame or two after a confirm / cancel (focus moves then)
            Screen soundScreen = Screen::Home;
        };

        // GOD HAND's centre in the logo's canvas (tools/logo/export_launcher_logo.py prints it); the burst is centred
        // there. kRearmedRect: the REARMED plate (left, top, right, bottom as fractions of the canvas, same script).
        constexpr float kLogoCentreX = 0.5048f, kLogoCentreY = 0.5197f;
        constexpr float kRearmedRect[4] = {0.3110f, 0.7152f, 0.6961f, 0.8930f};
        // Play: the plate's animation, taken from the game's roulette pick (K's capture at 1/8 speed): the letters
        // light up, the plate smears out both ways into a yellow bar, the bar thins to streaks, all gone at 0.28 s;
        // the rest of the launcher fades to black meanwhile and the game starts at kPlayLength.
        constexpr float kPlayLength = 0.34f;
        // The game's menu sounds (se/core.ses on the disc; K's picks): the cursor's tick when the keyboard or gamepad
        // moves it (0x164), confirm (0x15E: buttons, boxes, tabs, dropdown picks; clicks too), cancel (0x161: Back, B /
        // Esc), Play (0x160, the main menu's confirm; it rings on into the game's boot: the launcher leaves the audio
        // device open for the game). The volume slider makes none.
        enum MenuSound
        {
            kTick,
            kConfirm,
            kCancel,
            kPlay
        };
        constexpr int kMenuSoundIds[4] = {0x164, 0x15E, 0x161, 0x160};
        constexpr float kMusicVolume = 0.6f; // under the player's volume

        // A sound of the game's SE bank (.ses: "SES\0", f32 version; u32s at +0x10.. = sections: +0x10 the sound table
        // (u32 count, then from +0x20 0x30-byte entries: +4 u16 tone, +0xF 0x80 = unused, +0x10 u8 volume 0..127), +0x20
        // the tone table (16 bytes each: u32 offset into the sample data, u32 sample rate), +0x28 / +0x2C the sample
        // data (PS-ADPCM)) as 16-bit mono PCM. False when the bank or the sound is not what that describes.
        bool decodeSesSound(const std::vector<uint8_t> &ses, int id, std::vector<int16_t> &pcm, int &rate, float &volume)
        {
            auto u32 = [&](size_t o) -> uint32_t {
                return o + 4 > ses.size() ? 0u : static_cast<uint32_t>(ses[o]) | (static_cast<uint32_t>(ses[o + 1]) << 8) |
                                                 (static_cast<uint32_t>(ses[o + 2]) << 16) | (static_cast<uint32_t>(ses[o + 3]) << 24);
            };
            if (ses.size() < 0x40 || std::memcmp(ses.data(), "SES", 4) != 0)
                return false;
            const uint32_t table = u32(0x10), tones = u32(0x20), toneBytes = u32(0x24), data = u32(0x28), dataSize = u32(0x2C);
            if (id < 0 || static_cast<uint32_t>(id) >= u32(table) || data + static_cast<uint64_t>(dataSize) > ses.size())
                return false;
            const size_t entry = table + 0x20 + 0x30 * static_cast<size_t>(id);
            if (entry + 0x30 > ses.size() || (ses[entry + 0xF] & 0x80) != 0)
                return false;
            const uint32_t tone = static_cast<uint32_t>(ses[entry + 4]) | (static_cast<uint32_t>(ses[entry + 5]) << 8);
            if (16ull * (tone + 1) > toneBytes)
                return false;
            const uint32_t offset = u32(tones + 16 * tone);
            rate = static_cast<int>(u32(tones + 16 * tone + 4));
            volume = std::clamp(static_cast<float>(ses[entry + 0x10]) / 127.0f, 0.0f, 1.0f);
            if (rate < 4000 || rate > 96000 || offset >= dataSize)
                return false;
            // PS-ADPCM: 16-byte frames (shift | filter << 4, flags (bit 0: last frame), 14 bytes = 28 samples).
            static constexpr int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
            int s1 = 0, s2 = 0;
            pcm.clear();
            for (size_t p = data + offset; p + 16 <= data + static_cast<size_t>(dataSize); p += 16)
            {
                const int shift = ses[p] & 15, filter = std::min(ses[p] >> 4, 4);
                for (int i = 0; i < 28; ++i)
                {
                    const uint8_t b = ses[p + 2 + i / 2];
                    int v = (i & 1) ? (b >> 4) : (b & 15);
                    if (v > 7)
                        v -= 16;
                    v = shift <= 12 ? (v << 12) >> shift : 0;
                    v += (s1 * kFilter[filter][0] + s2 * kFilter[filter][1]) >> 6; // floor, as the SPU2
                    v = std::clamp(v, -32768, 32767);
                    pcm.push_back(static_cast<int16_t>(v));
                    s2 = s1;
                    s1 = v;
                }
                if ((ses[p + 1] & 1) != 0)
                    break;
            }
            return !pcm.empty();
        }

        // The menu sounds from the player's own disc (never shipped): loaded at the start and after an install.
        void loadMenuSounds(State &state, const fs::path &image)
        {
            if (!state.audio || state.hasMenuSound[kPlay] || image.empty())
                return;
            std::vector<uint8_t> ses;
            if (!disc_files::readFromImage(image, "se/core.ses", ses))
            {
                KM_INFO(Boot, "launcher: no menu sounds (se/core.ses not read from the disc image)");
                return;
            }
            for (int i = 0; i < 4; ++i)
            {
                std::vector<int16_t> pcm;
                int rate = 0;
                float volume = 1.0f;
                if (!decodeSesSound(ses, kMenuSoundIds[i], pcm, rate, volume))
                {
                    KM_INFO(Boot, "launcher: menu sound 0x" << std::hex << kMenuSoundIds[i] << std::dec << " not in se/core.ses");
                    continue;
                }
                Wave wave{};
                wave.frameCount = static_cast<unsigned int>(pcm.size());
                wave.sampleRate = static_cast<unsigned int>(rate);
                wave.sampleSize = 16;
                wave.channels = 1;
                wave.data = pcm.data();
                state.menuSounds[i] = LoadSoundFromWave(wave); // copies the samples
                state.hasMenuSound[i] = state.menuSounds[i].frameCount > 0;
                state.menuSoundBase[i] = volume;
            }
        }

        float userVolume(const State &state) { return state.muted ? 0.0f : state.volume; }

        void playMenuSound(State &state, MenuSound which, float scale = 1.0f)
        {
            if (!state.hasMenuSound[which])
                return;
            SetSoundVolume(state.menuSounds[which], state.menuSoundBase[which] * userVolume(state) * scale);
            // Not PlaySound: GHR.exe links winmm with /FORCE:MULTIPLE, and winmm's PlaySound took the call (no sound,
            // no error). raylib's PlaySound is exactly this.
            PlayAudioStream(state.menuSounds[which].stream);
        }

        uint32_t fnv1a(std::string_view text, uint32_t hash = 2166136261u)
        {
            for (const unsigned char c : text)
                hash = (hash ^ c) * 16777619u;
            return hash;
        }

        // One pick from a shuffled deck kept in GHR.ini ([Launcher] key = "<list id>:<last>:<rest, ...>", K Nov 10):
        // every entry comes once, in random order, before any comes back; a new deck never starts with the one just
        // used; a changed list (another id: a splash line added) starts a new deck.
        int dealFromDeck(State &state, const char *key, int count, uint32_t listId)
        {
            if (count <= 0)
                return -1;
            char id[16];
            std::snprintf(id, sizeof(id), "%08x", listId);
            const std::string saved = state.ini.get("Launcher", key, "");
            int last = -1;
            std::vector<int> rest;
            const size_t c1 = saved.find(':'), c2 = c1 == std::string::npos ? c1 : saved.find(':', c1 + 1);
            if (c2 != std::string::npos && saved.compare(0, c1, id) == 0)
            {
                last = std::atoi(saved.substr(c1 + 1, c2 - c1 - 1).c_str());
                std::string list = saved.substr(c2 + 1);
                for (size_t at = 0; at < list.size();)
                {
                    const size_t comma = std::min(list.find(',', at), list.size());
                    const std::string item = list.substr(at, comma - at);
                    const int v = item.empty() ? -1 : std::atoi(item.c_str());
                    if (v >= 0 && v < count && std::find(rest.begin(), rest.end(), v) == rest.end())
                        rest.push_back(v);
                    at = comma + 1;
                }
            }
            if (rest.empty())
            {
                for (int i = 0; i < count; ++i)
                    rest.push_back(i);
                std::mt19937 random(std::random_device{}() ^ static_cast<unsigned>(std::time(nullptr)));
                std::shuffle(rest.begin(), rest.end(), random);
                if (count > 1 && rest.front() == last)
                    std::swap(rest[0], rest[1]);
            }
            const int pick = rest.front();
            rest.erase(rest.begin());
            std::string value = std::string(id) + ":" + std::to_string(pick) + ":";
            for (size_t i = 0; i < rest.size(); ++i)
                value += (i ? "," : "") + std::to_string(rest[i]);
            state.ini.set("Launcher", key, value);
            state.ini.save();
            return pick;
        }

        // The start bark: deals this start's line and renders it in the background (with the music's reverb and the
        // music itself, for the tail on Play). Once per start, when the disc image is known.
        void startBark(State &state, const fs::path &image)
        {
            if (!state.audio || state.barkStarted || image.empty())
                return;
            state.barkStarted = true;
            uint32_t listId = 2166136261u;
            for (int i = 0; i < launcher_bark::count(); ++i)
                listId = fnv1a(std::string(launcher_bark::name(i)) + "\n", listId);
            state.barkLine = dealFromDeck(state, "BarkDeck", launcher_bark::count(), listId);
            const fs::path musicPath = state.hasMusic ? state.musicPath : fs::path();
            state.barkWorker = std::thread([&state, image, musicPath, line = state.barkLine] {
                const launcher_bark::ReadFile read = [&image](std::string_view path, std::vector<uint8_t> &bytes) {
                    return disc_files::readFromImage(image, path, bytes);
                };
                if (!launcher_bark::render(line, read, state.barkPcm))
                    state.barkPcm.lr.clear();
                if (musicPath.empty())
                    return;
                Wave wave = LoadWave(utf8(musicPath).c_str());
                if (wave.data != nullptr && wave.frameCount > 0)
                {
                    WaveFormat(&wave, static_cast<int>(wave.sampleRate), 16, 2);
                    const int16_t *pcm = static_cast<const int16_t *>(wave.data);
                    state.musicPcm.assign(pcm, pcm + static_cast<size_t>(wave.frameCount) * 2);
                    state.musicRate = static_cast<int>(wave.sampleRate);
                    launcher_bark::prepareWash(state.wash);
                }
                UnloadWave(wave);
            });
        }

        Sound soundFrom(const launcher_bark::Stereo &pcm)
        {
            std::vector<int16_t> samples(pcm.lr.size());
            for (size_t i = 0; i < samples.size(); ++i)
                samples[i] = static_cast<int16_t>(std::clamp(pcm.lr[i], -1.0f, 1.0f) * 32767.0f);
            Wave wave{};
            wave.frameCount = static_cast<unsigned int>(pcm.frames());
            wave.sampleRate = launcher_bark::kRate;
            wave.sampleSize = 16;
            wave.channels = 2;
            wave.data = samples.data();
            return LoadSoundFromWave(wave); // copies the samples
        }

        // Play: its own sound (lower under a bark, as K approved the mix), the bark, and the music's tail in place of
        // the music: it closes down and washes out under the voice, all faded out right after the last word. The
        // sounds ring on into the game's boot (the audio device is left to the game, as for Play's sound).
        void playBark(State &state)
        {
            if (state.barkWorker.joinable())
                state.barkWorker.join(); // ready long before Play, unless Play came at once
            const bool voice = !state.barkPcm.lr.empty();
            playMenuSound(state, kPlay, voice ? 0.55f : 1.0f);
            const auto start = std::chrono::steady_clock::now();
            float end = 1.5f;
            if (voice)
            {
                const Sound bark = soundFrom(state.barkPcm);
                SetSoundVolume(bark, userVolume(state));
                PlayAudioStream(bark.stream); // not PlaySound (winmm's, see playMenuSound)
                end = static_cast<float>(state.barkPcm.frames()) / launcher_bark::kRate;
            }
            if (state.hasMusic && !state.musicPcm.empty() && state.wash.fftSize != 0)
            {
                const size_t frames = state.musicPcm.size() / 2;
                const size_t from = static_cast<size_t>(GetMusicTimePlayed(state.music) * state.musicRate) % frames;
                launcher_bark::Stereo tail;
                launcher_bark::musicTail(state.musicPcm.data(), frames, state.musicRate, from, end, state.wash, tail);
                if (!tail.lr.empty())
                {
                    const Sound sound = soundFrom(tail);
                    SetSoundVolume(sound, kMusicVolume * userVolume(state));
                    StopMusicStream(state.music);
                    PlayAudioStream(sound.stream);
                    state.musicHandedOff = true;
                }
            }
            KM_INFO(Boot, "launcher: start bark " << (voice ? launcher_bark::name(state.barkLine) : "none (not read from the disc)")
                                                  << ", music tail " << (state.musicHandedOff ? "on" : "off") << " ("
                                                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
                                                  << " ms)");
        }

        void menuSound(State &state, MenuSound which)
        {
            playMenuSound(state, which);
            state.soundThisFrame = true;
            state.quietFrames = 2;
        }

        // The frame's sounds from what ImGui did (after the launcher's window): an item activated (click or A / Enter:
        // confirm; not the volume slider), a dropdown closed with B / Esc (cancel), the keyboard / gamepad cursor moved
        // to another item (tick; mouse hovering moves nothing). Sounds the screens play themselves (Back, Play) win.
        void frameSounds(State &state)
        {
            ImGuiContext &g = *GImGui;
            const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            const bool backKey = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            if (state.playTime < 0.0f && !state.soundThisFrame)
            {
                if (state.popupWasOpen && !popupOpen && backKey)
                    menuSound(state, kCancel);
                else if (g.ActiveIdIsJustActivated && g.ActiveId != 0 && g.ActiveId != state.volumeSliderId)
                {
                    const auto listed = [&](const std::vector<ImGuiID> &items) {
                        return std::find(items.begin(), items.end(), g.ActiveId) != items.end();
                    };
                    const bool inPopup = g.ActiveIdWindow != nullptr && (g.ActiveIdWindow->Flags & ImGuiWindowFlags_Popup) != 0;
                    if (listed(g_tickItems) || inPopup) // a box, a dropdown or a pick from its list
                    {
                        playMenuSound(state, kTick);
                        state.quietFrames = 2; // the dropdown's list opening moves the cursor
                    }
                    else if (!listed(g_ownSoundItems)) // Back / Play play their own sound when let go
                        menuSound(state, kConfirm);
                }
                else if (g.NavId != state.lastNavId && state.lastNavId != 0 && g.NavId != 0 && g.NavCursorVisible &&
                         state.quietFrames == 0 && state.soundScreen == state.screen)
                    playMenuSound(state, kTick);
            }
            if (state.quietFrames > 0 && !state.soundThisFrame)
                --state.quietFrames;
            state.lastNavId = g.NavId;
            state.popupWasOpen = popupOpen;
            state.soundScreen = state.screen;
        }


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
            const float ease = 1.0f - std::exp(-dt * 3.0f);
            state.parallax.x += (target.x - state.parallax.x) * ease;
            state.parallax.y += (target.y - state.parallax.y) * ease;
            const float shown = screen == Screen::Options ? 0.0f : 1.0f;
            state.burstShown += (shown - state.burstShown) * (1.0f - std::exp(-dt * 5.0f));
        }

        // background.png's effects, drawn over the whole window: a rising heat haze along the horizon, a fake depth of
        // field (the town and the walkers in focus, the near sand soft, a little softening everywhere to hide the
        // image's low resolution) and wispy clouds drifting on a plane above the desert, behind the town. The header
        // (#version) is added for the GL version raylib runs.
        const char *const kBackgroundShader = R"glsl(
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 texel; // 1 / the image's size
uniform float time; // seconds
uniform vec4 view;  // the part of the image in the window: x, y, width, height in image uv
out vec4 finalColor;

const float kHorizon = 0.625; // the horizon's height in background.png (0 top, 1 bottom)

float hash(vec2 p)
{
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}
float noise(vec2 p)
{
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}
float fbm(vec2 p)
{
    float v = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i)
    {
        v += a * noise(p);
        p = p * 2.03 + vec2(17.1, 9.2);
        a *= 0.5;
    }
    return v;
}

const vec2 kDisc[12] = vec2[](vec2(-0.326, -0.406), vec2(-0.840, -0.074), vec2(-0.696, 0.457), vec2(-0.203, 0.621),
                              vec2(0.962, -0.195), vec2(0.473, -0.480), vec2(0.519, 0.767), vec2(0.185, -0.893),
                              vec2(0.507, 0.064), vec2(0.896, 0.412), vec2(-0.322, -0.933), vec2(-0.792, -0.598));

void main()
{
    vec2 uv = fragTexCoord;
    float d = uv.y - kHorizon; // < 0 sky, > 0 sand

    // Heat haze: rising shimmer, strongest along the horizon, weaker over the sand, none high in the sky.
    float haze = smoothstep(-0.07, 0.0, d) * (0.35 + 0.65 * exp(-max(d, 0.0) * 14.0));
    float w = noise(vec2(uv.x * 60.0, uv.y * 150.0 + time * 2.4)) + 0.5 * noise(vec2(uv.x * 120.0, uv.y * 300.0 + time * 4.1)) - 0.75;
    uv += vec2(2.6, 0.8) * w * haze * texel;

    // Fake depth of field: the horizon (the town, the walkers) in focus, the near sand soft; a little softening
    // everywhere hides the image's low resolution.
    float radius = 1.3 + 2.8 * smoothstep(0.03, 0.37, d) + 0.5 * smoothstep(0.0, -0.4, d);
    vec3 centre = texture(texture0, uv).rgb;
    vec3 col = centre;
    for (int i = 0; i < 12; ++i)
        col += texture(texture0, uv + kDisc[i] * radius * texel).rgb;
    col /= 13.0;

    // Wispy clouds on a plane above the desert, drifting sideways (no loop seam: the noise never repeats).
    float h = kHorizon - fragTexCoord.y;
    if (h > 0.0)
    {
        float depth = 0.12 / (h + 0.03);
        vec2 q = vec2((fragTexCoord.x - 0.5) * depth * 6.0 + time * 0.018, depth * 14.0);
        q += 0.7 * vec2(fbm(q * 0.7 + vec2(0.0, time * 0.02)), fbm(q * 0.7 + vec2(5.2, 1.3)));
        float c = fbm(q);
        float a = smoothstep(0.50, 0.82, c) * smoothstep(0.0, 0.14, h) * 0.6;
        a *= smoothstep(0.0, 0.08, centre.b - centre.r); // only over the sky, behind the town
        col = mix(col, mix(vec3(1.0), col, 0.2), a);
    }
    // Toned down so the launcher's text reads over it: a little darker overall, more towards the bottom, a vignette.
    vec2 sp = (fragTexCoord - view.xy) / view.zw; // 0..1 over the window
    vec2 v = sp - 0.5;
    col *= (1.0 - 0.08) * (1.0 - 0.45 * smoothstep(0.45, 1.0, sp.y)) * (1.0 - 0.3 * smoothstep(0.2, 0.75, dot(v, v) * 2.0));
    finalColor = vec4(col, 1.0) * colDiffuse * fragColor;
}
)glsl";

        // The desert, covering the window: fitted to the height up to the image's own width (2.76:1; wider windows crop
        // it vertically), anchored right so the town and the walkers stay in frame when the window is narrower.
        // False without background.png (the gradient is drawn instead).
        bool drawDesert(const State &state)
        {
            const Texture2D &image = state.background;
            if (image.id == 0u || image.width <= 0 || image.height <= 0)
                return false;
            const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
            const float imageWidth = static_cast<float>(image.width), imageHeight = static_cast<float>(image.height);
            const float scale = std::max(width / imageWidth, height / imageHeight);
            const float shownWidth = width / scale, shownHeight = height / scale;
            const float x = imageWidth - shownWidth, y = (imageHeight - shownHeight) * 0.5f;
            const bool shaded = state.backgroundShader.id != 0u && state.backgroundShader.id != rlGetShaderIdDefault();
            if (shaded)
            {
                const float texel[2] = {1.0f / imageWidth, 1.0f / imageHeight};
                SetShaderValue(state.backgroundShader, state.backgroundTexel, texel, SHADER_UNIFORM_VEC2);
                SetShaderValue(state.backgroundShader, state.backgroundTime, &state.time, SHADER_UNIFORM_FLOAT);
                const float view[4] = {x / imageWidth, y / imageHeight, shownWidth / imageWidth, shownHeight / imageHeight};
                SetShaderValue(state.backgroundShader, state.backgroundView, view, SHADER_UNIFORM_VEC4);
                BeginShaderMode(state.backgroundShader);
            }
            DrawTexturePro(image, Rectangle{x, y, shownWidth, shownHeight}, Rectangle{0.0f, 0.0f, width, height},
                           Vector2{0.0f, 0.0f}, 0.0f, WHITE);
            if (shaded)
                EndShaderMode();
            return true;
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

        // The splash text (Minecraft's): a random line of resources/splash/splash.txt, one per start, yellow with a hard
        // shadow, tilted up across the wordmark's bottom right corner and pulsing. Long lines are drawn smaller, so
        // each stays within 40% of the wordmark's width. Over the screens with the logo (Home, Install), fading out with
        // the burst on Options.
        void loadSplash(State &state, const fs::path &folder)
        {
            std::ifstream in(folder / "splash.txt", std::ios::binary);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(in, line))
            {
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                    line.erase(0, 3); // a UTF-8 byte order mark (Notepad)
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                    line.pop_back();
                size_t start = 0;
                while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])))
                    ++start;
                if (start < line.size())
                    lines.push_back(line.substr(start));
            }
            // The font is a baked atlas (tools/splash/bake_font.py: splash.fnt + splash.png, Latin-1), not the .ttf:
            // its terms keep the raw file out of the repo, and raylib's own atlas packer clipped and garbled this
            // font's tall glyphs (it assumes none is taller than the font size).
            std::error_code error;
            const fs::path fontPath = folder / "splash.fnt";
            if (lines.empty() || !fs::is_regular_file(fontPath, error))
                return;
            uint32_t listId = 2166136261u;
            for (const std::string &l : lines)
                listId = fnv1a(l + "\n", listId);
            state.splash = lines[static_cast<size_t>(dealFromDeck(state, "SplashDeck", static_cast<int>(lines.size()), listId))];
            state.splashFont = LoadFont(utf8(fontPath).c_str());
            if (state.splashFont.texture.id == 0u || state.splashFont.texture.id == GetFontDefault().texture.id)
            {
                state.splash.clear();
                return;
            }
            GenTextureMipmaps(&state.splashFont.texture);
            SetTextureFilter(state.splashFont.texture, TEXTURE_FILTER_TRILINEAR);
        }

        void drawSplash(const State &state)
        {
            if (state.splash.empty() || !state.logoPlaced || state.burstShown < 0.01f)
                return;
            const float logoWidth = state.logoMax.x - state.logoMin.x, logoHeight = state.logoMax.y - state.logoMin.y;
            const char *text = state.splash.c_str();
            float size = logoWidth * 0.047f;
            const Vector2 measured = MeasureTextEx(state.splashFont, text, size, 0.0f);
            if (measured.x > logoWidth * 0.40f)
                size *= logoWidth * 0.40f / measured.x;
            size *= 1.0f + 0.05f * std::fabs(std::sin(state.time * 3.14159265f)); // the pulse, once a second
            const Vector2 box = MeasureTextEx(state.splashFont, text, size, 0.0f);
            const Vector2 at{state.logoMin.x + logoWidth * 0.90f, state.logoMin.y + logoHeight * 0.68f}; // on the D's lower right
            const Vector2 origin{box.x * 0.5f, box.y * 0.5f};
            const float shadow = size * 0.085f;
            const unsigned char alpha = static_cast<unsigned char>(255.0f * std::clamp(state.burstShown, 0.0f, 1.0f));
            DrawTextPro(state.splashFont, text, Vector2{at.x + shadow, at.y + shadow}, origin, -18.0f, size, 0.0f,
                        Color{62, 52, 0, alpha});
            DrawTextPro(state.splashFont, text, at, origin, -18.0f, size, 0.0f, Color{255, 236, 64, alpha});
        }

        // Text straight on the background, centred, with a soft dark shadow so it reads over the bright desert.
        void centeredText(const char *text, const ImVec4 &color)
        {
            const float width = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const float size = ImGui::GetFontSize(), r = std::max(1.0f, size * 0.09f);
            for (int ring = 1; ring <= 2; ++ring)
                for (int i = 0; i < 12; ++i)
                {
                    const float a = static_cast<float>(i) * 0.5236f;
                    draw->AddText(ImVec2(at.x + std::cos(a) * r * ring, at.y + r * 0.6f + std::sin(a) * r * ring),
                                  IM_COL32(10, 6, 4, ring == 1 ? 46 : 22), text);
                }
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

        // The lower right corner's volume: a mute button (a speaker, crossed out when muted) left of a slider; both kept
        // in GHR.ini.
        void drawVolume(State &state)
        {
            const float size = fontSize(state, 34.0f), margin = fontSize(state, 22.0f), slider = fontSize(state, 200.0f);
            const float gap = fontSize(state, 8.0f);
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - margin - slider - gap - size, ImGui::GetWindowHeight() - margin - size));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##mute", ImVec2(size, size)))
            {
                state.muted = !state.muted;
                state.ini.setBool("Launcher", "Mute", state.muted);
                state.ini.save();
            }
            const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImU32 ink = ImGui::GetColorU32(lit ? kAccent : kText);
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const auto at = [&](float x, float y) { return ImVec2(p.x + x * size, p.y + y * size); };
            draw->AddRectFilled(at(0.14f, 0.38f), at(0.32f, 0.62f), ink);
            const ImVec2 cone[4] = {at(0.31f, 0.38f), at(0.54f, 0.18f), at(0.54f, 0.82f), at(0.31f, 0.62f)};
            draw->AddConvexPolyFilled(cone, 4, ink);
            const float stroke = std::max(1.5f, size * 0.07f);
            if (state.muted || state.volume <= 0.0f)
            {
                draw->AddLine(at(0.66f, 0.36f), at(0.88f, 0.64f), ink, stroke);
                draw->AddLine(at(0.66f, 0.64f), at(0.88f, 0.36f), ink, stroke);
            }
            else
                for (int wave = 0; wave < (state.volume > 0.5f ? 2 : 1); ++wave)
                {
                    draw->PathArcTo(at(0.54f, 0.5f), size * (0.17f + 0.15f * static_cast<float>(wave)), -0.85f, 0.85f, 12);
                    draw->PathStroke(ink, ImDrawFlags_None, stroke);
                }
            ImGui::SameLine(0.0f, gap);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - margin - size + (size - ImGui::GetFrameHeight()) * 0.5f);
            ImGui::SetNextItemWidth(slider);
            float percent = state.volume * 100.0f;
            if (ImGui::SliderFloat("##volume", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            {
                state.volume = percent / 100.0f;
                if (state.muted && state.volume > 0.0f)
                {
                    state.muted = false;
                    state.ini.setBool("Launcher", "Mute", false);
                }
            }
            state.volumeSliderId = ImGui::GetItemID();
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                char text[16];
                std::snprintf(text, sizeof(text), "%.2f", state.volume);
                state.ini.set("Launcher", "Volume", text);
                state.ini.save();
            }
        }

        void drawLayer(const Texture2D &texture, const ImVec2 &min, const ImVec2 &max, Color tint)
        {
            if (texture.id == 0u || tint.a == 0)
                return;
            DrawTexturePro(texture, Rectangle{0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)},
                           Rectangle{min.x, min.y, max.x - min.x, max.y - min.y}, Vector2{0.0f, 0.0f}, 0.0f, tint);
        }

        unsigned char alphaOf(float a) { return static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f); }

        float ramp(float t, float a, float b) { return std::clamp((t - a) / (b - a), 0.0f, 1.0f); }

        // A horizontal band from (x0) fading to nothing at (x1), either way.
        void band(float x0, float x1, float y, float h, Color c)
        {
            Color clear = c;
            clear.a = 0;
            if (x1 >= x0)
                DrawRectangleGradientH(static_cast<int>(x0), static_cast<int>(y), static_cast<int>(x1 - x0) + 1,
                                       std::max(1, static_cast<int>(h)), c, clear);
            else
                DrawRectangleGradientH(static_cast<int>(x1), static_cast<int>(y), static_cast<int>(x0 - x1) + 1,
                                       std::max(1, static_cast<int>(h)), clear, c);
        }

        // The REARMED plate. t < 0: at rest; t >= 0: seconds since Play (the roulette pick, both ways from the plate).
        void drawPlate(const State &state, float t)
        {
            const ImVec2 min = state.logoMin, max = state.logoMax;
            const float w = max.x - min.x, h = max.y - min.y;
            const Color gold{240, 206, 90, 255};
            if (t < 0.0f)
            {
                drawLayer(state.rearmed, min, max, WHITE);
                drawLayer(state.rearmedText, min, max, gold);
                return;
            }
            const float px = min.x + w * kRearmedRect[0], py = min.y + h * kRearmedRect[1];
            const float pw = w * (kRearmedRect[2] - kRearmedRect[0]), ph = h * (kRearmedRect[3] - kRearmedRect[1]);
            const float cx = px + pw * 0.5f, screen = static_cast<float>(GetScreenWidth());
            const float reach = std::max(cx, screen - cx); // from the plate's centre to the farther screen edge
            const float glow = ramp(t, 0.055f, 0.07f), smear = ramp(t, 0.07f, 0.12f);
            const float bar = ramp(t, 0.11f, 0.13f) * (1.0f - ramp(t, 0.17f, 0.2f));
            const float streak = ramp(t, 0.17f, 0.19f) * (1.0f - ramp(t, 0.24f, 0.275f));
            const float live = 1.0f - ramp(t, 0.255f, 0.28f), body = 1.0f - ramp(t, 0.075f, 0.11f);
            drawLayer(state.rearmed, min, max, Color{255, 255, 255, alphaOf(body)});
            // The plate smeared out both ways (three stretched copies, a cheap blur), fading as the bar comes in.
            if (smear > 0.0f && bar < 1.0f && state.rearmed.id != 0u)
            {
                const Rectangle source{static_cast<float>(state.rearmed.width) * kRearmedRect[0],
                                       static_cast<float>(state.rearmed.height) * kRearmedRect[1],
                                       static_cast<float>(state.rearmed.width) * (kRearmedRect[2] - kRearmedRect[0]),
                                       static_cast<float>(state.rearmed.height) * (kRearmedRect[3] - kRearmedRect[1])};
                for (const float part : {0.55f, 0.8f, 1.0f})
                {
                    const float stretch = smear * reach * part;
                    DrawTexturePro(state.rearmed, source, Rectangle{px - stretch, py, pw + stretch * 2.0f, ph},
                                   Vector2{0.0f, 0.0f}, 0.0f, Color{255, 255, 255, alphaOf(0.2f * (1.0f - bar) * live)});
                }
            }
            if (bar > 0.0f)
            {
                const float y0 = py + ph * 0.18f, bh = ph * 0.64f, a = bar * live;
                for (const float side : {-1.0f, 1.0f})
                {
                    band(cx, cx + side * reach, y0, bh, Color{255, 226, 90, alphaOf(0.9f * a)});
                    band(cx, cx + side * reach * 0.7f, y0 + bh * 0.1f, bh * 0.8f, Color{255, 250, 215, alphaOf(0.6f * a)});
                }
            }
            if (streak > 0.0f || (smear > 0.0f && bar < 1.0f))
            {
                const float k = std::max(streak, smear * (1.0f - bar)) * live, thick = std::max(1.0f, ph / 45.0f);
                const float lines[5][3] = {{0.38f, 0.95f, 3.0f}, {0.5f, 0.7f, 2.0f}, {0.62f, 1.0f, 3.0f},
                                           {0.45f, 0.5f, 2.0f}, {0.56f, 0.85f, 2.0f}};
                for (const auto &line : lines)
                    for (const float side : {-1.0f, 1.0f})
                    {
                        const float y = py + ph * line[0], th = thick * line[2];
                        band(cx, cx + side * reach * line[1], y - th, th * 2.0f, Color{255, 170, 40, alphaOf(k)});
                        band(cx, cx + side * reach * line[1], y - th * 0.4f, th * 0.8f, Color{255, 245, 190, alphaOf(0.7f * k)});
                    }
            }
            // The letters: gold, then hot orange with a glow.
            drawLayer(state.rearmedText, min, max, Color{gold.r, gold.g, gold.b, alphaOf((1.0f - glow) * live)});
            if (glow > 0.0f)
            {
                BeginBlendMode(BLEND_ADDITIVE);
                drawLayer(state.rearmedGlow, min, max, Color{255, 100, 10, alphaOf(0.95f * glow * live)});
                EndBlendMode();
                drawLayer(state.rearmedText, min, max, Color{255, 190, 60, alphaOf(glow * live)});
                BeginBlendMode(BLEND_ADDITIVE);
                drawLayer(state.rearmedText, min, max, Color{160, 140, 100, alphaOf(0.7f * glow * live)});
                EndBlendMode();
            }
        }

        // The logo, after ImGui laid it out: the kana behind (moved a little with the cursor), GOD HAND, the plate.
        // While Play's animation runs the plate is drawn later, over the fade.
        void drawLogo(const State &state)
        {
            if (!state.logoThisFrame)
                return;
            const ImVec2 shift(state.parallax.x * fontSize(state, 10.0f), state.parallax.y * fontSize(state, 7.0f));
            drawLayer(state.kana, ImVec2(state.logoMin.x + shift.x, state.logoMin.y + shift.y),
                      ImVec2(state.logoMax.x + shift.x, state.logoMax.y + shift.y), WHITE);
            drawLayer(state.logo, state.logoMin, state.logoMax, WHITE);
            if (state.playTime < 0.0f)
                drawPlate(state, -1.0f);
        }

        void title(State &state)
        {
            if (state.logo.id != 0u && state.logo.height > 0)
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
                // Big: as wide as fits, at most half the window's height. Only laid out here: drawLogo draws the
                // layers after ImGui (tints, the additive glow and the Play animation need raylib).
                const float aspect = static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width);
                const float width = std::min({ImGui::GetWindowWidth() * 0.92f, fontSize(state, 1300.0f),
                                              ImGui::GetWindowHeight() * 0.50f / aspect});
                const float height = width * aspect;
                const ImVec2 start((ImGui::GetWindowWidth() - width) * 0.5f, ImGui::GetCursorPosY());
                ImGui::SetCursorPos(start);
                const ImVec2 screenStart = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, height));
                state.logoMin = screenStart;
                state.logoMax = ImVec2(screenStart.x + width, screenStart.y + height);
                state.logoPlaced = true;
                state.logoThisFrame = true;
                ImGui::SetCursorPos(ImVec2(0.0f, start.y + height));
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            }
            else
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 70.0f)));
                pushSize(state, 92.0f);
                centeredText("GOD HAND: REARMED", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
                ImGui::PopFont();
            }
            pushSize(state, 22.0f);
            centeredText("God Hand\u2122 for PS2 Recompiled, Refined, Rearmed.", kText);
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
            loadMenuSounds(state, image);
            startBark(state, image);
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
                centeredText("To play, point the port to your own copy of the game.", kTextSoft);
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
                // Play starts selected, its highlight showing, so A / Enter plays at once (SetItemDefaultFocus alone
                // left the cursor hidden until a d-pad press, K Oct 29).
                if (state.focusFirst)
                {
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                const bool play = bigButton(state, "Play", buttonWidth, true);
                ownSoundItem();
                if (play && state.playTime < 0.0f)
                {
                    state.playTime = 0.0f; // the plate's animation and the fade; the main loop starts the game after
                    state.soundThisFrame = true; // Play's own sound, next frame
                }
                if (bigButton(state, "Options", buttonWidth, false))
                {
                    state.screen = Screen::Options;
                    state.focusFirst = true;
                    g_hotkeysOpen = false;
                }
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
                pushSize(state, 20.0f);
                bool hide = !state.ini.getBool("Launcher", "Show", true);
                const char *label = "Don't show this again (hold Shift while starting to see it)";
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(label).x - ImGui::GetFrameHeight() -
                                      ImGui::GetStyle().ItemInnerSpacing.x) *
                                     0.5f);
                const bool toggled = ImGui::Checkbox(label, &hide);
                tickItem();
                if (toggled)
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
                                       "God Hand: Rearmed is an unofficial fan-made project, not affiliated with or endorsed by Capcom or Clover Studio. "
                                       "It contains no game data: play your own damn copy!"};
                const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                const float y = ImGui::GetWindowHeight() - lineHeight * 2.0f - fontSize(state, 18.0f);
                if (ImGui::GetCursorPosY() < y)
                    ImGui::SetCursorPosY(y);
                for (const char *line : lines)
                    centeredText(line, kTextSoft);
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
            const bool leave = ImGui::Button(state.installed ? "Continue" : "Back");
            if (!state.installed)
                ownSoundItem();
            if (leave)
            {
                if (!state.installed)
                    menuSound(state, kCancel);
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

        // A tab's settings come in categories: an orange heading with a rule across the panel (it is also the line above
        // the category's first setting), then the category's own settings table (the columns line up from table to
        // table). Only one tab is drawn a frame, so g_categories counts that tab's headings: room above all but the first.
        int g_categories = 0;

        void categoryGap() { ImGui::Dummy(ImVec2(0.0f, std::round(ImGui::GetFontSize() * (g_categories++ > 0 ? 0.8f : 0.2f)))); }

        void categoryHeading(const char *title)
        {
            categoryGap();
            ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
            ImGui::SeparatorText(title);
            ImGui::PopStyleColor();
        }

        bool beginCategory(const char *title)
        {
            categoryHeading(title);
            return beginSettings(title);
        }

        // A heading that folds its category away: the arrow takes the place of the rule's short left end.
        bool collapsibleHeading(const char *title, bool &open)
        {
            categoryGap();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float right = start.x + ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemOpen(open, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
            open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_NoTreePushOnOpen);
            tickItem();
            ImGui::PopStyleColor();
            const ImGuiStyle &style = ImGui::GetStyle();
            const float x0 = start.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(title, nullptr, true).x +
                             style.ItemSpacing.x;
            const float y = std::floor((ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f + 0.99f);
            if (right > x0 && style.SeparatorTextBorderSize > 0.0f)
                ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(right, y), ImGui::GetColorU32(ImGuiCol_Separator),
                                                    style.SeparatorTextBorderSize);
            return open;
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
            const bool toggled = ImGui::Checkbox(id, &value);
            tickItem();
            if (toggled)
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
            const float distance = value("CameraZoomOut", 0.0f), tilt = value("CameraTilt", 0.0f);
            const auto same = [](float a, float b) { return std::fabs(a - b) < 0.05f; };
            int current = -1;
            for (size_t i = 0; i < std::size(kCameraPresets); ++i)
            {
                const CameraPreset &preset = kCameraPresets[i];
                if (same(fov, preset.fieldOfView) && same(fight, preset.fightFieldOfView) &&
                    same(distance, preset.cameraZoomOut) && same(tilt, preset.cameraTilt))
                    current = static_cast<int>(i);
            }
            ImGui::SetNextItemWidth(-1.0f);
            const bool comboOpen = ImGui::BeginCombo("##camera", current >= 0 ? kCameraPresets[current].name : "Custom");
            tickItem();
            if (comboOpen)
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
                        state.ini.set("Video", "CameraZoomOut", text(preset.cameraZoomOut));
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

        // The port's shortcut keys (runtime/ps2_hotkeys.h): GHR.ini [Hotkeys], one line per action. The runtime's
        // table is set too, so a key bound here is live when the game starts from this launcher.
        namespace hk = ps2x::hotkeys;
        int g_hotkeyCapture = -1;
        double g_hotkeyStarted = 0.0;
        bool g_hotkeyWaitRelease = false; // the Enter / Space / click that opened the capture must go up first
        bool g_openHotkeyCapture = false;
        std::string g_hotkeyStatus;

        void loadHotkeys(const State &state)
        {
            for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
            {
                const hk::Action action = static_cast<hk::Action>(i);
                const std::string value = state.ini.get("Hotkeys", hk::info(action).id, "\x01");
                hk::Binding binding;
                if (value != "\x01" && hk::parse(value, binding) &&
                    !(action == hk::Action::RecordCase && binding.pad == ps2x::iop::PadButton::R3)) // old default (Nov 3)
                    hk::setBinding(action, binding);
            }
        }

        void storeHotkey(State &state, hk::Action action, hk::Binding binding)
        {
            hk::setBinding(action, binding);
            state.ini.set("Hotkeys", hk::info(action).id, binding.bound() ? hk::text(binding) : "none");
        }

        void drawHotkeys(State &state)
        {
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                loadHotkeys(state);
            }
            if (collapsibleHeading("Hotkeys", g_hotkeysOpen))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("The port's own keys. Click one, then press the key (with Shift or Ctrl held if you want), "
                                   "or pick a controller button. Right-click clears it. F1 (the Controls screen) stays as it "
                                   "is. The F2 menu's Controls tab edits the same list.");
                ImGui::PopStyleColor();
                if (ImGui::BeginTable("hotkeys", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.4f);
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
                    {
                        const hk::Action action = static_cast<hk::Action>(i);
                        const hk::Info &info = hk::info(action);
                        const hk::Binding binding = hk::binding(action);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(info.label);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(1000 + i);
                        const std::string label = (binding.bound() ? hk::text(binding) : std::string("-")) + "##hk";
                        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f)))
                        {
                            g_hotkeyCapture = i;
                            g_hotkeyStarted = GetTime();
                            g_openHotkeyCapture = true;
                            g_hotkeyWaitRelease = true;
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                        {
                            storeHotkey(state, action, hk::Binding{});
                            g_hotkeyStatus = std::string(info.label) + ": no key.";
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (!g_hotkeyStatus.empty())
                    ImGui::TextColored(kTextDim, "%s", g_hotkeyStatus.c_str());
                if (ImGui::Button("Default hotkeys"))
                {
                    for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
                        storeHotkey(state, static_cast<hk::Action>(i), hk::info(static_cast<hk::Action>(i)).def);
                    g_hotkeyStatus = "Default hotkeys restored.";
                }
            }

            if (g_openHotkeyCapture)
            {
                g_openHotkeyCapture = false;
                ImGui::OpenPopup("##hotkeycapture");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("##hotkeycapture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
            {
                g_captureFrame = true;
                const hk::Action action = static_cast<hk::Action>(g_hotkeyCapture);
                ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 0.5f));
                ImGui::TextUnformatted(hk::info(action).label);
                ImGui::TextColored(kTextDim, "Press a key (Shift or Ctrl with it if you like)...");
                ImGui::TextColored(kTextDim, "Esc cancels (%d s)", std::max(0, 10 - static_cast<int>(GetTime() - g_hotkeyStarted)));
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
                bool done = false;
                if (g_hotkeyWaitRelease)
                {
                    (void)hk::capturePressed(); // the queue holds the activating key still
                    if (!(IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER) || IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_LEFT)))
                        g_hotkeyWaitRelease = false;
                }
                else if (IsKeyPressed(KEY_ESCAPE) || GetTime() - g_hotkeyStarted > 10.0)
                {
                    g_hotkeyStatus = IsKeyPressed(KEY_ESCAPE) ? "Cancelled." : "Nothing pressed for 10 s: cancelled.";
                    done = true;
                }
                else if (const int key = hk::capturePressed(); key != 0)
                {
                    const hk::Binding binding{key, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT),
                                              IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)};
                    const hk::Action other = hk::conflict(action, binding);
                    if (other != hk::Action::Count)
                        g_hotkeyStatus = hk::text(binding) + " is already " + hk::info(other).label + "; not changed.";
                    else
                    {
                        storeHotkey(state, action, binding);
                        g_hotkeyStatus = std::string(hk::info(action).label) + ": " + hk::text(binding);
                        const std::string use = pc::keyboardKeyUse(kPlayer, key);
                        if (!use.empty() && !binding.shift && !binding.ctrl)
                            g_hotkeyStatus += "   (also " + use + " in the game's controls: it will do both)";
                    }
                    done = true;
                }
                // Or a controller button, as the game sees it (Right Stick Press, Select...): the game layer watches the pad data for it.
                if (!done)
                {
                    ImGui::TextColored(kTextDim, "...or a controller button:");
                    int padCount = 0;
                    const hk::PadName *pads = hk::padNames(padCount);
                    for (int p = 0; p < padCount; ++p)
                    {
                        if (p % 8 != 0)
                            ImGui::SameLine();
                        ImGui::PushID(2000 + p);
                        if (ImGui::SmallButton(pads[p].name))
                        {
                            const hk::Binding binding{0, false, false, pads[p].bit};
                            const hk::Action other = hk::conflict(action, binding);
                            if (other != hk::Action::Count)
                                g_hotkeyStatus = hk::text(binding) + " is already " + hk::info(other).label + "; not changed.";
                            else
                            {
                                storeHotkey(state, action, binding);
                                g_hotkeyStatus = std::string(hk::info(action).label) + ": " + hk::text(binding) +
                                                 "   (the game sees the button too)";
                            }
                            done = true;
                        }
                        ImGui::PopID();
                    }
                }
                if (done)
                {
                    g_hotkeyCapture = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        void drawControls(State &state)
        {
            if (beginCategory("Menus & Prompts"))
            {
                settingLabel("Menu Controls", "Western: the bottom button confirms and the right one cancels (A / B, or "
                                              "cross / circle). Japanese: the right button confirms and the bottom one "
                                              "cancels (circle / cross). Classic: as on the PS2, circle and cross confirm "
                                              "and triangle cancels. Menus, dialogs and the roulette.",
                             "F2 > Controls changes it while playing.");
                iniCombo(state, "##menubuttons", "Game", "MenuButtons", "western", [](const char *id, std::string &value) {
                    if (value == "modern")
                        value = "western"; // the name before Oct 31
                    return choiceCombo(id, kMenuButtons, value);
                });
                settingLabel("Button Prompts", "The buttons the game shows for a controller. Automatic picks them from the "
                                               "controller you use; keys and mouse always show keys. Prompt art: Xelu's "
                                               "free prompts (CC0).");
                iniCombo(state, "##buttonprompts", "Game", "ButtonPrompts", "auto",
                         [](const char *id, std::string &value) { return choiceCombo(id, kButtonPrompts, value); });
                ImGui::EndTable();
            }
            categoryHeading("Bindings");
            static int device = 0; // 0 keyboard & mouse, 1 controller
            ImGui::TextUnformatted("Bindings for");
            ImGui::SameLine();
            ImGui::RadioButton("Keyboard & mouse", &device, 0);
            tickItem();
            ImGui::SameLine();
            ImGui::RadioButton("Controller", &device, 1);
            tickItem();
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

            drawHotkeys(state);

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
            centeredText("Options", kHeading);
            ImGui::PopFont();
            const float width = std::min(ImGui::GetWindowWidth() - fontSize(state, 60.0f), fontSize(state, 1040.0f));
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 21.0f);
            const float footer = ImGui::GetFrameHeightWithSpacing() + fontSize(state, 36.0f);
            ImGui::BeginChild("options", ImVec2(width, ImGui::GetContentRegionAvail().y - footer),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            const bool advanced = state.ini.getBool("Launcher", "ShowAdvanced", false);
            g_categories = 0;
            if (ImGui::BeginTabBar("tabs"))
            {
                const std::string aspect = state.ini.get("Video", "AspectRatio", "16:9");
                if (ImGui::BeginTabItem("Display"))
                {
                    if (beginCategory("Game Window"))
                    {
                        settingLabel("Display Mode", "How the game opens.", "F11 switches between window and fullscreen "
                                                                            "while playing.");
                        std::string mode = state.ini.getBool("Video", "StartFullscreen", true)    ? "fullscreen"
                                           : state.ini.getBool("Video", "StartMaximized", false) ? "maximized"
                                                                                                 : "window";
                        if (choiceCombo("##window", kWindowModes, mode))
                        {
                            state.ini.setBool("Video", "StartFullscreen", mode == "fullscreen");
                            state.ini.setBool("Video", "StartMaximized", mode == "maximized");
                        }
                        subSetting("Window Size", "The window's size when it opens (not maximized or fullscreen).");
                        ImGui::BeginDisabled(mode != "window");
                        iniCombo(state, "##size", "Video", "WindowSize", "auto",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kWindowSizes, value); });
                        ImGui::EndDisabled();
                        settingLabel("Aspect Ratio", "The picture's shape. Wider than 4:3 shows more of the scene to the "
                                                     "sides; menus keep the PS2's proportions.");
                        iniCombo(state, "##aspect", "Video", "AspectRatio", "16:9",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kAspects, value); });
                        ImGui::EndTable();
                    }
                    if (beginCategory("Camera"))
                    {
                        settingLabel("Camera & FOV", "How much the gameplay camera shows and how it frames Gene (widescreen "
                                                     "already widens it sideways). Cutscenes keep their framing.",
                                     "F2 while playing tunes it live.",
                                     [&] { iniCheckbox(state, "Show advanced settings##advanced", "Launcher", "ShowAdvanced", false); });
                        cameraPresetCombo(state);
                        if (advanced)
                        {
                            subSetting("Field of View", "How much the camera shows, both ways, outside fights.");
                            iniCombo(state, "##fov", "Video", "FieldOfView", "100",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kFovs, value); });
                            subSetting("Field of View in Fights", "The same while the game's battle camera is on (Gene "
                                                                  "has an enemy targeted). 100% frames fights as on the PS2.");
                            iniCombo(state, "##fightfov", "Video", "FightFieldOfView", "100",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kFovs, value); });
                            subSetting("Camera Distance", "At a wider field of view, how far back the camera stays. "
                                                          "0% keeps Gene the size he is at 100%; 100% shows more round him.");
                            iniCombo(state, "##camdist", "Video", "CameraZoomOut", "0",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kCameraDistances, value); });
                            subSetting("Camera Tilt", "At a wider field of view, the camera also rises and looks down at "
                                                      "Gene by this much: more of the floor ahead, less sky.");
                            iniCombo(state, "##camtilt", "Video", "CameraTilt", "0",
                                     [](const char *id, std::string &value) { return choiceCombo(id, kCameraTilts, value); });
                            subSetting("Enemies Follow the View", "The angles round Gene in which enemies close in and "
                                                                  "lunge widen with the picture, so the ones you can see at "
                                                                  "the edges join in as on the PS2. Off: the original angles.");
                            iniCheckbox(state, "On##enemyview", "Game", "EnemiesFollowView", true);
                        }
                        settingLabel("Steady Idle Camera", "While Gene stands idle, the camera stops rocking and "
                                                           "shaking with his stomps.",
                                     "F2 > Camera switches it while playing.");
                        iniCheckbox(state, "On##steadyidle", "Video", "SteadyIdleCamera", false);
                        settingLabel("See Through", "When walls or pillars block the view of Gene or the enemies about "
                                                    "him, they dissolve in an oval round him, so the fight stays in view. Walls next "
                                                    "to the camera, which the PS2 drops, are drawn too. Known: a banded "
                                                    "blur where hazy distance shows through (the depth of field reads "
                                                    "the dissolve).",
                                     "F2 > Camera sizes it while playing.");
                        iniCheckbox(state, "On##seethrough", "Video", "SeeThrough", true);
                        ImGui::EndTable();
                    }
                    if (beginCategory("Interface"))
                    {
                        settingLabel("HUD Layout", "Where the health bar, radar and the rest of the in-game HUD go on a "
                                                   "wide screen. Wider than 16:9 keeps them at the edges of a 16:9 area.",
                                     "F7 switches it while playing.");
                        ImGui::BeginDisabled(lower(aspect) == "4:3");
                        iniCombo(state, "##hud", "Video", "HudLayout", "16:9",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kHudLayouts, value); });
                        ImGui::EndDisabled();
                        settingLabel("Menu Width", "Menus and screens (pause, options, roulette setup) are drawn for 4:3: "
                                                   "shown 4:3 with side bars, or stretched to the screen.");
                        ImGui::BeginDisabled(lower(aspect) == "4:3");
                        iniCombo(state, "##menus", "Video", "MenuLayout", "4:3",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kMenuLayouts, value); });
                        ImGui::EndDisabled();
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Graphics"))
                {
                    if (beginCategory("Renderer"))
                    {
                        settingLabel("Graphics API", "Vulkan is the default. If Vulkan fails to start, the game uses "
                                                     "Direct3D 11 instead.");
                        iniCombo(state, "##graphicsapi", "Video", "GraphicsAPI", "vulkan",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kGraphicsApis, value); });
                        settingLabel("Internal Resolution", "How sharp the 3D is drawn. 3x suits a 1440p screen, 4x and "
                                                            "up a 4K one; higher costs more GPU time.");
                        iniCombo(state, "##scale", "Video", "InternalResolution", "3",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kResolutions, value); });
                        ImGui::EndTable();
                    }
                    if (beginCategory("Effects"))
                    {
                        settingLabel("Post Effects", "The blur effects (depth of field, heat haze) are drawn from small "
                                                     "copies of the frame. PS2 resolution keeps them aligned but grainy "
                                                     "at high internal resolutions; Upscaled draws them sharp.");
                        iniCombo(state, "##posteffects", "Video", "PostEffects", "ps2",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kPostEffects, value); });
                        settingLabel("Depth of Field", "The game's blur of the distance. At high internal resolutions "
                                                       "its edges can show dark, shimmering fringes.",
                                     "F2 switches it while playing.");
                        iniCheckbox(state, "On##dof", "Video", "DepthOfField", true);
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Gameplay"))
                {
                    if (beginCategory("Speed & Feed"))
                    {
                        settingLabel("60 FPS", "Runs the game at 60 frames a second instead of 30, with its timers kept "
                                               "at the PS2's speed.");
                        iniCheckbox(state, "On##fps60", "Game", "Fps60", false);
                        ImGui::EndTable();
                    }
                    if (beginCategory("Move & Groove"))
                    {
                        settingLabel("Click-Stick Dash", "Clicking the left stick dashes (where the stick points, or "
                                                         "straight ahead).");
                        iniCheckbox(state, "On##l3", "Game", "L3Dash", false);
                        settingLabel("Control Style", "Classic: as on the PS2. Modern: the free camera: the right stick "
                                                      "turns the camera and Gene moves where you point; pressing "
                                                      "the right stick switches to brawl mode (classic controls) and back (the radar "
                                                      "glows while the free camera is on).",
                                     "F2 > Controls changes it while playing.");
                        iniCombo(state, "##controlstyle", "Game", "ControlStyle", "classic",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kControlStyles, value); });
                        if (lower(state.ini.get("Game", "ControlStyle", "classic")) == "modern")
                        {
                            subSetting("Modern: Auto", "Fighting starts brawl mode automatically; pressing the right stick switches back "
                                                       "to the free camera.");
                            iniCheckbox(state, "On##modernauto", "Game", "ModernAuto", true);
                            subSetting("Modern: Mouse Camera", "The mouse turns the free camera. Click in the game to "
                                                               "capture the mouse, Esc frees it.",
                                       "F2 > Controls sets the sensitivity.");
                            iniCheckbox(state, "On##mousecamera", "Game", "MouseCamera", true);
                        }
                        settingLabel("Busta-Chan Jive", "Sprint attacks carry Gene over or through the railings marked "
                                                        "for them: the diving attack and the flying kick jump over, the "
                                                        "slide goes under.",
                                     "F2 > Debug > Stage collision marks them.");
                        iniCheckbox(state, "On##sprinttraversal", "Game", "SprintTraversal", false);
                        ImGui::EndTable();
                    }
                    if (beginCategory("Style Shuffle"))
                    {
                        settingLabel("Moveset Presets", "Keep up to four sets of techniques as presets (the styles S, E, "
                                                        "X and Y) and swap between them in play with the d-pad. On the "
                                                        "Techniques screen, L2 / R2 cycle them.");
                        iniCheckbox(state, "On##styles", "Game", "StyleSwitching", false);
                        settingLabel("Buck Breaker", "Choose how to punish stunned enemies with a new slot on the "
                                                     "Techniques screen. Works with Moveset Presets.");
                        iniCheckbox(state, "On##buckbreaker", "Game", "BuckBreaker", false);
                        ImGui::EndTable();
                    }
                    if (beginCategory("Fightin' Excitement"))
                    {
                        settingLabel("Cunt Punter", "Ball Buster works on female enemies too, the same as on men.");
                        iniCheckbox(state, "On##ballbusterwomen", "Game", "BallBusterWomen", false);
                        settingLabel("Sway Bar", "A stun gauge under each enemy's health bar: it fills as you hit them and "
                                                 "drains while they are stunned.");
                        iniCheckbox(state, "On##swaybar", "Game", "SwayBar", false);
                        settingLabel("Tension is Placable", "Pressing R2 / RT again while the God Hand is unleashed ends "
                                                            "it and keeps the tension left in the meter.");
                        iniCheckbox(state, "On##tensionplacable", "Game", "TensionPlacable", false);
                        if (state.ini.getBool("Game", "TensionPlacable", false))
                        {
                            subSetting("Off the Cuff", "The God Hand unleashes with any tension in the meter, not "
                                                       "only a full one.\nOnly unleashing the God Hand with a full meter "
                                                       "makes Gene invulnerable.");
                            iniCheckbox(state, "On##offthecuff", "Game", "OffTheCuff", false);
                        }
                        settingLabel("Wild Curry Funk", "Lets you pick up, carry, and throw chairs.");
                        iniCheckbox(state, "On##liftchairs", "Game", "LiftChairs", false);
                        ImGui::EndTable();
                    }
                    if (beginCategory("Roulette Wheel & Reel"))
                    {
                        settingLabel("Roulette Timer", "How long you have to pick a roulette move. Double time gives "
                                                       "twice as long (the dial runs half as fast), at 30 and 60 FPS.");
                        iniCombo(state, "##roulette", "Game", "RouletteTimer", "normal",
                                 [](const char *id, std::string &value) { return choiceCombo(id, kRouletteTimers, value); });
                        settingLabel("Target Select", "While the roulette wheel is up, flick the right stick or "
                                                      "press the d-pad left or right to aim it at the next enemy "
                                                      "that way.");
                        iniCheckbox(state, "On##roulettecycle", "Game", "RouletteTargetCycle", false);
                        settingLabel("Dry Spinning", "R1 opens the roulette even with no orbs left. Nothing "
                                                     "can be picked without orbs, but the roulette camera "
                                                     "still turns Gene (Classic camera).");
                        iniCheckbox(state, "On##roulettenoorbs", "Game", "RouletteNoOrbs", false);
                        ImGui::EndTable();
                    }
                    if (beginCategory("Skip Poker"))
                    {
                        settingLabel("Intro Skipper", "Start at the main menu: no Capcom and Clover logos and no "
                                                      "\"Press Start\" screen.");
                        iniCheckbox(state, "On##skipintro", "Game", "SkipIntro", false);
                        settingLabel("Hard-On", "Hard can be picked from the start instead of after a clear. Your "
                                                "save is not changed.");
                        iniCheckbox(state, "On##hard", "Game", "UnlockHard", false);
                        settingLabel("Mute Music", "Stage, boss, menu and minigame music plays silent. Cutscenes keep "
                                                   "their sound (their music and voices are one track).");
                        iniCheckbox(state, "On##music", "Sound", "MuteMusic", false);
                        ImGui::EndTable();
                    }
                    if (beginCategory("Misc. & Risk"))
                    {
                        settingLabel("Better Shop & Casino Poses", "The shop and casino girls lean on their counters "
                                                                   "and tables in poses made for each, instead of the "
                                                                   "game's standing idle.",
                                     "F2 > NPCs sets the poses.");
                        iniCheckbox(state, "On##npcposes", "Game", "NpcPoses", false);
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Controls"))
                {
                    drawControls(state);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Mods"))
                {
                    if (beginCategory("Character"))
                    {
                        settingLabel("Character", "Who you play as. Currently, other characters use Gene's moves and his "
                                                  "God Hand.");
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
                        const bool pickedNew = ImGui::Combo("##character", &picked, names.data(), static_cast<int>(names.size()));
                        tickItem();
                        if (pickedNew)
                            state.ini.set("Mods", "Character", list[static_cast<size_t>(picked)].id);
                        if (!list[static_cast<size_t>(picked)].credit.empty())
                            ImGui::TextColored(kTextDim, "%s", list[static_cast<size_t>(picked)].credit.c_str());
                        ImGui::EndTable();
                    }
                    if (beginCategory("Texture Packs"))
                    {
                        settingLabel("Replacement Textures", "HD texture packs in PCSX2's format.");
                        iniCheckbox(state, "Use replacement textures##tex", "Runtime", "PS2X_PCSX2_TEXTURES", true);
                        subSetting("Texture Pack Folder", "The folder that holds replacements/. Empty: textures/SLUS-21503 "
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
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            const float backWidth = fontSize(state, 200.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - backWidth) * 0.5f);
            const bool back = ImGui::Button("Back", ImVec2(backWidth, 0.0f));
            ownSoundItem();
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            const bool cancel = !g_captureFrame && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
            ImGui::PopFont();
            if (back || cancel)
            {
                menuSound(state, kCancel);
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
        // [Video] CameraDistance (100 = Gene keeps his size) became CameraZoomOut (0 = that) on Oct 30: carry it over.
        if (state.ini.get("Video", "CameraZoomOut").empty() && !state.ini.get("Video", "CameraDistance").empty())
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", 100.0 - std::atof(state.ini.get("Video", "CameraDistance").c_str()));
            state.ini.set("Video", "CameraZoomOut", text);
        }
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

        ps2_host_display::openHostWindow(kWindowTitle);
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
        const auto loadImage = [&](Texture2D &texture, const char *name)
        {
            const fs::path path = context.exeDir / "resources" / "images" / name;
            if (!fs::is_regular_file(path, error))
                return;
            texture = LoadTexture(utf8(path).c_str());
            if (texture.id != 0u)
            {
                GenTextureMipmaps(&texture);
                SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
            }
        };
        loadImage(state.logo, "logo.png");
        loadImage(state.kana, "kana.png");
        loadImage(state.rearmed, "rearmed.png");
        loadImage(state.rearmedText, "rearmed_text.png");
        loadImage(state.rearmedGlow, "rearmed_glow.png");
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
        const fs::path backgroundPath = context.exeDir / "resources" / "images" / "background.png";
        if (fs::is_regular_file(backgroundPath, error))
        {
            state.background = LoadTexture(utf8(backgroundPath).c_str());
            if (state.background.id != 0u)
            {
                GenTextureMipmaps(&state.background);
                SetTextureFilter(state.background, TEXTURE_FILTER_TRILINEAR);
                SetTextureWrap(state.background, TEXTURE_WRAP_CLAMP);
                const int gl = rlGetVersion();
                const char *header = (gl == RL_OPENGL_33 || gl == RL_OPENGL_43) ? "#version 330\n"
                                     : gl == RL_OPENGL_ES_30                    ? "#version 300 es\nprecision highp float;\n"
                                                                                : nullptr;
                if (header != nullptr)
                {
                    const std::string source = std::string(header) + kBackgroundShader;
                    state.backgroundShader = LoadShaderFromMemory(nullptr, source.c_str());
                    state.backgroundTexel = GetShaderLocation(state.backgroundShader, "texel");
                    state.backgroundTime = GetShaderLocation(state.backgroundShader, "time");
                    state.backgroundView = GetShaderLocation(state.backgroundShader, "view");
                }
            }
        }
        loadSplash(state, context.exeDir / "resources" / "splash");
        InitAudioDevice();
        state.audio = IsAudioDeviceReady();
        const fs::path musicPath = context.exeDir / "resources" / "music" / "launcher.ogg";
        state.musicPath = musicPath;
        if (state.audio && fs::is_regular_file(musicPath, error))
        {
            state.music = LoadMusicStream(utf8(musicPath).c_str());
            state.hasMusic = IsMusicValid(state.music);
            if (state.hasMusic)
            {
                state.music.looping = true; // the file is cut to loop seamlessly
                SetMusicVolume(state.music, kMusicVolume);
                PlayMusicStream(state.music);
            }
        }
        state.volume = std::clamp(static_cast<float>(std::atof(state.ini.get("Launcher", "Volume", "0.8").c_str())), 0.0f, 1.0f);
        state.muted = state.ini.getBool("Launcher", "Mute", false);
        if (state.installed)
        {
            loadMenuSounds(state, context.iso);
            startBark(state, context.iso);
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
            state.logoThisFrame = false;
            state.soundThisFrame = false;
            g_tickItems.clear();
            g_ownSoundItems.clear();
            if (state.playTime >= 0.0f)
            {
                if (state.playTime == 0.0f)
                    playBark(state);
                state.playTime += std::min(GetFrameTime(), 0.05f);
                if (state.playTime >= kPlayLength)
                {
                    state.result = Result::Play;
                    state.done = true;
                }
            }
            if (state.hasMusic && !state.musicHandedOff)
            {
                const float fade = state.playTime >= 0.0f ? std::clamp(state.playTime / (kPlayLength - 0.02f), 0.0f, 1.0f) : 0.0f;
                SetMusicVolume(state.music, kMusicVolume * userVolume(state) * (1.0f - fade));
                UpdateMusicStream(state.music);
            }
            const bool playing = state.playTime >= 0.0f;
            BeginDrawing();
            if (!drawDesert(state))
                drawBackground();
            drawBurst(state);
            rlImGuiBegin();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            if (ImGui::Begin("##launcher", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus))
            {
                // While Play's animation runs nothing takes input (and nothing greys out: it is fading anyway).
                if (playing)
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
                    ImGui::BeginDisabled();
                }
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
                if (state.screen != Screen::Options)
                    drawVolume(state);
                if (playing)
                {
                    ImGui::EndDisabled();
                    ImGui::PopStyleVar();
                }
            }
            ImGui::End();
            frameSounds(state);
            rlImGuiEnd();
            drawLogo(state);
            drawSplash(state);
            if (playing)
            {
                DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                              Color{0, 0, 0, alphaOf(state.playTime / (kPlayLength - 0.02f))});
                if (state.logoThisFrame)
                    drawPlate(state, state.playTime);
            }
            EndDrawing();
        }
        state.ini.save();
        saveControls();
        if (state.logo.id != 0u)
            UnloadTexture(state.logo);
        for (Texture2D *texture : {&state.kana, &state.rearmed, &state.rearmedText, &state.rearmedGlow})
            if (texture->id != 0u)
                UnloadTexture(*texture);
        if (state.burst.id != 0u)
            UnloadTexture(state.burst);
        if (state.background.id != 0u)
            UnloadTexture(state.background);
        if (state.backgroundShader.id != 0u && state.backgroundShader.id != rlGetShaderIdDefault())
            UnloadShader(state.backgroundShader);
        if (state.splashFont.texture.id != 0u)
            UnloadFont(state.splashFont);
        if (state.barkWorker.joinable())
            state.barkWorker.join();
        if (state.hasMusic)
            UnloadMusicStream(state.music);
        if (state.result == Result::Play && state.audio)
        {
            // The game takes the audio device over (the runtime opens it only when it is not open yet), so Play's
            // sound rings on while the game boots; the sound's few samples are left to the device until then.
            KM_INFO(Boot, "launcher: audio device handed to the game");
        }
        else
        {
            for (int i = 0; i < 4; ++i)
                if (state.hasMenuSound[i])
                    UnloadSound(state.menuSounds[i]);
            if (state.audio)
                CloseAudioDevice();
        }
        rlImGuiShutdown(); // the F12 debugger sets ImGui up again later
        // One empty frame, so the window does not keep showing the launcher until the game's first picture.
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
        KM_INFO(Boot, "launcher: " << (state.result == Result::Play ? "play" : "quit"));
        return state.result;
    }
}
