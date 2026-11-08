#pragma once

// Buck Breaker's punish fits: per punish and per body (skeleton), how far Gene stands from his victim, moved by
// Forward / Side / Turn sliders in F2 > Debug (punish_fit.txt; notes in claude/punish-notes.md "Punish fit").

#include <cstdint>
#include <filesystem>

class PS2Runtime;
struct Pcsx2GsUi;

namespace kamite::punish_fit
{
    // The punishes that place Gene for the pair (func_001299F0). Pummel has no placement.
    enum Punish : int
    {
        Suplex,
        Stinger,
        CobraTwist,
        Spanking,
        kPunishCount
    };

    // With [Game] BuckBreaker on: wraps the pair placement and loads punish_fit.txt (else resources/punish_fit.txt).
    void install(PS2Runtime &runtime, const std::filesystem::path &exeDir);

    // EE thread: this enemy's body is marked "doesn't fit" for the punish (Buck Breaker then keeps its own punish).
    [[nodiscard]] bool doesntFit(const uint8_t *rdram, uint32_t enemy, int punish);

    // The F2 Debug tab's section (under slow motion and frame step).
    void drawPanel(const Pcsx2GsUi *ui);
}
