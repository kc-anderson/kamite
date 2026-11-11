#pragma once

// Signal Style Switch's sound on a switch (K, Nov 11): resources/sfx/style_switch.ogg, played through raylib's audio
// device (the one the launcher left open) at kVolume.
namespace kamite::style_sfx
{
    void play(); // any thread; restarts it if still playing (a new switch cuts the last)
}
