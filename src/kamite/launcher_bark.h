#pragma once

// The start bark (K, Nov 10): on Play a voice says "God Hand" over the launcher's music washing out, like the title
// barks of other action games. The lines come from the player's own disc (never shipped): the opening narration's
// "...the title of God Hand" in three treatments, and ten cutscene lines K picked. Each start renders one line in the
// background; Play renders the music's tail from wherever the music is. Plain C++ (no raylib, no OS calls), so it
// moves to the other platforms with the launcher. Sound design and K's approvals: claude/start-bark.md.
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace kamite::launcher_bark
{
    constexpr int kRate = 48000; // every buffer here: 48 kHz, interleaved stereo float

    struct Stereo
    {
        std::vector<float> lr;
        size_t frames() const { return lr.size() / 2; }
    };

    // The lines, in a fixed order (the shuffle deck in GHR.ini stores their indices).
    int count();
    const char *name(int line);

    // Reads a file of the disc ("se/r000.afs") into bytes; false when it is not there.
    using ReadFile = std::function<bool(std::string_view path, std::vector<uint8_t> &bytes)>;

    // The line's sound, ready to play at Play's press: the reverse swell (kVoiceAt seconds), the voice with its room,
    // faded out right after the last word. False when its file can't be read or decoded.
    constexpr float kVoiceAt = 0.20f;
    bool render(int line, const ReadFile &read, Stereo &out);

    // The music's tail lasts at most this long (every bark ends sooner).
    constexpr float kMaxTail = 3.0f;

    // The music's reverb, made once before Play (the tail needs it at once when Play is pressed).
    struct Wash
    {
        size_t fftSize = 0;
        std::vector<std::vector<double>> spectrum; // per channel: interleaved re, im
    };
    void prepareWash(Wash &wash);

    // The music on Play: from frame `from` of the looping track (interleaved stereo, 16-bit, `rate` Hz), the dry
    // music closes down (low-pass sweep) and fades fast while its reverb rings on, everything faded out at `end`
    // seconds after the press (the bark's end). Played at the music's own volume in place of the music.
    void musicTail(const int16_t *music, size_t frames, int rate, size_t from, float end, const Wash &wash, Stereo &out);
}
