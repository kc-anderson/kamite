#include "kamite/style_sfx.h"

#include "kamite/hd_textures.h"
#include "kamite/log.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <system_error>

namespace kamite::style_sfx
{
    namespace
    {
        namespace fs = std::filesystem;
        std::mutex g_lock;
        std::vector<fs::path> g_files;
        bool g_scanned = false;
        int g_selected = -1;
        Sound g_sound{};
        bool g_loaded = false;
        float g_gain = 0.8f;

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        void unloadLocked()
        {
            if (g_loaded)
                UnloadSound(g_sound);
            g_loaded = false;
            g_sound = Sound{};
        }
        void scanLocked()
        {
            const fs::path previous = g_selected >= 0 && g_selected < static_cast<int>(g_files.size()) ? g_files[g_selected] : fs::path{};
            g_files.clear();
            std::error_code error;
            const fs::path dir = hd_textures::exeDirectory() / "style_sfx";
            for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error))
            {
                const std::string ext = lower(it->path().extension().string());
                if (it->is_regular_file(error) && (ext == ".wav" || ext == ".ogg" || ext == ".mp3"))
                    g_files.push_back(it->path());
            }
            std::sort(g_files.begin(), g_files.end());
            g_scanned = true;
            g_selected = -1;
            for (size_t i = 0; i < g_files.size(); ++i)
                if (g_files[i] == previous)
                    g_selected = static_cast<int>(i);
            if (g_selected < 0)
                unloadLocked();
            KM_INFO(Game, "style sfx: " << g_files.size() << " file(s) in " << dir.string());
        }
    }

    void rescan()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        scanLocked();
    }

    std::vector<std::string> names()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (!g_scanned)
            scanLocked();
        std::vector<std::string> out;
        for (const fs::path &file : g_files)
            out.push_back(file.filename().string());
        return out;
    }

    int selected()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_selected;
    }

    void select(int index)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        unloadLocked();
        g_selected = index >= 0 && index < static_cast<int>(g_files.size()) ? index : -1;
        if (g_selected < 0 || !IsAudioDeviceReady())
            return;
        Wave wave = LoadWave(g_files[g_selected].string().c_str());
        if (wave.frameCount == 0)
        {
            KM_WARN(Game, "style sfx: can't read " << g_files[g_selected].string());
            g_selected = -1;
            return;
        }
        g_sound = LoadSoundFromWave(wave); // copies the samples
        UnloadWave(wave);
        g_loaded = true;
    }

    float gain()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        return g_gain;
    }

    void setGain(float gain)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_gain = gain;
    }

    void play()
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (!g_loaded || !IsAudioDeviceReady())
            return;
        SetSoundVolume(g_sound, g_gain);
        // Not PlaySound: GHR.exe links winmm with /FORCE:MULTIPLE and winmm's PlaySound takes the call (launcher.cpp).
        // raylib's PlaySound is exactly this; on a sound still playing it starts it again from the top.
        PlayAudioStream(g_sound.stream);
    }
}
