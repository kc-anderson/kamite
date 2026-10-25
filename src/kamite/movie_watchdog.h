#pragma once

#include "ps2_runtime.h"

namespace kamite
{
    // After FunctionHooks::attach, before callprofile and runtime.run().
    void installMovieWatchdog(PS2Runtime &runtime);
}
