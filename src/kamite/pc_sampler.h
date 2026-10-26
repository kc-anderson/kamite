#pragma once

class PS2Runtime;
struct R5900Context;

namespace kamite::pcsampler
{
    // KAMITE_PC_SAMPLE_ALL=1 adds every thread of the process to the report (busy share, hottest host functions):
    // the GS, VU1, IOP and window threads, for finding a frame's bottleneck outside the EE.
    // KAMITE_PC_SAMPLE=<seconds>: a host thread samples the main game thread's PC about 2000 times a second and
    // every <seconds> logs where the game's own code spends its time ([pc-sample], by recompiled function; module code
    // by 256-byte region). For finding what slows the game down (the F3 Speed below 100%).
    void installFromEnvironment(PS2Runtime &runtime);

    // From the frame hook (the EE thread): the main game thread's context, the one sampled.
    void noteMainContext(const R5900Context *ctx);
}
