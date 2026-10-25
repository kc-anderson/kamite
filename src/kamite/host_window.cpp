#include "kamite/host_window.h"

#include "ps2_host_backend.h" // raylib.h
#include "runtime/ps2_display_options.h"

namespace kamite::hostwindow
{
    HeadlessMethod requestHeadless()
    {
#if defined(KAMITE_RUNTIME_HAS_SET_HEADLESS) && KAMITE_RUNTIME_HAS_SET_HEADLESS
        ps2_host_display::setHeadless(true);
        return HeadlessMethod::RuntimeSetHeadless;
#else
        SetConfigFlags(FLAG_WINDOW_HIDDEN);
        return HeadlessMethod::HiddenWindowFlag;
#endif
    }

    const char *methodName(HeadlessMethod method)
    {
        switch (method)
        {
        case HeadlessMethod::RuntimeSetHeadless:
            return "runtime setHeadless (hidden window, no audio device)";
        case HeadlessMethod::HiddenWindowFlag:
            return "raylib FLAG_WINDOW_HIDDEN before initialize (hidden window, audio muted)";
        }
        return "?";
    }
}
