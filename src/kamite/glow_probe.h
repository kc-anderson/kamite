#pragma once

class PS2Runtime;

// Diagnostic (Oct 25): in the casino the glow ("esp05": bright-part extract, 2 x 25-tap blur, additive composite) was
// built twice every frame in K's GIF dump, about 12 times as bright as one pass, and some additive flare sprites
// were sent twice. Not known yet whether the PS2 does the same or the port spawns or draws the effect twice (the 60 FPS
// patch?). This logs the effect's setup (Panel_setupOnce 0x2E8318, $a0 the effect object) per frame: how often, and
// whether by one object twice or by two objects. Observation only.
namespace kamite::glow_probe
{
    void install(PS2Runtime &runtime);
}
