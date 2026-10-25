// See glow_probe.h.
#include "kamite/glow_probe.h"

#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_pnach.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace kamite::glow_probe
{
    namespace
    {
        constexpr uint32_t kPullEsp = 0x2FAA88u;     // PullEsp(type): a free pool slot, constructed as that type
        constexpr uint32_t kEsp05Ctor = 0x2FFBD0u;   // the esp05 constructor (type 6; $a0 the slot)
        constexpr uint32_t kSetup = 0x2E8318u;       // Panel_setupOnce (an esp05's per-frame packet setup)
        constexpr uint32_t kPool = 0x61B7C0u;        // the effect pool: 0x400 slots of 0x400 bytes
        constexpr uint32_t kEsp05Type = 6u;
        // PullEsp's two callers (the return addresses after the jal) and the register each keeps its emitter in.
        constexpr uint32_t kEmitterReturnA = 0x302DD4u; // func_003029C0: $s2
        constexpr uint32_t kEmitterReturnB = 0x304744u; // func_003041B8: $s3

        struct Live
        {
            uint64_t born = 0;
            uint64_t setups = 0;
            uint64_t lastSeen = 0;
        };
        uint64_t g_vblanks = 0;
        std::map<uint32_t, Live> g_live;  // esp05 objects seen set up, by address (EE thread)
        std::vector<uint32_t> g_frame;    // this vblank's setups
        std::string g_pull;               // the last PullEsp(6) call's emitter description (EE thread)
        const uint8_t *g_ram = nullptr;

        uint32_t rd32(uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        int16_t rd16s(uint32_t address)
        {
            int16_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        float rdFloat(uint32_t address)
        {
            float v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        bool valid(uint32_t address) { return address >= 0x100000u && address < 0x2000000u; }

        // PullEsp entry: for an esp05, who asks and with what parameters.
        void onPull(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 4) != kEsp05Type)
                return;
            const uint32_t ra = GPR_U32(ctx, 31);
            const uint32_t emitter = ra == kEmitterReturnA ? GPR_U32(ctx, 18) : ra == kEmitterReturnB ? GPR_U32(ctx, 19) : 0u;
            std::ostringstream out;
            out << "via 0x" << std::hex << ra;
            if (valid(emitter))
            {
                const uint32_t param = rd32(emitter + 0x100u), parent = rd32(emitter + 0x108u);
                out << " emitter 0x" << emitter << " (frame " << std::dec << rdFloat(emitter + 0x180u) << ", start "
                    << rd16s(emitter + 0x186u) << ", end " << rd16s(emitter + 0x188u) << ", countdown " << rdFloat(emitter + 0x17Cu)
                    << ", parent 0x" << std::hex << parent << ")";
                if (valid(param))
                    out << " param 0x" << param << ": count " << std::dec << static_cast<int>(g_ram[(param + 0xBCu) & 0x1FFFFFFu])
                        << ", interval " << static_cast<int>(g_ram[(param + 0xBDu) & 0x1FFFFFFu]) << ", type "
                        << static_cast<int>(g_ram[(param + 0x133u) & 0x1FFFFFFu]) << ", mode "
                        << static_cast<int>(g_ram[(param + 0x137u) & 0x1FFFFFFu]);
                if (valid(parent))
                    out << "; parent frame " << rdFloat(parent + 0x180u) << ", start " << rd16s(parent + 0x186u) << ", end "
                        << rd16s(parent + 0x188u) << ", param 0x" << std::hex << rd32(parent + 0x100u);
            }
            g_pull = out.str();
        }

        // The esp05 constructor: the slot it lands in.
        void onCtor(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t slot = GPR_U32(ctx, 4);
            KM_INFO(Game, "[glow] esp05 born at 0x" << std::hex << slot << " (slot " << std::dec << (slot - kPool) / 0x400u
                                                      << ") on game frame " << g_vblanks << " " << g_pull);
            g_pull.clear();
        }

        void onSetup(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (g_frame.size() < 16u)
                g_frame.push_back(GPR_U32(ctx, 4));
        }

        void onVblank()
        {
            ++g_vblanks;
            for (const uint32_t p : g_frame)
            {
                Live &live = g_live[p];
                if (live.setups == 0u)
                {
                    live.born = g_vblanks;
                    KM_INFO(Game, "[glow] esp05 0x" << std::hex << p << " first set up on vblank " << std::dec << g_vblanks
                                                    << " (lifetime " << rd16s(p + 0x238u) << ", frame " << rd16s(p + 0x22Eu) << ")");
                }
                ++live.setups;
                live.lastSeen = g_vblanks;
            }
            if (g_frame.size() > 1u)
            {
                static int shown = 0;
                if (shown < 3)
                {
                    ++shown;
                    std::ostringstream out;
                    out << "[glow] " << g_frame.size() << " esp05 set up in one vblank:";
                    for (const uint32_t p : g_frame)
                        out << " 0x" << std::hex << p;
                    KM_INFO(Game, out.str());
                }
            }
            g_frame.clear();
            for (auto it = g_live.begin(); it != g_live.end();)
            {
                if (g_vblanks - it->second.lastSeen > 4u)
                {
                    KM_INFO(Game, "[glow] esp05 0x" << std::hex << it->first << " gone: last set up on vblank " << std::dec
                                                    << it->second.lastSeen << ", " << it->second.setups << " setups since vblank "
                                                    << it->second.born);
                    it = g_live.erase(it);
                }
                else
                    ++it;
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        g_ram = runtime.memory().getRDRAM();
        if (!g_ram)
            return;
        FunctionHooks &hooks = FunctionHooks::instance();
        const bool ok = hooks.observeEntry(kPullEsp, "PullEsp (glow probe)", &onPull) &&
                        hooks.observeEntry(kEsp05Ctor, "esp05 ctor (glow probe)", &onCtor) &&
                        hooks.observeEntry(kSetup, "Panel_setupOnce (glow probe)", &onSetup);
        if (ok)
            ps2x::pnach::addFrameHook(&onVblank);
        else
            KM_WARN(Game, "[glow] probe not installed (a function is not hookable)");
    }
}
