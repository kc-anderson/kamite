#pragma once

// Kamite: run-time loaded guest code (SN Systems SNR2 DLLs). A second, page-allocated dense function table over
// guest RAM outside the generated table's range [g_ps2RecompiledFunctionTableBase, g_ps2RecompiledFunctionTableEnd).
// hasFunction / lookupFunction / dispatchGuestBranch (single lookup) try the generated table first, then this one,
// so boot-ELF dispatch does the same work as before and module dispatch costs one range check plus two loads.
// The game layer fills it when the guest links a module (SNR2 0x394C18) and clears it on unlink (0x394D00).
// Declared here, not in ps2_runtime.h: that header is in the generated code's precompiled header and every generated
// file includes it, so touching it would recompile all boot functions.

#include <cstdint>

struct R5900Context;
class PS2Runtime;

namespace ps2x
{
    using ModuleFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    // False for unaligned addresses, addresses at or above 32 MB and addresses inside the generated table's range.
    bool setModuleFunction(uint32_t address, ModuleFunction func);
    // Clears every module slot in [begin, end) (pages stay allocated).
    void clearModuleFunctions(uint32_t begin, uint32_t end);
    // The module-table entry at address, or nullptr (diagnostics).
    ModuleFunction moduleFunctionAt(uint32_t address);
}
