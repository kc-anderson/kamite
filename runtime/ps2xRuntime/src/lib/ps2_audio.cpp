#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include "ps2_host_backend.h"
#include "runtime/ps2_time_control.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <span>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
    std::vector<uint8_t> buildWavFromPcm(const int16_t *pcm, size_t sampleCount, uint32_t sampleRate)
    {
        const uint32_t dataSize = static_cast<uint32_t>(sampleCount * 2);
        const uint32_t fileSize = 36 + dataSize;
        std::vector<uint8_t> wav(8 + fileSize);

        uint8_t *p = wav.data();
        p[0] = 'R';
        p[1] = 'I';
        p[2] = 'F';
        p[3] = 'F';
        p[4] = static_cast<uint8_t>(fileSize);
        p[5] = static_cast<uint8_t>(fileSize >> 8);
        p[6] = static_cast<uint8_t>(fileSize >> 16);
        p[7] = static_cast<uint8_t>(fileSize >> 24);
        p[8] = 'W';
        p[9] = 'A';
        p[10] = 'V';
        p[11] = 'E';
        p[12] = 'f';
        p[13] = 'm';
        p[14] = 't';
        p[15] = ' ';
        p[16] = 16;
        p[17] = 0;
        p[18] = 0;
        p[19] = 0;
        p[20] = 1;
        p[21] = 0;
        p[22] = 1;
        p[23] = 0;
        p[24] = static_cast<uint8_t>(sampleRate);
        p[25] = static_cast<uint8_t>(sampleRate >> 8);
        p[26] = static_cast<uint8_t>(sampleRate >> 16);
        p[27] = static_cast<uint8_t>(sampleRate >> 24);
        const uint32_t byteRate = sampleRate * 2;
        p[28] = static_cast<uint8_t>(byteRate);
        p[29] = static_cast<uint8_t>(byteRate >> 8);
        p[30] = static_cast<uint8_t>(byteRate >> 16);
        p[31] = static_cast<uint8_t>(byteRate >> 24);
        p[32] = 2;
        p[33] = 0;
        p[34] = 16;
        p[35] = 0;
        p[36] = 'd';
        p[37] = 'a';
        p[38] = 't';
        p[39] = 'a';
        p[40] = static_cast<uint8_t>(dataSize);
        p[41] = static_cast<uint8_t>(dataSize >> 8);
        p[42] = static_cast<uint8_t>(dataSize >> 16);
        p[43] = static_cast<uint8_t>(dataSize >> 24);
        if (dataSize != 0u)
            std::memcpy(p + 44, pcm, dataSize);
        return wav;
    }

    // PS2X_AUDIO_STRETCH: time-stretch (WSOLA, the method of SoundTouch's TDStretch that PCSX2 uses) for when the game
    // runs below full speed. The emulated SPU2 then makes less than 48000 frames per real second and the sound card
    // ran dry: cuts every few hundred ms at 83% speed on 2 cores (PROGRESS §69). Stretched, the sound is continuous and
    // keeps its pitch, only slower, like the game. Interleaved stereo 48 kHz.
    class TimeStretch
    {
    public:
        static constexpr size_t kSequence = 1920u; // 40 ms per piece
        static constexpr size_t kOverlap = 384u;   // 8 ms cross-fade between pieces
        static constexpr size_t kSeek = 720u;      // 15 ms searched for the best splice

        void reset()
        {
            m_input.clear();
            m_inputPos = 0u;
            m_skipFraction = 0.0;
            m_haveTail = false;
            m_outputEnd = 0u;
        }

        void push(std::span<const int16_t> samples)
        {
            m_input.reserve(m_input.size() + samples.size());
            for (const int16_t sample : samples)
                m_input.push_back(static_cast<float>(sample));
        }

        // Appends stretched frames to out: each piece outputs kSequence - kOverlap frames and consumes
        // tempo * (kSequence - kOverlap) input frames (tempo < 1: more output than input).
        void process(double tempo, std::vector<int16_t> &out)
        {
            while (availableFrames() >= kSeek + kSequence)
            {
                const size_t offset = m_haveTail ? bestOffset() : 0u;
                const float *piece = m_input.data() + (m_inputPos + offset) * 2u;
                for (size_t i = 0u; i < kOverlap; ++i)
                {
                    const float w = static_cast<float>(i) / static_cast<float>(kOverlap);
                    for (size_t c = 0u; c < 2u; ++c)
                    {
                        const float value = m_haveTail ? m_tail[i * 2u + c] * (1.0f - w) + piece[i * 2u + c] * w : piece[i * 2u + c];
                        out.push_back(clamp16(value));
                    }
                }
                for (size_t i = kOverlap; i < kSequence - kOverlap; ++i)
                    for (size_t c = 0u; c < 2u; ++c)
                        out.push_back(clamp16(piece[i * 2u + c]));
                std::copy(piece + (kSequence - kOverlap) * 2u, piece + kSequence * 2u, m_tail.begin());
                m_haveTail = true;
                m_outputEnd = m_inputPos + offset + (kSequence - kOverlap);
                const double skip = tempo * static_cast<double>(kSequence - kOverlap) + m_skipFraction;
                const size_t whole = static_cast<size_t>(skip);
                m_skipFraction = skip - static_cast<double>(whole);
                m_inputPos += whole;
            }
            // Drop consumed input now and then.
            if (m_inputPos > 16384u)
            {
                m_input.erase(m_input.begin(), m_input.begin() + static_cast<std::ptrdiff_t>(m_inputPos * 2u));
                m_outputEnd = m_outputEnd > m_inputPos ? m_outputEnd - m_inputPos : 0u;
                m_inputPos = 0u;
            }
        }

        // Leaving the stretch: playback continues with the input frame right after the last one output (the start of
        // the held tail), so nothing is played twice and there is no splice. It used to restart at m_inputPos, up to
        // offset + (1 - tempo) * 1536 frames (~20 ms) before that point, with a hard cut: the same sound twice.
        void flush(std::vector<int16_t> &out)
        {
            const size_t start = m_haveTail ? std::min(m_outputEnd, m_input.size() / 2u) : m_inputPos;
            for (size_t i = start * 2u; i < m_input.size(); ++i)
                out.push_back(clamp16(m_input[i]));
            reset();
        }

        // Input samples held here, not yet output (they reach the queue later, or at flush).
        size_t heldSamples() const { return m_input.size() - m_inputPos * 2u; }

    private:
        size_t availableFrames() const { return m_input.size() / 2u - m_inputPos; }

        static int16_t clamp16(float value)
        {
            return static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
        }

        // The offset within kSeek whose first kOverlap frames best match the previous piece's tail (normalized cross-
        // correlation of the summed channels, every other frame).
        size_t bestOffset() const
        {
            const float *base = m_input.data() + m_inputPos * 2u;
            size_t best = 0u;
            double bestScore = -1e300;
            for (size_t offset = 0u; offset < kSeek; ++offset)
            {
                const float *candidate = base + offset * 2u;
                double correlation = 0.0, energy = 1e-9;
                for (size_t i = 0u; i < kOverlap; i += 2u)
                {
                    const float a = m_tail[i * 2u] + m_tail[i * 2u + 1u];
                    const float b = candidate[i * 2u] + candidate[i * 2u + 1u];
                    correlation += static_cast<double>(a) * b;
                    energy += static_cast<double>(b) * b;
                }
                const double score = correlation / std::sqrt(energy);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = offset;
                }
            }
            return best;
        }

        std::vector<float> m_input;
        size_t m_inputPos = 0u; // frames of m_input already consumed
        size_t m_outputEnd = 0u; // frame of m_input after the last one output (the held tail starts there)
        double m_skipFraction = 0.0;
        std::array<float, kOverlap * 2u> m_tail{};
        bool m_haveTail = false;
    };
}

namespace ps2_vag
{
    bool decode(const uint8_t *data, uint32_t sizeBytes,
                std::vector<int16_t> &outPcm, uint32_t &outSampleRate);
}

struct PS2AudioBackend::Impl
{
    struct TrackedSound
    {
        Sound snd;
        uint32_t sampleKey;
    };
    std::vector<TrackedSound> activeSounds;
    static constexpr size_t kRingSamples = 16384u;
    std::array<int16_t, kRingSamples> ring{};
    size_t read = 0u;
    size_t queued = 0u;
    static constexpr size_t kPrimeSamples = 4096u;
    static constexpr size_t kRateTargetSamples = 8192u;
    static constexpr int32_t kFadeFrames = 128;
    bool primed = false;
    int32_t fadePosition = 0;
    int32_t gapRemaining = 0;
    double readFraction = 0.0; // PS2X_AUDIO_RATE_CONTROL: position between the frame at read and the next
    // PS2X_AUDIO_STRETCH: production speed (frames made per real second / 48000, smoothed) and the stretcher.
    TimeStretch stretch;
    std::vector<int16_t> stretched;
    bool stretching = false;
    double speed = 1.0;
    uint64_t rateFrames = 0u;
    std::chrono::steady_clock::time_point rateStart{};
    std::chrono::steady_clock::time_point lastSubmit{};
    uint32_t slowWindows = 0u;
    uint64_t gaps = 0u;      // PS2X_AUDIO_STATS: submissions after more than 50 ms without audio
    double longestGap = 0.0; // and the longest such pause (s), per stats line
    std::array<int16_t, 2> lastOutput{};
    std::array<int16_t, 2> fadeSource{};
    std::array<int16_t, 2> gapSource{};
    std::mutex pcmMutex;
    AudioStream stream{};
    bool streamReady = false;
    std::FILE *dump = nullptr;
    uint32_t dumpBytes = 0u;
    // PS2X_AUDIO_OUT_DUMP=<file>: raw PCM (s16le stereo 48 kHz, no header) of exactly what the callback hands the sound
    // card, after the queue, rate control and time-stretch (PS2X_AUDIO_DUMP is the SPU2 output before all of them).
    // Written also while muted, so hidden runs can measure host-side repeats and skips. Flushed per callback.
    std::FILE *outDump = nullptr;
    bool statsEnabled = false;
    bool everPrimed = false;
    uint64_t producedFrames = 0u;
    uint64_t requestedFrames = 0u;
    uint64_t missingFrames = 0u;
    uint64_t underruns = 0u;
    uint64_t droppedFrames = 0u;
    std::chrono::steady_clock::time_point statsStart = std::chrono::steady_clock::now();
    static Impl *streamOwner;

    static void audioCallback(void *buffer, unsigned int frames)
    {
        auto *output = static_cast<int16_t *>(buffer);
        const size_t requested = static_cast<size_t>(frames) * 2u;
        std::fill_n(output, requested, int16_t{0});
        Impl *owner = streamOwner;
        if (!owner)
            return;
        std::lock_guard<std::mutex> lock(owner->pcmMutex);
        owner->requestedFrames += frames;
        if (!owner->primed && owner->queued >= kPrimeSamples)
        {
            owner->primed = true;
            owner->everPrimed = true;
            owner->fadePosition = 0;
            owner->fadeSource = owner->lastOutput;
        }
        // PS2X_AUDIO_RATE_CONTROL (default on): play up to 0.5% faster or slower to keep the queue near
        // kRateTargetSamples (~85 ms, half the ring: the IOP thread makes audio in bursts and 43 ms ran dry now and then).
        // The emulated SPU2 makes 48 kHz of emulated time, which never matches the sound card exactly: the game runs a
        // little under or (locked to a 59.997 Hz monitor, PROGRESS §67) over real time. Without this the queue drained
        // (underruns) or filled up its 170 ms and ~127 ms were dropped at once.
        static const bool rateControl = [] { const char *v = std::getenv("PS2X_AUDIO_RATE_CONTROL"); return !(v && *v == '0'); }();
        const double fill = (static_cast<double>(owner->queued) - static_cast<double>(kRateTargetSamples)) / static_cast<double>(kRateTargetSamples);
        const double ratio = 1.0 + std::clamp(fill * 0.005, -0.005, 0.005);
        for (size_t frame = 0u; frame < frames; ++frame)
        {
            if (owner->primed && owner->queued < 2u)
            {
                owner->primed = false;
                ++owner->underruns;
                owner->gapRemaining = kFadeFrames;
                owner->gapSource = owner->lastOutput;
            }
            if (owner->primed && rateControl && owner->queued >= 4u)
            {
                // Linear interpolation between the frame at read and the next, stepping by the current ratio.
                owner->fadePosition = std::min(owner->fadePosition + 1, kFadeFrames);
                const int32_t fade = owner->fadePosition;
                const double frac = owner->readFraction;
                for (size_t channel = 0u; channel < 2u; ++channel)
                {
                    const double a = owner->ring[(owner->read + channel) % kRingSamples];
                    const double b = owner->ring[(owner->read + 2u + channel) % kRingSamples];
                    const int32_t sample = static_cast<int32_t>(a + (b - a) * frac);
                    owner->lastOutput[channel] = static_cast<int16_t>((sample * fade + owner->fadeSource[channel] * (kFadeFrames - fade)) / kFadeFrames);
                }
                owner->readFraction += ratio;
                while (owner->readFraction >= 1.0 && owner->queued >= 4u)
                {
                    owner->readFraction -= 1.0;
                    owner->read = (owner->read + 2u) % kRingSamples;
                    owner->queued -= 2u;
                }
            }
            else if (owner->primed)
            {
                owner->fadePosition = std::min(owner->fadePosition + 1, kFadeFrames);
                const int32_t fade = owner->fadePosition;
                for (size_t channel = 0u; channel < 2u; ++channel)
                {
                    const int32_t sample = owner->ring[(owner->read + channel) % kRingSamples];
                    owner->lastOutput[channel] = static_cast<int16_t>((sample * fade + owner->fadeSource[channel] * (kFadeFrames - fade)) / kFadeFrames);
                }
                owner->read = (owner->read + 2u) % kRingSamples;
                owner->queued -= 2u;
                owner->readFraction = 0.0;
            }
            else
            {
                owner->missingFrames += owner->everPrimed ? 1u : 0u;
                owner->gapRemaining = std::max(owner->gapRemaining - 1, 0);
                for (size_t channel = 0u; channel < 2u; ++channel)
                    owner->lastOutput[channel] = static_cast<int16_t>(owner->gapSource[channel] * owner->gapRemaining / kFadeFrames);
            }
            output[frame * 2u] = owner->lastOutput[0];
            output[frame * 2u + 1u] = owner->lastOutput[1];
        }
        if (owner->outDump)
        {
            std::fwrite(output, sizeof(int16_t), requested, owner->outDump);
            std::fflush(owner->outDump);
        }
    }

    void finishDump()
    {
        if (!dump)
            return;
        auto header = buildWavFromPcm(nullptr, 0u, 48000u);
        header[22] = 2;
        header[28] = 0;
        header[29] = 0xEE;
        header[30] = 2;
        header[32] = 4;
        const uint32_t length = dumpBytes + 36u;
        for (uint32_t byte = 0u; byte < 4u; ++byte)
        {
            header[4u + byte] = static_cast<uint8_t>(length >> (byte * 8u));
            header[40u + byte] = static_cast<uint8_t>(dumpBytes >> (byte * 8u));
        }
        std::fseek(dump, 0, SEEK_SET);
        std::fwrite(header.data(), 1u, header.size(), dump);
        std::fclose(dump);
        dump = nullptr;
    }
};

PS2AudioBackend::Impl *PS2AudioBackend::Impl::streamOwner = nullptr;

PS2AudioBackend::PS2AudioBackend() : m_impl(std::make_unique<Impl>())
{
}

PS2AudioBackend::~PS2AudioBackend()
{
    if (m_impl)
    {
        stopAll();
        setAudioReady(false);
    }
}

void PS2AudioBackend::setAudioReady(bool ready)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (ready == m_audioReady)
        return;
    m_audioReady = ready;
#if !defined(PLATFORM_VITA)
    if (ready)
    {
        const char *audioStats = std::getenv("PS2X_AUDIO_STATS");
        m_impl->statsEnabled = audioStats && *audioStats && *audioStats != '0';
        m_impl->statsStart = std::chrono::steady_clock::now();
        const char *audioMute = std::getenv("PS2X_AUDIO_MUTE");
        if ((audioMute && *audioMute && *audioMute != '0') || ps2x::timecontrol::muted())
            ps2x::timecontrol::setMuted(true);
        SetAudioStreamBufferSizeDefault(512);
        if (const char *outPath = std::getenv("PS2X_AUDIO_OUT_DUMP"); outPath && *outPath && !m_impl->outDump)
            m_impl->outDump = std::fopen(outPath, "wb");
        m_impl->stream = LoadAudioStream(48000, 16, 2);
        m_impl->streamReady = m_impl->stream.buffer != nullptr;
        if (m_impl->streamReady)
        {
            Impl::streamOwner = m_impl.get();
            SetAudioStreamCallback(m_impl->stream, &Impl::audioCallback);
            PlayAudioStream(m_impl->stream);
        }
        const char *path = std::getenv("PS2X_AUDIO_DUMP");
        if (path && *path)
        {
            m_impl->dump = std::fopen(path, "wb");
            if (m_impl->dump)
            {
                std::array<uint8_t, 44> header{};
                std::fwrite(header.data(), 1u, header.size(), m_impl->dump);
                m_impl->dumpBytes = 0u;
            }
        }
    }
    else
    {
        if (m_impl->streamReady)
        {
            StopAudioStream(m_impl->stream);
            UnloadAudioStream(m_impl->stream);
            m_impl->streamReady = false;
            Impl::streamOwner = nullptr;
        }
        m_impl->finishDump();
        if (m_impl->outDump)
        {
            std::fclose(m_impl->outDump);
            m_impl->outDump = nullptr;
        }
        resetPcm();
    }
#endif
}

void PS2AudioBackend::resetPcm()
{
    std::lock_guard<std::mutex> lock(m_impl->pcmMutex);
    m_impl->read = 0u;
    m_impl->queued = 0u;
    m_impl->readFraction = 0.0;
    m_impl->stretch.reset();
    m_impl->stretching = false;
    m_impl->speed = 1.0;
    m_impl->rateFrames = 0u;
    m_impl->rateStart = {};
    m_impl->lastSubmit = {};
    m_impl->slowWindows = 0u;
    m_impl->primed = false;
    m_impl->fadePosition = 0;
    m_impl->gapRemaining = 0;
    m_impl->lastOutput = {};
    m_impl->fadeSource = {};
    m_impl->gapSource = {};
    m_impl->everPrimed = false;
    m_impl->producedFrames = 0u;
    m_impl->requestedFrames = 0u;
    m_impl->missingFrames = 0u;
    m_impl->underruns = 0u;
    m_impl->droppedFrames = 0u;
    m_impl->statsStart = std::chrono::steady_clock::now();
}

void PS2AudioBackend::submitPcm(std::span<const int16_t> samples)
{
    std::lock_guard<std::mutex> lock(m_impl->pcmMutex);
    if (m_impl->dump && samples.size_bytes() <= 0xFFFFFF00u - m_impl->dumpBytes)
    {
        const size_t written = std::fwrite(samples.data(), sizeof(int16_t), samples.size(), m_impl->dump);
        m_impl->dumpBytes += static_cast<uint32_t>(written * sizeof(int16_t));
    }
    if (!m_impl->streamReady)
        return;
    m_impl->producedFrames += samples.size() / 2u;
    // PS2X_AUDIO_STRETCH (default on): below ~97% speed (measured from how fast the emulated SPU2 makes audio over
    // 0.5 s windows) the audio goes through TimeStretch; back above 99% it is passed through untouched again.
    static const bool stretchEnabled = [] { const char *v = std::getenv("PS2X_AUDIO_STRETCH"); return !(v && *v == '0'); }();
    // A stall (loading, a shader compile: no audio for over 50 ms, while the SPU2 hands over 512 frames = 10.7 ms at
    // a time) is a gap. Counted for the stats whether or not the stretch is on (round 10: with the stretch off by
    // default every "0 gaps" said nothing about stalls vs a sustained slowdown).
    bool gap = false;
    {
        Impl &d = *m_impl;
        const auto now = std::chrono::steady_clock::now();
        gap = d.lastSubmit != std::chrono::steady_clock::time_point{} && now - d.lastSubmit > std::chrono::milliseconds(50);
        if (d.lastSubmit != std::chrono::steady_clock::time_point{})
            d.longestGap = std::max(d.longestGap, std::chrono::duration<double>(now - d.lastSubmit).count());
        if (gap)
            ++d.gaps;
        d.lastSubmit = now;
    }
    if (stretchEnabled)
    {
        Impl &d = *m_impl;
        const auto now = std::chrono::steady_clock::now();
        // The stall restarts the speed window: counted in, one ~40 ms load hitch pulled a 0.5 s window under 0.925 and
        // switched the stretch on for seconds: every sound's attack played twice, 5-20 ms apart (hidden run 2026-10-17
        // 09:01, 131 repeats of 5-50 ms in the output). The stall is now a short underrun.
        if (d.rateStart == std::chrono::steady_clock::time_point{} || gap)
        {
            d.rateStart = now;
            d.rateFrames = 0u;
        }
        else
            d.rateFrames += samples.size() / 2u;
        const double window = std::chrono::duration<double>(now - d.rateStart).count();
        bool leaving = false;
        if (window >= 0.5)
        {
            const double measured = static_cast<double>(d.rateFrames) / (window * 48000.0);
            // A window with almost no audio (loading, pause) says nothing about the speed.
            if (measured > 0.2)
            {
                d.speed = std::min(1.1, d.speed * 0.6 + measured * 0.4);
                d.slowWindows = measured < 0.97 ? d.slowWindows + 1u : 0u;
            }
            d.rateFrames = 0u;
            d.rateStart = now;
            // On only after two slow windows in a row (1 s): a real slowdown, not a single dip.
            if (!d.stretching && d.speed < 0.97 && d.slowWindows >= 2u)
            {
                d.stretching = true;
                d.stretch.reset();
                if (d.statsEnabled)
                    std::fprintf(stderr, "[audio] time-stretch on (speed %.2f)\n", d.speed);
            }
            // Off only once the queue is back near its target too: leaving at 140 ms (after a state load) the fine
            // rate control (0.5%) took minutes to bring it back, the sound lagging the picture meanwhile; the stretch
            // drains it at up to 5% in about a second.
            else if (d.stretching && d.speed > 0.99 &&
                     std::abs(static_cast<double>(d.queued + d.stretch.heldSamples()) -
                              static_cast<double>(Impl::kRateTargetSamples)) < 0.25 * static_cast<double>(Impl::kRateTargetSamples))
            {
                d.stretching = false;
                leaving = true;
                if (d.statsEnabled)
                    std::fprintf(stderr, "[audio] time-stretch off (speed %.2f)\n", d.speed);
            }
        }
        if (d.stretching)
        {
            // Tempo = speed, nudged by up to 10% to bring the queue back to its target.
            const double fill = (static_cast<double>(d.queued + d.stretch.heldSamples()) - static_cast<double>(Impl::kRateTargetSamples)) /
                                static_cast<double>(Impl::kRateTargetSamples);
            const double tempo = std::clamp(d.speed * (1.0 + std::clamp(0.2 * fill, -0.1, 0.1)), 0.5, 1.05);
            d.stretched.clear();
            d.stretch.push(samples);
            d.stretch.process(tempo, d.stretched);
            samples = d.stretched;
        }
        else if (leaving)
        {
            d.stretched.clear();
            d.stretch.flush(d.stretched);
            d.stretched.insert(d.stretched.end(), samples.begin(), samples.end());
            samples = d.stretched;
        }
    }
    if (samples.size() > Impl::kRingSamples)
        samples = samples.last(Impl::kRingSamples);
    if (m_impl->queued + samples.size() > Impl::kRingSamples)
    {
        const size_t drop = std::min(m_impl->queued, std::max(m_impl->queued + samples.size() - Impl::kRingSamples,
                                                          m_impl->queued > 4096u ? m_impl->queued - 4096u : 0u));
        m_impl->read = (m_impl->read + drop) % Impl::kRingSamples;
        m_impl->queued -= drop;
        m_impl->droppedFrames += drop / 2u;
        m_impl->fadePosition = 0;
        m_impl->fadeSource = m_impl->lastOutput;
    }
    const size_t write = (m_impl->read + m_impl->queued) % Impl::kRingSamples;
    for (size_t i = 0u; i < samples.size(); ++i)
        m_impl->ring[(write + i) % Impl::kRingSamples] = samples[i];
    m_impl->queued += samples.size();
    if (m_impl->statsEnabled)
    {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - m_impl->statsStart).count();
        if (elapsed >= 2.0)
        {
            // Seconds since the first stats line's window began (the [audio] lines had no time of their own).
            static const auto statsEpoch = m_impl->statsStart;
            std::fprintf(stderr, "[audio] t=%.2f %.0f produced/s, %.0f requested/s, %.2f ms queued, %llu missing frames, %llu underruns, %llu dropped frames, %llu gaps > 50 ms (longest %.0f ms)\n",
                         std::chrono::duration<double>(now - statsEpoch).count(),
                         m_impl->producedFrames / elapsed, m_impl->requestedFrames / elapsed,
                         m_impl->queued / 96.0, static_cast<unsigned long long>(m_impl->missingFrames),
                         static_cast<unsigned long long>(m_impl->underruns), static_cast<unsigned long long>(m_impl->droppedFrames),
                         static_cast<unsigned long long>(m_impl->gaps), m_impl->longestGap * 1000.0);
            m_impl->gaps = 0u;
            m_impl->longestGap = 0.0;
            m_impl->producedFrames = 0u;
            m_impl->requestedFrames = 0u;
            m_impl->missingFrames = 0u;
            m_impl->underruns = 0u;
            m_impl->droppedFrames = 0u;
            m_impl->statsStart = now;
        }
    }
}

void PS2AudioBackend::onVagTransfer(const uint8_t *rdram, uint32_t srcAddr, uint32_t sizeBytes)
{
    if (!rdram || sizeBytes < 48)
        return;

    const uint32_t physAddr = srcAddr & PS2_RAM_MASK;
    if (physAddr + sizeBytes > PS2_RAM_SIZE)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(rdram + physAddr, sizeBytes, pcm, sampleRate))
        return;

    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = std::move(sample);
    m_mostRecentSampleKey = physAddr;
}

void PS2AudioBackend::onVagTransferFromBuffer(const uint8_t *data, uint32_t sizeBytes, uint32_t keyAddr)
{
    if (!data || sizeBytes < 48)
        return;

    std::vector<int16_t> pcm;
    uint32_t sampleRate = 44100;
    if (!ps2_vag::decode(data, sizeBytes, pcm, sampleRate))
        return;

    const uint32_t physAddr = keyAddr & PS2_RAM_MASK;
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample sample;
    sample.pcm = std::move(pcm);
    sample.sampleRate = sampleRate;
    m_sampleBank[physAddr] = sample;
    m_mostRecentSampleKey = physAddr;
    m_loadOrderSamples.push_back(std::move(sample));
    m_loadOrderSampleKeys.push_back(physAddr);
    constexpr size_t kMaxLoadOrderSamples = 32;
    if (m_loadOrderSamples.size() > kMaxLoadOrderSamples)
    {
        m_loadOrderSamples.erase(m_loadOrderSamples.begin());
        m_loadOrderSampleKeys.erase(m_loadOrderSampleKeys.begin());
    }
}

namespace
{
    constexpr uint32_t LIBSD_CMD_SET_VOICE = 0x8010u;
}

void PS2AudioBackend::onSoundCommand(uint32_t sid, uint32_t rpcNum,
                                     const uint8_t *sendBuf, uint32_t sendSize,
                                     uint8_t *recvBuf, uint32_t recvSize)
{
    if (sid != 0x80000701u)
        return;

    if ((rpcNum == LIBSD_CMD_SET_VOICE || (rpcNum & 0xFF00u) == 0x8100u) &&
        sendBuf && sendSize >= 20)
    {
        uint32_t sampleAddr = 0;
        uint32_t voiceIndex = 0xFFFFFFFFu;
        for (int vo = 4; vo >= 0 && voiceIndex == 0xFFFFFFFFu; vo -= 4)
        {
            if (vo < static_cast<int>(sendSize))
            {
                uint32_t v = 0;
                std::memcpy(&v, sendBuf + vo, sizeof(v));
                if (v < 24u)
                    voiceIndex = v;
            }
        }

        constexpr uint32_t kMinPlausibleAddr = 0x1000u;
        for (int off = 12; off <= 24 && sampleAddr == 0; off += 4)
        {
            if (sendSize >= static_cast<uint32_t>(off + 4))
            {
                uint32_t cand = 0;
                std::memcpy(&cand, sendBuf + off, sizeof(cand));
                if (cand >= kMinPlausibleAddr && (cand <= PS2_RAM_MASK || (cand & ~PS2_RAM_MASK) == 0))
                    sampleAddr = cand;
            }
        }
        if (sampleAddr == 0)
            sampleAddr = m_mostRecentSampleKey;

        float pitch = 1.0f;
        if (sendSize >= 12)
        {
            uint16_t pitchHalf = 0;
            std::memcpy(&pitchHalf, sendBuf + 8, sizeof(pitchHalf));
            if (pitchHalf != 0)
                pitch = 4096.0f / static_cast<float>(pitchHalf);
        }
        play(sampleAddr, pitch, 1.0f, voiceIndex);
    }
}

void PS2AudioBackend::play(uint32_t sampleAddr, float pitch, float volume, uint32_t voiceIndex)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    DecodedSample *sampleToPlay = nullptr;
    uint32_t sampleKey = 0;

    auto it = m_sampleBank.find(sampleAddr & PS2_RAM_MASK);
    if (it != m_sampleBank.end())
    {
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    else if (voiceIndex != 0xFFFFFFFFu &&
             voiceIndex < m_loadOrderSamples.size() &&
             voiceIndex < m_loadOrderSampleKeys.size())
    {
        sampleToPlay = &m_loadOrderSamples[voiceIndex];
        sampleKey = m_loadOrderSampleKeys[voiceIndex];
    }
    else
    {
        it = m_sampleBank.find(m_mostRecentSampleKey);
        if (it == m_sampleBank.end())
            return;
        sampleToPlay = &it->second;
        sampleKey = it->first;
    }
    if (!sampleToPlay || sampleToPlay->pcm.empty())
        return;

    const bool isBgm = (sampleToPlay->pcm.size() > static_cast<size_t>(sampleToPlay->sampleRate * 5));
    playDecodedSample(sampleKey, *sampleToPlay, pitch, volume, isBgm);
}

void PS2AudioBackend::pruneFinishedSounds()
{
#if defined(PLATFORM_VITA)
    return;
#else
    auto &sounds = m_impl->activeSounds;
    auto it = sounds.begin();
    while (it != sounds.end())
    {
        if (!IsSoundPlaying(it->snd))
        {
            UnloadSound(it->snd);
            it = sounds.erase(it);
        }
        else
        {
            ++it;
        }
    }
#endif
}

void PS2AudioBackend::playDecodedSample(uint32_t sampleKey, DecodedSample &sample, float pitch, float volume,
                                        bool isBgm)
{
#if defined(PLATFORM_VITA)
    (void)sampleKey;
    (void)sample;
    (void)pitch;
    (void)volume;
    (void)isBgm;
    return;
#else
    if (!m_audioReady || sample.pcm.empty())
        return;

    pruneFinishedSounds();

    for (const auto &t : m_impl->activeSounds)
    {
        if (t.sampleKey == sampleKey && IsSoundPlaying(t.snd))
            return;
    }

    auto &sounds = m_impl->activeSounds;
    if (isBgm)
    {
        for (auto it = sounds.begin(); it != sounds.end();)
        {
            if (IsSoundPlaying(it->snd))
            {
                StopSound(it->snd);
                UnloadSound(it->snd);
                it = sounds.erase(it);
            }
            else
                ++it;
        }
    }

    constexpr int kMaxConcurrentSounds = 4;
    while (static_cast<int>(sounds.size()) >= kMaxConcurrentSounds)
    {
        StopSound(sounds.front().snd);
        UnloadSound(sounds.front().snd);
        sounds.erase(sounds.begin());
    }

    std::vector<uint8_t> wav = buildWavFromPcm(sample.pcm.data(), sample.pcm.size(), sample.sampleRate);
    Wave wave = LoadWaveFromMemory(".wav", wav.data(), static_cast<int>(wav.size()));
    if (wave.frameCount <= 0)
        return;
    Sound snd = LoadSoundFromWave(wave);
    UnloadWave(wave);
    SetSoundPitch(snd, pitch);
    SetSoundVolume(snd, volume);
    m_impl->activeSounds.push_back({snd, sampleKey});
    PlaySound(snd);
#endif
}

void PS2AudioBackend::stop(uint32_t voiceId)
{
    (void)voiceId;
}

void PS2AudioBackend::stopAll()
{
    std::lock_guard<std::mutex> lock(m_mutex);
#if defined(PLATFORM_VITA)
    return;
#else
    for (auto &t : m_impl->activeSounds)
    {
        StopSound(t.snd);
        UnloadSound(t.snd);
    }
    m_impl->activeSounds.clear();
#endif
}

// The F2 menu's mute (ps2_time_control.h): the stream's master volume. Opening the device resets the volume, so
// setAudioReady applies PS2X_AUDIO_MUTE through here after it is open; before that only the flag is kept.
namespace
{
    std::atomic<bool> g_audioMuted{false};
}

void ps2x::timecontrol::setMuted(bool muted)
{
    g_audioMuted.store(muted, std::memory_order_relaxed);
#if !defined(PLATFORM_VITA)
    if (IsAudioDeviceReady())
        SetMasterVolume(muted ? 0.0f : 1.0f);
#endif
}

bool ps2x::timecontrol::muted()
{
    return g_audioMuted.load(std::memory_order_relaxed);
}
