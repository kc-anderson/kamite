#pragma once

#include <string>
#include <vector>

// Signal Style Switch's sound on a switch (K, Nov 11; temporary F2 picker while K tries his own sounds): the .wav / .ogg /
// .mp3 files in style_sfx\ next to GHR.exe, played through raylib's audio device (the one the launcher left open).
namespace kamite::style_sfx
{
    void rescan();                          // reads style_sfx\ again (F2 button, and at the first use)
    std::vector<std::string> names();       // the files found, sorted
    int selected();                         // -1 = none
    void select(int index);                 // loads it (unloads the last)
    float gain();
    void setGain(float gain);
    void play();                            // any thread; restarts it if still playing (a new switch cuts the last)
}
