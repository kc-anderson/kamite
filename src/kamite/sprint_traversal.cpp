// Sprint attack traversal: during a sprint attack, Gene passes the stage collision groups marked for it.
//
// The marks: sprint_traversal.txt, one line per room and group: `r100 Fx_2h00 jump slide` (jump = sprint + X / Y, slide
// = sprint + A; `none` clears a shipped mark). resources/sprint_traversal.txt ships with the port; the one next to the
// exe holds the marks set in F2 > Debug > Stage collision and wins line by line. Groups are the stage collision's named
// groups (cCollisionScroll D_005CAE50, hitbox_overlay.cpp has the layout and claude/action-vaulting.md the findings).
//
// Gene only: his update func_00108AB0 checks walls once a frame with ChkCollScrWall (jal at 0x1094B8: a0 Gene, a1 the
// old position, a2 the attribute mask 0x200 / 0x224, f12 0.4 m, a3 the result) and the floor after it (func_001329C0
// at 0x109520). km_trav_off at 0x1094A8 (the argument setup, before the call) switches the marked groups' polygons
// off (bit 1 of +0x10, the bit SetCollEnable writes) when Gene is in a matching attack; km_trav_on at 0x1094E0 (where
// the hit and no-hit paths and the skipped check meet) switches back exactly the ones it switched. Enemies, objects
// and the camera check walls outside that window, so for them nothing changes.
#include "kamite/sprint_traversal.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ps2_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace kamite::sprint_traversal
{
    namespace
    {
        constexpr uint32_t kScroll = 0x5CAE50u;
        constexpr uint32_t kScrHeader = 0x00u, kScrGroups = 0x04u, kScrPolys = 0x14u;
        constexpr uint32_t kHdrGroupCount = 0x20u, kHdrPolyCount = 0x28u;
        constexpr uint32_t kGroupStride = 0x14u, kGroupFirst = 0x08u, kGroupCount = 0x0Au, kGroupName = 0x0Cu;
        constexpr uint32_t kPolyStride = 0x14u, kPolyFlags = 0x10u, kPolyOff = 2u;
        constexpr uint32_t kMotion = 0x428u; // Gene's current motion (test-mode.md)
        constexpr uint32_t kPhase = 0x2F5u;  // Gene's phase byte (+0x2F4 mode, +0x2F5 phase)
        constexpr uint8_t kSprintAttackPhase = 12;

        // Gene's sprint attacks (K's case 2026-10-30_11-24-30, room 0x100: the run E9D860 in phase 2, then phase 12
        // with one motion per attack, in the order K did them).
        constexpr uint32_t kJumpMotions[] = {0xED53E0u /* sprint + X */, 0xE8B8A0u /* sprint + Y */};
        constexpr uint32_t kSlideMotions[] = {0xED6A60u /* sprint + A */};

        bool g_enabled = false;
        fs::path g_file, g_shipped;
        std::mutex g_mutex;                                  // guards g_marks and g_version
        std::map<std::pair<uint16_t, std::string>, int> g_marks; // (room, group) -> mark bits
        uint32_t g_version = 1;

        // Game thread.
        std::vector<uint32_t> g_switched; // polygon flag addresses km_trav_off switched off this frame
        uint16_t g_cacheRoom = 0xFFFFu;
        uint32_t g_cacheVersion = 0, g_cacheGroups = 0;
        std::vector<std::pair<uint32_t, int>> g_cache; // this room's marked groups: (group index, marks)

        bool inRam(uint32_t address, uint32_t size)
        {
            const uint32_t a = address & 0x1FFFFFFFu;
            return a >= 0x100000u && a + size <= PS2_RAM_SIZE;
        }
        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        uint16_t rd16(const uint8_t *rdram, uint32_t a)
        {
            uint16_t v = 0;
            std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 2);
            return v;
        }
        void wr32(uint8_t *rdram, uint32_t a, uint32_t v) { std::memcpy(rdram + (a & 0x01FFFFFFu), &v, 4); }

        std::string oneWord(std::string s)
        {
            std::replace(s.begin(), s.end(), ' ', '_');
            return s;
        }

        void read(const fs::path &file)
        {
            std::ifstream in(file);
            std::string line;
            while (std::getline(in, line))
            {
                std::istringstream words(line);
                std::string room, group, word;
                if (!(words >> room >> group) || room.empty() || room[0] == '#' || (room[0] != 'r' && room[0] != 'R'))
                    continue;
                const unsigned long id = std::strtoul(room.c_str() + 1, nullptr, 16);
                int bits = 0;
                while (words >> word)
                    bits |= word == "jump" ? kJump : word == "slide" ? kSlide : 0;
                g_marks[{static_cast<uint16_t>(id), oneWord(group)}] = bits;
            }
        }

        bool save()
        {
            std::ofstream out(g_file, std::ios::trunc);
            if (!out)
                return false;
            out << "# Sprint attack traversal: stage collision groups Gene passes during the sprint attacks.\n"
                   "# Set in F2 > Debug > Stage collision. One line per room and group: jump = sprint + X / Y,\n"
                   "# slide = sprint + A, none = neither. These lines win over resources/sprint_traversal.txt.\n";
            for (const auto &[key, bits] : g_marks)
            {
                char room[8];
                std::snprintf(room, sizeof(room), "r%x", key.first);
                out << room << ' ' << key.second;
                if (bits & kJump)
                    out << " jump";
                if (bits & kSlide)
                    out << " slide";
                if (!bits)
                    out << " none";
                out << '\n';
            }
            return static_cast<bool>(out);
        }

        int moveKind(const uint8_t *rdram, uint32_t gene)
        {
            if (rdram[(gene + kPhase) & 0x01FFFFFFu] != kSprintAttackPhase)
                return 0;
            const uint32_t motion = rd32(rdram, gene + kMotion);
            for (uint32_t m : kJumpMotions)
                if (m == motion)
                    return kJump;
            for (uint32_t m : kSlideMotions)
                if (m == motion)
                    return kSlide;
            return 0;
        }

        // Before Gene's wall check.
        void switchOff(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            g_switched.clear();
            const uint32_t gene = rd32(rdram, game::GameWork::address(game::GameWork::kPlayerObj));
            if (!inRam(gene, 0x600u))
                return;
            const int kind = moveKind(rdram, gene);
            if (!kind)
                return;
            const uint32_t header = rd32(rdram, kScroll + kScrHeader);
            const uint32_t groups = rd32(rdram, kScroll + kScrGroups), polys = rd32(rdram, kScroll + kScrPolys);
            if (!inRam(header, 0x30u))
                return;
            const uint32_t groupCount = rd16(rdram, header + kHdrGroupCount), polyCount = rd16(rdram, header + kHdrPolyCount);
            if (!inRam(groups, groupCount * kGroupStride) || !inRam(polys, polyCount * kPolyStride))
                return;
            const uint16_t room = rd16(rdram, game::GameWork::address(game::GameWork::kRoom));
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (room != g_cacheRoom || g_version != g_cacheVersion || groups != g_cacheGroups)
                {
                    g_cacheRoom = room;
                    g_cacheVersion = g_version;
                    g_cacheGroups = groups;
                    g_cache.clear();
                    for (uint32_t i = 0; i < groupCount; ++i)
                    {
                        char name[9] = {};
                        std::memcpy(name, rdram + ((groups + i * kGroupStride + kGroupName) & 0x01FFFFFFu), 8);
                        const auto it = g_marks.find({room, oneWord(name)});
                        if (it != g_marks.end() && it->second)
                            g_cache.emplace_back(i, it->second);
                    }
                }
            }
            for (const auto &[index, bits] : g_cache)
            {
                if (!(bits & kind) || index >= groupCount)
                    continue;
                const uint32_t g = groups + index * kGroupStride;
                const uint32_t first = rd16(rdram, g + kGroupFirst), count = rd16(rdram, g + kGroupCount);
                for (uint32_t p = first; p < first + count && p < polyCount; ++p)
                {
                    const uint32_t f = polys + p * kPolyStride + kPolyFlags;
                    const uint32_t flags = rd32(rdram, f);
                    if (flags & kPolyOff)
                        continue; // already off (the game's own switch): left alone
                    wr32(rdram, f, flags | kPolyOff);
                    g_switched.push_back(f);
                }
            }
        }

        // After it, on every path.
        void switchOn(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            for (uint32_t f : g_switched)
                wr32(rdram, f, rd32(rdram, f) & ~kPolyOff);
            g_switched.clear();
        }
    }

    void install(PS2Runtime &, const fs::path &exeDir)
    {
        g_file = exeDir / "sprint_traversal.txt";
        g_shipped = exeDir / "resources" / "sprint_traversal.txt";
        if (const char *on = std::getenv("KAMITE_SPRINT_TRAVERSAL"))
            g_enabled = *on && *on != '0';
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            read(g_shipped);
            read(g_file);
        }
        ps2xRegisterHostHook("km_trav_off", &switchOff);
        ps2xRegisterHostHook("km_trav_on", &switchOn);
        ps2xSetPatchFlag("sprint_traversal", g_enabled);
        KM_INFO(Game, "sprint attack traversal: " << (g_enabled ? "on" : "off") << ", " << g_marks.size() << " groups marked");
    }

    bool enabled() { return g_enabled; }

    int marks(uint16_t room, const std::string &group)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_marks.find({room, oneWord(group)});
        return it == g_marks.end() ? 0 : it->second;
    }

    bool setMarks(uint16_t room, const std::string &group, int bits)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_marks[{room, oneWord(group)}] = bits;
        ++g_version;
        return save();
    }
}
