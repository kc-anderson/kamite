#pragma once

#include <string>
#include <vector>

// Host gamepads behind the "SDL-<n>/<element>" bindings of controls.ini (PCSX2's syntax). With SDL3 (the default)
// the numbers follow PCSX2: an Xbox controller keeps its XInput number, the others take the lowest free one, and the
// motors work. PS2X_SDL_INPUT=0 (controls.ini [InputSources] SDL = false) reads GLFW's gamepads instead (the raylib
// input of older versions: GLFW joystick n is SDL-<n>, no vibration). Everything here runs on the window thread.
namespace ps2_host_gamepads
{
    constexpr int kMaxDevices = 16;

    // SDL3's SDL_GamepadButton / SDL_GamepadAxis numbering.
    enum Button
    {
        South,
        East,
        West,
        North,
        Back,
        Guide,
        Start,
        LeftStick,
        RightStick,
        LeftShoulder,
        RightShoulder,
        DPadUp,
        DPadDown,
        DPadLeft,
        DPadRight,
        Misc1,
        RightPaddle1,
        LeftPaddle1,
        RightPaddle2,
        LeftPaddle2,
        Touchpad,
        ButtonCount
    };

    enum Axis
    {
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger,
        AxisCount
    };

    // Opens SDL on the first call, then handles controllers being plugged in and out.
    void update();
    [[nodiscard]] bool connected(int device);
    [[nodiscard]] bool button(int device, Button button);
    // Sticks -1..1 (down and right positive), triggers 0..1.
    [[nodiscard]] float axis(int device, Axis axis);
    // 0..1 each (large = low frequency motor); sent when it changes and refreshed while it runs.
    void setRumble(int device, float large, float small);

    struct DeviceInfo
    {
        int device = -1;
        std::string name;
    };
    [[nodiscard]] std::vector<DeviceInfo> devices();

    // The button art a controller's name points to (button prompts; GLFW gives no controller type, only the name).
    // Unknown: not connected, or the name says neither.
    enum class Style
    {
        Unknown,
        Xbox,
        PlayStation,
        Nintendo, // Switch Pro, Joy-Cons, Switch-style pads: positional buttons, Nintendo's letters
        Steam     // Steam Deck, Steam Controller
    };
    [[nodiscard]] Style style(int device);
}
