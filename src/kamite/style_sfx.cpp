#include "kamite/style_sfx.h"

#include "kamite/hd_textures.h"
#include "kamite/log.h"

#include "raylib.h"

#include <filesystem>
#include <mutex>

namespace kamite::style_sfx
{
    namespace
    {
        constexpr float kVolume = 0.5f; // K's pick (Nov 11)
        std::mutex g_lock;
        Sound g_sound{};
        bool g_tried = false, g_loaded = false;
    }

    void play()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (!IsAudioDeviceReady())
            return;
        if (!g_tried)
        {
            g_tried = true;
            const std::filesystem::path path = hd_textures::exeDirectory() / "resources" / "sfx" / "style_switch.ogg";
            Wave wave = LoadWave(path.string().c_str());
            if (wave.frameCount == 0)
                KM_WARN(Game, "styles: can't read " << path.string() << ": no switch sound");
            else
            {
                g_sound = LoadSoundFromWave(wave); // copies the samples
                SetSoundVolume(g_sound, kVolume);
                g_loaded = true;
            }
            UnloadWave(wave);
        }
        // Not PlaySound: GHR.exe links winmm with /FORCE:MULTIPLE and winmm's PlaySound takes the call (launcher.cpp).
        // raylib's PlaySound is exactly this; on a sound still playing it starts it again from the top.
        if (g_loaded)
            PlayAudioStream(g_sound.stream);
    }
}
