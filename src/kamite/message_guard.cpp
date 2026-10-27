#include "kamite/message_guard.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstring>
#include <set>

namespace kamite::message_guard
{
    namespace
    {
        constexpr uint32_t kSetMessage = 0x002AF590u;   // cMessDrawFont_setMessage(drawer, id)
        constexpr uint32_t kMessageManager = 0x003C23A4u; // cMessage: 16 bank pointers; id >> 12 picks the bank
        constexpr uint32_t kEmptyCodes = 0x40u;         // codes checked: a real text never has 64 zero codes
        constexpr uint16_t kPageEnd = 0x8001u; // the game's own strings end with it; cMessCommon_isPageEndCode takes 0x80xx (0xA0xx only ends a line: measurePage then walked on)
        std::set<uint32_t> g_reported;

        uint32_t rd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4); return v; }
        bool inRam(uint32_t a, uint32_t size) { a &= 0x01FFFFFFu; return a >= 0x100000u && a + size <= 0x02000000u; }

        void onSetMessage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t id = GPR_U32(ctx, 5) & 0xFFFFu;
            const uint32_t mgr = rd32(rdram, kMessageManager);
            if (!inRam(mgr, 64u))
                return;
            const uint32_t bank = rd32(rdram, mgr + 4u * (id >> 12));
            if (!inRam(bank, 8u))
                return;
            const uint32_t count = rd32(rdram, bank);
            if ((id & 0xFFFu) >= count || !inRam(bank + 8u * (id & 0xFFFu), 8u))
                return;
            const uint32_t text = bank + rd32(rdram, bank + 8u * (id & 0xFFFu) + 4u);
            if (!inRam(text, 2u * kEmptyCodes))
                return;
            const uint8_t *p = rdram + (text & 0x01FFFFFFu);
            for (uint32_t i = 0; i < 2u * kEmptyCodes; ++i)
                if (p[i] != 0u)
                    return;
            std::memcpy(rdram + (text & 0x01FFFFFFu), &kPageEnd, 2);
            if (g_reported.insert(id).second)
                KM_WARN(Game, "message guard: message 0x" << std::hex << id << " at 0x" << text << std::dec
                                                        << " was an empty buffer (no end code); ended it, so it draws as nothing instead of hanging the text measurer");
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kSetMessage))
            return;
        FunctionHooks::instance().observeEntry(kSetMessage, "message_guard_set_message", &onSetMessage);
    }
}
