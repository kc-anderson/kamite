#pragma once

#include "ps2_runtime.h"

namespace kamite
{
    // [Video] AspectRatio (KAMITE_ASPECT): the picture's shape. Before runtime.initialize (the window takes this
    // shape) and before the guest starts (the camera and HUD constants are switched in the game code).
    void installWidescreen();
    // [Video] AspectRatio changed while playing (the OPTIONS screen; EE thread): the hooks' values, the camera and the
    // enemies' cones, the HUD's narrowing, the host display and PCSX2's custom aspect, as installWidescreen sets them.
    void setWidescreenAspect(const char *value);
    // The picture's shape set by installWidescreen (width / height; 4:3 = 1.3333).
    float aspectRatio();
}
