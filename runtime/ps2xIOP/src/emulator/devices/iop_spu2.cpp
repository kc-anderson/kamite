#include "iop_spu2.h"
#include "../core/iop_cpu.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ps2x::iop::detail
{
    extern const IopCpuState *g_iopLogCpu; // iop_emulator.cpp (diagnostics only)

    namespace
    {
        constexpr uint32_t kCoreStride = 0x400u;
        constexpr uint32_t kSharedBase = 0x760u;
        constexpr uint32_t kVoiceParamsEnd = 0x180u;
        constexpr uint32_t kVoiceAddressBase = 0x1C0u;
        constexpr uint32_t kVoiceAddressStride = 0xCu;
        constexpr uint32_t kVoiceAddressEnd = kVoiceAddressBase + IopSpu2::kVoices * kVoiceAddressStride;
        constexpr uint32_t kKeyOn0 = 0x1A0u;
        constexpr uint32_t kKeyOn1 = 0x1A2u;
        constexpr uint32_t kKeyOff0 = 0x1A4u;
        constexpr uint32_t kKeyOff1 = 0x1A6u;
        constexpr uint32_t kTransferAddressHigh = 0x1A8u;
        constexpr uint32_t kTransferAddressLow = 0x1AAu;
        constexpr uint32_t kTransferData = 0x1ACu;
        constexpr uint32_t kAutoDma = 0x1B0u;
        constexpr uint32_t kEndFlags0 = 0x340u;
        constexpr uint32_t kEndFlags1 = 0x342u;
        constexpr uint32_t kStatus = 0x344u;

        constexpr uint32_t kParamPitch = 2u;
        constexpr uint32_t kParamAdsr1 = 3u;
        constexpr uint32_t kParamAdsr2 = 4u;
        constexpr uint32_t kParamEnvelope = 5u;

        constexpr uint32_t kAddressMask = 0xFFFFFu;
        constexpr uint8_t kLoopEnd = 0x1u;
        constexpr uint8_t kLoopRepeat = 0x2u;
        constexpr uint8_t kLoopStart = 0x4u;
        constexpr uint32_t kSamplesPerBlock = 28u;
        constexpr int32_t kPitchStep = 4096;
        constexpr int32_t kMaxPitch = 0x3FFF;
        constexpr int64_t kEnvelopeMax = 0x7FFFFFFF;
        constexpr uint16_t kStatusDmaReady = 0x80u;

        constexpr uint8_t kPhaseOff = 0u;
        constexpr uint8_t kPhaseAttack = 1u;
        constexpr uint8_t kPhaseDecay = 2u;
        constexpr uint8_t kPhaseSustain = 3u;
        constexpr uint8_t kPhaseSustainEnd = 4u;
        constexpr uint8_t kPhaseRelease = 5u;

        constexpr std::array<int32_t, 8> kInverseExpOffsets{0, 4, 6, 8, 9, 10, 11, 12};

        // PS2X_AUDIO_OFF=1: skip all SPU2 mixing (performance test; the game runs silent).
        bool audioDisabled()
        {
            static const bool disabled = [] { const char *v = std::getenv("PS2X_AUDIO_OFF"); return v && *v && *v != '0'; }();
            return disabled;
        }

        int64_t envelopeRate(int32_t index)
        {
            const int32_t shift = index >> 2;
            int64_t rate = (index & 3) + 4;
            if (shift < 0)
                rate >>= std::min(-shift, 62);
            else
                rate <<= std::min(shift, 40);
            return std::min<int64_t>(rate, 0x3FFFFFFF);
        }

        int32_t volume(uint16_t value)
        {
            if (value & 0x8000u)
                return 0;
            return static_cast<int16_t>(value << 1u);
        }

        int32_t scale(int32_t sample, int32_t gain)
        {
            return static_cast<int32_t>((static_cast<int64_t>(sample) * gain) >> 15);
        }

        int32_t saturate(int32_t sample)
        {
            return std::clamp(sample, -32768, 32767);
        }

        const std::array<std::array<int16_t, 4>, 256> &interpolationWeights()
        {
            static const auto weights = []
            {
                std::array<double, 512> kernel{};
                double total = 0.0;
                for (size_t index = 0u; index < kernel.size(); ++index)
                {
                    const double position = static_cast<double>(index) + 0.5;
                    const double window = 1.0 + (std::cos(std::numbers::pi * position / 511.5) - 1.0) * 0.5
                                              + (std::cos(std::numbers::pi * position / 255.75) - 1.0) * 0.08;
                    const double value = std::sin(std::numbers::pi * position / 500.0) * window / position;
                    kernel[511u - index] = value;
                    total += value;
                }
                const double normalization = 32640.0 * 128.0 / total;
                std::array<std::array<int16_t, 4>, 256> result{};
                for (size_t phase = 0u; phase < result.size(); ++phase)
                {
                    const std::array<size_t, 4> indices{255u - phase, 511u - phase, phase + 256u, phase};
                    double sum = 0.0;
                    for (const size_t index : indices)
                        sum += kernel[index] * normalization;
                    const double correction = (sum - 32640.0) / 4.0;
                    for (size_t tap = 0u; tap < indices.size(); ++tap)
                        result[phase][tap] = static_cast<int16_t>(std::lround(kernel[indices[tap]] * normalization - correction));
                }
                return result;
            }();
            return weights;
        }
    }

    IopSpu2::IopSpu2()
        : m_ram(kRamHalfwords)
    {
        reset();
    }

    namespace
    {
        uint32_t g_statKeyOn = 0u;
        uint32_t g_statAdma = 0u;
        uint32_t g_statNonZero = 0u;
        uint32_t g_statVoiceNonZero = 0u;
        uint32_t g_statReverbNonZero = 0u;
        // Samples per core for which the streamed (ADMA) block was late and the last sample was held.
        uint32_t g_admaLateRun[2] = {};

        // When an ADMA block's data is read from IOP RAM (docs/PROGRESS.md §94). The sound driver (the CRI library)
        // refills the buffer right behind the hardware, so the moment matters: the SPU2 has a two-half input buffer and
        // the DMA moves each block in when the previous one starts playing, one block (256 samples) ahead.
        // PS2X_ADMA_COPY: ahead (default, as the hardware), all (the whole transfer when it starts: the second block came
        // two blocks early), 0 (when it starts playing: one block late). Read early or late, the decoder's write and
        // the read could fall into a phase where the right half of each block mixed old and new samples (a hiss on the
        // right channel), or the right channel came from another moment (FMV speech heard on the right, then left).
        enum class AdmaRead
        {
            Late,
            All,
            Ahead
        };

        AdmaRead admaReadMode()
        {
            static const AdmaRead mode = []
            {
                const char *v = std::getenv("PS2X_ADMA_COPY");
                if (v && *v == '0')
                    return AdmaRead::Late;
                if (v && (std::strcmp(v, "all") == 0 || std::strcmp(v, "1") == 0))
                    return AdmaRead::All;
                return AdmaRead::Ahead;
            }();
            return mode;
        }

        struct StagedAdmaBlock
        {
            const uint8_t *source = nullptr;
            size_t position = 0u;
            bool valid = false;
            std::array<uint8_t, 0x400> data{};
        };
        std::array<StagedAdmaBlock, 2> g_stagedAdma;

        void stageAdmaBlock(uint32_t core, const uint8_t *source, size_t position)
        {
            StagedAdmaBlock &staged = g_stagedAdma[core & 1u];
            std::memcpy(staged.data.data(), source + position, staged.data.size());
            staged.source = source;
            staged.position = position;
            staged.valid = true;
        }

        const uint8_t *stagedAdmaBlock(uint32_t core, const uint8_t *source, size_t position)
        {
            StagedAdmaBlock &staged = g_stagedAdma[core & 1u];
            if (!staged.valid || staged.source != source || staged.position != position)
                return nullptr;
            staged.valid = false;
            return staged.data.data();
        }
    }

    void IopSpu2::reset()
    {
        m_regs.fill(0u);
        std::fill(m_ram.begin(), m_ram.end(), uint16_t{0});
        for (auto &core : m_voices)
            core.fill(Voice{});
        m_endx.fill(0u);
        m_transferAddress.fill(0u);
        m_adma = {};
        m_cycle = 0u;
        for (auto &core : m_decoders)
            core.fill(Decoder{});
        m_interpolation = {};
        m_audioFrames = 0u;
    }

    uint16_t IopSpu2::read16(uint32_t physicalAddress, uint64_t cycle)
    {
        advanceTo(cycle);
        const uint32_t offset = (physicalAddress - kBase) & 0x7FEu;
        if (offset >= kSharedBase)
            return reg(offset);
        const uint32_t core = offset / kCoreStride;
        const uint32_t local = offset % kCoreStride;
        if (local < kVoiceParamsEnd)
        {
            const uint32_t voice = local >> 4u;
            if (((local & 0xFu) >> 1u) == kParamEnvelope && voice < kVoices)
                return static_cast<uint16_t>(std::max(m_voices[core][voice].envelope, 0) >> 16);
            return reg(offset);
        }
        if (local >= kVoiceAddressBase && local < kVoiceAddressEnd)
        {
            const Voice &voice = m_voices[core][(local - kVoiceAddressBase) / kVoiceAddressStride];
            switch (((local - kVoiceAddressBase) % kVoiceAddressStride) >> 1u)
            {
            case 0:
                return static_cast<uint16_t>(voice.startA >> 16);
            case 1:
                return static_cast<uint16_t>(voice.startA);
            case 2:
                return static_cast<uint16_t>(voice.loopStartA >> 16);
            case 3:
                return static_cast<uint16_t>(voice.loopStartA);
            case 4:
                return static_cast<uint16_t>(voice.nextA >> 16);
            default:
                return static_cast<uint16_t>(voice.nextA);
            }
        }
        switch (local)
        {
        case kTransferAddressHigh:
            return static_cast<uint16_t>(m_transferAddress[core] >> 16);
        case kTransferAddressLow:
            return static_cast<uint16_t>(m_transferAddress[core]);
        case kEndFlags0:
            return static_cast<uint16_t>(m_endx[core]);
        case kEndFlags1:
            return static_cast<uint16_t>(m_endx[core] >> 16);
        default:
            return reg(offset);
        }
    }

    void IopSpu2::write16(uint32_t physicalAddress, uint16_t value, uint64_t cycle)
    {
        advanceTo(cycle);
        const uint32_t offset = (physicalAddress - kBase) & 0x7FEu;
        static uint32_t admasLogLeft = [] { const char *v = std::getenv("PS2X_SPU2_DMA_LOG"); return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u; }();
        if (admasLogLeft != 0u && (offset == kAutoDma || offset == kCoreStride + kAutoDma) && reg(offset) != value)
        {
            --admasLogLeft;
            std::fprintf(stderr, "[spu2-dma] core %u ADMAS %04x -> %04x cycle %llu iop pc=%06x ra=%06x\n", offset >= kCoreStride ? 1u : 0u,
                         reg(offset), value, static_cast<unsigned long long>(cycle), g_iopLogCpu ? g_iopLogCpu->pc : 0u,
                         g_iopLogCpu ? g_iopLogCpu->gpr[31] : 0u);
        }
        reg(offset) = value;
        if (offset >= kSharedBase)
        {
            for (uint32_t sharedCore = 0u; sharedCore < kCores; ++sharedCore)
            {
                const uint32_t shared = kSharedBase + sharedCore * 0x28u;
                if (offset == shared || offset == shared + 2u)
                    reg(offset + 0x10u) = static_cast<uint16_t>(volume(value));
            }
            return;
        }
        const uint32_t core = offset / kCoreStride;
        const uint32_t local = offset % kCoreStride;
        if (local < kVoiceParamsEnd)
        {
            const uint32_t voice = local >> 4u;
            if ((local & 0xFu) <= 2u && voice < kVoices)
                reg(offset + 0xCu) = static_cast<uint16_t>(volume(value));
            if (((local & 0xFu) >> 1u) == kParamEnvelope && voice < kVoices)
                m_voices[core][voice].envelope = static_cast<int32_t>((static_cast<uint32_t>(value) << 16) | value) & 0x7FFFFFFF;
            return;
        }
        if (local >= kVoiceAddressBase && local < kVoiceAddressEnd)
        {
            Voice &voice = m_voices[core][(local - kVoiceAddressBase) / kVoiceAddressStride];
            const uint32_t high = (static_cast<uint32_t>(value) & 0xFu) << 16;
            switch (((local - kVoiceAddressBase) % kVoiceAddressStride) >> 1u)
            {
            case 0:
                voice.startA = high | (voice.startA & 0xFFF8u);
                break;
            case 1:
                voice.startA = (voice.startA & 0xF0000u) | (value & 0xFFF8u);
                break;
            case 2:
                voice.loopStartA = high | (voice.loopStartA & 0xFFF8u);
                voice.manualLoop = true;
                break;
            case 3:
                voice.loopStartA = (voice.loopStartA & 0xF0000u) | (value & 0xFFF8u);
                voice.manualLoop = true;
                break;
            case 4:
                voice.nextA = high | (voice.nextA & 0xFFFFu);
                voice.sampleIndex = kSamplesPerBlock;
                break;
            default:
                voice.nextA = (voice.nextA & 0xF0000u) | value;
                voice.sampleIndex = kSamplesPerBlock;
                break;
            }
            return;
        }
        switch (local)
        {
        case kKeyOn0:
        case kKeyOn1:
        {
            const uint32_t first = local == kKeyOn0 ? 0u : 16u;
            for (uint32_t bit = 0; bit < 16u && first + bit < kVoices; ++bit)
                if ((value >> bit) & 1u)
                    keyOn(core, first + bit);
            break;
        }
        case kKeyOff0:
        case kKeyOff1:
        {
            const uint32_t first = local == kKeyOff0 ? 0u : 16u;
            for (uint32_t bit = 0; bit < 16u && first + bit < kVoices; ++bit)
                if ((value >> bit) & 1u)
                    keyOff(core, first + bit);
            break;
        }
        case kTransferAddressHigh:
            m_transferAddress[core] = ((static_cast<uint32_t>(value) & 0xFu) << 16) | (m_transferAddress[core] & 0xFFFFu);
            break;
        case kTransferAddressLow:
            m_transferAddress[core] = (m_transferAddress[core] & 0xF0000u) | value;
            break;
        case kTransferData:
            m_ram[m_transferAddress[core]] = value;
            m_transferAddress[core] = (m_transferAddress[core] + 1u) & kAddressMask;
            break;
        case kEndFlags0:
            m_endx[core] &= 0xFF0000u;
            break;
        case kEndFlags1:
            m_endx[core] &= 0xFFFFu;
            break;
        default:
            break;
        }
    }

    bool IopSpu2::autoDma(uint32_t core) const noexcept
    {
        return (reg(core * kCoreStride + kAutoDma) & (core + 1u)) != 0u;
    }

    void IopSpu2::dmaStarted(uint32_t core) noexcept
    {
        reg(core * kCoreStride + kStatus) |= kStatusDmaReady;
    }

    void IopSpu2::admaStart(uint32_t core, const uint8_t *source, size_t bytes, uint64_t cycle)
    {
        advanceTo(cycle);
        Adma &adma = m_adma[core & 1u];
        ++g_statAdma;
        static uint32_t logLeft = []
        {
            const char *value = std::getenv("PS2X_SPU2_LOG");
            return value ? static_cast<uint32_t>(std::strtoul(value, nullptr, 10)) : 0u;
        }();
        if (logLeft != 0u)
        {
            --logLeft;
            std::fprintf(stderr, "[spu2] ADMA core %u start %zu bytes (left in previous: %zu) cycle %llu\n", core & 1u, bytes,
                         adma.bytes > adma.position ? adma.bytes - adma.position : 0u, static_cast<unsigned long long>(cycle));
        }
        adma.source = source;
        adma.bytes = bytes & ~static_cast<size_t>(0x3FFu);
        adma.position = 0u;
        const AdmaRead mode = admaReadMode();
        if (mode == AdmaRead::All && source && adma.bytes != 0u)
        {
            static std::array<std::vector<uint8_t>, kCores> copies;
            std::vector<uint8_t> &buffer = copies[core & 1u];
            buffer.assign(source, source + adma.bytes);
            adma.source = buffer.data();
        }
        else if (mode == AdmaRead::Ahead && source && adma.bytes != 0u)
        {
            stageAdmaBlock(core & 1u, source, 0u); // the SPU2 has a free half: the first block moves now
        }
        // Keep playing the current block; the new transfer continues where it ends (no gap at buffer swaps).
    }

    std::array<int32_t, 2> IopSpu2::admaSample(uint32_t core)
    {
        Adma &adma = m_adma[core];
        if (adma.index >= 256u)
        {
            if (!adma.source || adma.position + 0x400u > adma.bytes)
            {
                // The next block is late (in the FMVs, by about one sample every 5120: the CRI feed and this clock
                // drift apart slightly). A zero here was a click in the middle of the sound (-2208, 0, -2255); the
                // hardware keeps playing what is in its buffer, so hold the last sample of the previous block instead
                // (docs/PROGRESS.md §92). PS2X_ADMA_HOLD=0 goes back to silence.
                static const bool hold = [] { const char *v = std::getenv("PS2X_ADMA_HOLD"); return !(v && *v == '0'); }();
                if (autoDma(core))
                    ++g_admaLateRun[core & 1u];
                if (hold && adma.source)
                    return {adma.left[255], adma.right[255]};
                return {0, 0};
            }
            // 2026-10-19 (user: music "cut out or crackled" in stage 3, no host-side gap): log streamed
            // blocks that came 1 ms or more late; until then the last sample was held, heard as a cut-out or click.
            if (const uint32_t late = g_admaLateRun[core & 1u]; late != 0u)
            {
                g_admaLateRun[core & 1u] = 0u;
                static uint32_t logsLeft = 40u;
                if (late >= 48u && logsLeft != 0u)
                {
                    --logsLeft;
                    std::fprintf(stderr, "[spu2-adma] core %u stream block %u samples (%.1f ms) late at t=%.3f s; last sample held\n",
                                 core, late, static_cast<double>(late) / 48.0, static_cast<double>(m_cycle) / (768.0 * 48000.0));
                }
            }
            const uint8_t *block = adma.source + adma.position;
            if (const uint8_t *staged = stagedAdmaBlock(core, adma.source, adma.position))
                block = staged;
            std::memcpy(adma.left.data(), block, 0x200u);
            std::memcpy(adma.right.data(), block + 0x200u, 0x200u);
            reportAdmaBlock(core & 1u, adma.left.data(), adma.right.data());
            // PS2X_ADMA_DUMP=<file>: every ADMA block as it is played, s16le stereo per core (core in the low bit of
            // a 4-byte header per block: 'A', core, 0, 0, then 256 left/right sample pairs).
            static std::FILE *admaDump = []() -> std::FILE *
            {
                const char *path = std::getenv("PS2X_ADMA_DUMP");
                return path && *path ? std::fopen(path, "wb") : nullptr;
            }();
            if (admaDump)
            {
                const uint8_t header[4] = {'A', static_cast<uint8_t>(core), 0u, 0u};
                std::fwrite(header, 1u, sizeof(header), admaDump);
                for (uint32_t i = 0; i < 256u; ++i)
                {
                    const int16_t pair[2] = {adma.left[i], adma.right[i]};
                    std::fwrite(pair, sizeof(int16_t), 2u, admaDump);
                }
            }
            adma.position += 0x400u;
            adma.index = 0u;
            // This block starts playing and frees the other half of the SPU2's ADMA buffer: the hardware DMA moves the
            // next block of the transfer into it now, one block (256 samples) ahead of playing it.
            if (admaReadMode() == AdmaRead::Ahead && adma.position + 0x400u <= adma.bytes)
                stageAdmaBlock(core, adma.source, adma.position);
        }
        const uint32_t i = adma.index++;
        return {adma.left[i], adma.right[i]};
    }

    void IopSpu2::dmaWrite(uint32_t core, const uint8_t *source, size_t bytes, uint64_t cycle)
    {
        advanceTo(cycle);
        uint32_t address = m_transferAddress[core];
        for (size_t i = 0; i + 2u <= bytes; i += 2u)
        {
            m_ram[address] = static_cast<uint16_t>(source[i] | (source[i + 1u] << 8));
            address = (address + 1u) & kAddressMask;
        }
        m_transferAddress[core] = address;
    }

    void IopSpu2::dmaRead(uint32_t core, uint8_t *destination, size_t bytes, uint64_t cycle)
    {
        advanceTo(cycle);
        uint32_t address = m_transferAddress[core];
        for (size_t i = 0; i + 2u <= bytes; i += 2u)
        {
            destination[i] = static_cast<uint8_t>(m_ram[address]);
            destination[i + 1u] = static_cast<uint8_t>(m_ram[address] >> 8);
            address = (address + 1u) & kAddressMask;
        }
        m_transferAddress[core] = address;
    }

    void IopSpu2::advanceTo(uint64_t cycle)
    {
        if (cycle <= m_cycle)
            return;
        const uint64_t samples = (cycle - m_cycle) / kCyclesPerSample;
        if (samples == 0u)
            return;
        m_cycle += samples * kCyclesPerSample;
        if (audioDisabled())
        {
            // No mixing: only drain the ADMA stream at the real rate so the sound driver keeps refilling it.
            for (uint64_t sample = 0; sample < samples; ++sample)
            {
                admaSample(0u);
                admaSample(1u);
            }
            return;
        }
        static const bool statusLog = [] { const char *v = std::getenv("PS2X_SPU2_LOG"); return v && *v && *v != '0'; }();
        for (uint64_t sample = 0; sample < samples; ++sample)
        {
            // Every 5 s of SPU2 time (a plain sample counter: the old test on m_cycle, already advanced past the whole
            // batch, could skip a window and fold two into the next, showing ADMA at twice its real rate).
            static uint32_t logSamples = 0u;
            if (statusLog && ++logSamples >= 48000u * 5u && (logSamples = 0u, true))
            {
                for (uint32_t core = 0u; core < kCores; ++core)
                {
                    const uint32_t base = core * kCoreStride;
                    const uint32_t shared = kSharedBase + core * 0x28u;
                    std::fprintf(stderr, "[spu2] core%u MMIX=%04x ATTR=%04x MVOL=%04x/%04x AVOL=%04x/%04x BVOL=%04x/%04x ADMA=%u\n",
                                 core, reg(base + 0x198u), reg(base + 0x19Au), reg(shared), reg(shared + 2u), reg(shared + 8u),
                                 reg(shared + 10u), reg(shared + 12u), reg(shared + 14u), autoDma(core) ? 1u : 0u);
                }
                const size_t ramNonZero =
                    static_cast<size_t>(std::count_if(m_ram.begin(), m_ram.end(), [](uint16_t v) { return v != 0u; }));
                std::fprintf(stderr, "[spu2] last 5 s: keyon=%u adma=%u voiceSamples!=0: %u output!=0: %u reverb!=0: %u | SPU2 RAM non-zero halfwords: %zu\n",
                             g_statKeyOn, g_statAdma, g_statVoiceNonZero, g_statNonZero, g_statReverbNonZero, ramNonZero);
                g_statKeyOn = g_statAdma = g_statNonZero = g_statVoiceNonZero = g_statReverbNonZero = 0u;
            }
            const auto core0 = mixCore(0u, {});
            const auto output = mixCore(1u, core0);
            if (output[0] != 0 || output[1] != 0)
                ++g_statNonZero;
            reportSilentDrop(core0, output);
            m_audioBuffer[m_audioFrames * 2u] = static_cast<int16_t>(output[0]);
            m_audioBuffer[m_audioFrames * 2u + 1u] = static_cast<int16_t>(output[1]);
            if (++m_audioFrames == m_audioBuffer.size() / 2u)
                flushAudio();
        }
    }

    // 2026-10-18 diagnosis: the whole mix went exactly silent for 8-10 ms about every 0.4 s in stage 2 while
    // The game was loud around it. When the output drops to exact zero right after loud sound, log (rate-limited) what
    // the mixer saw: ATTR/MMIX per core, MVOL/AVOL/BVOL/EVOL as written, core 0's output and how many voices sound.
    void IopSpu2::reportSilentDrop(const std::array<int32_t, 2> &core0, const std::array<int32_t, 2> &output)
    {
        static uint64_t loudUntil = 0u; // sample index until which the recent output counts as loud
        static uint64_t nextLog = 0u;
        static uint32_t logsLeft = 12u;
        static uint64_t sampleIndex = 0u;
        ++sampleIndex;
        const int32_t peak = std::max(std::abs(output[0]), std::abs(output[1]));
        if (peak >= 1500)
        {
            loudUntil = sampleIndex + 96u; // within 2 ms of loud output
            return;
        }
        if (output[0] != 0 || output[1] != 0 || sampleIndex > loudUntil || logsLeft == 0u || sampleIndex < nextLog)
            return;
        --logsLeft;
        nextLog = sampleIndex + 48000u / 2u;
        loudUntil = 0u;
        uint32_t sounding[kCores] = {};
        for (uint32_t core = 0u; core < kCores; ++core)
            for (uint32_t voice = 0u; voice < kVoices; ++voice)
                if (m_voices[core][voice].phase != 0u && m_voices[core][voice].envelope > 0)
                    ++sounding[core];
        const uint32_t s0 = kSharedBase;
        const uint32_t s1 = kSharedBase + 0x28u;
        std::fprintf(stderr,
                     "[spu2-silence] output dropped to exact 0 after loud sound (t=%.3f s): ATTR %04x/%04x MMIX %04x/%04x "
                     "MVOL %04x,%04x/%04x,%04x EVOL %04x,%04x/%04x,%04x AVOL1 %04x,%04x BVOL1 %04x,%04x core0 out %d,%d voices %u/%u ADMA %u/%u\n",
                     static_cast<double>(m_cycle) / (768.0 * 48000.0), reg(0x19Au), reg(kCoreStride + 0x19Au), reg(0x198u),
                     reg(kCoreStride + 0x198u), reg(s0), reg(s0 + 2u), reg(s1), reg(s1 + 2u), reg(s0 + 4u), reg(s0 + 6u),
                     reg(s1 + 4u), reg(s1 + 6u), reg(s1 + 8u), reg(s1 + 10u), reg(s1 + 12u), reg(s1 + 14u), core0[0], core0[1],
                     sounding[0], sounding[1], autoDma(0u) ? 1u : 0u, autoDma(1u) ? 1u : 0u);
    }

    // 2026-10-19: the mix dropped to exact 0 after loud sound with only the core 1 stream (ADMA) playing and
    // no block late, so the stream itself delivered silence. Log what the stream gives: runs of all-zero blocks right
    // after loud ones (with their length; seconds = a track change), and non-zero blocks equal to one of the last 64
    // (a stale buffer played again, heard as a stutter or crackle). At most 60 lines.
    void IopSpu2::reportAdmaBlock(uint32_t core, const int16_t *left, const int16_t *right)
    {
        static uint32_t logsLeft = 60u;
        static uint32_t zeroRun[kCores] = {};
        static bool loud[kCores] = {};
        static std::array<std::array<uint64_t, 64>, kCores> recent{};
        static uint32_t recentNext[kCores] = {};
        const double seconds = static_cast<double>(m_cycle) / (768.0 * 48000.0);
        int32_t peak = 0;
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < 256u; ++i)
        {
            peak = std::max({peak, std::abs(static_cast<int32_t>(left[i])), std::abs(static_cast<int32_t>(right[i]))});
            hash = (hash ^ static_cast<uint16_t>(left[i])) * 1099511628211ull;
            hash = (hash ^ static_cast<uint16_t>(right[i])) * 1099511628211ull;
        }
        if (peak == 0)
        {
            if (loud[core])
                ++zeroRun[core];
            return;
        }
        if (zeroRun[core] != 0u && logsLeft != 0u)
        {
            --logsLeft;
            std::fprintf(stderr, "[spu2-adma] core %u stream gave %u silent blocks (%.1f ms) after loud audio, ending at t=%.3f s\n", core,
                         zeroRun[core], static_cast<double>(zeroRun[core]) * 256.0 / 48.0, seconds);
        }
        zeroRun[core] = 0u;
        loud[core] = peak >= 1500;
        if (peak >= 64 && logsLeft != 0u &&
            std::find(recent[core].begin(), recent[core].end(), hash) != recent[core].end())
        {
            --logsLeft;
            std::fprintf(stderr, "[spu2-adma] core %u stream block repeats one of the last 64 (peak %d) at t=%.3f s\n", core, peak,
                         seconds);
        }
        recent[core][recentNext[core]++ & 63u] = hash;
    }

    void IopSpu2::flushAudio()
    {
        // PS2X_AUDIO_DUMP=<file>: raw PCM, s16le stereo 48 kHz, of everything the SPU2 outputs (offline analysis).
        static std::FILE *dump = []() -> std::FILE *
        {
            const char *path = std::getenv("PS2X_AUDIO_DUMP");
            return path && *path ? std::fopen(path, "wb") : nullptr;
        }();
        if (dump && m_audioFrames != 0u)
        {
            std::fwrite(m_audioBuffer.data(), sizeof(int16_t), m_audioFrames * 2u, dump);
            std::fflush(dump);
        }
        if (m_audioFrames != 0u && m_audioSink)
            m_audioSink(std::span<const int16_t>(m_audioBuffer.data(), m_audioFrames * 2u));
        m_audioFrames = 0u;
    }

    std::array<int32_t, 2> IopSpu2::mixCore(uint32_t core, const std::array<int32_t, 2> &external)
    {
        const uint32_t base = core * kCoreStride;
        const uint32_t leftMask = reg(base + 0x188u) | (static_cast<uint32_t>(reg(base + 0x18Au)) << 16u);
        const uint32_t leftWetMask = reg(base + 0x18Cu) | (static_cast<uint32_t>(reg(base + 0x18Eu)) << 16u);
        const uint32_t rightMask = reg(base + 0x190u) | (static_cast<uint32_t>(reg(base + 0x192u)) << 16u);
        const uint32_t rightWetMask = reg(base + 0x194u) | (static_cast<uint32_t>(reg(base + 0x196u)) << 16u);
        std::array<int32_t, 2> output{};
        std::array<int32_t, 2> voiceWet{};
        for (uint32_t index = 0u; index < kVoices; ++index)
        {
            const int32_t sample = runVoice(core, index);
            if (sample == 0)
                continue;
            ++g_statVoiceNonZero;
            const uint32_t params = base + index * 0x10u;
            const int32_t left = scale(sample, volume(reg(params)));
            const int32_t right = scale(sample, volume(reg(params + 2u)));
            if (leftMask & (1u << index))
                output[0] += left;
            if (rightMask & (1u << index))
                output[1] += right;
            if (leftWetMask & (1u << index))
                voiceWet[0] += left;
            if (rightWetMask & (1u << index))
                voiceWet[1] += right;
        }
        const uint16_t mixer = reg(base + 0x198u);
        const uint32_t shared = kSharedBase + core * 0x28u;
        // MMIX (PCSX2 REG_S_MMIX, psx-spx): 0x800/0x400 voice dry L/R, 0x200/0x100 voice wet, 0x80/0x40 sound data
        // input (ADMA) dry, 0x20/0x10 its wet, 0x08/0x04 external input dry (core 1: the output of core 0), 0x02/0x01
        // its wet. AVOL scales the external input, BVOL the ADMA input, EVOL the reverb output.
        const std::array<int32_t, 2> input = admaSample(core);
        std::array<int32_t, 2> dry{};
        std::array<int32_t, 2> wet{};
        for (uint32_t channel = 0u; channel < 2u; ++channel)
        {
            const int32_t adma = scale(input[channel], static_cast<int16_t>(reg(shared + 0x0Cu + channel * 2u)));
            const int32_t ext = scale(external[channel], static_cast<int16_t>(reg(shared + 8u + channel * 2u)));
            if (mixer & (0x800u >> channel))
                dry[channel] += saturate(output[channel]);
            if (mixer & (0x80u >> channel))
                dry[channel] += adma;
            if (mixer & (0x08u >> channel))
                dry[channel] += ext;
            if (mixer & (0x200u >> channel))
                wet[channel] += saturate(voiceWet[channel]);
            if (mixer & (0x20u >> channel))
                wet[channel] += adma;
            if (mixer & (0x02u >> channel))
                wet[channel] += ext;
            wet[channel] = saturate(wet[channel]);
        }
        static const bool reverbEnabled = [] { const char *v = std::getenv("PS2X_SPU2_REVERB"); return !v || *v != '0'; }();
        const std::array<int32_t, 2> reverb = reverbEnabled ? reverbFrame(core, wet) : std::array<int32_t, 2>{};
        if (reverb[0] != 0 || reverb[1] != 0)
            ++g_statReverbNonZero;
        for (uint32_t channel = 0u; channel < 2u; ++channel)
        {
            const int32_t mixed =
                saturate(dry[channel]) + scale(reverb[channel], static_cast<int16_t>(reg(shared + 4u + channel * 2u)));
            const int32_t master = volume(reg(shared + channel * 2u));
            output[channel] = scale(saturate(mixed), master);
        }
        // ATTR bit 14 ("mute"). 2026-10-18: the whole mix went digitally silent for 8-10 ms about every 0.4 s
        // in stage 2 fights and the God Reel menu (logs/audio_out_last.pcm, the user's "sound cuts out"). Exact zero
        // under a reverb tail can only come from this final gate (or MVOL 0). PCSX2 ignores the bit (spu2sys.cpp
        // REG_C_ATTR: "thiscore.Mute = 0"), because drivers rewrite ATTR around their transfers. Ignored here too;
        // PS2X_SPU2_ATTR_MUTE=1 brings the gate back. The first times it would have muted are logged.
        if (!(reg(base + 0x19Au) & 0x4000u) && (output[0] != 0 || output[1] != 0))
        {
            static const bool honourMute = [] { const char *v = std::getenv("PS2X_SPU2_ATTR_MUTE"); return v && *v == '1'; }();
            static uint32_t logsLeft = 6u;
            static uint64_t nextLog = 0u; // in 48 kHz samples (768 IOP cycles each): at most one line per 0.25 s
            const uint64_t now = m_cycle / 768u;
            if (logsLeft != 0u && now >= nextLog)
            {
                --logsLeft;
                nextLog = now + 48000u / 4u;
                std::fprintf(stderr, "[spu2] core%u ATTR=%04x has the mute bit (14) clear while sounding: %s\n", core,
                             reg(base + 0x19Au), honourMute ? "muted (PS2X_SPU2_ATTR_MUTE=1)" : "ignored, as PCSX2 does");
            }
            if (honourMute)
                output = {};
        }
        return output;
    }

    // Reverb unit, following psx-spx "SPU Reverb Formula" / "Reverb Buffer Resampling" (the MIT-licensed
    // reimplementation in nathanialf/ico-pc port/audio/spu2.c served as the reference): the wet input is decimated to
    // 24 kHz with a 39-tap half-band FIR, every second frame one reverb step runs over the work area [ESA, EEA] in SPU2
    // RAM (same/different side reflections, four combs, two all-pass filters) and the output is interpolated back to
    // 48 kHz with the same filter. ATTR bit 7 enables the work-area writes. PS2X_SPU2_REVERB=0 disables it.
    std::array<int32_t, 2> IopSpu2::reverbFrame(uint32_t core, const std::array<int32_t, 2> &wet)
    {
        static constexpr std::array<int16_t, 39> kFir = {
            -1, 0, 2, 0, -10, 0, 35, 0, -103, 0, 266, 0, -616, 0, 1332, 0, -2960, 0, 10246, 16384,
            10246, 0, -2960, 0, 1332, 0, -616, 0, 266, 0, -103, 0, 35, 0, -10, 0, 2, 0, -1,
        };
        constexpr uint32_t kHistMask = 63u;
        Reverb &rv = m_reverb[core];
        const uint32_t pos = static_cast<uint32_t>(rv.frame++ & kHistMask);
        rv.in[0][pos] = static_cast<int16_t>(wet[0]);
        rv.in[1][pos] = static_cast<int16_t>(wet[1]);

        // Half-band filter: only the even taps and the centre are non-zero, and it is symmetric.
        auto firEven = [&](const std::array<int16_t, 64> &h) -> int32_t
        {
            int32_t acc = 0;
            for (uint32_t k = 0u; k < 19u; k += 2u)
                acc += kFir[k] * (static_cast<int32_t>(h[(pos - k) & kHistMask]) + h[(pos - 38u + k) & kHistMask]);
            return acc;
        };

        std::array<int32_t, 2> out{};
        if ((pos & 1u) == 0u)
        {
            // Even frame: the zero-stuffed output only meets the centre tap: (0x4000 * x) >> 14 = x.
            rv.out[0][pos] = 0;
            rv.out[1][pos] = 0;
            out[0] = rv.out[0][(pos - 19u) & kHistMask];
            out[1] = rv.out[1][(pos - 19u) & kHistMask];
            return out;
        }

        std::array<int32_t, 2> in{};
        for (uint32_t ch = 0u; ch < 2u; ++ch)
            in[ch] = saturate((firEven(rv.in[ch]) + 0x4000 * rv.in[ch][(pos - 19u) & kHistMask]) >> 15);

        const uint32_t base = core * kCoreStride;
        auto pair = [&](uint32_t offset) -> uint32_t
        { return ((static_cast<uint32_t>(reg(base + offset)) & 0xFu) << 16u) | reg(base + offset + 2u); };
        const uint32_t esa = pair(0x2E0u);
        const uint32_t end = ((static_cast<uint32_t>(reg(base + 0x33Cu)) & 0xFu) << 16u) | 0xFFFFu;
        std::array<int32_t, 2> step{};
        if (esa <= end)
        {
            const uint32_t size = end - esa + 1u;
            if (rv.current < esa || rv.current > end)
                rv.current = esa;
            const uint32_t rel = rv.current - esa;
            const bool write = (reg(base + 0x19Au) & 0x80u) != 0u;
            const uint32_t shared = kSharedBase + core * 0x28u;
            auto vol = [&](uint32_t offset) -> int32_t { return static_cast<int16_t>(reg(shared + offset)); };
            const int32_t vIIR = vol(0x14u), vC1 = vol(0x16u), vC2 = vol(0x18u), vC3 = vol(0x1Au), vC4 = vol(0x1Cu);
            const int32_t vWALL = vol(0x1Eu), vAPF1 = vol(0x20u), vAPF2 = vol(0x22u);
            const std::array<int32_t, 2> vIN = {vol(0x24u), vol(0x26u)};
            // Address registers (halfword units) of the 0x2E4 block, in order: APF1 size, APF2 size, SAME L/R dst,
            // COMB1 L/R, COMB2 L/R, SAME L/R src, DIFF L/R dst, COMB3 L/R, COMB4 L/R, DIFF L/R src, APF1 L/R dst,
            // APF2 L/R dst.
            auto areg = [&](uint32_t index) -> int64_t { return static_cast<int64_t>(pair(0x2E4u + 4u * index)); };
            auto addr = [&](int64_t offset) -> uint32_t
            {
                int64_t a = (static_cast<int64_t>(rel) + offset) % static_cast<int64_t>(size);
                if (a < 0)
                    a += size;
                return (esa + static_cast<uint32_t>(a)) & kAddressMask;
            };
            auto rd = [&](int64_t offset) -> int32_t { return static_cast<int16_t>(m_ram[addr(offset)]); };
            auto wr = [&](int64_t offset, int32_t value) { m_ram[addr(offset)] = static_cast<uint16_t>(saturate(value)); };
            const int64_t apf1Size = areg(0), apf2Size = areg(1);
            if (write)
            {
                for (uint32_t ch = 0u; ch < 2u; ++ch)
                {
                    const int32_t input = scale(vIN[ch], in[ch]);
                    const int64_t sameDst = areg(2u + ch), sameSrc = areg(8u + ch);
                    int32_t prev = rd(sameDst - 1);
                    int32_t t = saturate(input + scale(rd(sameSrc), vWALL) - prev);
                    wr(sameDst, scale(t, vIIR) + prev);
                    const int64_t diffDst = areg(10u + ch), diffSrc = areg(ch ? 16u : 17u); // sources are crossed
                    prev = rd(diffDst - 1);
                    t = saturate(input + scale(rd(diffSrc), vWALL) - prev);
                    wr(diffDst, scale(t, vIIR) + prev);
                }
            }
            for (uint32_t ch = 0u; ch < 2u; ++ch)
            {
                int32_t o = scale(vC1, rd(areg(4u + ch))) + scale(vC2, rd(areg(6u + ch))) + scale(vC3, rd(areg(12u + ch))) +
                            scale(vC4, rd(areg(14u + ch)));
                o = saturate(o);
                const int64_t apf1Dst = areg(18u + ch);
                int32_t a = rd(apf1Dst - apf1Size);
                o = saturate(o - scale(vAPF1, a));
                if (write)
                    wr(apf1Dst, o);
                o = saturate(scale(o, vAPF1) + a);
                const int64_t apf2Dst = areg(20u + ch);
                a = rd(apf2Dst - apf2Size);
                o = saturate(o - scale(vAPF2, a));
                if (write)
                    wr(apf2Dst, o);
                o = saturate(scale(o, vAPF2) + a);
                step[ch] = o;
            }
            rv.current = rv.current + 1u > end ? esa : rv.current + 1u;
        }
        rv.out[0][pos] = static_cast<int16_t>(step[0]);
        rv.out[1][pos] = static_cast<int16_t>(step[1]);
        for (uint32_t ch = 0u; ch < 2u; ++ch)
            out[ch] = saturate(firEven(rv.out[ch]) >> 14);
        return out;
    }

    void IopSpu2::keyOn(uint32_t core, uint32_t index)
    {
        ++g_statKeyOn;
        Voice &voice = m_voices[core][index];
        if (voice.startA & 7u)
            voice.startA = ((voice.startA + 8u) & ~7u) & kAddressMask;
        voice.nextA = voice.startA;
        voice.sampleIndex = kSamplesPerBlock;
        voice.counter = 0;
        voice.loopFlags = 0u;
        voice.manualLoop = false;
        voice.envelope = 0;
        voice.phase = kPhaseAttack;
        m_decoders[core][index] = {};
        m_interpolation[core][index] = {};
        m_endx[core] &= ~(1u << index);
        if (audioDisabled())
        {
            // Voices are not run: report them finished right away so the driver frees them.
            voice.phase = kPhaseOff;
            m_endx[core] |= 1u << index;
        }
    }

    void IopSpu2::keyOff(uint32_t core, uint32_t index)
    {
        Voice &voice = m_voices[core][index];
        if (voice.phase != kPhaseOff)
            voice.phase = kPhaseRelease;
    }

    int32_t IopSpu2::runVoice(uint32_t core, uint32_t index)
    {
        Voice &voice = m_voices[core][index];
        const uint32_t params = core * kCoreStride + index * 0x10u;
        const int32_t pitch = std::min<int32_t>(reg(params + kParamPitch * 2u), kMaxPitch);
        if (voice.phase == kPhaseOff && pitch == 0)
            return 0;
        const uint16_t adsr1 = reg(params + kParamAdsr1 * 2u);
        const uint16_t adsr2 = reg(params + kParamAdsr2 * 2u);
        if (voice.phase != kPhaseOff)
            stepEnvelope(voice, adsr1, adsr2);
        voice.counter += pitch;
        while (voice.counter > 0)
        {
            stepAddress(core, index);
            voice.counter -= kPitchStep;
        }
        if (voice.phase == kPhaseOff || voice.envelope == 0)
            return 0;
        const int32_t fraction = voice.counter + kPitchStep;
        const auto &weights = interpolationWeights()[std::min(fraction >> 4, 255)];
        const auto &history = m_interpolation[core][index];
        int32_t pcm = 0;
        for (size_t tap = 0u; tap < history.size(); ++tap)
            pcm += scale(history[tap], weights[tap]);
        return scale(saturate(pcm), voice.envelope >> 16);
    }

    void IopSpu2::decodeBlock(uint32_t core, uint32_t index, uint32_t address)
    {
        Decoder &decoder = m_decoders[core][index];
        constexpr std::array<int32_t, 5> first{0, 60, 115, 98, 122};
        constexpr std::array<int32_t, 5> second{0, 0, -52, -55, -60};
        const uint16_t header = m_ram[address];
        const uint32_t predictor = (header >> 4u) & 0xFu;
        const int32_t c1 = predictor < first.size() ? first[predictor] : 0;
        const int32_t c2 = predictor < second.size() ? second[predictor] : 0;
        const uint32_t shift = header & 0xFu;
        for (uint32_t sample = 0u; sample < kSamplesPerBlock; ++sample)
        {
            const uint16_t packed = m_ram[(address + 1u + sample / 4u) & kAddressMask];
            const int32_t nibble = (packed >> ((sample % 4u) * 4u)) & 0xFu;
            const int32_t signedNibble = nibble >= 8 ? nibble - 16 : nibble;
            const int32_t raw = (signedNibble * 4096) >> shift;
            const int32_t filtered = raw + ((decoder.history1 * c1 + decoder.history2 * c2 + 32) >> 6);
            const int32_t decoded = saturate(filtered);
            decoder.pcm[sample] = static_cast<int16_t>(decoded);
            decoder.history2 = decoder.history1;
            decoder.history1 = decoded;
        }
    }

    void IopSpu2::stepAddress(uint32_t core, uint32_t index)
    {
        Voice &voice = m_voices[core][index];
        if (voice.sampleIndex >= kSamplesPerBlock)
        {
            const uint32_t block = voice.nextA & ~7u & kAddressMask;
            if (voice.phase != kPhaseOff)
                decodeBlock(core, index, block);
            voice.loopFlags = static_cast<uint8_t>(m_ram[block] >> 8);
            if ((voice.loopFlags & kLoopStart) && !voice.manualLoop)
                voice.loopStartA = block;
            voice.sampleIndex = 0u;
            voice.nextA = (block + 1u) & kAddressMask;
        }
        if (voice.phase != kPhaseOff)
        {
            Decoder &decoder = m_decoders[core][index];
            decoder.previous = decoder.current;
            decoder.current = decoder.pcm[voice.sampleIndex];
            auto &history = m_interpolation[core][index];
            history[0] = history[1];
            history[1] = history[2];
            history[2] = history[3];
            history[3] = static_cast<int16_t>(decoder.current);
        }
        ++voice.sampleIndex;
        if ((voice.sampleIndex & 3u) != 0u)
            return;
        voice.nextA = (voice.nextA + 1u) & kAddressMask;
        if ((voice.nextA & 7u) != 0u)
            return;
        if (voice.loopFlags & kLoopEnd)
        {
            m_endx[core] |= 1u << index;
            voice.nextA = voice.loopStartA;
            if (!(voice.loopFlags & kLoopRepeat))
            {
                voice.envelope = 0;
                voice.phase = kPhaseOff;
            }
        }
    }

    void IopSpu2::stepEnvelope(Voice &voice, uint16_t adsr1, uint16_t adsr2)
    {
        const bool attackExp = (adsr1 & 0x8000u) != 0u;
        const int32_t attackRate = (adsr1 >> 8) & 0x7F;
        const int32_t decayRate = (adsr1 >> 4) & 0xF;
        const int32_t sustainLevel = adsr1 & 0xF;
        const bool sustainExp = (adsr2 & 0x8000u) != 0u;
        const bool sustainDown = (adsr2 & 0x4000u) != 0u;
        const int32_t sustainRate = (adsr2 >> 6) & 0x7F;
        const bool releaseExp = (adsr2 & 0x20u) != 0u;
        const int32_t releaseRate = adsr2 & 0x1F;
        int64_t value = voice.envelope;
        const int32_t expOffset = kInverseExpOffsets[(value >> 28) & 7];

        switch (voice.phase)
        {
        case kPhaseAttack:
            if (value >= kEnvelopeMax)
            {
                voice.phase = kPhaseDecay;
                break;
            }
            value += envelopeRate((attackRate ^ 0x7F) - ((attackExp && value >= 0x60000000) ? 0x18 : 0x10));
            if (value >= kEnvelopeMax)
            {
                value = kEnvelopeMax;
                voice.phase = kPhaseDecay;
            }
            break;
        case kPhaseDecay:
        {
            value -= envelopeRate(((decayRate ^ 0x1F) * 4) - 0x18 + expOffset);
            const int64_t level = sustainLevel == 0xF ? kEnvelopeMax : (static_cast<int64_t>(sustainLevel) + 1) << 27;
            if (value <= level)
            {
                value = std::max<int64_t>(value, 0);
                voice.phase = kPhaseSustain;
            }
            break;
        }
        case kPhaseSustain:
            if (sustainDown)
            {
                value -= sustainExp ? envelopeRate((sustainRate ^ 0x7F) - 0x1B + expOffset)
                                    : envelopeRate((sustainRate ^ 0x7F) - 0xF);
                if (value <= 0)
                {
                    value = 0;
                    voice.phase = kPhaseSustainEnd;
                }
            }
            else
            {
                value += envelopeRate((sustainRate ^ 0x7F) - ((sustainExp && value >= 0x60000000) ? 0x18 : 0x10));
                if (value >= kEnvelopeMax)
                {
                    value = kEnvelopeMax;
                    voice.phase = kPhaseSustainEnd;
                }
            }
            break;
        case kPhaseSustainEnd:
            value = sustainDown ? 0 : kEnvelopeMax;
            break;
        case kPhaseRelease:
            value -= releaseExp ? envelopeRate(((releaseRate ^ 0x1F) * 4) - 0x18 + expOffset)
                                : envelopeRate(((releaseRate ^ 0x1F) * 4) - 0xC);
            if (value <= 0)
            {
                value = 0;
                voice.phase = kPhaseOff;
            }
            break;
        default:
            value = 0;
            break;
        }
        voice.envelope = static_cast<int32_t>(value);
    }
}
