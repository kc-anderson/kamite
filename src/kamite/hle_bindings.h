#pragma once

// SDK functions located in SLUS_215.03 and bound by ADDRESS to the runtime's HLE handlers at start-up.
// GENERIC mechanism from the Urban Reign port's src/urban/hle_bindings.h. The game's table
// (kamite_sdk_functions.inc) and the bound-name list come from port_prep/hle-bindings (installed).

#include "ps2_runtime.h"

#include <cstdint>
#include <string_view>

namespace kamite
{
    struct SdkFunction
    {
        uint32_t address;
        std::string_view name;
        std::string_view method;
        float score;
    };

    // SDK functions listed in kamite/kamite_sdk_functions.inc (address 0 = none).
    const SdkFunction *findSdkFunction(std::string_view name);
    const SdkFunction *sdkFunctionAt(uint32_t address);

    // Binds the located SDK functions whose work the runtime does at the API level (SIF/IOP, file I/O, CD/DVD,
    // DECI2/TTY; MPEG/IPU only with KAMITE_MPEG_HLE=1 or PS2X_IPU=0) to the runtime's handlers.
    // KAMITE_HLE_SKIP=name,name leaves those as recompiled code.
    void installHleBindings(PS2Runtime &runtime);
}
