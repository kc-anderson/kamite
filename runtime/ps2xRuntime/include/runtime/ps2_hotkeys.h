#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The port's keyboard shortcuts (the F keys), as a table the game layer and its menus can show and rebind. Each
// action has one binding: a raylib key with optional Shift / Ctrl. The window loop asks pressed() for the actions it
// handles itself and fire()s the others, which the game thread picks up with take(). F1 (the Controls screen) and
// Esc are not here: they stay fixed.
//
// The game layer keeps the bindings (kamite.ini [Hotkeys], one line per action id); this table is the live copy.
namespace ps2x::hotkeys
{
    enum class Action : int
    {
        Menu,        // the F2 menu
        SaveState,   // F5
        LoadState,   // F9
        FpsOverlay,  // F3
        Fullscreen,  // F11
        HudLayout,   // F7: 16:9 / 4:3 HUD
        Dump2D,      // F6: one frame's 2D to the log
        ReloadPnach, // F4
        RamDump,     // F8: RAM + VU1 trace
        DebugDump,   // Shift+F8
        GifDump,     // F10
        DrawDump,    // Shift+F10
        DebugPanel,  // F12
        Count
    };

    struct Binding
    {
        int key = 0; // raylib KEY_*, 0 = unbound
        bool shift = false;
        bool ctrl = false;
        bool operator==(const Binding &o) const noexcept { return key == o.key && shift == o.shift && ctrl == o.ctrl; }
        bool operator!=(const Binding &o) const noexcept { return !(*this == o); }
    };

    struct Info
    {
        const char *id;    // the ini key ("SaveState")
        const char *label; // for menus ("Save state")
        Binding def;
    };

    [[nodiscard]] const Info &info(Action action);
    [[nodiscard]] Binding binding(Action action);
    void setBinding(Action action, Binding binding);
    // Another action that already uses this binding (Count = none).
    [[nodiscard]] Action conflict(Action action, Binding binding);

    // "Shift+F8", "F5", "" (unbound) <-> Binding. parse() takes the same text (case does not matter; "none" clears).
    [[nodiscard]] std::string text(Binding binding);
    [[nodiscard]] bool parse(std::string_view text, Binding &out);
    [[nodiscard]] const char *keyName(int raylibKey); // "" for a key that cannot be named
    // The keys a hotkey may use: the F keys, letters, digits, keypad, and the named keys; not Esc or F1.
    [[nodiscard]] bool bindable(int raylibKey);
    // The key pressed this frame that bindable() accepts (0 = none); for "press a key" capture. Window thread.
    [[nodiscard]] int capturePressed();

    // Window thread: the binding's key went down this frame with its modifiers (and no extra Shift/Ctrl).
    [[nodiscard]] bool pressed(Action action);
    // Any thread: fire() counts a press, take() returns true once per press (for actions handled off the window thread).
    void fire(Action action);
    [[nodiscard]] bool take(Action action);
}
