// See jukebox.h.
#include "kamite/jukebox.h"

#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "kamite/game_layout.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <cstring>

namespace kamite::jukebox
{
    namespace
    {
        constexpr uint32_t kJukeBoxOpen = game::GameWork::address(0x10u); // D_007474B0 (GameWork+0x10): JukeBox_Load's object, 0 when closed
        constexpr uint32_t kGameFlags = game::GameWork::address(game::GameWork::kFlags5D8); // GameWork.flags5D8, the game-state word
        constexpr uint32_t kPlayerOff = game::kFlags5D8PlayerOff; // its bits that stop the player's Move (func_001F6C40 sets them)
        constexpr uint32_t kPlayerMove = 0x100530u;   // the player's Move (vtable 0x41B6A0 + 0x54)

        GuestFunction g_move = nullptr;

        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }

        void wr32(uint8_t *rdram, uint32_t address, uint32_t v)
        {
            std::memcpy(rdram + (address & 0x1FFFFFFu), &v, sizeof(v));
        }

        // The player's Move: with the radio open, the player-off bits are out of the game-state word while it runs and
        // back after it (only those two bits: the Move may change the others).
        void moveWithJukebox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t flags = rd32(rdram, kGameFlags);
            const bool lift = open(rdram) && (flags & kPlayerOff) != 0u;
            if (lift)
                wr32(rdram, kGameFlags, flags & ~kPlayerOff);
            g_move(rdram, ctx, runtime);
            if (lift)
                wr32(rdram, kGameFlags, (rd32(rdram, kGameFlags) & ~kPlayerOff) | (flags & kPlayerOff));
        }
    }

    bool open(const uint8_t *rdram)
    {
        return rdram && rd32(rdram, kJukeBoxOpen) != 0u;
    }

    void install(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kPlayerMove) || !(g_move = runtime.lookupFunction(kPlayerMove)) ||
            !FunctionHooks::instance().replace(kPlayerMove, "player Move (jukebox)", &moveWithJukebox, ReplacementStatus::Native,
                                               "Gene stays in the picture while the jukebox is open"))
            KM_WARN(Game, "jukebox: the player's Move 0x100530 is not hookable; Gene is hidden while the radio is open");
    }
}
