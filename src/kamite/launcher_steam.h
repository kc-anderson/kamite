#pragma once

// The launcher's "Add to Steam" (K Nov 10): a non-Steam shortcut to GHR.exe in the most recently signed-in Steam
// account's library, with library art. Built only with KAMITE_STEAM_SHORTCUT (off for Android); the operating-system
// pieces (where Steam lives, its process, downloads) are one small block per platform in launcher_steam.cpp, and a
// platform without them reports Steam as not there, so the button stays hidden.
#include <atomic>
#include <filesystem>
#include <string>

namespace kamite::launcher::steam
{
    struct Status
    {
        bool available = false; // Steam is installed and an account has signed in on this PC
        bool added = false;     // our entry is in that account's shortcut list
    };
    // Cheap (two small files): called when the launcher opens and after a job.
    Status probe();
    // Steam's own process is running (its shortcut list is read at start and written at exit: it must restart).
    bool running();

    enum class Stage : int
    {
        Downloading,
        ClosingSteam,
        Writing,
        StartingSteam,
        Done
    };
    struct Result
    {
        bool ok = false;
        std::string message; // the popup's text: what happened, the art that could not be downloaded
    };
    // The entry starts the running program (GHR.exe, wherever it is now).
    // Downloads the library art, closes Steam if it runs (and starts it again after), adds or updates the entry and
    // writes the art. Blocks for seconds: run it on a thread. `stage` follows its progress for the popup.
    Result addOrUpdate(const std::filesystem::path &resources, std::atomic<int> &stage);
}
