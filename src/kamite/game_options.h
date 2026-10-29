#pragma once

#include "ps2_runtime.h"

namespace kamite
{
    // Optional gameplay/sound changes from kamite.ini [Game] (the launcher's Gameplay tab). Before runtime.run().
    //   UnlockHard (KAMITE_UNLOCK_HARD)  Hard can be picked on the difficulty select without a clear. The save's own
    //                                     unlock flag is only read, so a save is left as it was.
    //   SkipIntro  (KAMITE_SKIP_INTRO)   boot straight to the main menu (no logos, no "Press Start"): r020 patches.
    //   MuteMusic  (KAMITE_MUTE_MUSIC)   the game's music streams (cSndBgmNode) play at volume 0; voices, sound
    //                                     effects and movies are unchanged.
    void installGameOptions(PS2Runtime &runtime);

    // The F2 menu (any thread): the mute takes effect at the music's next volume change (a room or track change).
    bool muteMusic();
    void setMuteMusic(bool on);
}
