#pragma once

#include <functional>
#include <string>
#include <vector>

struct Pcsx2GsUi; // pcsx2gs_api.h: the UI calls a panel draws with

// In-game menu (F2) over the image of PCSX2's GS: one window with a tab per registered panel. The game keeps running;
// while the menu is open the controllers and the keyboard drive the menu instead of the game. Panels draw on the GS
// thread with the DLL's UI table (Pcsx2GsUi). PS2X_GAME_MENU=0 turns it off.
namespace ps2x::menu
{
    using DrawPanel = std::function<void(const Pcsx2GsUi *ui)>;

    // Register before the game loop starts. A title registered again adds a section to that tab, drawn after the
    // earlier ones; tabs appear in setTabOrder's order (the rest after, in registration order).
    void addPanel(std::string title, DrawPanel draw);
    void setTabOrder(std::vector<std::string> titles);
    // The window title (e.g. "Urban Reign") and the hint line under the tabs.
    void setTitle(std::string title, std::string hint);
    // Shown instead of the tabs when no panel is registered (e.g. "no mod is on").
    void setEmptyText(std::string text);
    [[nodiscard]] bool isOpen() noexcept;
    // Window thread, once per host frame: F2 opens/closes it and the input goes to the menu while it is open.
    void hostTick();

    // A short notice in the corner where the save-state and patch messages show (any thread): shown for `seconds`,
    // the last half second fading. A newer notice replaces the one shown.
    void notify(std::string text, float seconds = 1.5f);
    // For the presenter: the notice to draw now and its alpha; false when there is none.
    [[nodiscard]] bool notice(std::string &text, float &alpha);

    // "Press a key" for a panel: begin on the GS thread, the window thread (hostTick) takes the next key the hotkeys
    // table accepts (with Shift / Ctrl as held), Esc cancels. captured() is true once, with the key (0 = cancelled).
    void beginKeyCapture();
    [[nodiscard]] bool capturing();
    [[nodiscard]] bool captured(int &key, bool &shift, bool &ctrl);
}
