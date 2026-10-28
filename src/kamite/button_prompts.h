#pragma once

// The game's button prompts show the player's PC bindings (port_prep/prompts/DESIGN_2026-10-18.md): the text-icon sheet
// (texture 0x18) and the action-prompt sheet (texture 0x0F) are redrawn with the bound keys (or Xbox-style controller
// names) and written over the sheets in guest RAM, where the game re-sends them from on every draw. A PlayStation
// controller gets the original art back. KAMITE_PROMPTS=0 leaves the sheets alone.

#include "ps2_runtime.h"

namespace kamite::button_prompts
{
    // After FunctionHooks::attach, before runtime.run() (registers a vblank frame hook and starts the builder thread).
    void install(PS2Runtime &runtime);
    // The F2 menu (any thread): the sheets are drawn again at the builder's next pass (a changed prompt set).
    void requestRebuild();
}
