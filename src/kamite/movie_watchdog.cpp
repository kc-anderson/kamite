// Sweep R3. The ending (r023 cR023::ending 0x799358) plays descriptor 1 (staff_us.sfd, flags 0x23: no START skip) and
// loops `do { wait 1 frame } while (cMovie_GetStatus(pool, h) != 0)` (0x7993AC-0x7993DC) with no timeout; the r000 movie
// list polls the same way at 0x161164. The game itself turns CRI's ERROR (stat 4, 0x2D37D4) and PLAYEND (0x2D3A9C) into
// "finished" (track destroyed, slot freed, GetStatus 0). Left uncovered: a port-side stall where neither comes (IPU
// waiting for input that never arrives, stream or audio stalled). Then, after 20 s of game time without one decoded
// macroblock, report the movie finished once; both callers call cMovie_Stop (0x2B4D68) themselves right after.
#include "kamite/movie_watchdog.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h" // GPR_U32 / SET_GPR_U32
#include "runtime/ps2_memory.h"

extern "C" uint64_t ps2_ipu_macroblocks(void);

namespace kamite
{
    namespace
    {
        constexpr uint32_t kGetStatus = 0x2B4EC8u;
        constexpr uint32_t kEndingReturn = 0x7993DCu;    // r023 ending loop (jal at 0x7993D4)
        constexpr uint32_t kMovieListReturn = 0x16116Cu; // r000 movie list (jal at 0x161164)
        constexpr uint64_t kStallFields = 1200u;         // 20 s at 60 fields/s of emulated time
        GuestFunction g_getStatus = nullptr;
        uint32_t g_handle = 0;
        uint64_t g_lastMacroblocks = 0, g_lastProgressField = 0;

        void getStatusWithWatchdog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31), handle = GPR_U32(ctx, 5);
            g_getStatus(rdram, ctx, runtime);
            if ((ra != kEndingReturn && ra != kMovieListReturn) || GPR_U32(ctx, 2) == 0u)
            {
                g_handle = 0;
                return;
            }
            const uint64_t field = runtime->memory().gs().vsyncTick.load(std::memory_order_relaxed);
            const uint64_t mb = ps2_ipu_macroblocks();
            if (handle != g_handle || mb != g_lastMacroblocks)
            {
                g_handle = handle;
                g_lastMacroblocks = mb;
                g_lastProgressField = field;
                return;
            }
            if (field - g_lastProgressField < kStallFields)
                return;
            KM_ERROR(Game, "[movie] " << (ra == kEndingReturn ? "ending" : "movie list") << " movie (handle " << handle
                                      << ", status " << GPR_U32(ctx, 2) << ") made no decode progress for 20 s at "
                                      << mb << " macroblocks: reporting it finished so the game goes on");
            SET_GPR_U32(ctx, 2, 0u);
            g_handle = 0;
        }
    }

    void installMovieWatchdog(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kGetStatus) || !(g_getStatus = runtime.lookupFunction(kGetStatus)) ||
            !FunctionHooks::instance().replace(kGetStatus, "cMovie_GetStatus_watchdog", &getStatusWithWatchdog,
                                               ReplacementStatus::Native, "sweep R3: 20 s no-progress timeout for movie polls"))
            KM_WARN(Game, "movie watchdog off (0x2b4ec8 not hookable)");
    }
}
