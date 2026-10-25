#pragma once

// Headless (hidden) runs. Kept in its own file because it includes raylib.h, whose names (CloseWindow, ShowCursor,
// Rectangle...) clash with windows.h in main.cpp.

namespace kamite::hostwindow
{
    enum class HeadlessMethod
    {
        // The runtime's own switch (LightVelox/PS2Recomp sotc-port 29d26f9 "Add headless mode"):
        // ps2_host_display::setHeadless(true) = hidden window, no audio device. Used when the runtime header declares it
        // (CMake defines KAMITE_RUNTIME_HAS_SET_HEADLESS).
        RuntimeSetHeadless,
        // Runtimes without it (the Urban Reign runtime at 2fa53c4): raylib's FLAG_WINDOW_HIDDEN is set before
        // PS2Runtime::initialize(). raylib 5.5 ORs config flags (rcore.c SetConfigFlags: CORE.Window.flags |= flags) and
        // InitPlatform turns the flag into glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE), so the runtime's own
        // SetConfigFlags(FLAG_WINDOW_RESIZABLE | ...) keeps it. The audio device still opens: main.cpp mutes it
        // (PS2X_AUDIO_MUTE=1).
        HiddenWindowFlag,
    };

    // Call once, before PS2Runtime::initialize().
    HeadlessMethod requestHeadless();
    const char *methodName(HeadlessMethod method);
}
