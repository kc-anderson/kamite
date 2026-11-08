// Buck Breaker: the punish for stunned enemies, per style. See buck_breaker.h; research in the project's
// claude/punish-notes.md (SLUS_215.03, Nov 7).
#include "kamite/buck_breaker.h"

#include "kamite/disc_files.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/punish_fit.h"
#include "kamite/style_presets.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_pnach.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

namespace kamite::buck_breaker
{
    namespace
    {
        namespace ent = game::IDBaseEnt;
        namespace work = game::GameWork;

        uint8_t *g_ram = nullptr;

        // ---- the punishes ----
        enum Pick : uint8_t
        {
            Normal, // the game's own, by enemy
            Pummel,
            Spanking,
            Suplex,
            Stinger,
            CobraTwist,
            kPickCount
        };
        const char *const kPickNames[kPickCount] = {"Normal", "Pummel", "Spanking", "Suplex", "Stinger", "Cobra Twist"};

        // func_002DCD50 (the front stun-punish prompt) picks the prompt's word and its callback with a switch on the
        // enemy number: a0 = number - 0x205 at 0x2DD028, the table 0x44D430 (0x7A cases), out of range = the default
        // case (SUPLEX). Each case calls cActionButton_set(0x568288, 0xA, word, 0, callback, enemy, 0); the callback puts
        // Gene and the enemy into the punish's states, and neither looks at the number again.
        constexpr uint32_t kCaseTable = 0x44D430u, kCases = 0x7Au;
        constexpr uint32_t kSuplexCase = 0x2DD050u;
        // The case each pick goes to, and the a0 that reaches it: Pummel 0x209 (the shared callback 0x2DD270), Spanking
        // 0x21A, Suplex out of range (the default), Stinger 0x20A, Cobra Twist 0x205.
        constexpr uint32_t kPickCase[kPickCount] = {0u, 0x2DD168u, 0x2DD148u, kSuplexCase, 0x2DD090u, 0x2DD22Cu};
        constexpr uint32_t kPickIndex[kPickCount] = {0u, 0x209u - 0x205u, 0x21Au - 0x205u, kCases, 0x20Au - 0x205u, 0u};
        // The generic punishes' cases (K, Nov 7: only these are replaced): Suplex (and the default), Stinger, Pummel
        // (0x218's own branch to the shared callback, the 17-number group), Spanking, the nine that flip Suplex / Pummel
        // (+0x621), Cobra Twist. Left alone: Stake Driver 0x2DD070 (the midgets), 0x246's Gorilla Suplex + Pummel
        // 0x2DD0B0, Gorilla Suplex 0x2DD128, the four Pummels with their own motions 0x2DD1C0 / 1D4 / 1E8 / 1FC.
        constexpr uint32_t kGenericCases[] = {kSuplexCase, 0x2DD090u, 0x2DD118u, 0x2DD148u, 0x2DD168u, 0x2DD17Cu, 0x2DD22Cu};

        std::atomic<int> g_pick{Normal}; // the pick in force (EE thread: each vblank and on a change)
        int g_tableOk = 0;                // 0 not checked yet, 1 the disc's table, -1 not (every pick acts as Normal)

        uint32_t ram32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }

        bool genericCase(uint32_t target)
        {
            for (uint32_t c : kGenericCases)
                if (c == target)
                    return true;
            return false;
        }

        bool tableOk(const uint8_t *rdram)
        {
            if (g_tableOk == 0)
            {
                g_tableOk = 1;
                for (int p = Pummel; p < kPickCount; ++p)
                    if (kPickIndex[p] < kCases && ram32(rdram, kCaseTable + 4u * kPickIndex[p]) != kPickCase[p])
                        g_tableOk = -1;
                if (g_tableOk < 0)
                    KM_WARN(Game, "buck breaker: the punish table at 0x44D430 is not the disc's; punishes left as the game's");
            }
            return g_tableOk > 0;
        }

        // The paired punishes play both halves from the enemy's motion table (+0x304, entry i at pack + 4 + 4i): the
        // enemy's in its punish phase (29 / 31 / 33 / 34), Gene's through cEm00_GetPlMotion (0x23A300, indices 0x38,
        // 0x3A-0x3C, 0x3E-0x41, 0x42-0x44). The generic enemies all use em/em00.dat's table (3998 entries), but a room
        // loads only the blocks its enemies need (the casino's: 0-5, 16-828, 3441-3524, 3793-3851; others read 0):
        // Suplex and Stinger sit in the common block, Spanking and Cobra Twist in their own. The entries a pick needs
        // and the room lacks are copied from the disc's em00.dat (byte for byte as the game loads them: no relocation)
        // into kMotionArea, and the table's empty offsets pointed there (u32, wrapping). Pummel (Gene's phase 56) only
        // hits, from Gene's own motions: any enemy. An enemy without the shared table keeps its own punish.
        constexpr uint32_t kEm00Entries = 3998u;
        struct MotionSet
        {
            std::vector<uint16_t> entries;
            uint32_t area = 0u, room = 0u;   // guest place and its size
            std::vector<uint32_t> at;        // each entry's offset in the area
            std::vector<uint8_t> bytes;      // the area's contents (64-byte aligned entries)
        };
        MotionSet g_sets[kPickCount];
        bool g_keptLogged[0x80] = {}; // enemy numbers 0x200.. already logged as keeping their punish
        uint32_t g_filledPack[kPickCount] = {}; // the last table filled per pick (the log)

        std::vector<uint16_t> range(uint16_t first, uint16_t last)
        {
            std::vector<uint16_t> out;
            for (uint32_t i = first; i <= last; ++i)
                out.push_back(static_cast<uint16_t>(i));
            return out;
        }

        // At start (Buck Breaker on): the four punishes' entries from the disc's em/em00.dat.
        void loadMotionSets()
        {
            // victim 329-332, Gene 333-334 | victim 805-810, Gene 811-816 | victim 1543-1550, Gene 1551-1558 | victim
            // 1327-1332, the launch after it (mode 1 phase 35) 1333-1336, Gene 1337-1342.
            g_sets[Suplex].entries = range(329, 334);
            g_sets[Stinger].entries = range(805, 816);
            g_sets[CobraTwist].entries = range(1543, 1558);
            g_sets[Spanking].entries = range(1327, 1342);
            // Places fixed for good (a save state keeps a table pointing at them): +0x0..0x7000 is the first build's
            // Spanking (without the launch), not reused.
            const uint32_t places[kPickCount][2] = {
                {}, {}, {0x21000u, 0x9000u}, {0x7000u, 0x8000u}, {0xF000u, 0x9000u}, {0x18000u, 0x9000u}};
            for (int p = Spanking; p < kPickCount; ++p)
            {
                g_sets[p].area = kMotionArea + places[p][0];
                g_sets[p].room = places[p][1];
            }
            static_assert(kMotionArea + 0x21000u + 0x9000u <= kMotionAreaEnd, "the punishes' motions fit");
            const auto file = disc_files::open() ? disc_files::find("em/em00.dat") : std::nullopt;
            std::vector<uint8_t> dat;
            if (!file || !disc_files::read(*file, dat) || dat.size() < 4u + 8u * kEm00Entries)
            {
                KM_WARN(Game, "buck breaker: em/em00.dat not read; Spanking and Cobra Twist only where the room has them");
                return;
            }
            auto u32 = [&](size_t o) {
                uint32_t v;
                std::memcpy(&v, dat.data() + o, sizeof(v));
                return v;
            };
            if (u32(0) != kEm00Entries)
            {
                KM_WARN(Game, "buck breaker: em/em00.dat is not the one known (" << u32(0) << " entries)");
                return;
            }
            for (int p = Spanking; p < kPickCount; ++p)
            {
                MotionSet &set = g_sets[p];
                std::vector<uint8_t> bytes;
                std::vector<uint32_t> at;
                bool ok = true;
                for (uint16_t i : set.entries)
                {
                    const uint32_t begin = u32(4u + 4u * i);
                    uint32_t end = 0u;
                    for (uint32_t j = i + 1u; j < kEm00Entries && !end; ++j)
                        end = u32(4u + 4u * j);
                    if (!end)
                        end = static_cast<uint32_t>(dat.size());
                    const uint32_t type = u32(4u + 4u * kEm00Entries + 4u * i);
                    if (begin == 0u || end <= begin || end > dat.size() || (type != 0x00544F4Du && type != 0x00514553u))
                    {
                        ok = false; // not "MOT" / "SEQ" where they should be
                        break;
                    }
                    at.push_back(static_cast<uint32_t>(bytes.size()));
                    bytes.insert(bytes.end(), dat.begin() + begin, dat.begin() + end);
                    bytes.resize((bytes.size() + 0x3Fu) & ~size_t(0x3F), 0u);
                }
                if (!ok || bytes.size() > set.room)
                {
                    KM_WARN(Game, "buck breaker: " << kPickNames[p] << "'s motions not as expected in em00.dat; it works only where the room has them");
                    continue;
                }
                set.at = std::move(at);
                set.bytes = std::move(bytes);
            }
        }

        // The pick's motions in this enemy's table (filled in when the room lacks them); false: keep its own punish.
        bool ensureMotions(uint8_t *rdram, uint32_t enemy, int pick)
        {
            if (pick == Pummel)
                return true;
            const uint32_t pack = ram32(rdram, enemy + game::GameObj::kMotionData) & 0x1FFFFFFu;
            if (pack < 0x100000u || pack > 0x2000000u - 8u * kEm00Entries - 8u || ram32(rdram, pack) != kEm00Entries)
                return false;
            MotionSet &set = g_sets[pick];
            bool missing = false;
            for (uint16_t i : set.entries)
                missing = missing || ram32(rdram, pack + 4u + 4u * i) == 0u;
            if (!missing)
                return true;
            if (set.bytes.empty() || set.at.size() != set.entries.size())
                return false;
            if (std::memcmp(rdram + set.area, set.bytes.data(), set.bytes.size()) != 0)
                std::memcpy(rdram + set.area, set.bytes.data(), set.bytes.size());
            for (size_t k = 0; k < set.entries.size(); ++k)
            {
                const uint32_t slot = pack + 4u + 4u * set.entries[k];
                if (ram32(rdram, slot) == 0u)
                {
                    const uint32_t offset = set.area + set.at[k] - pack;
                    std::memcpy(rdram + slot, &offset, sizeof(offset));
                }
            }
            if (g_filledPack[pick] != pack)
            {
                g_filledPack[pick] = pack;
                KM_INFO(Game, "buck breaker: " << kPickNames[pick] << "'s motions added to the room's enemy motions (table 0x"
                                               << std::hex << pack << std::dec << ")");
            }
            return true;
        }

        // The pick's punish in punish_fit's list (-1: Pummel, which does not place Gene).
        int fitPunish(int pick)
        {
            switch (pick)
            {
            case Suplex:
                return punish_fit::Suplex;
            case Stinger:
                return punish_fit::Stinger;
            case CobraTwist:
                return punish_fit::CobraTwist;
            case Spanking:
                return punish_fit::Spanking;
            default:
                return -1;
            }
        }

        // Replaces sltiu v1,a0,0x7A at 0x2DD02C (func_002DCD50; a0 = enemy number - 0x205, s1 = the enemy): a generic
        // punish goes to the picked one's case.
        void punishCase(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            uint32_t index = GPR_U32(ctx, 4);
            const int pick = g_pick.load(std::memory_order_relaxed);
            if (pick > Normal && pick < kPickCount && tableOk(rdram))
            {
                const uint32_t was = index < kCases ? ram32(rdram, kCaseTable + 4u * index) : kSuplexCase;
                const uint32_t enemy = GPR_U32(ctx, 17);
                // F2 > Debug > Punish fit: a body marked "doesn't fit" for the pick keeps its own punish.
                const int fit = fitPunish(pick);
                if (genericCase(was) && !(fit >= 0 && punish_fit::doesntFit(rdram, enemy, fit)))
                {
                    if (ensureMotions(rdram, enemy, pick))
                        index = kPickIndex[pick];
                    else if (const uint32_t number = index + 0x205u - 0x200u; number < 0x80u && !g_keptLogged[number])
                    {
                        g_keptLogged[number] = true;
                        KM_INFO(Game, "buck breaker: enemy 0x" << std::hex << (number + 0x200u) << std::dec << " keeps its own punish ("
                                                              << kPickNames[pick] << " needs the generic enemies' motions)");
                    }
                }
            }
            SET_GPR_U32(ctx, 4, index);
            SET_GPR_U32(ctx, 3, index < kCases ? 1u : 0u);
        }

        // ---- the picks in the save ----
        // cCoreSaveData is 0x14A0 bytes; the game uses up to +0xBC3, style_presets +0xC00..+0x1440 (its styles and its
        // strings). Ours: +0x1480, 16 bytes, with a magic and a checksum (a new game's memset or another tool's bytes read
        // as "all Normal"). The checkpoint slot (a whole copy of the record taken at every room change and put back on
        // Continue) gets every change too, as style_presets does.
        constexpr uint32_t kCoreSave = 0x569B70u; // cCoreSave D_00569B70: { cCoreSaveData *data; }
        constexpr uint32_t kCheckpointPointer = 0x3C264Cu;
        constexpr uint32_t kSaveSize = game::CoreSaveData::kSize;
        constexpr uint32_t kSkills = game::CoreSaveData::kSkill;
        constexpr uint32_t kBlockOffset = 0x1480u;
        constexpr uint32_t kBlockMagic = 0x42424D4Bu; // "KMBB"
        constexpr uint8_t kBlockVersion = 1u;
        constexpr int kPositions = 5; // None (or styles off), S, E, X, Y
        struct Block
        {
            uint32_t magic;
            uint8_t version;
            uint8_t pick[kPositions];
            uint8_t reserved[2];
            uint32_t checksum;
        };
        static_assert(sizeof(Block) == 16u, "Block layout");
        static_assert(kBlockOffset >= 0x1440u && kBlockOffset + sizeof(Block) <= kSaveSize, "Block in the record's free tail");

        uint32_t checksumOf(const Block &block)
        {
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&block);
            uint32_t sum = 0x9E3779B9u;
            for (size_t i = 0; i < offsetof(Block, checksum); ++i)
                sum = (sum ^ bytes[i]) * 0x01000193u;
            return sum;
        }

        uint32_t rd32(uint32_t address) { return ram32(g_ram, address); }
        void wr32(uint32_t address, uint32_t value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        uint64_t rd64(uint32_t address)
        {
            uint64_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        void wr64(uint32_t address, uint64_t value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        uint16_t rd16(uint32_t address)
        {
            uint16_t v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        void wr16(uint32_t address, uint16_t value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        float rdFloat(uint32_t address)
        {
            float v;
            std::memcpy(&v, g_ram + (address & 0x1FFFFFFu), sizeof(v));
            return v;
        }
        void wrFloat(uint32_t address, float value) { std::memcpy(g_ram + (address & 0x1FFFFFFu), &value, sizeof(value)); }
        uint8_t &byteAt(uint32_t address) { return g_ram[address & 0x1FFFFFFu]; }

        bool validPointer(uint32_t p, uint32_t room = 0x100u)
        {
            p &= 0x1FFFFFFu;
            return p >= 0x100000u && room < 0x2000000u && p < 0x2000000u - room;
        }

        uint32_t dataPointer()
        {
            const uint32_t data = rd32(kCoreSave);
            return validPointer(data, kSaveSize) ? (data & 0x1FFFFFFu) : 0u;
        }

        void readBlock(uint32_t data, Block &block)
        {
            std::memcpy(&block, g_ram + data + kBlockOffset, sizeof(Block));
            if (block.magic == kBlockMagic && block.version == kBlockVersion && block.checksum == checksumOf(block))
            {
                for (uint8_t &p : block.pick)
                    if (p >= kPickCount)
                        p = Normal;
                return;
            }
            block = Block{};
            block.magic = kBlockMagic;
            block.version = kBlockVersion;
        }

        void writeBlock(uint32_t data, Block &block)
        {
            block.checksum = checksumOf(block);
            std::memcpy(g_ram + data + kBlockOffset, &block, sizeof(Block));
            // The checkpoint slot, when it holds this game's record: every move it owns is owned now (moves are only
            // ever gained), as style_presets checks.
            const uint32_t checkpoint = rd32(kCheckpointPointer) & 0x1FFFFFFu;
            if (validPointer(checkpoint, kSaveSize) && checkpoint != data)
            {
                const int8_t *was = reinterpret_cast<const int8_t *>(g_ram + checkpoint + kSkills);
                const int8_t *now = reinterpret_cast<const int8_t *>(g_ram + data + kSkills);
                bool sameGame = true;
                for (int id = 0; id < 0x80 && sameGame; ++id)
                    sameGame = was[id] < 0 || now[id] >= 0;
                if (sameGame)
                    std::memcpy(g_ram + checkpoint + kBlockOffset, &block, sizeof(Block));
            }
        }

        // 0 None (or Style switching off), 1..4 the style on.
        int position()
        {
            const int at = style_presets::activePosition();
            return at >= 0 && at < kPositions ? at : 0;
        }

        // ---- the Techniques screen's slot ----
        // cSubScrWaza (sub screen kind 1, at cSubScr +0x310): +0x5A state (0 browsing), +0x25C cursor row (s8, -1 =
        // RETURN), +0x284 column (1 = the single slots), parts[id] at +0x80 (the layout ids of id/local/us/
        // idsubscrwaza_us.idd's first IDP). The single slots' rows: name text parts[0x13 + row] (its children: the box,
        // record id -1, and the button parts[0x1A + row], whose child is the "down+" art), the cursor frame parts[1]
        // (placed from the table 0x420000: (x, y) per row, 8 bytes, 0x30 a column; column 1 has a sixth row, (195, 230)),
        // the RETURN frame parts[0x18]. Our slot: the Down + A row (4) drawn again one row lower (a second cIDBase_trans
        // with every other entry NO_DRAW), its name our message, its button the icon sheet's circle cell. The game's
        // cursor stays on row 4 while ours is on the slot; for the screen's update it reads -1 (RETURN's way: no row
        // lit, Gene stands, no move info), then the frame is put on the slot and RETURN's frame hidden.
        constexpr uint32_t kSubScrPointer = game::SubScr::kPointer;
        constexpr int32_t kTechniquesKind = 1;
        constexpr uint32_t kWazaOffset = 0x310u, kWazaState = 0x5Au, kWazaCursor = 0x25Cu, kWazaColumn = 0x284u;
        constexpr uint32_t kSlotsLayout = 0x290u; // the screen's third layout: its move is the update's last call
        constexpr int kNameRow3 = 0x16, kNameRow4 = 0x17, kButtonRow4 = 0x1E, kCursorFrame = 0x01, kReturnFrame = 0x18;
        constexpr int8_t kRow4 = 4;
        constexpr uint32_t kCursorTable = 0x420000u;
        constexpr uint32_t kSlotPlace = kCursorTable + 0x30u + 5u * 8u; // column 1, row 5
        constexpr uint32_t kInfoRecord = 53u;                             // "Attack Str" (id -1: by its index)
        constexpr float kInfoX = 395.0f, kInfoY = 353.0f;                 // its record's position (the check)
        // Entry fields (cIDBase.h): +0x1C src, +0x20 parent, +0x2A id, +0x38 / +0x3C position, +0x70 f70[4] (a
        // texture entry's uv rectangle: u0, v0, u1, v1).
        constexpr uint32_t kEntSrc = 0x1Cu, kEntParent = 0x20u, kEntId = 0x2Au, kEntPosX = 0x38u, kEntPosY = 0x3Cu;
        constexpr uint32_t kEntUv = 0x70u;
        constexpr uint32_t kNoDraw = game::kIdentFlagNoDraw;
        // The slots' buttons come from the text icon sheet's top row (0x41FFA0: square 0.5, triangle 0.25, cross 0.75);
        // its first cell is circle, which the prompt sets draw as the button bound to circle (B on Xbox pads).
        constexpr float kCircleUv[4] = {0.0f, 0.0f, 0.25f, 0.25f};
        constexpr uint8_t kBright = 2u, kDim = 3u; // the screen's name colour sets (lit / not)
        constexpr uint16_t kSlotMessage = 0x3044u;  // a "<7d>" placeholder of the core bank (style_presets uses others)
        constexpr uint32_t kSlotText = 0x000FFC00u; // guest scratch below the ELF (tutorial_text.cpp keeps the map)
        constexpr uint32_t kSlotTextRoom = 0x100u;
        constexpr uint32_t kMessageBankPointer = 0x3C23A4u; // *(*(u32 *)0x3C23A4 + 0xC) = the core message bank
        constexpr uint16_t kSpace = 0x10u, kEnd = 0x8001u;
        // The menu command words (GameWork +0x1A0 pressed, +0x1B0 repeat), as the screen's input 0x16DF38 tests them:
        // down / up on the repeat word, left / right, OK (bit 28) and Cancel (bit 29) on the pressed one. The menu
        // buttons setting (Western / Japanese / Classic) is already in them.
        constexpr uint64_t kCmdDown = ((0x8000ull << 24) | 0x8000ull) << 10;
        constexpr uint64_t kCmdUp = ((0x8000ull << 24) | 0x8000ull) << 9;
        constexpr uint64_t kCmdSide = ((0xC000ull << 24) | 0xC000ull) << 12;
        constexpr uint64_t kCmdOk = 0x10000000ull, kCmdCancel = 0x20000000ull;
        constexpr uint32_t kSeCall = 0x2CB8A0u, kCursorSound = 0x15Fu; // the screen's cursor sound (0x16DFC4)
        constexpr uint32_t kTrans = 0x2AAC28u;                          // cIDBase_trans

        bool g_onSlot = false;   // the cursor is on our slot (the game's stays on Down + A)
        uint32_t g_faked = 0u;   // the screen whose cursor reads -1 for its update (put back at its last move call)
        uint32_t g_hidInfo = 0u; // "Attack Str", hidden by us while on the slot

        uint32_t techniquesScreen()
        {
            const uint32_t subScr = rd32(kSubScrPointer) & 0x1FFFFFFu;
            if (!validPointer(subScr, 0x1000u) || static_cast<int32_t>(rd32(subScr + game::SubScr::kKind)) != kTechniquesKind)
                return 0u;
            return subScr + kWazaOffset;
        }
        uint32_t part(uint32_t waza, int id) { return rd32(waza + 0x80u + 4u * static_cast<uint32_t>(id)) & 0x1FFFFFFu; }
        int8_t cursorOf(uint32_t waza) { return static_cast<int8_t>(byteAt(waza + kWazaCursor)); }

        bool callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address, std::initializer_list<uint32_t> args)
        {
            if (!runtime || !runtime->hasFunction(address))
                return false;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return false;
            const R5900Context saved = *ctx;
            int reg = 4;
            for (uint32_t a : args)
            {
                if (reg > 11)
                    break;
                SET_GPR_U32(ctx, reg, a);
                ++reg;
            }
            ctx->pc = address; // a hooked function finds its hook by the pc (game_options.cpp callGuest)
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            *ctx = saved;
            return true;
        }

        void cursorSound(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            callGuest(rdram, ctx, runtime, kSeCall, {0x5FEE00u, 0u, kCursorSound, 0xFFFFFFFFu, 0xFFFFFFFFu, 0u, 0u});
        }

        // "Attack Str" (entries[53], checked by its record's position), when the screen still has it.
        uint32_t infoEntry(uint32_t waza)
        {
            const uint32_t entries = rd32(waza + game::IDBase::kEnt) & 0x1FFFFFFu;
            const uint32_t count = rd32(waza + game::IDBase::kEntNum);
            if (count <= kInfoRecord || count > 400u || !validPointer(entries, count * ent::kSize))
                return 0u;
            const uint32_t info = entries + kInfoRecord * ent::kSize;
            const uint32_t src = rd32(info + kEntSrc) & 0x1FFFFFFu;
            if (!validPointer(src, 0x10u) || rdFloat(src + 0x08u) != kInfoX || rdFloat(src + 0x0Cu) != kInfoY)
                return 0u;
            return info;
        }

        void showInfo(uint32_t waza)
        {
            if (g_hidInfo && waza && infoEntry(waza) == g_hidInfo)
                wr32(g_hidInfo + ent::kFlags, rd32(g_hidInfo + ent::kFlags) & ~kNoDraw);
            g_hidInfo = 0u;
        }

        void leaveSlot(uint32_t waza)
        {
            g_onSlot = false;
            showInfo(waza);
        }

        void unfake()
        {
            if (!g_faked)
                return;
            if (cursorOf(g_faked) == -1)
                byteAt(g_faked + kWazaCursor) = static_cast<uint8_t>(kRow4);
            g_faked = 0u;
        }

        // The screen's update (0x16C730, $a0 = the screen): on our slot its cursor reads -1.
        void onWazaUpdate(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_ram)
                return;
            const uint32_t waza = GPR_U32(ctx, 4) & 0x1FFFFFFu;
            if (waza != techniquesScreen())
                return;
            unfake();
            if (!g_onSlot)
                return;
            if (byteAt(waza + kWazaState) != 0u || byteAt(waza + kWazaColumn) != 1u || cursorOf(waza) != kRow4)
            {
                leaveSlot(waza);
                return;
            }
            byteAt(waza + kWazaCursor) = 0xFFu;
            g_faked = waza;
        }

        // cIDBase_move (0x2AAA40, $a0 = the layout): the screen's main layout (its update's first move call) gets the
        // cursor frame on our slot; the slots layout (its last call) gives the cursor back.
        void onLayoutMove(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_faked)
                return;
            const uint32_t layout = GPR_U32(ctx, 4) & 0x1FFFFFFu;
            if (layout == g_faked + kSlotsLayout)
            {
                unfake();
                return;
            }
            if (layout != g_faked)
                return;
            const uint32_t waza = layout;
            const uint32_t frame = part(waza, kCursorFrame), back = part(waza, kReturnFrame);
            if (validPointer(frame, ent::kSize))
            {
                wr32(frame + ent::kFlags, rd32(frame + ent::kFlags) & ~kNoDraw);
                wrFloat(frame + kEntPosX, rdFloat(kSlotPlace));
                wrFloat(frame + kEntPosY, rdFloat(kSlotPlace + 4u));
            }
            if (validPointer(back, ent::kSize))
                wr32(back + ent::kFlags, rd32(back + ent::kFlags) | kNoDraw);
            if (!g_hidInfo)
                if (const uint32_t info = infoEntry(waza); info && (rd32(info + ent::kFlags) & kNoDraw) == 0u)
                {
                    wr32(info + ent::kFlags, rd32(info + ent::kFlags) | kNoDraw);
                    g_hidInfo = info;
                }
        }

        void cycle()
        {
            const uint32_t data = dataPointer();
            if (!data)
                return;
            Block block;
            readBlock(data, block);
            const int at = position();
            block.pick[at] = static_cast<uint8_t>((block.pick[at] + 1) % kPickCount);
            writeBlock(data, block);
            g_pick.store(block.pick[at], std::memory_order_relaxed);
            static const char kLetters[kPositions + 1] = "-SEXY";
            KM_INFO(Game, "buck breaker: punish " << kPickNames[block.pick[at]]
                                                   << (at ? std::string(" (style ") + kLetters[at] + ")" : std::string(" (base)")));
        }

        // The screen's input (0x16DF38, $a0 = the screen), before it reads the command words: Down on Down + A (and Up
        // on RETURN in the single slots' column, and left / right on the chain's sixth row) goes to our slot; on it, OK
        // cycles the punish, Up goes back to Down + A, and Down / left / right / Cancel do what they do on Down + A
        // (RETURN, the square chain, leaving).
        void onWazaInput(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_ram)
                return;
            const uint32_t waza = GPR_U32(ctx, 4) & 0x1FFFFFFu;
            if (waza != techniquesScreen())
                return;
            unfake();
            if (byteAt(waza + kWazaState) != 0u)
            {
                if (g_onSlot)
                    leaveSlot(waza);
                return;
            }
            const uint32_t trgAt = work::address(work::kTrg), repAt = work::address(work::kRep);
            uint64_t trg = rd64(trgAt), rep = rd64(repAt);
            const uint8_t column = byteAt(waza + kWazaColumn);
            const int8_t row = cursorOf(waza);
            const bool onDownA = column == 1u && row == kRow4;
            bool sound = false;
            if (!g_onSlot)
            {
                if (onDownA && (rep & kCmdDown))
                {
                    g_onSlot = true;
                    rep &= ~kCmdDown;
                    sound = true;
                }
                else if (column == 1u && row == -1 && (rep & kCmdUp))
                    g_onSlot = true; // RETURN -> up: the game goes to Down + A (its sound), the cursor shows on the slot
                else if (column == 0u && row == 5 && (trg & kCmdSide))
                {
                    byteAt(waza + kWazaCursor) = static_cast<uint8_t>(kRow4); // the chain's sixth row faces the slot
                    g_onSlot = true;                                         // (the game had nothing there: RETURN)
                }
            }
            else if (!onDownA)
                leaveSlot(waza);
            else if (trg & kCmdOk)
            {
                cycle();
                trg &= ~kCmdOk;
                sound = true;
            }
            else if (rep & kCmdUp)
            {
                leaveSlot(waza);
                rep &= ~kCmdUp;
                sound = true;
            }
            else if ((rep & kCmdDown) || (trg & (kCmdSide | kCmdCancel)))
                leaveSlot(waza);
            wr64(trgAt, trg);
            wr64(repAt, rep);
            if (sound)
                cursorSound(rdram, ctx, runtime);
        }

        uint16_t glyph(char c)
        {
            if (c >= 'A' && c <= 'Z')
                return static_cast<uint16_t>(0x40 + (c - 'A'));
            if (c >= 'a' && c <= 'z')
                return static_cast<uint16_t>(0x5A + (c - 'a'));
            if (c == ':')
                return 0x31u;
            return kSpace;
        }

        bool pointMessage(uint16_t id, uint32_t text)
        {
            const uint32_t holder = rd32(kMessageBankPointer);
            if (!validPointer(holder))
                return false;
            const uint32_t bank = rd32((holder & 0x1FFFFFFu) + 0xCu);
            if (!validPointer(bank, 0x1000u))
                return false;
            const uint32_t word = bank + (static_cast<uint32_t>(id) - 0x3000u) * 8u + 4u;
            if (rd32(word) != text - bank)
                wr32(word, text - bank);
            return true;
        }

        bool sameEntry(uint32_t a, uint32_t b) { return (a & 0x1FFFFFFu) == (b & 0x1FFFFFFu); }

        // The screen's draw (func_0016DC18, $s4 = the screen), right after its main layout's cIDBase_trans (0x16DD78):
        // our slot, drawn on its own after the rows, as one more row would be.
        void onSlotDraw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (!g_ram)
                return;
            const uint32_t waza = GPR_U32(ctx, 20) & 0x1FFFFFFu;
            if (waza != techniquesScreen())
                return;
            unfake();
            const uint32_t data = dataPointer();
            const uint32_t entries = rd32(waza + game::IDBase::kEnt) & 0x1FFFFFFu;
            const uint32_t count = rd32(waza + game::IDBase::kEntNum);
            if (!data || count == 0u || count > 400u || !validPointer(entries, count * ent::kSize))
                return;
            auto inLayout = [&](uint32_t e) {
                return e >= entries && e < entries + count * ent::kSize && (e - entries) % ent::kSize == 0u;
            };
            const uint32_t name = part(waza, kNameRow4), above = part(waza, kNameRow3), button = part(waza, kButtonRow4);
            if (!inLayout(name) || !inLayout(above) || !inLayout(button) || byteAt(name + ent::kKind) != game::kIdentKindText)
                return;
            uint32_t box = 0u;
            for (uint32_t i = 0; i < count && !box; ++i)
            {
                const uint32_t e = entries + i * ent::kSize;
                if (e != button && sameEntry(rd32(e + kEntParent), name) && static_cast<int8_t>(byteAt(e + kEntId)) == -1 &&
                    byteAt(e + ent::kKind) != game::kIdentKindText)
                    box = e;
            }
            if (!box)
                return;
            const float dx = rdFloat(name + ent::kDrawX) - rdFloat(above + ent::kDrawX);
            const float dy = rdFloat(name + ent::kDrawY) - rdFloat(above + ent::kDrawY);
            if (!(dx > -2.0f && dx < 2.0f && dy > 10.0f && dy < 100.0f))
                return; // not the layout we know: no slot rather than a misplaced one

            Block block;
            readBlock(data, block);
            const int pick = block.pick[position()];
            std::vector<uint16_t> text;
            for (const char *c = "Punish: "; *c; ++c)
                text.push_back(glyph(*c));
            for (const char *c = kPickNames[pick]; *c; ++c)
                text.push_back(glyph(*c));
            text.push_back(kEnd);
            if (text.size() * sizeof(uint16_t) > kSlotTextRoom)
                return;
            std::memcpy(g_ram + kSlotText, text.data(), text.size() * sizeof(uint16_t));
            if (!pointMessage(kSlotMessage, kSlotText))
                return;

            // Everything but the slot's three entries and the name's parents (the screen's own NO_DRAW / CHILD on the
            // column's group still decides, e.g. the picker hides the other column) is NO_DRAW for the second pass.
            std::vector<uint32_t> keep{name, box, button};
            for (uint32_t p = rd32(name + kEntParent) & 0x1FFFFFFu; p && inLayout(p) && keep.size() < 24u;
                 p = rd32(p + kEntParent) & 0x1FFFFFFu)
                keep.push_back(p);
            std::vector<uint32_t> flags(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t e = entries + i * ent::kSize;
                flags[i] = rd32(e + ent::kFlags);
                bool kept = false;
                for (uint32_t k : keep)
                    kept = kept || k == e;
                if (!kept)
                    wr32(e + ent::kFlags, flags[i] | kNoDraw);
            }
            struct Was
            {
                uint32_t entry;
                float x, y;
            };
            const Was places[3] = {{name, rdFloat(name + ent::kDrawX), rdFloat(name + ent::kDrawY)},
                                   {box, rdFloat(box + ent::kDrawX), rdFloat(box + ent::kDrawY)},
                                   {button, rdFloat(button + ent::kDrawX), rdFloat(button + ent::kDrawY)}};
            const uint16_t msg = rd16(name + ent::kMsg);
            const uint8_t colourSet = byteAt(name + ent::kColourSet);
            float uv[4];
            for (int i = 0; i < 4; ++i)
                uv[i] = rdFloat(button + kEntUv + 4u * static_cast<uint32_t>(i));

            for (const Was &w : places)
            {
                wrFloat(w.entry + ent::kDrawX, w.x + dx);
                wrFloat(w.entry + ent::kDrawY, w.y + dy);
            }
            wr16(name + ent::kMsg, kSlotMessage);
            byteAt(name + ent::kColourSet) = g_onSlot ? kBright : kDim;
            for (int i = 0; i < 4; ++i)
                wrFloat(button + kEntUv + 4u * static_cast<uint32_t>(i), kCircleUv[i]);

            callGuest(rdram, ctx, runtime, kTrans, {waza});

            for (const Was &w : places)
            {
                wrFloat(w.entry + ent::kDrawX, w.x);
                wrFloat(w.entry + ent::kDrawY, w.y);
            }
            wr16(name + ent::kMsg, msg);
            byteAt(name + ent::kColourSet) = colourSet;
            for (int i = 0; i < 4; ++i)
                wrFloat(button + kEntUv + 4u * static_cast<uint32_t>(i), uv[i]);
            for (uint32_t i = 0; i < count; ++i)
                wr32(entries + i * ent::kSize + ent::kFlags, flags[i]);
        }

        // Each vblank (EE thread): the pick in force, and the slot let go when the screen is not browsing.
        void onVblank()
        {
            const uint32_t data = dataPointer();
            int pick = Normal;
            if (data)
            {
                Block block;
                readBlock(data, block);
                pick = block.pick[position()];
            }
            g_pick.store(pick, std::memory_order_relaxed);
            if (g_onSlot || g_hidInfo)
            {
                const uint32_t waza = techniquesScreen();
                if (!waza)
                {
                    g_onSlot = false;
                    g_hidInfo = 0u; // the layout went with the screen
                }
                else if (byteAt(waza + kWazaState) != 0u)
                    leaveSlot(waza);
            }
        }

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v && *v != '0';
        }
    }

    void styleCreated(int from, int to)
    {
        if (!g_ram || from < 0 || from >= kPositions || to < 0 || to >= kPositions || from == to)
            return;
        const uint32_t data = dataPointer();
        if (!data)
            return;
        Block block;
        readBlock(data, block);
        if (block.pick[to] == block.pick[from])
            return;
        block.pick[to] = block.pick[from];
        writeBlock(data, block);
    }

    void install(PS2Runtime &runtime)
    {
        // RDRAM even when off: a style first visited still takes its punish (styleCreated), for when it is turned on.
        g_ram = runtime.memory().getRDRAM();
        ps2xRegisterHostHook("km_buck_breaker_case", &punishCase);
        ps2xRegisterHostHook("km_buck_breaker_slot", &onSlotDraw);
        const bool on = g_ram && envOn("KAMITE_BUCK_BREAKER");
        ps2xSetPatchFlag("buck_breaker", on);
        if (!on)
        {
            KM_INFO(Game, "buck breaker: off ([Game] BuckBreaker=0)");
            return;
        }
        loadMotionSets();
        ps2x::pnach::addFrameHook(&onVblank);
        FunctionHooks &hooks = FunctionHooks::instance();
        hooks.observeEntry(0x16C730u, "cSubScrWaza update (Buck Breaker)", &onWazaUpdate);
        hooks.observeEntry(0x16DF38u, "cSubScrWaza input (Buck Breaker)", &onWazaInput);
        hooks.observeEntry(0x2AAA40u, "cIDBase_move (Buck Breaker)", &onLayoutMove);
        KM_INFO(Game, "buck breaker: on (punish slot on the Techniques screen)");
    }
}
