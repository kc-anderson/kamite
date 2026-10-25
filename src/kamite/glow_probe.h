#pragma once

class PS2Runtime;

// Diagnostic (Oct 25, round 2): at 60 FPS two esp05 glow objects (the screen glow: bright-part extract, blur, additive
// composite) live side by side (pool slots 0 and 56), at 30 FPS one; the picture is twice as bright and grainy. The
// effect code itself counts game frames everywhere (emitter intervals, children's lifetimes; an esp05's own lifetime
// is -1, it lives until its emitter ends it), so the doubling comes from whatever spawns or ends the emitter. This logs
// "[glow]" lines: each esp05's birth (its slot, the game frame, the emitter that pulled it and that emitter's
// parameters: count, interval, mode, start/end frames) and its death (age in frames, setups seen), so a 30 FPS run
// and a 60 FPS run of the same room can be compared. Observation only; strip with the fix (claude/strip-list.md).
namespace kamite::glow_probe
{
    void install(PS2Runtime &runtime);
}
