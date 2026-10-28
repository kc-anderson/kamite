#include "runtime/ps2_hotkeys.h"

#include "ps2_host_backend.h"

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ps2x::hotkeys
{
    namespace
    {
        constexpr int kCount = static_cast<int>(Action::Count);

        constexpr Info kInfo[kCount] = {
            {"Menu", "Dev menu (F2 menu)", {KEY_F2}},
            {"SaveState", "Save state", {KEY_F5}},
            {"LoadState", "Load state", {KEY_F9}},
            {"FpsOverlay", "FPS overlay", {KEY_F3}},
            {"Fullscreen", "Full screen", {KEY_F11}},
            {"HudLayout", "HUD layout 16:9 / 4:3", {KEY_F7}},
            {"Dump2D", "Write one frame's 2D to the log", {KEY_F6}},
            {"ReloadPnach", "Reload .pnach patches", {KEY_F4}},
            {"RamDump", "RAM dump + VU1 trace", {KEY_F8}},
            {"DebugDump", "Debug dump", {KEY_F8, true}},
            {"GifDump", "GIF dump", {KEY_F10}},
            {"DrawDump", "Draw dump", {KEY_F10, true}},
            {"DebugPanel", "Debugger", {KEY_F12}},
        };

        std::mutex g_mutex;
        Binding g_bindings[kCount] = {
            kInfo[0].def, kInfo[1].def, kInfo[2].def, kInfo[3].def, kInfo[4].def, kInfo[5].def, kInfo[6].def,
            kInfo[7].def, kInfo[8].def, kInfo[9].def, kInfo[10].def, kInfo[11].def, kInfo[12].def,
        };
        std::atomic<uint32_t> g_fired[kCount];
        std::atomic<uint32_t> g_taken[kCount];

        struct Named
        {
            const char *name;
            int key;
        };
        constexpr Named kNamed[] = {
            {"Tab", KEY_TAB}, {"Backspace", KEY_BACKSPACE}, {"Return", KEY_ENTER}, {"Keypad+Enter", KEY_KP_ENTER},
            {"Insert", KEY_INSERT}, {"Delete", KEY_DELETE}, {"Pause", KEY_PAUSE}, {"Home", KEY_HOME}, {"End", KEY_END},
            {"Left", KEY_LEFT}, {"Up", KEY_UP}, {"Right", KEY_RIGHT}, {"Down", KEY_DOWN}, {"PageUp", KEY_PAGE_UP},
            {"PageDown", KEY_PAGE_DOWN}, {"CapsLock", KEY_CAPS_LOCK}, {"Space", KEY_SPACE}, {"Comma", KEY_COMMA},
            {"Minus", KEY_MINUS}, {"Period", KEY_PERIOD}, {"Slash", KEY_SLASH}, {"Semicolon", KEY_SEMICOLON},
            {"Equal", KEY_EQUAL}, {"BracketLeft", KEY_LEFT_BRACKET}, {"Backslash", KEY_BACKSLASH},
            {"BracketRight", KEY_RIGHT_BRACKET}, {"Apostrophe", KEY_APOSTROPHE}, {"QuoteLeft", KEY_GRAVE},
            {"NumLock", KEY_NUM_LOCK}, {"ScrollLock", KEY_SCROLL_LOCK}, {"Print", KEY_PRINT_SCREEN}, {"Menu", KEY_KB_MENU},
            {"Keypad+Plus", KEY_KP_ADD}, {"Keypad+Minus", KEY_KP_SUBTRACT}, {"Keypad+Multiply", KEY_KP_MULTIPLY},
            {"Keypad+Divide", KEY_KP_DIVIDE}, {"Keypad+Decimal", KEY_KP_DECIMAL},
        };

        bool sameText(std::string_view a, std::string_view b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        int keyFromName(std::string_view name)
        {
            if (name.size() == 1u)
            {
                const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
                if (c >= 'A' && c <= 'Z')
                    return KEY_A + (c - 'A');
                if (c >= '0' && c <= '9')
                    return KEY_ZERO + (c - '0');
            }
            if ((name.size() == 2u || name.size() == 3u) && (name[0] == 'F' || name[0] == 'f'))
            {
                int number = 0;
                bool digits = true;
                for (size_t i = 1; i < name.size(); ++i)
                {
                    if (!std::isdigit(static_cast<unsigned char>(name[i])))
                        digits = false;
                    number = number * 10 + (name[i] - '0');
                }
                if (digits && number >= 1 && number <= 12)
                    return KEY_F1 + number - 1;
            }
            if (name.size() == 8u && sameText(name.substr(0, 7), "Keypad+") && std::isdigit(static_cast<unsigned char>(name[7])))
                return KEY_KP_0 + (name[7] - '0');
            for (const Named &n : kNamed)
                if (sameText(name, n.name))
                    return n.key;
            return 0;
        }

        bool shiftDown() { return IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT); }
        bool ctrlDown() { return IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL); }
        bool modifier(int key)
        {
            return key == KEY_LEFT_SHIFT || key == KEY_RIGHT_SHIFT || key == KEY_LEFT_CONTROL || key == KEY_RIGHT_CONTROL ||
                   key == KEY_LEFT_ALT || key == KEY_RIGHT_ALT;
        }
    }

    const Info &info(Action action)
    {
        return kInfo[static_cast<int>(action)];
    }

    Binding binding(Action action)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_bindings[static_cast<int>(action)];
    }

    void setBinding(Action action, Binding binding)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_bindings[static_cast<int>(action)] = binding;
    }

    Action conflict(Action action, Binding binding)
    {
        if (binding.key == 0)
            return Action::Count;
        std::lock_guard<std::mutex> lock(g_mutex);
        for (int i = 0; i < kCount; ++i)
            if (i != static_cast<int>(action) && g_bindings[i] == binding)
                return static_cast<Action>(i);
        return Action::Count;
    }

    const char *keyName(int key)
    {
        static thread_local char text[16];
        if (key >= KEY_A && key <= KEY_Z)
        {
            text[0] = static_cast<char>('A' + (key - KEY_A));
            text[1] = 0;
            return text;
        }
        if (key >= KEY_ZERO && key <= KEY_NINE)
        {
            text[0] = static_cast<char>('0' + (key - KEY_ZERO));
            text[1] = 0;
            return text;
        }
        if (key >= KEY_KP_0 && key <= KEY_KP_9)
        {
            std::snprintf(text, sizeof(text), "Keypad+%d", key - KEY_KP_0);
            return text;
        }
        if (key >= KEY_F1 && key <= KEY_F12)
        {
            std::snprintf(text, sizeof(text), "F%d", key - KEY_F1 + 1);
            return text;
        }
        for (const Named &n : kNamed)
            if (n.key == key)
                return n.name;
        return "";
    }

    std::string text(Binding binding)
    {
        if (binding.key == 0)
            return {};
        const char *name = keyName(binding.key);
        if (!*name)
            return {};
        std::string out;
        if (binding.ctrl)
            out += "Ctrl+";
        if (binding.shift)
            out += "Shift+";
        return out + name;
    }

    bool parse(std::string_view text, Binding &out)
    {
        Binding b;
        std::string_view rest = text;
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.front())))
            rest.remove_prefix(1u);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
            rest.remove_suffix(1u);
        if (rest.empty() || sameText(rest, "none") || sameText(rest, "-"))
        {
            out = Binding{};
            return true;
        }
        for (;;)
        {
            if (rest.size() > 5u && sameText(rest.substr(0, 5), "Ctrl+"))
            {
                b.ctrl = true;
                rest.remove_prefix(5u);
            }
            else if (rest.size() > 6u && sameText(rest.substr(0, 6), "Shift+"))
            {
                b.shift = true;
                rest.remove_prefix(6u);
            }
            else
                break;
        }
        b.key = keyFromName(rest);
        if (b.key == 0 || !bindable(b.key))
            return false;
        out = b;
        return true;
    }

    bool bindable(int key)
    {
        if (key == 0 || key == KEY_ESCAPE || key == KEY_F1 || modifier(key))
            return false;
        return *keyName(key) != 0;
    }

    int capturePressed()
    {
        for (int key = GetKeyPressed(); key != 0; key = GetKeyPressed())
            if (bindable(key))
                return key;
        return 0;
    }

    bool pressed(Action action)
    {
        const Binding b = binding(action);
        if (b.key == 0 || !IsKeyPressed(b.key))
            return false;
        return shiftDown() == b.shift && ctrlDown() == b.ctrl;
    }

    void fire(Action action)
    {
        g_fired[static_cast<int>(action)].fetch_add(1u, std::memory_order_relaxed);
    }

    bool take(Action action)
    {
        const int i = static_cast<int>(action);
        const uint32_t fired = g_fired[i].load(std::memory_order_relaxed);
        if (fired == g_taken[i].load(std::memory_order_relaxed))
            return false;
        g_taken[i].store(fired, std::memory_order_relaxed);
        return true;
    }
}
