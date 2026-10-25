// Controls screen (F1 / controller Back+Start): Tokyo Jungle's in-game rebinding screen (cellPad.c overlay) for this
// runtime. The bindings live in ps2_host_input (PCSX2 [PadN] syntax, the controls file); this file is the screen: input,
// capture, drawing and the KAMITE_CONTROLS_SELFTEST script. Window thread only.
#include "runtime/ps2_controls_screen.h"
#include "ps2_host_backend.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"

#include <rlgl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::controls
{
    namespace
    {
        namespace pc = ps2_host_input::controls;
        using Clock = std::chrono::steady_clock;

        // Rows in the order of Tokyo Jungle's screen (face buttons first). Actions from the game manual
        // (port_prep/controls/labels/kamite_labels.txt); buttons without a known action show the button only.
        struct Row
        {
            const char *key;    // PCSX2 [PadN] key
            const char *action; // empty: no known action
            const char *button;
        };
        constexpr std::array<Row, 24> kRows{{
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
            {"LUp", "Move forward", "left stick up"},
            {"LDown", "Step back", "left stick down"},
            {"LLeft", "Move left", "left stick left"},
            {"LRight", "Move right", "left stick right"},
            {"RUp", "Duck", "right stick up"},
            {"RDown", "Back flip", "right stick down"},
            {"RLeft", "Sidestep left", "right stick left"},
            {"RRight", "Sidestep right", "right stick right"},
        }};
        constexpr int kRowCount = static_cast<int>(kRows.size());
        constexpr int kKeyColumns = 3;               // Key 1-3: keyboard keys or mouse buttons
        constexpr int kColumnCount = kKeyColumns + 1; // + Controller
        constexpr uint32_t kPad = 0u;                // player 1
        constexpr float kPressed = 0.6f;              // an axis half counts as a button above this...
        constexpr float kReleased = 0.3f;             // ...until it falls below this

        std::string rowLabel(int row)
        {
            const Row &entry = kRows[static_cast<size_t>(row)];
            if (!*entry.action)
                return entry.button;
            return std::string(entry.action) + "  (" + entry.button + ")";
        }

        enum class Command
        {
            Toggle, // F1
            Up,
            Down,
            Left,
            Right,
            Accept, // Enter / Space / A: rebind
            Clear,  // Delete / Backspace / X
            Reset,  // F5 (twice)
            Back,   // Esc / B: close
            Other,  // any other key: disarms F5
        };

        bool g_open = false;
        bool g_capture = false;
        bool g_f5Armed = false;
        bool g_saveFailed = false; // the last close could not save: the next close leaves anyway
        int g_row = 0;
        int g_col = 0;
        std::string g_status;
        Clock::time_point g_captureStart{};
        int g_pendingPadDevice = -1; // Controller column: Back or Start pressed while capturing, recorded on release
        int g_pendingPadButton = 0;
        int g_pendingKey = 0; // key columns: Shift/Ctrl/Alt pressed alone while capturing, recorded on release (CR-7)
        bool g_prevFocused = false;

        // Sweep CR-7: the last capture, undone when the window loses the focus right after it (Windows took that key).
        struct LastCapture
        {
            int row = -1; // -1: none
            std::vector<std::string> before;
            std::string binding;
            Clock::time_point at{};
        };
        LastCapture g_lastCapture;
        constexpr auto kFocusUndoWindow = std::chrono::milliseconds(500);

        // Modifiers: Shift/Ctrl/Alt are bindable (recorded on release); the Windows keys are not, only held.
        constexpr std::array<int, 8> kModifierKeys{KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CONTROL, KEY_RIGHT_CONTROL,
                                                   KEY_LEFT_ALT,   KEY_RIGHT_ALT,   KEY_LEFT_SUPER,   KEY_RIGHT_SUPER};
        constexpr const char *kComboStatus =
            "Key combinations (Alt+Tab, Alt+F4, Windows key...) are not recorded: press one key on its own  (Esc cancels)";

        // Controller state of the previous tick (edges), focused window only.
        struct PadSnapshot
        {
            std::array<bool, ps2_host_gamepads::kMaxDevices * ps2_host_gamepads::ButtonCount> buttons{};
            std::array<bool, ps2_host_gamepads::kMaxDevices * ps2_host_gamepads::AxisCount * 2> axes{}; // [+, -] halves
        };
        PadSnapshot g_prevPad;
        bool g_prevChord = false;
        std::array<bool, 4> g_navPrev{};
        std::array<Clock::time_point, 4> g_navSince{};
        std::array<Clock::time_point, 4> g_navRepeat{};

        Font g_font{};
        int g_fontSize = 0;
        bool g_fontOwned = false;

        // KAMITE_CONTROLS_SELFTEST
        struct SelfTest
        {
            bool active = false;
            std::vector<std::string> steps;
            size_t next = 0u;
            int wait = 0;
            int checks = 0;
            std::vector<std::string> failures;
            int shots = 0;
            bool shotPending = false;
        };
        SelfTest g_test;

        size_t buttonSlot(int device, int button)
        {
            return static_cast<size_t>(device * ps2_host_gamepads::ButtonCount + button);
        }

        size_t axisSlot(int device, int axis, bool negative)
        {
            return static_cast<size_t>((device * ps2_host_gamepads::AxisCount + axis) * 2 + (negative ? 1 : 0));
        }

        PadSnapshot snapshotPads(bool focused)
        {
            PadSnapshot now;
            if (!focused)
                return now; // GLFW reads controllers for unfocused windows too: never react to them then
            using namespace ps2_host_gamepads;
            for (int device = 0; device < kMaxDevices; ++device)
            {
                if (!connected(device))
                    continue;
                for (int b = 0; b < ButtonCount; ++b)
                    now.buttons[buttonSlot(device, b)] = button(device, static_cast<Button>(b));
                for (int a = 0; a < AxisCount; ++a)
                {
                    const float value = axis(device, static_cast<Axis>(a));
                    for (int half = 0; half < 2; ++half)
                    {
                        const bool negative = half == 1;
                        if (negative && (a == LeftTrigger || a == RightTrigger))
                            continue;
                        const float amount = negative ? -value : value;
                        const bool was = g_prevPad.axes[axisSlot(device, a, negative)];
                        now.axes[axisSlot(device, a, negative)] = amount > kPressed || (was && amount > kReleased);
                    }
                }
            }
            return now;
        }

        bool buttonEdge(const PadSnapshot &now, int button)
        {
            for (int device = 0; device < ps2_host_gamepads::kMaxDevices; ++device)
                if (now.buttons[buttonSlot(device, button)] && !g_prevPad.buttons[buttonSlot(device, button)])
                    return true;
            return false;
        }

        bool chordDown(const PadSnapshot &now)
        {
            for (int device = 0; device < ps2_host_gamepads::kMaxDevices; ++device)
                if (now.buttons[buttonSlot(device, ps2_host_gamepads::Back)] && now.buttons[buttonSlot(device, ps2_host_gamepads::Start)])
                    return true;
            return false;
        }

        bool isModifier(int code)
        {
            return std::find(kModifierKeys.begin(), kModifierKeys.end(), code) != kModifierKeys.end();
        }

        // A modifier key other than `except` is held.
        bool modifierHeld(int except = 0)
        {
            for (int code : kModifierKeys)
                if (code != except && IsKeyDown(code))
                    return true;
            return false;
        }

        void setStatus(std::string text)
        {
            g_status = std::move(text);
        }

        std::string openStatus()
        {
            if (pc::envOverride(kPad))
                return "PS2X_PAD1 is set in the environment: the game uses those bindings; edits here go to the file only.";
            if (pc::path().empty())
                return "No controls file set: changes last until the game closes.";
            if (!pc::fileError().empty()) // sweep CR-6: the screen shows defaults then, and a save replaces the file
                return pc::fileError() + ": showing the default layout; saving here replaces that file.";
            return {};
        }

        void open(bool drawable, bool byPad)
        {
            if (!drawable)
            {
                std::cerr << "[controls] the Controls screen cannot be drawn over this renderer (Renderer=pcsx2); not opened" << std::endl;
                return;
            }
            g_open = true;
            g_capture = false;
            g_f5Armed = false;
            g_saveFailed = false;
            g_col = byPad ? kKeyColumns : 0; // a controller user starts on the Controller column
            ps2_host_input::setOverlayOpen(true);
            setStatus(openStatus());
            std::cerr << "[controls] opened (" << (byPad ? "controller Back+Start" : "F1") << ")" << std::endl;
        }

        void close(bool force)
        {
            std::string message;
            if (!pc::save(message))
            {
                std::cerr << "[controls] " << message << std::endl;
                if (!force && !g_saveFailed && !pc::path().empty())
                {
                    g_saveFailed = true;
                    setStatus(message + ". Close again to leave (saving is retried on the next close and on exit).");
                    return;
                }
            }
            else if (!message.empty())
                std::cerr << "[controls] " << message << std::endl;
            g_open = false;
            g_capture = false;
            g_f5Armed = false;
            g_pendingKey = 0;
            g_lastCapture.row = -1; // saved: no longer undone by a focus loss
            ps2_host_input::setOverlayOpen(false);
            std::cerr << "[controls] closed" << std::endl;
        }

        struct Split
        {
            std::vector<std::string> keys;
            std::vector<std::string> pads;
            std::vector<std::string> other;
        };

        Split splitRow(int row)
        {
            Split split;
            for (std::string &binding : pc::bindings(kPad, kRows[static_cast<size_t>(row)].key))
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
            pc::setBindings(kPad, kRows[static_cast<size_t>(row)].key, std::move(list));
        }

        // A binding goes into the cursor's cell. Within one action it appears once (moved, not copied); another action
        // may use the same key too (the status line says so).
        void captured(const std::string &binding)
        {
            g_capture = false;
            g_pendingKey = 0;
            g_lastCapture = {g_row, pc::bindings(kPad, kRows[static_cast<size_t>(g_row)].key), binding, Clock::now()};
            Split split = splitRow(g_row);
            std::vector<std::string> &list = g_col < kKeyColumns ? split.keys : split.pads;
            size_t at = g_col < kKeyColumns ? static_cast<size_t>(g_col) : 0u;
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
            storeRow(g_row, split);
            std::string text = rowLabel(g_row) + ": " + pc::displayName(binding);
            for (int other = 0; other < kRowCount; ++other)
            {
                if (other == g_row)
                    continue;
                for (const std::string &existing : pc::bindings(kPad, kRows[static_cast<size_t>(other)].key))
                    if (pc::sameBinding(existing, binding))
                    {
                        text += "   (also on " + rowLabel(other) + ")";
                        other = kRowCount;
                        break;
                    }
            }
            setStatus(text);
            std::cerr << "[controls] " << kRows[static_cast<size_t>(g_row)].key << " column " << g_col + 1 << " = " << binding << std::endl;
        }

        void clearCell()
        {
            Split split = splitRow(g_row);
            std::vector<std::string> &list = g_col < kKeyColumns ? split.keys : split.pads;
            const size_t at = g_col < kKeyColumns ? static_cast<size_t>(g_col) : 0u;
            if (at >= list.size())
            {
                setStatus("Nothing to clear there.");
                return;
            }
            const std::string removed = list[at];
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(at));
            storeRow(g_row, split);
            setStatus(rowLabel(g_row) + ": " + pc::displayName(removed) + " removed");
            std::cerr << "[controls] " << kRows[static_cast<size_t>(g_row)].key << " column " << g_col + 1 << " cleared (" << removed << ")" << std::endl;
        }

        void startCapture()
        {
            g_capture = true;
            g_captureStart = Clock::now();
            g_pendingPadDevice = -1;
            g_pendingKey = 0;
            setStatus(g_col < kKeyColumns ? "Press a key or mouse button for " + rowLabel(g_row) + "  (Esc cancels)"
                                          : "Press a controller button or move a stick/trigger for " + rowLabel(g_row) +
                                                "  (Back+Start or Esc cancels)");
        }

        void command(Command cmd, bool drawable, bool fromPad)
        {
            if (!g_open)
            {
                if (cmd == Command::Toggle)
                    open(drawable, fromPad);
                return;
            }
            if (cmd != Command::Reset && g_f5Armed)
            {
                g_f5Armed = false;
                setStatus({});
            }
            switch (cmd)
            {
            case Command::Toggle:
            case Command::Back:
                close(false);
                break;
            case Command::Up:
                g_row = (g_row + kRowCount - 1) % kRowCount;
                break;
            case Command::Down:
                g_row = (g_row + 1) % kRowCount;
                break;
            case Command::Left:
                g_col = (g_col + kColumnCount - 1) % kColumnCount;
                break;
            case Command::Right:
                g_col = (g_col + 1) % kColumnCount;
                break;
            case Command::Accept:
                startCapture();
                break;
            case Command::Clear:
                clearCell();
                break;
            case Command::Reset:
                if (!g_f5Armed)
                {
                    g_f5Armed = true;
                    setStatus("Press F5 again to reset every binding of player 1 to the default layout.");
                }
                else
                {
                    g_f5Armed = false;
                    pc::resetDefaults(kPad);
                    setStatus("Default layout restored (saved when the screen closes).");
                    std::cerr << "[controls] player 1 reset to the default layout" << std::endl;
                }
                break;
            case Command::Other:
                break;
            }
        }

        void cancelCapture(const char *why)
        {
            g_capture = false;
            g_pendingKey = 0;
            setStatus(why);
        }

        // Sweep CR-7: the window lost the focus (Alt+Tab, the Windows key, a click on another window, a shortcut that
        // opened one). A capture in progress is cancelled, so neither the keys of that shortcut nor the click that brings
        // the window back are recorded (nor a held Back/Start, which the focus-gated snapshot would show as released);
        // a capture made just before is undone: Windows acted on that key, so it is no binding (Print opens the
        // Snipping Tool). Only a change of focus counts, so a window that never had it (hidden self-test) is unaffected.
        void focusLost()
        {
            if (g_capture)
                cancelCapture("Cancelled: the game window lost the focus.");
            if (g_lastCapture.row >= 0 && Clock::now() - g_lastCapture.at < kFocusUndoWindow)
            {
                const size_t row = static_cast<size_t>(g_lastCapture.row);
                pc::setBindings(kPad, kRows[row].key, g_lastCapture.before);
                setStatus(pc::displayName(g_lastCapture.binding) + " was not kept for " + rowLabel(g_lastCapture.row) +
                          ": the game window lost the focus right after it (a Windows shortcut?).");
                std::cerr << "[controls] " << kRows[row].key << " = " << g_lastCapture.binding << " undone: the window lost the focus" << std::endl;
            }
            g_lastCapture.row = -1;
        }

        // While capturing: the first new key, mouse button (key columns) or controller input (Controller column).
        void captureTick(const PadSnapshot &now)
        {
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_F1))
                return cancelCapture("Cancelled.");
            if (g_col < kKeyColumns)
            {
                // Sweep CR-7: Windows shortcuts are no bindings. Shift/Ctrl/Alt are recorded on their release, and only
                // when nothing else was pressed meanwhile (Alt+Tab and Alt+F4 recorded LeftAlt on the press, and Alt+F4
                // then saved it on the way out); a key, F key or mouse button pressed while a modifier or a Windows key
                // is held is a combination and is never recorded.
                int key = 0;
                for (int code : pc::bindableKeys())
                    if (IsKeyPressed(code))
                    {
                        key = code;
                        break;
                    }
                bool fKey = false;
                for (int code = KEY_F2; code <= KEY_F12; ++code)
                    fKey = fKey || IsKeyPressed(code);
                int mouse = -1;
                for (int button : {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE})
                    if (mouse < 0 && IsWindowFocused() && IsMouseButtonPressed(button))
                        mouse = button;
                if (g_pendingKey != 0)
                {
                    if ((key != 0 && key != g_pendingKey) || fKey || mouse >= 0 || modifierHeld(g_pendingKey))
                    {
                        g_pendingKey = 0;
                        return setStatus(kComboStatus);
                    }
                    if (!IsKeyDown(g_pendingKey))
                        return captured(pc::keyBinding(g_pendingKey));
                    return; // still held: waiting for the release (or a second key, which makes it a combination)
                }
                if (key != 0 && isModifier(key))
                {
                    if (modifierHeld(key))
                        return setStatus(kComboStatus);
                    g_pendingKey = key;
                    return setStatus("Release " + pc::displayName(pc::keyBinding(key)) + " to use it for " + rowLabel(g_row) + "  (Esc cancels)");
                }
                if ((key != 0 || fKey || mouse >= 0) && modifierHeld())
                    return setStatus(kComboStatus);
                if (key != 0)
                    return captured(pc::keyBinding(key));
                if (fKey)
                    return setStatus("F keys stay the port's shortcuts (F3 FPS, F5/F9 save state, F11 full screen...). Press another key.");
                if (mouse >= 0)
                    return captured(pc::mouseBinding(mouse));
                if (buttonEdge(now, ps2_host_gamepads::East))
                    return cancelCapture("Cancelled.");
                return;
            }
            using namespace ps2_host_gamepads;
            // Back+Start (the close gesture) cancels here. Back and Start alone are recorded on release, so pressing
            // the gesture no longer binds Back or Start over the cell's controller button (sweep CR-3); another input
            // while one of them is held records the held one (the first press wins).
            if (chordDown(now))
                return cancelCapture("Cancelled.");
            if (g_pendingPadDevice >= 0 && !now.buttons[buttonSlot(g_pendingPadDevice, g_pendingPadButton)])
                return captured(pc::padButtonBinding(g_pendingPadDevice, g_pendingPadButton));
            for (int device = 0; device < kMaxDevices; ++device)
            {
                for (int b = 0; b < ButtonCount; ++b)
                    if (b != Guide && now.buttons[buttonSlot(device, b)] && !g_prevPad.buttons[buttonSlot(device, b)])
                    {
                        if (g_pendingPadDevice >= 0)
                            return captured(pc::padButtonBinding(g_pendingPadDevice, g_pendingPadButton));
                        if (b != Back && b != Start)
                            return captured(pc::padButtonBinding(device, b));
                        g_pendingPadDevice = device;
                        g_pendingPadButton = b;
                    }
                for (int a = 0; a < AxisCount; ++a)
                    for (bool negative : {false, true})
                        if (now.axes[axisSlot(device, a, negative)] && !g_prevPad.axes[axisSlot(device, a, negative)])
                            return captured(g_pendingPadDevice >= 0 ? pc::padButtonBinding(g_pendingPadDevice, g_pendingPadButton)
                                                                    : pc::padAxisBinding(device, a, negative));
            }
            if (g_pendingPadDevice >= 0)
                return; // waiting for the release (or the second half of the gesture)
            if (Clock::now() - g_captureStart > std::chrono::seconds(10))
                cancelCapture("No controller input for 10 s: cancelled.");
        }

        // D-pad or left stick: a move on the press, then repeats while held.
        void padNavigation(const PadSnapshot &now, bool drawable)
        {
            using namespace ps2_host_gamepads;
            constexpr std::array<Command, 4> kCommands{Command::Up, Command::Down, Command::Left, Command::Right};
            const std::array<std::pair<int, std::pair<int, bool>>, 4> sources{{
                {DPadUp, {LeftY, true}}, {DPadDown, {LeftY, false}}, {DPadLeft, {LeftX, true}}, {DPadRight, {LeftX, false}}}};
            const auto time = Clock::now();
            for (size_t i = 0; i < 4u; ++i)
            {
                bool held = false;
                for (int device = 0; device < kMaxDevices; ++device)
                    held = held || now.buttons[buttonSlot(device, sources[i].first)] ||
                           now.axes[axisSlot(device, sources[i].second.first, sources[i].second.second)];
                if (held && !g_navPrev[i])
                {
                    command(kCommands[i], drawable, true);
                    g_navSince[i] = g_navRepeat[i] = time;
                }
                else if (held && time - g_navSince[i] > std::chrono::milliseconds(400) && time - g_navRepeat[i] > std::chrono::milliseconds(100))
                {
                    command(kCommands[i], drawable, true);
                    g_navRepeat[i] = time;
                }
                g_navPrev[i] = held;
            }
        }

        void keyboardTick(bool drawable)
        {
            auto pressedOrRepeat = [](int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); };
            if (IsKeyPressed(KEY_F1))
                return command(Command::Toggle, drawable, false);
            if (IsKeyPressed(KEY_ESCAPE))
                return command(Command::Back, drawable, false);
            if (IsKeyPressed(KEY_F5))
                return command(Command::Reset, drawable, false);
            if (pressedOrRepeat(KEY_UP))
                return command(Command::Up, drawable, false);
            if (pressedOrRepeat(KEY_DOWN))
                return command(Command::Down, drawable, false);
            if (pressedOrRepeat(KEY_LEFT))
                return command(Command::Left, drawable, false);
            if (pressedOrRepeat(KEY_RIGHT))
                return command(Command::Right, drawable, false);
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER) || IsKeyPressed(KEY_SPACE))
                return command(Command::Accept, drawable, false);
            if (IsKeyPressed(KEY_DELETE) || IsKeyPressed(KEY_BACKSPACE))
                return command(Command::Clear, drawable, false);
            for (int code : pc::bindableKeys())
                if (IsKeyPressed(code))
                    return command(Command::Other, drawable, false);
        }

        // ---- self-test (KAMITE_CONTROLS_SELFTEST) ----------------------------------------------------------------------
        constexpr const char *kDefaultScript =
            "dumpdefaults,wait:240,f1,wait:5,shot,down,down,right,enter,key:U,wait:2,shot,esc,wait:3,holdcheck,"
            "expect:Square=Keyboard/U,expect:Square=Keyboard/J,expect:Square=SDL-0/FaceWest,"
            "f1,right,right,right,enter,pad:SDL-0/RightShoulder,esc,wait:3,"
            "expect:Square=SDL-0/RightShoulder,expect:Square!=SDL-0/FaceWest,"
            "extedit:RUp=Keyboard/Keypad+8,wait:150,expect:RUp=Keyboard/Keypad+8,"
            "f1,f5,f5,esc,wait:3,expect:Square!=Keyboard/U,expect:Square=SDL-0/FaceWest,expect:Square=Keyboard/J,"
            "expect:RUp!=Keyboard/Keypad+8,holdcheck,done";

        void testCheck(bool ok, const std::string &what)
        {
            ++g_test.checks;
            std::cerr << "[controls] selftest " << (ok ? "ok   " : "FAIL ") << what << std::endl;
            if (!ok)
                g_test.failures.push_back(what);
        }

        // "expectlive:": the live table only (the file may be unreadable on purpose).
        void testExpectLive(const std::string &spec)
        {
            const bool negate = spec.find("!=") != std::string::npos;
            const size_t at = negate ? spec.find("!=") : spec.find('=');
            if (at == std::string::npos)
                return testCheck(false, "bad expectlive '" + spec + "'");
            const bool live = pc::liveBinding(kPad, spec.substr(0, at), spec.substr(at + (negate ? 2u : 1u)));
            testCheck(live == !negate, "expectlive " + spec + " (live table " + (live ? "has" : "lacks") + ")");
        }

        void testExpect(const std::string &spec)
        {
            const bool negate = spec.find("!=") != std::string::npos;
            const size_t at = negate ? spec.find("!=") : spec.find('=');
            if (at == std::string::npos)
                return testCheck(false, "bad expect '" + spec + "'");
            const std::string key = spec.substr(0, at);
            const std::string binding = spec.substr(at + (negate ? 2u : 1u));
            std::vector<std::string> list;
            std::string error;
            bool inFile = false;
            if (!pc::fileBindings(pc::path(), kPad, key, list, error))
                return testCheck(false, "expect " + spec + ": cannot read the file (" + error + ")");
            for (const std::string &entry : list)
                inFile = inFile || pc::sameBinding(entry, binding);
            const bool live = pc::liveBinding(kPad, key, binding);
            testCheck(inFile == !negate && live == !negate,
                      "expect " + spec + " (file " + (inFile ? "has" : "lacks") + ", live table " + (live ? "has" : "lacks") + ")");
        }

        // A change made outside the game (an editor): [Pad1] gets one more line, written straight to the file.
        void testExternalEdit(const std::string &spec)
        {
            const size_t at = spec.find('=');
            std::ifstream in(pc::path(), std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            in.close();
            const size_t section = text.find("[Pad1]\n");
            if (at == std::string::npos || section == std::string::npos)
                return testCheck(false, "extedit " + spec + ": no [Pad1] section");
            text.insert(section + 7u, spec.substr(0, at) + " = " + spec.substr(at + 1u) + "\n");
            std::ofstream out(pc::path(), std::ios::binary | std::ios::trunc);
            out << text;
            std::cerr << "[controls] selftest: external edit " << spec << std::endl;
        }

        void testStep(const std::string &step, bool drawable)
        {
            auto arg = [&](size_t prefix) { return step.substr(prefix); };
            std::cerr << "[controls] selftest step " << step << std::endl;
            if (step.rfind("wait:", 0) == 0)
                g_test.wait = std::max(0, std::atoi(arg(5).c_str()));
            else if (step == "f1")
            {
                if (g_capture)
                    cancelCapture("Cancelled.");
                else
                    command(Command::Toggle, drawable, false);
            }
            else if (step == "padopen")
                command(Command::Toggle, drawable, true);
            else if (step == "up" || step == "down" || step == "left" || step == "right")
                command(step == "up" ? Command::Up : step == "down" ? Command::Down : step == "left" ? Command::Left : Command::Right, drawable, false);
            else if (step == "enter")
                command(Command::Accept, drawable, false);
            else if (step == "delete")
                command(Command::Clear, drawable, false);
            else if (step == "f5")
                command(Command::Reset, drawable, false);
            else if (step == "esc")
            {
                if (g_capture)
                    cancelCapture("Cancelled.");
                else
                    command(Command::Back, drawable, false);
            }
            else if (step.rfind("key:", 0) == 0)
            {
                // The same naming path as a real key press: the raylib key whose binding has that name.
                std::string binding;
                for (int code : pc::bindableKeys())
                    if (pc::sameBinding(pc::keyBinding(code), "Keyboard/" + arg(4)))
                        binding = pc::keyBinding(code);
                if (!g_capture || g_col >= kKeyColumns || binding.empty())
                    testCheck(false, step + ": not capturing a key column or unknown key");
                else
                    captured(binding);
            }
            else if (step.rfind("pad:", 0) == 0 || step.rfind("mouse:", 0) == 0)
            {
                const bool pad = step[0] == 'p';
                const std::string binding = pad ? arg(4) : pc::mouseBinding(arg(6) == "Left" ? MOUSE_BUTTON_LEFT : arg(6) == "Right" ? MOUSE_BUTTON_RIGHT : MOUSE_BUTTON_MIDDLE);
                const bool rightColumn = pad ? g_col >= kKeyColumns : g_col < kKeyColumns;
                if (!g_capture || !rightColumn || pc::kindOf(binding) != (pad ? pc::Kind::Controller : pc::Kind::Keys))
                    testCheck(false, step + ": not capturing that column or bad binding");
                else
                    captured(binding);
            }
            else if (step.rfind("expect:", 0) == 0)
                testExpect(arg(7));
            else if (step.rfind("extedit:", 0) == 0)
                testExternalEdit(arg(8));
            else if (step.rfind("expectlive:", 0) == 0)
                testExpectLive(arg(11));
            else if (step == "utf16")
            {
                // An editor saved the file as UTF-16: it must be refused and the current bindings kept.
                std::ofstream out(pc::path(), std::ios::binary | std::ios::trunc);
                out.write("\xFF\xFE[\0P\0a\0d\0\x31\0]\0\n\0", 16);
                std::cerr << "[controls] selftest: wrote a UTF-16 controls file" << std::endl;
            }
            else if (step == "holdcheck")
                testCheck(!ps2_host_input::overlayOpen() && !ps2_host_input::inputHeld(0u) && !g_open,
                          std::string("screen closed, game input not blocked (overlay ") + (ps2_host_input::overlayOpen() ? "open" : "closed") +
                              ", player 1 hold " + (ps2_host_input::inputHeld(0u) ? "on" : "off") + ")");
            else if (step == "shot")
                g_test.shotPending = true;
            else if (step == "dumpdefaults")
            {
                std::istringstream lines(pc::defaultsDump(kPad));
                std::string line;
                while (std::getline(lines, line))
                    std::cerr << "[controls] default pad1 " << line << std::endl;
            }
            else if (step == "done")
            {
                if (g_test.failures.empty())
                    std::cerr << "[controls] selftest PASS (" << g_test.checks << " checks)" << std::endl;
                else
                    std::cerr << "[controls] selftest FAIL (" << g_test.failures.size() << " of " << g_test.checks << " checks failed; first: "
                              << g_test.failures.front() << ")" << std::endl;
            }
            else
                testCheck(false, "unknown step '" + step + "'");
        }

        void testTick(bool drawable)
        {
            static bool started = false;
            if (!started)
            {
                started = true;
                const char *value = std::getenv("KAMITE_CONTROLS_SELFTEST");
                if (!value || !*value || std::string_view(value) == "0")
                    return;
                const std::string script = std::string_view(value) == "1" ? kDefaultScript : value;
                std::stringstream input(script);
                std::string step;
                while (std::getline(input, step, ','))
                    if (!step.empty())
                        g_test.steps.push_back(step);
                g_test.active = true;
                std::cerr << "[controls] selftest: " << g_test.steps.size() << " steps, file " << pc::path() << std::endl;
            }
            if (!g_test.active || g_test.shotPending)
                return; // a screenshot is taken by draw() first
            if (g_test.wait > 0)
            {
                --g_test.wait;
                return;
            }
            if (g_test.next >= g_test.steps.size())
            {
                g_test.active = false;
                return;
            }
            testStep(g_test.steps[g_test.next++], drawable);
        }

        // ---- drawing ----------------------------------------------------------------------------------------------------
        void ensureFont(int size)
        {
            if (size == g_fontSize && g_font.texture.id != 0)
                return;
            if (g_fontOwned)
                UnloadFont(g_font);
            g_fontOwned = false;
            g_fontSize = size;
            const char *windows = std::getenv("WINDIR");
            const std::string path = std::string(windows ? windows : "C:/Windows") + "/Fonts/segoeui.ttf";
            if (FileExists(path.c_str()))
            {
                g_font = LoadFontEx(path.c_str(), size, nullptr, 0);
                if (g_font.texture.id != 0)
                {
                    SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
                    g_fontOwned = true;
                    return;
                }
            }
            g_font = GetFontDefault();
        }

        float textWidth(const std::string &text, float size)
        {
            return MeasureTextEx(g_font, text.c_str(), size, 1.0f).x;
        }

        std::string fit(std::string text, float size, float width)
        {
            if (textWidth(text, size) <= width)
                return text;
            while (text.size() > 1u && textWidth(text + "...", size) > width)
                text.pop_back();
            return text + "...";
        }

        void drawText(const std::string &text, float x, float y, float size, Color color)
        {
            DrawTextEx(g_font, text.c_str(), Vector2{x, y}, size, 1.0f, color);
        }

        // Sweep CR-8: one key or button bound to two PS2 buttons presses both (F on L3 and on another action), and the
        // table shows it only by reading every row; the status line names it only right after a capture. The screen
        // lists every shared key/button on a line of its own. Rebuilt only when player 1's bindings change (the names
        // need parsing). Chords and unreadable bindings ("+n" in the table) are left out.
        const std::string &sharedBindingsText()
        {
            static std::string signature;
            static std::string text;
            std::vector<std::vector<std::string>> rows(static_cast<size_t>(kRowCount));
            std::string now;
            for (int row = 0; row < kRowCount; ++row)
            {
                rows[static_cast<size_t>(row)] = pc::bindings(kPad, kRows[static_cast<size_t>(row)].key);
                for (const std::string &binding : rows[static_cast<size_t>(row)])
                    now += binding + '\n';
                now += '\x1f';
            }
            if (now == signature)
                return text;
            signature = std::move(now);
            std::vector<std::pair<std::string, std::vector<int>>> uses; // display name -> rows, in table order
            for (int row = 0; row < kRowCount; ++row)
                for (const std::string &binding : rows[static_cast<size_t>(row)])
                {
                    if (pc::kindOf(binding) == pc::Kind::Other)
                        continue;
                    const std::string name = pc::displayName(binding);
                    auto use = std::find_if(uses.begin(), uses.end(), [&](const auto &entry) { return entry.first == name; });
                    if (use == uses.end())
                        uses.push_back({name, {row}});
                    else if (use->second.back() != row)
                        use->second.push_back(row);
                }
            text.clear();
            for (const auto &[name, list] : uses)
            {
                if (list.size() < 2u)
                    continue;
                text += text.empty() ? "Shared bindings, pressed together: " : "     ";
                text += name + " =";
                for (size_t i = 0; i < list.size(); ++i)
                {
                    const Row &entry = kRows[static_cast<size_t>(list[i])];
                    text += (i == 0u ? " " : " + ") + (*entry.action ? std::string(entry.action) + " (" + entry.key + ")" : std::string(entry.key));
                }
            }
            return text;
        }

        // Self-test "shot": the back buffer as drawn so far, next to the controls file.
        void takeShot()
        {
            g_test.shotPending = false;
            rlDrawRenderBatchActive();
            const std::filesystem::path folder = pc::path().empty() ? std::filesystem::path(".") : std::filesystem::path(pc::path()).parent_path();
            const std::string file = (folder / ("controls_selftest_" + std::to_string(++g_test.shots) + ".png")).string();
            Image screen = LoadImageFromScreen();
            ExportImage(screen, file.c_str());
            UnloadImage(screen);
            std::cerr << "[controls] selftest screenshot " << file << std::endl;
        }
    }

    bool isOpen() noexcept
    {
        return g_open;
    }

    void hostTick(bool drawable)
    {
        const bool focused = IsWindowFocused();
        // Sweep CR-5: the Back+Start gate in ps2_host_input holds a controller's Back/Start back from the game for a
        // moment, so a chord started with Start does not pause the game behind this screen. Only while Back+Start can
        // open it (drawn by this renderer, window focused: snapshotPads ignores controllers otherwise), so Start has no
        // delay when the chord could do nothing. Runs before ps2_host_input::sample(), which applies it.
        ps2_host_input::setChordGate(drawable && focused);
        const bool lostFocus = g_prevFocused && !focused;
        g_prevFocused = focused;
        const PadSnapshot now = snapshotPads(focused);
        const bool chord = chordDown(now);
        const bool chordEdge = chord && !g_prevChord;
        if (lostFocus && g_open)
            focusLost(); // sweep CR-7
        testTick(drawable);
        if (!g_open)
        {
            if (IsKeyPressed(KEY_F1))
                command(Command::Toggle, drawable, false);
            else if (chordEdge)
                command(Command::Toggle, drawable, true);
        }
        else if (g_capture)
            captureTick(now);
        else
        {
            keyboardTick(drawable);
            if (g_open && !g_capture)
            {
                if (chordEdge)
                    command(Command::Toggle, drawable, true);
                else if (buttonEdge(now, ps2_host_gamepads::East))
                    command(Command::Back, drawable, true);
                else if (buttonEdge(now, ps2_host_gamepads::South))
                    command(Command::Accept, drawable, true);
                else if (buttonEdge(now, ps2_host_gamepads::West))
                    command(Command::Clear, drawable, true);
                else
                    padNavigation(now, drawable);
            }
        }
        if (!g_open)
            g_navPrev = {};
        g_prevPad = now;
        g_prevChord = chord;
    }

    void draw()
    {
        if (!g_open)
        {
            if (g_test.shotPending)
                takeShot();
            return;
        }
        const float screenW = static_cast<float>(GetScreenWidth());
        const float screenH = static_cast<float>(GetScreenHeight());
        constexpr int kLines = kRowCount + 9; // title, note, header, 24 rows, shared bindings (CR-8), status, 2 help lines, padding
        const int fontSize = std::clamp(static_cast<int>((screenH - 24.0f) / (kLines * 1.3f)), 10, 30);
        ensureFont(fontSize);
        const float size = static_cast<float>(fontSize);
        const float line = std::floor(size * 1.3f);
        const float margin = std::max(8.0f, size * 0.8f);
        const float panelW = std::min(screenW - 16.0f, size * 50.0f);
        const float panelH = std::min(screenH - 8.0f, line * kLines + margin * 2.0f);
        const float x0 = std::floor((screenW - panelW) * 0.5f);
        const float y0 = std::floor((screenH - panelH) * 0.5f);
        const Color text{232, 232, 236, 255};
        const Color dim{150, 156, 168, 255};
        const Color accent{255, 196, 64, 255};

        DrawRectangle(0, 0, static_cast<int>(screenW), static_cast<int>(screenH), Fade(BLACK, 0.55f));
        DrawRectangle(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(panelW), static_cast<int>(panelH), Color{18, 20, 26, 238});
        DrawRectangleLines(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(panelW), static_cast<int>(panelH), Color{90, 80, 50, 255});

        const float innerX = x0 + margin;
        const float innerW = panelW - margin * 2.0f;
        float y = y0 + margin;
        drawText("Controls - player 1", innerX, y, size, accent);
        const std::string where = pc::path().empty() ? std::string("no file") : std::filesystem::path(pc::path()).filename().string();
        drawText(where + (pc::dirty() ? "  (unsaved)" : ""), innerX + innerW - textWidth(where + (pc::dirty() ? "  (unsaved)" : ""), size), y, size, dim);
        y += line;
        drawText("The game keeps running: pause it first (Start).", innerX, y, size, dim);
        y += line;

        const float actionW = innerW * 0.34f;
        const float keyW = innerW * 0.15f;
        const float padW = innerW - actionW - keyW * kKeyColumns;
        auto columnX = [&](int col) { return innerX + actionW + keyW * static_cast<float>(std::min(col, kKeyColumns)); };
        auto columnW = [&](int col) { return col < kKeyColumns ? keyW : padW; };
        drawText("Action", innerX, y, size, dim);
        for (int col = 0; col < kColumnCount; ++col)
            drawText(col < kKeyColumns ? "Key " + std::to_string(col + 1) : std::string("Controller"), columnX(col) + 6.0f, y, size, dim);
        y += line;
        DrawLine(static_cast<int>(innerX), static_cast<int>(y - 3.0f), static_cast<int>(innerX + innerW), static_cast<int>(y - 3.0f), Color{70, 72, 80, 255});

        for (int row = 0; row < kRowCount; ++row)
        {
            const Split split = splitRow(row);
            if (row == g_row)
                DrawRectangle(static_cast<int>(innerX - 4.0f), static_cast<int>(y), static_cast<int>(innerW + 8.0f), static_cast<int>(line), Color{255, 255, 255, 18});
            std::string label = rowLabel(row);
            const size_t hidden = split.other.size() + (split.keys.size() > kKeyColumns ? split.keys.size() - kKeyColumns : 0u) +
                                  (split.pads.size() > 1u ? split.pads.size() - 1u : 0u);
            if (hidden != 0u)
                label += "  +" + std::to_string(hidden);
            drawText(fit(label, size, actionW - 8.0f), innerX, y + (line - size) * 0.5f, size, *kRows[static_cast<size_t>(row)].action ? text : dim);
            for (int col = 0; col < kColumnCount; ++col)
            {
                const float cx = columnX(col);
                const float cw = columnW(col);
                const bool cursor = row == g_row && col == g_col;
                if (cursor)
                {
                    DrawRectangle(static_cast<int>(cx + 2.0f), static_cast<int>(y + 1.0f), static_cast<int>(cw - 4.0f), static_cast<int>(line - 2.0f),
                                  g_capture ? Color{190, 60, 50, 150} : Color{255, 196, 64, 60});
                    DrawRectangleLines(static_cast<int>(cx + 2.0f), static_cast<int>(y + 1.0f), static_cast<int>(cw - 4.0f), static_cast<int>(line - 2.0f), accent);
                }
                std::string cell;
                if (cursor && g_capture)
                    cell = col < kKeyColumns ? "press a key" : "press a button";
                else if (col < kKeyColumns)
                    cell = static_cast<size_t>(col) < split.keys.size() ? pc::displayName(split.keys[static_cast<size_t>(col)]) : "-";
                else
                    cell = split.pads.empty() ? "-" : pc::displayName(split.pads.front());
                drawText(fit(cell, size, cw - 14.0f), cx + 7.0f, y + (line - size) * 0.5f, size, cell == "-" ? dim : text);
            }
            y += line;
        }

        y += line * 0.5f;
        if (const std::string &shared = sharedBindingsText(); !shared.empty()) // sweep CR-8
            drawText(fit(shared, size, innerW), innerX, y, size, Color{255, 128, 96, 255});
        y += line;
        if (!g_status.empty())
            drawText(fit(g_status, size, innerW), innerX, y, size, accent);
        y += line;
        drawText(fit("Arrows / D-pad: move     Enter / A: rebind     Delete / X: clear     F5 twice: default layout", size, innerW), innerX, y, size, dim);
        y += line;
        drawText(fit("Esc / B / F1: save and close     Back+Start on a controller opens and closes this screen", size, innerW), innerX, y, size, dim);

        if (g_test.shotPending)
            takeShot();
    }

    void shutdown()
    {
        // Sweep CR-9: the window is closing or the run ended: the user's Sticky/Toggle/Filter Keys hotkeys come back now
        // (main ends in _Exit, which runs no atexit handler).
        ps2_host_input::restoreShortcutKeys();
        if (g_open)
            close(true);
        else if (pc::dirty())
        {
            std::string message;
            pc::save(message);
            std::cerr << "[controls] on exit: " << message << std::endl;
        }
        if (g_fontOwned)
            UnloadFont(g_font);
        g_fontOwned = false;
        g_font = Font{};
        g_fontSize = 0;
    }
}
