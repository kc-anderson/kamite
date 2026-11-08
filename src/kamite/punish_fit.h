#pragma once

// Buck Breaker's punish fits: a punish made for one body (Stinger for the fat men, Spanking for the women...) fitted
// to every other body offline (tools/models/punish_fit; claude/punish-fit-offline.md). Per punish and body (skeleton):
// curves over the punish's motions of Gene's placement (forward, side, turn) and height, and the victim's height and
// waist bend. Placement moves Gene every frame; the heights and the bend rewrite the pair's motions as they start.
// resources/punish_fit.txt (punish_fit.txt next to the exe overrides it, for trying a fit without a rebuild).

#include <cstdint>
#include <filesystem>

class PS2Runtime;

namespace kamite::punish_fit
{
    // The punishes that place Gene for the pair (func_001299F0). Pummel has no placement.
    enum Punish : int
    {
        Suplex,
        Stinger,
        CobraTwist,
        Spanking,
        GorillaSuplex, // not a pick: a Suplex pick plays it on the bodies marked for it (the fat and wide men)
        kPunishCount
    };

    // Guest memory for the rewritten motions: the DLL slot hole's free top, above Buck Breaker's motions
    // (function_hooks' hole guard skips it).
    inline constexpr uint32_t kRewriteArea = 0x00810000u;
    inline constexpr uint32_t kRewriteAreaEnd = 0x00860000u;

    // With [Game] BuckBreaker on: wraps the pair placement, observes motion starts, loads the fits.
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);

    // EE thread: this enemy's body is marked "doesn't fit" for the punish (Buck Breaker then keeps its own punish).
    [[nodiscard]] bool doesntFit(const uint8_t *rdram, uint32_t enemy, int punish);
    // EE thread: a Suplex pick plays the Gorilla Suplex on this enemy's body (its Suplex fit says "gorilla").
    [[nodiscard]] bool suplexAsGorilla(const uint8_t *rdram, uint32_t enemy);
}
