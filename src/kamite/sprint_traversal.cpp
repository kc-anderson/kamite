// Sprint attack traversal: during a sprint attack, Gene passes the stage collision groups marked for it.
//
// The marks: sprint_traversal.txt, one line per room and group: `r100 Fx_2h00 jump slide` (jump = sprint + X / Y, slide
// = sprint + A; `none` clears a shipped mark). Windows: one line each, `r100 H02_w00 window x y z dx dz offset sill
// width height depth` (sprint_traversal.h has the fields; depth may be left out: 0); a wall's windows come all from one file (the user's, when it has
// any line for that wall; `r100 H02_w00 window none` drops the shipped ones). resources/sprint_traversal.txt ships with the port; the one next to the
// exe holds the marks set in F2 > Debug > Stage collision and wins line by line. Groups are the stage collision's named
// groups (cCollisionScroll D_005CAE50, hitbox_overlay.cpp has the layout and claude/sprint-traversal.md the findings).
//
// Gene only: his update func_00108AB0 checks walls once a frame with ChkCollScrWall (jal at 0x1094B8: a0 Gene, a1 the
// old position, a2 the attribute mask 0x200 / 0x224, f12 0.4 m, a3 the result) and the floor after it (func_001329C0
// at 0x109520). km_trav_off at 0x1094A8 (the argument setup, before the call) switches the marked groups' polygons
// off (bit 1 of +0x10, the bit SetCollEnable writes) when Gene is in a matching attack; km_trav_on at 0x1094E0 (where
// the hit and no-hit paths and the skipped check meet) switches back exactly the ones it switched. Enemies, objects
// and the camera check walls outside that window, so for them nothing changes.
//
// Windows: walls are never switched off. km_trav_window at 0x1094C8 (the hit path: the pushed-out x / z at sp+0x20 /
// sp+0x28 are about to be written to Gene's position, f0 already holds x) gives back Gene's own x / z when he is in
// sprint + X / Y within a marked window's width and 1 m of its wall (of either face, with a depth), so the push-out
// does nothing.
#include "kamite/sprint_traversal.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ps2_memory.h"
#include "ps2_runtime_macros.h" // GPR_U32

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
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
        using Key = std::pair<uint16_t, std::string>; // (room, group)
        std::mutex g_mutex;                          // guards the three below and g_version
        std::map<Key, int> g_shippedMarks;           // resources/sprint_traversal.txt
        std::map<Key, int> g_userMarks;              // sprint_traversal.txt: only where it differs from the shipped
        std::map<Key, int> g_marks;                  // in effect: the shipped, then the user's over them
        using Windows = std::vector<Window>;
        std::map<Key, Windows> g_shippedWindows;     // resources/sprint_traversal.txt
        std::map<Key, Windows> g_userWindows;        // sprint_traversal.txt: a wall's whole set (empty = `window none`)
        std::map<Key, Windows> g_windows;            // in effect
        bool g_windowsDirty = false;                 // setWindow changes not saved yet
        uint32_t g_version = 1;

        // Game thread.
        std::vector<uint32_t> g_switched; // polygon flag addresses km_trav_off switched off this frame
        uint16_t g_cacheRoom = 0xFFFFu;
        uint32_t g_cacheVersion = 0, g_cacheGroups = 0;
        std::vector<std::pair<uint32_t, int>> g_cache; // this room's marked groups: (group index, marks)
        uint16_t g_winRoom = 0xFFFFu;
        uint32_t g_winVersion = 0;
        Windows g_winCache; // this room's windows

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
        float rdf(const uint8_t *rdram, uint32_t a)
        {
            float v = 0.0f;
            std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wrf(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & 0x01FFFFFFu), &v, 4); }

        std::string windowText(const Window &w)
        {
            char text[220];
            std::snprintf(text, sizeof(text), "%.3f %.3f %.3f %.4f %.4f %.3f %.3f %.3f %.3f %.3f", w.x, w.y, w.z, w.dx, w.dz,
                          w.offset, w.sill, w.width, w.height, w.depth);
            return text;
        }
        bool sameWindows(const Windows &a, const Windows &b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (windowText(a[i]) != windowText(b[i]))
                    return false;
            return true;
        }
        // g_windows from the shipped and the user's sets (g_mutex held).
        void mergeWindows()
        {
            g_windows = g_shippedWindows;
            for (const auto &[key, list] : g_userWindows)
                g_windows[key] = list;
            for (auto it = g_windows.begin(); it != g_windows.end();)
                it = it->second.empty() ? g_windows.erase(it) : std::next(it);
        }
        // A wall's set after an edit: kept as the user's unless it equals the shipped one (g_mutex held).
        void putWall(const Key &key, const Windows &list)
        {
            const auto shipped = g_shippedWindows.find(key);
            const Windows none;
            if (sameWindows(list, shipped == g_shippedWindows.end() ? none : shipped->second))
                g_userWindows.erase(key);
            else
                g_userWindows[key] = list;
            mergeWindows();
            ++g_version;
        }

        std::string oneWord(std::string s)
        {
            std::replace(s.begin(), s.end(), ' ', '_');
            return s;
        }

        void read(const fs::path &file, std::map<Key, int> &into, std::map<Key, Windows> &windowsInto)
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
                const Key key{static_cast<uint16_t>(id), oneWord(group)};
                std::streampos rest = words.tellg();
                if (words >> word && word == "window")
                {
                    Windows &list = windowsInto[key]; // `window none` leaves the wall's set empty
                    Window w;
                    if (words >> w.x >> w.y >> w.z >> w.dx >> w.dz >> w.offset >> w.sill >> w.width >> w.height)
                    {
                        if (!(words >> w.depth))
                            w.depth = 0.0f;
                        const float len = std::sqrt(w.dx * w.dx + w.dz * w.dz);
                        if (len > 0.0f)
                        {
                            w.dx /= len;
                            w.dz /= len;
                            list.push_back(w);
                        }
                    }
                    continue;
                }
                words.clear();
                words.seekg(rest);
                int bits = 0;
                while (words >> word)
                    bits |= word == "jump" ? kJump : word == "slide" ? kSlide : 0;
                into[key] = bits;
            }
        }

        bool save()
        {
            std::ofstream out(g_file, std::ios::trunc);
            if (!out)
                return false;
            out << "# Sprint attack traversal: stage collision groups Gene passes during the sprint attacks.\n"
                   "# Set in F2 > Debug > Stage collision. One line per room and group: jump = sprint + X / Y,\n"
                   "# slide = sprint + A, none = clears a mark resources/sprint_traversal.txt ships. These lines win\n"
                   "# over that file; a group marked as it ships (or unmarked and not shipped) has no line here.\n"
                   "# Windows (sprint + X / Y dive through): `window x y z dx dz offset sill width height depth`, one each;\n"
                   "# a wall listed here takes its windows from here only (`window none` = no windows on it).\n";
            for (const auto &[key, bits] : g_userMarks)
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
            for (const auto &[key, list] : g_userWindows)
            {
                char room[8];
                std::snprintf(room, sizeof(room), "r%x", key.first);
                if (list.empty())
                    out << room << ' ' << key.second << " window none\n";
                for (const Window &w : list)
                    out << room << ' ' << key.second << " window " << windowText(w) << '\n';
            }
            g_windowsDirty = false;
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

        // The wall check's hit path (0x1094C8): inside a marked window, the push-out is undone (Gene's own x / z).
        void windowPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t gene = GPR_U32(ctx, 17); // s1
            if (!inRam(gene, 0x600u) || moveKind(rdram, gene) != kJump)
                return;
            const uint32_t pos = rd32(rdram, gene + game::GameObj::kPos);
            if (!inRam(pos, 12u))
                return;
            const float px = rdf(rdram, pos), pz = rdf(rdram, pos + 8u);
            const uint16_t room = rd16(rdram, game::GameWork::address(game::GameWork::kRoom));
            bool inside = false;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (room != g_winRoom || g_version != g_winVersion)
                {
                    g_winRoom = room;
                    g_winVersion = g_version;
                    g_winCache.clear();
                    for (const auto &[key, list] : g_windows)
                        if (key.first == room)
                            g_winCache.insert(g_winCache.end(), list.begin(), list.end());
                }
                for (const Window &w : g_winCache)
                {
                    const float cx = w.x + w.dx * w.offset, cz = w.z + w.dz * w.offset;
                    const float along = (px - cx) * w.dx + (pz - cz) * w.dz;
                    const float across = (px - cx) * -w.dz + (pz - cz) * w.dx;
                    if (std::fabs(along) <= 0.5f * w.width && across >= std::min(0.0f, w.depth) - 1.0f &&
                        across <= std::max(0.0f, w.depth) + 1.0f)
                    {
                        inside = true;
                        break;
                    }
                }
            }
            if (!inside)
                return;
            ctx->f[0] = px;                              // the x about to be stored (loaded in the branch's delay slot)
            wrf(rdram, GPR_U32(ctx, 29) + 0x28u, pz);    // the z it loads next
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
            read(g_shipped, g_shippedMarks, g_shippedWindows);
            read(g_file, g_userMarks, g_userWindows);
            for (auto it = g_userWindows.begin(); it != g_userWindows.end();)
            {
                const auto shipped = g_shippedWindows.find(it->first);
                const Windows none;
                it = sameWindows(it->second, shipped == g_shippedWindows.end() ? none : shipped->second) ? g_userWindows.erase(it)
                                                                                                         : std::next(it);
            }
            mergeWindows();
            // Lines that only repeat the shipped marks are dropped at the next save.
            for (auto it = g_userMarks.begin(); it != g_userMarks.end();)
            {
                const auto shipped = g_shippedMarks.find(it->first);
                const int base = shipped == g_shippedMarks.end() ? 0 : shipped->second;
                it = it->second == base ? g_userMarks.erase(it) : std::next(it);
            }
            g_marks = g_shippedMarks;
            for (const auto &[key, bits] : g_userMarks)
                g_marks[key] = bits;
        }
        ps2xRegisterHostHook("km_trav_off", &switchOff);
        ps2xRegisterHostHook("km_trav_on", &switchOn);
        ps2xRegisterHostHook("km_trav_window", &windowPass);
        ps2xSetPatchFlag("sprint_traversal", g_enabled);
        KM_INFO(Game, "sprint attack traversal: " << (g_enabled ? "on" : "off") << ", " << g_marks.size() << " groups marked, "
                                                   << [] { size_t n = 0; for (const auto &w : g_windows) n += w.second.size(); return n; }()
                                                   << " windows");
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
        const Key key{room, oneWord(group)};
        const auto shipped = g_shippedMarks.find(key);
        const int base = shipped == g_shippedMarks.end() ? 0 : shipped->second;
        if (bits == base)
            g_userMarks.erase(key);
        else
            g_userMarks[key] = bits;
        if (bits)
            g_marks[key] = bits;
        else
            g_marks.erase(key);
        ++g_version;
        return save();
    }

    std::vector<WindowRef> windows(uint16_t room)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::vector<WindowRef> out;
        for (const auto &[key, list] : g_windows)
            if (key.first == room)
                for (size_t i = 0; i < list.size(); ++i)
                    out.push_back({key.second, static_cast<int>(i), list[i]});
        return out;
    }

    bool addWindow(uint16_t room, const std::string &group, const Window &window)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Key key{room, oneWord(group)};
        const auto it = g_windows.find(key);
        Windows list = it == g_windows.end() ? Windows{} : it->second;
        list.push_back(window);
        putWall(key, list);
        return save();
    }

    void setWindow(uint16_t room, const std::string &group, int index, const Window &window)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Key key{room, oneWord(group)};
        const auto it = g_windows.find(key);
        if (it == g_windows.end() || index < 0 || static_cast<size_t>(index) >= it->second.size())
            return;
        Windows list = it->second;
        list[static_cast<size_t>(index)] = window;
        putWall(key, list);
        g_windowsDirty = true;
    }

    bool removeWindow(uint16_t room, const std::string &group, int index)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const Key key{room, oneWord(group)};
        const auto it = g_windows.find(key);
        if (it == g_windows.end() || index < 0 || static_cast<size_t>(index) >= it->second.size())
            return true;
        Windows list = it->second;
        list.erase(list.begin() + index);
        putWall(key, list);
        return save();
    }

    bool flushWindows()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return !g_windowsDirty || save();
    }
}
