#pragma once

#include "ps2x/state_io.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

class PS2Memory;

namespace ps2x::asyncvif
{
    enum class ItemKind : uint8_t
    {
        GifPath3,
        Vif1,
    };

    struct Item
    {
        ItemKind kind = ItemKind::Vif1;
        std::vector<uint8_t> data;
    };

    void setDefaultEnabled(bool enabled);
    void setHostPacingDefault(bool enabled);
    void setFbrstProvider(std::function<uint32_t()> provider);
    void setGifProcessor(std::function<void(const uint8_t *, uint32_t)> processor);
    bool submitGif(const uint8_t *data, uint32_t sizeBytes);
    // MTVU: the EE cleared CSR.SIGNAL; the GIF stage thread replays the GIF data the GS deferred at the stalled
    // SIGNAL (ps2xGsReleaseStall), so the GS front end only ever runs on that thread. False when there is no
    // GIF stage (the caller then replays itself).
    bool postGifStallRelease();
    bool active(PS2Memory &memory);
    bool onWorker();
    bool onAsyncThread(); // VU worker or GIF stage thread
    // PS2X_MTVU_TRACE=<seconds>: from that many seconds after start, record timestamped events from every thread
    // (job kicks, VU jobs, GIF chunks, SIGNAL/FINISH, the GS handler, game frames, pacing sleeps, vblanks) and write
    // them to mtvu_trace.txt once the buffer is full. For finding which thread waits for which in a frame.
    void traceEvent(char code, uint32_t arg = 0u) noexcept;
    uint32_t currentFbrst();
    std::vector<uint8_t> takeBuffer();
    void enqueue(PS2Memory &memory, std::vector<Item> &&items);
    void sync(PS2Memory &memory);
    void stop(PS2Memory &memory);

    struct Vu1TimingHooks
    {
        std::function<uint64_t()> eeCycle;
        std::function<uint64_t()> vu1Cycles;
        std::function<uint64_t(uint64_t)> consumeEeCycles;
    };

    void setVu1Timing(Vu1TimingHooks hooks);
    // How long a VU1 program keeps the guest waiting, in EE cycles per VU1 cycle (PS2X_VU1_TIMING, default 1). The
    // port's 60 FPS mode halves it: the game then asks twice the PS2's work per second of the emulated VU1, which
    // (at 1) could not finish the casino's heaviest views in one field and dropped them to 30 frames a second while
    // the real VU work took 12 of the 16.7 ms (Oct 26 trace). PS2X_VU1_TIMING, when set, wins.
    void setVu1TimingScale(double scale);
    void vu1Kick();
    bool vu1Busy(PS2Memory &memory);

    // MTVU timing model (docs/PROGRESS.md §50). PS2X_MTVU_VBLANK_WAIT=0 turns it off.
    // The emulated vblank waits until the VU/GIF workers have no runnable work (data the GS deferred at a SIGNAL waits for
    // the EE and does not count), as in PCSX2: a slow VU1 slows the game down instead of making a frame miss the field.
    // With it on, the host pacing of vu1Busy and the EE scheduler's catch-up are off.
    bool vblankWaitEnabled();
    // True while the VU worker or the GIF stage has queued or running work.
    bool pipelineBusy(PS2Memory &memory);
    // Waits up to `timeout` for the workers to go idle; returns pipelineBusy() afterwards.
    bool waitPipelineIdle(PS2Memory &memory, std::chrono::microseconds timeout);
    void saveTimingState(ps2x::state::Writer &out);
    void loadTimingState(ps2x::state::Reader &in);
}
