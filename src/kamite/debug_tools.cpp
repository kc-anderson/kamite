// Dev tools in the F2 menu's Debug tab: the game's display switches and a room jump (dev-only: strip-list).
//
// Display switches. GameWork +0x5E0 (0x747A80) is the preview's DSP group (game::Dsp): one bit per part of the game,
// and a SET bit HIDES that part (the dog race and the jukebox set them to hide the world and Gene, the door jump sets
// DSP_ACTBTN to hide the prompt; func_002BF708 sets them all). Retail still reads most of them: Gene (0x1020F8,
// 0x107F28), the enemies (12 enemy draw functions), the objects (0x1B80C0, IsTargetVisible_14B470), the stage
// (0x319848), the screen filters (classCFILTER_TAG_Filter_main), the shadows (cObjBase_KageDraw), the coat
// (IsTargetVisibleOrForced_1004C8), the action-button prompt (0x1F7360). A ticked box sets its bit at every frame
// start; untick it and the bit gets the value the game last gave it (a clear by the game while hidden counts; a set
// by the game meanwhile is missed, so a part the game hid meanwhile may show again: a dev tool).
//
// Room jump. RoomJmpTbl.dat (etc/debugres, loaded by retail into D_00747A44 for the casino's jump points): u32 group
// count, u32 group table offset, u32 record offset; groups of (u16 room, u8 count, u8 first); 0x30-byte records of
// position (metres), heading (radians when it fits -PI..PI, else degrees: cRoomJump_setNextPos) and the developers'
// Shift-JIS name. The jump is the game's forced door jump func_001FFF30 (cScenario_beginRoomJumpForce's body:
// a0 = the door D_00583EC0, a1 = the room id, a2 = the landing position, f12 = the heading, a3 = arg, t0 = the fade
// (1 white, 2 black), t1 = doorArg), the one func_002BF568 uses to restart a room where Gene stands. Only rooms whose
// module is on the retail disc are listed (the table also has the developers' test rooms). The landing point's story
// flags are not set: saving after a jump can leave the save in a state the game never makes.
#include "kamite/debug_tools.h"
#include "kamite/bone_tweaks.h"
#include "kamite/disc_files.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h" // GPR_U32 / SET_GPR_U32
#include "runtime/ps2_memory.h"

#include "runtime/ps2_game_menu.h"
#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kamite::debug_tools
{
    namespace
    {
        using namespace kamite::game;

        constexpr uint32_t kForcedDoorJump = 0x001FFF30u; // func_001FFF30 (cScenario_beginRoomJumpForce's body)
        constexpr uint32_t kDoor = 0x00583EC0u;           // D_00583EC0, the door jump's cDoor
        constexpr uint32_t kLanding = 0x000FFF00u;        // 16 bytes of guest scratch for the landing position
                                                          // (below the ELF, past npc_poses' slots, unused by the game)
        constexpr uint32_t kFadeBlack = 2u;
        constexpr float kPi = 3.1415927f;

        struct Switch
        {
            uint32_t bit;
            const char *label;
        };
        constexpr Switch kSwitches[] = {
            {Dsp::kPl, "Gene (DSP_PL)"},
            {Dsp::kPl01, "Gene's coat (DSP_PL01)"},
            {Dsp::kEm, "Enemies and NPCs (DSP_EM)"},
            {Dsp::kOm, "Objects (DSP_OM)"},
            {Dsp::kObj, "Game objects (DSP_OBJ)"},
            {Dsp::kScr, "Stage (DSP_SCR)"},
            {Dsp::kEff, "Effects (DSP_EFF)"},
            {Dsp::kEmitter, "Emitters (DSP_EMITTER)"},
            {Dsp::kKage, "Shadows (DSP_KAGE)"},
            {Dsp::kFilter, "Screen filters: blur, haze (DSP_FILTER)"},
            {Dsp::kWorldLight, "World light (DSP_WORLD_LIGHT)"},
            {Dsp::kCock, "HUD (DSP_COCK)"},
            {Dsp::kActBtn, "Action-button prompt (DSP_ACTBTN)"},
        };

        std::atomic<uint32_t> g_hide{0};   // the bits to keep set = hidden (UI thread writes, game thread reads)
        uint32_t g_hiding = 0;             // game thread: the bits it is keeping set now
        uint32_t g_gameWants = 0;          // game thread: of those, the ones the game had set

        struct Point
        {
            uint16_t room = 0;
            uint8_t index = 0;
            float pos[3] = {};
            float heading = 0.0f; // radians
            std::string label;
        };
        std::vector<Point> g_points;
        std::vector<std::string> g_labels; // for the combo
        std::atomic<int> g_request{-1};     // a point to jump to, taken at the next frame start
        int g_selected = 0;                 // UI thread
        std::mutex g_statusMutex;
        std::string g_status;
        std::string g_hands;
        PS2Runtime::RecompiledFunction g_jump = nullptr;
        uint16_t g_room = 0xFFFFu;          // the room Gene is in (game thread writes)

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t address, uint32_t v) { std::memcpy(rdram + (address & 0x01FFFFFFu), &v, 4); }
        bool inRam(uint32_t address, uint32_t size)
        {
            const uint32_t a = address & 0x1FFFFFFFu;
            return a >= 0x100000u && a + size <= PS2_RAM_SIZE;
        }

        // The developers' names (Shift-JIS in the table), in English. "なまえをいれてください" ("please enter a
        // name") and points without a name here are "(unnamed)".
        const char *nameOf(uint16_t room, uint8_t index)
        {
            struct Name
            {
                uint16_t room;
                uint8_t index;
                const char *name;
            };
            static constexpr Name kNames[] = {
                {0x000, 0, "Casino entrance"}, {0x001, 0, "Before the casino door"}, {0x002, 0, "Meeting room entrance"},
                {0x003, 0, "Test point"}, {0x005, 0, "Second floor entrance"}, {0x006, 0, "Third floor entrance"},
                {0x040, 0, "Ring 1"}, {0x040, 1, "Ring 2"}, {0x040, 2, "Ring 3"},
                {0x100, 0, "Starting place"}, {0x100, 1, "Second half"}, {0x101, 0, "Elvis's mansion entrance"},
                {0x102, 0, "Western town entrance"}, {0x102, 1, "Western town, before the boss"}, {0x102, 2, "Test point"},
                {0x104, 0, "Elvis's room"}, {0x105, 0, "Western cliff"}, {0x105, 1, "In front of the inn"},
                {0x105, 2, "Cliff"}, {0x110, 0, "Start"},
                {0x200, 0, "Start"}, {0x201, 0, "Start"}, {0x201, 1, "Second half start"}, {0x202, 0, "Start"},
                {0x203, 0, "Start"}, {0x204, 0, "Event square"}, {0x205, 0, "Start"}, {0x206, 0, "Start"}, {0x207, 0, "Start"},
                {0x300, 0, "Merry-go-round square entrance"}, {0x301, 1, "Cannon game"}, {0x304, 1, "Before the attraction"},
                {0x400, 0, "Drill room"}, {0x401, 0, "Beginning"}, {0x402, 0, "Event room"}, {0x403, 0, "Mid-boss room"},
                {0x404, 0, "Press room"}, {0x405, 0, "Steel bridge room"}, {0x405, 1, "Cage"}, {0x406, 0, "Devil Hand room"},
                {0x407, 0, "Lift room"}, {0x408, 0, "Box-throwing room"}, {0x409, 0, "Cage room"},
                {0x500, 0, "Water city"}, {0x501, 0, "Signboard"}, {0x502, 0, "Floodgate"}, {0x503, 0, "Entrance"},
                {0x504, 0, "Church"}, {0x505, 0, "Before the boss room"}, {0x506, 0, "The band"}, {0x507, 0, "Back alley"},
                {0x508, 0, "Start"}, {0x509, 0, "Harbour town 2"},
                {0x600, 0, "Stepping stones"}, {0x601, 0, "Drop point"}, {0x601, 1, "Uphill slope"}, {0x602, 0, "Boss room"},
                {0x603, 0, "Showdown: the sentai Five"}, {0x604, 0, "Before the lift"}, {0x605, 0, "Showdown: Psycho Chibi"},
                {0x606, 0, "Cave 1"}, {0x607, 0, "Cave 2"}, {0x608, 0, "Drawbridge"},
                {0x700, 0, "Desert"}, {0x701, 0, "Mirage town"}, {0x702, 0, "Pyramid"}, {0x704, 0, "Boss room"},
                {0x800, 0, "Tower entrance"}, {0x801, 0, "Satan's room"}, {0x802, 0, "Mirage town, second half"},
                {0x803, 0, "Boss entrance 1"}, {0x804, 0, "Boss entrance 2"}, {0x805, 0, "Boss entrance 3"},
                {0x806, 0, "Tower top"},
            };
            for (const Name &n : kNames)
                if (n.room == room && n.index == index)
                    return n.name;
            return "(unnamed)";
        }

        bool roomModuleExists(uint16_t room)
        {
            char path[48];
            std::snprintf(path, sizeof(path), "dll/debugoptimize/r%x%02x.rel", (room >> 8) & 0xF, room & 0xFF);
            return disc_files::find(path).has_value();
        }

        void loadPoints()
        {
            if (!disc_files::open())
                return;
            const auto file = disc_files::find("etc/debugres/roomjmptbl.dat");
            std::vector<uint8_t> d;
            if (!file || !disc_files::read(*file, d) || d.size() < 16)
                return;
            uint32_t groups = 0, groupOffset = 0, recordOffset = 0;
            std::memcpy(&groups, d.data(), 4);
            std::memcpy(&groupOffset, d.data() + 4, 4);
            std::memcpy(&recordOffset, d.data() + 8, 4);
            for (uint32_t g = 0; g < groups && groupOffset + 4 * g + 4 <= d.size(); ++g)
            {
                uint16_t room = 0;
                std::memcpy(&room, d.data() + groupOffset + 4 * g, 2);
                const uint8_t count = d[groupOffset + 4 * g + 2], first = d[groupOffset + 4 * g + 3];
                if (!roomModuleExists(room))
                    continue;
                for (uint8_t k = 0; k < count; ++k)
                {
                    const size_t r = recordOffset + 0x30u * (first + k);
                    if (r + 16 > d.size())
                        break;
                    Point p;
                    p.room = room;
                    p.index = k;
                    std::memcpy(p.pos, d.data() + r, 12);
                    float w = 0.0f;
                    std::memcpy(&w, d.data() + r + 12, 4);
                    p.heading = (w < -kPi || w > kPi) ? w * (kPi / 180.0f) : w;
                    char label[128];
                    std::snprintf(label, sizeof(label), "r%x%02x  %s %s%s", (room >> 8) & 0xF, room & 0xFF,
                                  ((room >> 8) & 0xF) == 0 ? "Casino & rings:" : ("Stage " + std::to_string((room >> 8) & 0xF) + ":").c_str(),
                                  nameOf(room, k), count > 1 ? (" (#" + std::to_string(k + 1) + ")").c_str() : "");
                    p.label = label;
                    g_points.push_back(std::move(p));
                }
            }
            for (const Point &p : g_points)
                g_labels.push_back(p.label);
        }

        // Gene in play: a player, no cutscene, door jump, talk, soft event or sub screen, the player not switched off.
        bool canJump(const uint8_t *rdram, std::string &why)
        {
            const uint32_t player = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
            if (!inRam(player, 0x700u))
            {
                why = "No Gene in this room.";
                return false;
            }
            const uint32_t sta = rd32(rdram, GameWork::address(GameWork::kFlags5E4));
            if (sta & (Sta::kEvent | Sta::kRoomJump | Sta::kTalk | Sta::kSoftEvent | Sta::kSubScr))
            {
                why = "Not now: a cutscene, a door, a talk or a menu is running.";
                return false;
            }
            if (rd32(rdram, GameWork::address(GameWork::kFlags5D8)) & kFlags5D8PlayerOff)
            {
                why = "Not now: the game has the player switched off.";
                return false;
            }
            const uint32_t subScr = rd32(rdram, SubScr::kPointer);
            if (inRam(subScr, 0x60u) && static_cast<int32_t>(rd32(rdram, subScr + SubScr::kKind)) != -1)
            {
                why = "Not now: close the game's menu first.";
                return false;
            }
            return true;
        }

        void jump(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const Point &p)
        {
            std::string why;
            if (!canJump(rdram, why))
            {
                setStatus(why);
                return;
            }
            if (!g_jump && runtime->hasFunction(kForcedDoorJump))
                g_jump = runtime->lookupFunction(kForcedDoorJump);
            if (!g_jump)
            {
                setStatus("The game's door jump is not in this build.");
                return;
            }
            for (int i = 0; i < 3; ++i)
                std::memcpy(rdram + kLanding + 4 * i, &p.pos[i], 4);
            wr32(rdram, kLanding + 12, 0);
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 4, kDoor);
            SET_GPR_U32(ctx, 5, p.room);
            SET_GPR_U32(ctx, 6, kLanding);
            SET_GPR_U32(ctx, 7, 0u);
            SET_GPR_U32(ctx, 8, kFadeBlack);
            SET_GPR_U32(ctx, 9, 0u);
            ctx->f[12] = p.heading;
            g_jump(rdram, ctx, runtime);
            *ctx = saved;
            KM_INFO(Game, "debug: room jump to " << p.label);
            setStatus("Jumped to " + p.label + ".");
        }

#if defined(KAMITE_HAS_GS_MENU)
        void drawPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 2u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again.");
                return;
            }
            ui->separator_text("Hide");
            ui->text_disabled("The game's own display switches (names from the preview disc's debug menu).");
            uint32_t hide = g_hide.load();
            for (const Switch &s : kSwitches)
            {
                int on = (hide & s.bit) ? 1 : 0;
                if (ui->checkbox(s.label, &on))
                    hide = on ? (hide | s.bit) : (hide & ~s.bit);
            }
            g_hide.store(hide);
            if (hide && ui->button("Show everything##showall"))
                g_hide.store(0);

            ui->separator_text("Model");
            if (ui->version >= 3u)
            {
                using bone_tweaks::Tune;
                auto slider = [&](const char *label, Tune t, float lo, float hi, const char *fmt) {
                    float v = bone_tweaks::tuning(t);
                    ui->set_next_width(300.0f);
                    if (ui->slider_float(label, &v, lo, hi, fmt))
                        bone_tweaks::setTuning(t, v);
                };
                if (bone_tweaks::hasHair())
                {
                    slider("Hair: stiffness##hk", Tune::HairStiffness, 1.0f, 200.0f, "%.0f");
                    slider("Hair: damping##hc", Tune::HairDamping, 0.0f, 30.0f, "%.1f");
                    slider("Hair: gravity##hg", Tune::HairGravity, 0.0f, 2.0f, "%.2f");
                    slider("Hair: wind##hw", Tune::HairWind, 0.0f, 10.0f, "%.1f");
                    ui->text_disabled("Hair values go in mods/characters/xavier/character.ini [Hair] to keep them.");
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                ui->text(g_hands.c_str());
            }
            ui->text_disabled("The hand meshes the game shows now (the fist, open hand and grab shapes it swaps).");

            ui->separator_text("Room jump");
            if (g_points.empty())
            {
                ui->text_disabled("The jump table (etc/debugres/RoomJmpTbl.dat) was not found on the disc image.");
                return;
            }
            char here[64];
            std::snprintf(here, sizeof(here), "Gene is in r%x%02x.", (g_room >> 8) & 0xF, g_room & 0xFF);
            ui->text(here);
            std::vector<const char *> labels;
            for (const std::string &l : g_labels)
                labels.push_back(l.c_str());
            ui->set_next_width(420.0f);
            ui->combo("##jumppoint", &g_selected, labels.data(), static_cast<int>(labels.size()));
            ui->same_line();
            if (ui->button("Jump##jump"))
            {
                g_request.store(g_selected);
                setStatus("Jumping...");
            }
            ui->text_disabled("The game's door jump with a black fade. The story flags of the place are not set: "
                              "don't save after a jump you don't want in your save. Other stages: untested.");
            std::lock_guard<std::mutex> lock(g_statusMutex);
            if (!g_status.empty())
                ui->text(g_status.c_str());
        }
#endif
    }

    void install(PS2Runtime &)
    {
        loadPoints();
        KM_INFO(Game, "debug tools: " << g_points.size() << " room jump points");
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("Debug", &drawPanel);
#endif
    }

    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::memcpy(&g_room, rdram + GameWork::address(GameWork::kRoom), 2);
        {
            static uint32_t frames = 0;
            if (++frames % 6u == 0u)
            {
                std::string hands = "Hand meshes shown: " + bone_tweaks::handNodes(rdram);
                std::lock_guard<std::mutex> lock(g_statusMutex);
                g_hands = std::move(hands);
            }
        }
        // Display switches.
        const uint32_t address = GameWork::address(GameWork::kFlagsDsp);
        const uint32_t hide = g_hide.load(std::memory_order_relaxed);
        if (hide || g_hiding)
        {
            uint32_t w = rd32(rdram, address);
            const uint32_t starting = hide & ~g_hiding, stopping = g_hiding & ~hide;
            g_gameWants = (g_gameWants & ~starting) | (w & starting); // what the game had when the hiding began
            g_gameWants &= ~(~w & hide & g_hiding);                   // cleared by the game while hidden
            w = (w | hide) & ~(stopping & ~g_gameWants);
            g_gameWants &= ~stopping;
            g_hiding = hide;
            wr32(rdram, address, w);
        }
        // Room jump.
        const int request = g_request.exchange(-1);
        if (request >= 0 && request < static_cast<int>(g_points.size()) && ctx && runtime)
            jump(rdram, ctx, runtime, g_points[static_cast<size_t>(request)]);
    }
}
