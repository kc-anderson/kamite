// See glow_probe.h.
#include "kamite/glow_probe.h"

#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_pnach.h"

#include <cstdint>
#include <sstream>
#include <vector>

namespace kamite::glow_probe
{
    namespace
    {
        constexpr uint32_t kSetup = 0x2E8318u; // Panel_setupOnce (the esp05 glow's per-frame packet setup)
        std::vector<uint32_t> g_frame;         // this vblank's setups (EE thread only)
        uint64_t g_vblanks = 0, g_framesWithSetup = 0, g_twiceSame = 0, g_twiceOther = 0;
        int g_detailed = 0;

        void onSetup(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (g_frame.size() < 16u)
                g_frame.push_back(GPR_U32(ctx, 4));
        }

        void onVblank()
        {
            ++g_vblanks;
            if (!g_frame.empty())
            {
                ++g_framesWithSetup;
                if (g_frame.size() > 1u)
                {
                    bool same = true;
                    for (const uint32_t p : g_frame)
                        same = same && p == g_frame[0];
                    ++(same ? g_twiceSame : g_twiceOther);
                    if (g_detailed < 5)
                    {
                        ++g_detailed;
                        std::ostringstream out;
                        out << "glow probe: the glow was set up " << g_frame.size() << " times in one vblank by";
                        for (const uint32_t p : g_frame)
                            out << " 0x" << std::hex << p;
                        KM_INFO(Game, out.str());
                    }
                }
                g_frame.clear();
            }
            if (g_vblanks % 1800u == 0u && g_framesWithSetup != 0u)
                KM_INFO(Game, "glow probe (last 30 s): glow set up in " << g_framesWithSetup << " vblanks; more than once in "
                                                                       << g_twiceSame << " (one object) + " << g_twiceOther
                                                                       << " (several objects)");
            if (g_vblanks % 1800u == 0u)
                g_framesWithSetup = g_twiceSame = g_twiceOther = 0u;
        }
    }

    void install(PS2Runtime &)
    {
        if (FunctionHooks::instance().observeEntry(kSetup, "Panel_setupOnce (glow probe)", &onSetup))
            ps2x::pnach::addFrameHook(&onVblank);
    }
}
