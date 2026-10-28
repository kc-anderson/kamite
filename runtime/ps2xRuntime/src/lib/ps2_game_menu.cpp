#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_hotkeys.h"

#include "ps2_host_backend.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/gs/gs_pcsx2_backend.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define PS2X_HAS_GAME_MENU 1
#endif

namespace ps2x::menu
{
    namespace
    {
        struct Panel
        {
            std::string title;
            std::vector<DrawPanel> draws; // the tab's sections, in registration order
        };
        std::vector<std::string> g_order;

        std::mutex g_mutex; // panels and texts (registered before the menu can open, drawn on the GS thread)
        std::vector<Panel> g_panels;
        std::string g_title = "Menu";
        std::string g_hint;
        std::string g_empty;
        std::atomic<bool> g_open{false};
        std::atomic<bool> g_closedByWindow{false}; // the window's close button (GS thread)
        std::atomic<bool> g_keepPlaying{true};     // the game keeps the controllers and keys; the menu has the mouse
                                                   // (on by default, K, Oct 27: the menu is a dev tool used mid-fight)
        std::atomic<bool> g_keepPlayingChanged{false};
        std::atomic<int> g_captureState{0}; // 0 idle, 1 capturing, 2 captured (g_captured*)
        std::atomic<bool> g_captureFresh{false}; // a capture just began: wait for the activating key to go up
        std::atomic<int> g_capturedKey{0};
        std::atomic<bool> g_capturedShift{false}, g_capturedCtrl{false};
        std::mutex g_captureMutex;
        std::string g_capturedUse; // the game button the captured key is bound to ("" = none)

        bool enabled()
        {
            static const bool on = []
            {
                const char *value = std::getenv("PS2X_GAME_MENU");
                return !(value && *value == '0');
            }();
            return on;
        }

#if defined(PS2X_HAS_GAME_MENU)
        // GS thread, at every present while the menu is open.
        void drawMenu(const Pcsx2GsUi *ui, void *)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            int open = 1;
            if (ui->begin_window(g_title.c_str(), &open, 760.0f, 540.0f))
            {
                if (!g_hint.empty())
                    ui->text_disabled(g_hint.c_str());
                int keep = g_keepPlaying.load(std::memory_order_relaxed) ? 1 : 0;
                if (ui->checkbox("Keep playing while this is open (controller and keyboard play; the mouse works the menu)", &keep))
                {
                    g_keepPlaying.store(keep != 0, std::memory_order_relaxed);
                    g_keepPlayingChanged.store(true, std::memory_order_relaxed);
                }
                if (g_panels.empty())
                {
                    ui->spacing();
                    ui->text_wrapped(g_empty.c_str());
                }
                else if (ui->begin_tab_bar("##panels"))
                {
                    for (const Panel &panel : g_panels)
                    {
                        if (ui->begin_tab(panel.title.c_str()))
                        {
                            for (size_t i = 0; i < panel.draws.size(); ++i)
                            {
                                if (i > 0)
                                    ui->spacing();
                                ui->push_id(static_cast<int>(i));
                                panel.draws[i](ui);
                                ui->pop_id();
                            }
                            ui->end_tab();
                        }
                    }
                    ui->end_tab_bar();
                }
            }
            ui->end_window();
            if (!open)
                g_closedByWindow.store(true, std::memory_order_relaxed);
        }

        // Keyboard, mouse and any controller, for the menu's navigation.
        Pcsx2GsMenuInput readInput()
        {
            Pcsx2GsMenuInput input{};
            const Vector2 mouse = GetMousePosition();
            input.mouse_x = mouse.x;
            input.mouse_y = mouse.y;
            input.mouse_buttons = (IsMouseButtonDown(MOUSE_BUTTON_LEFT) ? 1u : 0u) | (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ? 2u : 0u) |
                                  (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) ? 4u : 0u);
            input.wheel = GetMouseWheelMove();
            const struct { int key; uint32_t bit; } keys[] = {
                {KEY_UP, PCSX2GS_KEY_UP}, {KEY_DOWN, PCSX2GS_KEY_DOWN}, {KEY_LEFT, PCSX2GS_KEY_LEFT},
                {KEY_RIGHT, PCSX2GS_KEY_RIGHT}, {KEY_ENTER, PCSX2GS_KEY_ENTER}, {KEY_KP_ENTER, PCSX2GS_KEY_ENTER},
                {KEY_SPACE, PCSX2GS_KEY_SPACE}, {KEY_ESCAPE, PCSX2GS_KEY_ESCAPE}, {KEY_TAB, PCSX2GS_KEY_TAB},
                {KEY_LEFT_SHIFT, PCSX2GS_KEY_SHIFT}, {KEY_RIGHT_SHIFT, PCSX2GS_KEY_SHIFT},
                {KEY_LEFT_CONTROL, PCSX2GS_KEY_CTRL}, {KEY_RIGHT_CONTROL, PCSX2GS_KEY_CTRL}};
            for (const auto &entry : keys)
                if (IsKeyDown(entry.key))
                    input.keys |= entry.bit;
            using namespace ps2_host_gamepads;
            for (int device = 0; device < kMaxDevices; ++device)
            {
                if (!connected(device))
                    continue;
                const float x = axis(device, LeftX);
                const float y = axis(device, LeftY);
                if (button(device, DPadUp) || y < -0.5f)
                    input.pad |= PCSX2GS_PAD_UP;
                if (button(device, DPadDown) || y > 0.5f)
                    input.pad |= PCSX2GS_PAD_DOWN;
                if (button(device, DPadLeft) || x < -0.5f)
                    input.pad |= PCSX2GS_PAD_LEFT;
                if (button(device, DPadRight) || x > 0.5f)
                    input.pad |= PCSX2GS_PAD_RIGHT;
                if (button(device, South))
                    input.pad |= PCSX2GS_PAD_ACCEPT;
                if (button(device, East))
                    input.pad |= PCSX2GS_PAD_BACK;
                if (button(device, LeftShoulder))
                    input.pad |= PCSX2GS_PAD_L1;
                if (button(device, RightShoulder))
                    input.pad |= PCSX2GS_PAD_R1;
            }
            return input;
        }

        // Who has the input while the menu is open: the menu (the game idles) or, with "keep playing", the game,
        // with only the mouse for the menu.
        void applyInputOwner(bool open)
        {
            const bool keep = g_keepPlaying.load(std::memory_order_relaxed);
            ps2_host_input::setBlocked(open && !keep);
            ps2_host_input::setMenuHasMouse(open); // the cursor is free for the menu either way
        }

        void setOpen(bool open)
        {
            g_open.store(open, std::memory_order_relaxed);
            applyInputOwner(open);
            GSPcsx2Backend::SetMenu(open ? reinterpret_cast<void *>(&drawMenu) : nullptr, nullptr);
            std::fprintf(stderr, "[menu] %s\n", open ? "opened" : "closed");
        }
#endif
    }

    namespace
    {
        void sortPanels()
        {
            // By the order given; tabs not in it keep their place after those, in registration order.
            std::vector<Panel> sorted;
            for (const std::string &title : g_order)
                for (Panel &panel : g_panels)
                    if (panel.title == title && !panel.draws.empty())
                    {
                        sorted.push_back(std::move(panel));
                        panel.draws.clear();
                    }
            for (Panel &panel : g_panels)
                if (!panel.draws.empty())
                    sorted.push_back(std::move(panel));
            g_panels = std::move(sorted);
        }
    }

    void addPanel(std::string title, DrawPanel draw)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Panel &panel : g_panels)
            if (panel.title == title)
            {
                panel.draws.push_back(std::move(draw));
                return;
            }
        g_panels.push_back({std::move(title), {std::move(draw)}});
        sortPanels();
    }

    void setTabOrder(std::vector<std::string> titles)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_order = std::move(titles);
        sortPanels();
    }

    void setTitle(std::string title, std::string hint)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_title = std::move(title);
        g_hint = std::move(hint);
    }

    void setEmptyText(std::string text)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_empty = std::move(text);
    }

    bool isOpen() noexcept
    {
        return g_open.load(std::memory_order_relaxed);
    }

    void hostTick()
    {
#if defined(PS2X_HAS_GAME_MENU)
        if (!enabled() || !GSPcsx2Backend::Active())
            return;
        const bool capturing = isOpen() && g_captureState.load(std::memory_order_relaxed) == 1;
        if (!capturing && ps2x::hotkeys::pressed(ps2x::hotkeys::Action::Menu))
            setOpen(!isOpen());
        else if (g_closedByWindow.exchange(false, std::memory_order_relaxed) && isOpen())
            setOpen(false);
        if (g_keepPlayingChanged.exchange(false, std::memory_order_relaxed))
            applyInputOwner(isOpen());
        if (capturing && isOpen())
        {
            // The Enter / Space that pressed the button must go up first, or it would be the key captured.
            static bool s_waitRelease = true;
            const bool activators = IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER) || IsKeyDown(KEY_SPACE);
            if (g_captureFresh.exchange(false, std::memory_order_relaxed))
                s_waitRelease = true;
            if (s_waitRelease)
            {
                if (!activators)
                    s_waitRelease = false;
                else
                    (void)ps2x::hotkeys::capturePressed(); // drop what is queued meanwhile
            }
            else if (IsKeyPressed(KEY_ESCAPE))
            {
                g_capturedKey.store(0, std::memory_order_relaxed);
                g_captureState.store(2, std::memory_order_release);
            }
            else if (const int key = ps2x::hotkeys::capturePressed(); key != 0)
            {
                {
                    // The game button this key is bound to, read here on the window thread (the bindings' owner).
                    std::lock_guard<std::mutex> lock(g_captureMutex);
                    g_capturedUse = ps2_host_input::controls::keyboardKeyUse(0u, key);
                }
                g_capturedKey.store(key, std::memory_order_relaxed);
                g_capturedShift.store(IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT), std::memory_order_relaxed);
                g_capturedCtrl.store(IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL), std::memory_order_relaxed);
                g_captureState.store(2, std::memory_order_release);
            }
        }
        else if (!isOpen())
            g_captureState.store(0, std::memory_order_relaxed);
        if (isOpen())
        {
            Pcsx2GsMenuInput input = readInput();
            if (g_keepPlaying.load(std::memory_order_relaxed))
            {
                input.keys = 0u; // they go to the game
                input.pad = 0u;
            }
            GSPcsx2Backend::SetMenuInput(&input);
        }
#endif
    }

    namespace
    {
        std::mutex g_noticeMutex;
        std::string g_notice;
        float g_noticeSeconds = 0.0f;
        std::chrono::steady_clock::time_point g_noticeTime;
    }

    void notify(std::string text, float seconds)
    {
        std::lock_guard<std::mutex> lock(g_noticeMutex);
        g_notice = std::move(text);
        g_noticeSeconds = seconds;
        g_noticeTime = std::chrono::steady_clock::now();
    }

    void beginKeyCapture()
    {
        g_captureFresh.store(true, std::memory_order_relaxed);
        g_captureState.store(1, std::memory_order_relaxed);
    }

    void cancelKeyCapture()
    {
        g_captureState.store(0, std::memory_order_relaxed);
    }

    bool capturing()
    {
        return g_captureState.load(std::memory_order_relaxed) == 1;
    }

    bool captured(int &key, bool &shift, bool &ctrl, std::string &gameUse)
    {
        if (g_captureState.load(std::memory_order_acquire) != 2)
            return false;
        key = g_capturedKey.load(std::memory_order_relaxed);
        shift = g_capturedShift.load(std::memory_order_relaxed);
        ctrl = g_capturedCtrl.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_captureMutex);
            gameUse = key ? g_capturedUse : std::string();
        }
        g_captureState.store(0, std::memory_order_relaxed);
        return true;
    }

    bool notice(std::string &text, float &alpha)
    {
        std::lock_guard<std::mutex> lock(g_noticeMutex);
        if (g_notice.empty())
            return false;
        const float seconds = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_noticeTime).count();
        if (seconds >= g_noticeSeconds)
        {
            g_notice.clear();
            return false;
        }
        text = g_notice;
        const float fade = g_noticeSeconds - seconds;
        alpha = fade < 0.5f ? fade / 0.5f : 1.0f;
        return true;
    }
}
