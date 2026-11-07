// Sprint attack traversal: during a sprint attack, Gene passes the stage collision groups marked for it.
//
// The marks: sprint_traversal.txt, one line per room and group: `r100 Fx_2h00 jump slide` (jump = sprint + X / Y, slide
// = sprint + A; `none` clears a shipped mark). Windows: one line each, `r100 H02_w00 window x y z dx dz offset sill
// width height depth [jump] [slide]` (sprint_traversal.h has the fields; depth may be left out: 0; no word = jump only); a wall's windows come all from one file (the user's, when it has
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
#include "runtime/ee_scheduler.h"
#include "ps2_runtime_macros.h" // GPR_U32

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
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
            std::string out = text;
            if (w.passes != kJump) // the default (and every window before Nov 4) writes no words
                out += w.passes == kSlide ? " slide" : w.passes == (kJump | kSlide) ? " jump slide" : " none";
            if (!w.twoWay) // two-way (every window before Nov 6) writes no word
                out += w.side > 0 ? " oneway" : " oneway_back";
            if (w.linked())
            {
                std::snprintf(text, sizeof(text), " door %02X %02X", w.doorRoom & 0xFF, w.doorPoint & 0xFF);
                out += text;
            }
            return out;
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
                        // Then the depth (optional) and the moves that pass (optional: jump / slide / none).
                        int passes = 0;
                        bool named = false;
                        while (words >> word)
                        {
                            std::string a, b;
                            if (word == "door" && words >> a >> b)
                            {
                                w.doorRoom = static_cast<int>(std::strtoul(a.c_str(), nullptr, 16) & 0xFFu);
                                w.doorPoint = static_cast<int>(std::strtoul(b.c_str(), nullptr, 16) & 0xFFu);
                            }
                            else if (word == "oneway" || word == "oneway_back")
                            {
                                w.twoWay = false;
                                w.side = word == "oneway" ? 1 : -1;
                            }
                            else if (word == "jump" || word == "slide" || word == "none")
                            {
                                named = true;
                                passes |= word == "jump" ? kJump : word == "slide" ? kSlide : 0;
                            }
                            else
                                w.depth = static_cast<float>(std::atof(word.c_str()));
                        }
                        w.passes = named ? passes : kJump;
                        if (w.linked())
                            w.twoWay = false; // a door's window only leads toward the door
                        if (w.twoWay && w.depth != 0.0f) // the side it was marked from, should it be made one-way
                            w.side = w.depth > 0.0f ? 1 : -1;
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
                   "# Windows: `window x y z dx dz offset sill width height depth [jump] [slide]` (no word = jump), one each;\n"
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
        // How far in front of the window's face (and past its far face) Gene counts as diving through. 1 m: the shipped
        // r0 windows are marked per face of a 1.7 m wall with no depth, so the two faces' boxes must meet inside the
        // wall (0.6 m left a 0.5 m gap there and Gene stuck in it: K, Nov 4).
        constexpr float kWindowMargin = 1.0f;
        constexpr float kWindowFloor = 0.6f; // a window this far above or below Gene's floor is another floor's
        // A one-way window lets Gene through only facing its way (his heading, the root's y angle).
        bool facesThrough(const Window &w, const uint8_t *rdram, uint32_t gene)
        {
            if (w.twoWay)
                return true;
            const float heading = rdf(rdram, gene + game::GameObj::kRot + 4u);
            return windowSide(w, std::sin(heading), std::cos(heading)) == w.side;
        }
        void windowPass(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t gene = GPR_U32(ctx, 17); // s1
            const int kind = inRam(gene, 0x600u) ? moveKind(rdram, gene) : 0;
            if (!kind)
                return;
            const uint32_t pos = rd32(rdram, gene + game::GameObj::kPos);
            if (!inRam(pos, 12u))
                return;
            const float px = rdf(rdram, pos), py = rdf(rdram, pos + 4u), pz = rdf(rdram, pos + 8u);
            const uint16_t room = rd16(rdram, game::GameWork::address(game::GameWork::kRoom));
            const Window *inside = nullptr;
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
                    // Only from the window's own floor: Gene's position stays on his floor through the dive, so one
                    // well below (or above) is at another floor's wall (K, Nov 6: r404's platform windows let him
                    // through the wall from the ground under them).
                    if (!(w.passes & kind) || std::fabs(w.y - py) > kWindowFloor || !facesThrough(w, rdram, gene))
                        continue;
                    const float cx = w.x + w.dx * w.offset, cz = w.z + w.dz * w.offset;
                    const float along = (px - cx) * w.dx + (pz - cz) * w.dz;
                    const float across = (px - cx) * -w.dz + (pz - cz) * w.dx;
                    if (std::fabs(along) <= 0.5f * w.width && across >= std::min(0.0f, w.depth) - kWindowMargin &&
                        across <= std::max(0.0f, w.depth) + kWindowMargin)
                    {
                        inside = &w;
                        break;
                    }
                }
            }
            if (!inside)
                return;
            // Only the window's own wall lets go: of the push-out (pushed - now) the part along the wall's normal
            // (-dz, dx) is undone, the rest kept, so other walls (a shop counter by the window) still hold. It used to
            // undo the whole push (every wall within the window's box let go: K, Nov 4, behind the shop counter).
            // The x about to be stored is f0 (loaded in the branch's delay slot), the z sp+0x28 (loaded next).
            const uint32_t zAt = GPR_U32(ctx, 29) + 0x28u;
            const float qx = ctx->f[0], qz = rdf(rdram, zAt);
            const float nx = -inside->dz, nz = inside->dx;
            const float k = (qx - px) * nx + (qz - pz) * nz;
            ctx->f[0] = qx - k * nx;
            wrf(rdram, zAt, qz - k * nz);
        }

        // Lifting Gene's drawn body over a high sill (K, Nov 4: the shop windows' 0.95 m look right; taller sills clip).
        // The dive's height is all in the motion: his position stays on the floor (case jumping_shop_windows: y 0.249
        // through sprint + X / Y). So the lift is drawn only: in his update (func_00108AB0) his position's y is raised
        // for cModel_calcWorldParts (km_trav_lift, 0x1097D4, its jal's delay slot; after IK_InverseKinematics, which
        // reads the position: feet planting) and put back after it (km_trav_lift_end, 0x1097DC, the next jal's delay
        // slot). calcWorldParts rebuilds the root (func_147A48 from the position) and the parts' world matrices.
        // Collision, the floor check and the game's state never see it (his hurt spheres do: they come from the bones).
        // Lift = (window floor + sill) - Gene's floor - what the move clears now, full between the wall's faces and
        // fading out over the box's margin; jump windows, jump moves, windows on Gene's own floor only (case
        // super_jump: the balcony's windows 3.35 m above the shop's lifted him to them).
        // What the dive (sprint + X) clears follows its motion frame (+0x41C): his torso's bottom (hurt spheres, case
        // flush_X) is 0.15 m at the take-off, 0.30 at frame 7, 0.74 at 10, 1.0 at 14..15. A run-up crosses the wall
        // near that top (0.95, K's shop windows); a flush jump (sprinting against the wall) at frames 2..7, 0.7 m
        // lower: the fixed 0.95 barely lifted it (K, Nov 4). The flying kick (sprint + Y) keeps 0.95 (K: flush fine).
        // The lift moves at most 0.125 m a vblank toward its target, so a flush dive rises instead of jumping.
        constexpr float kDiveClears = 0.95f;
        constexpr uint32_t kMotionFrame = 0x41Cu;
        constexpr float kLiftStep = 0.125f;   // metres a vblank
        // The dive's torso bottom (m above the floor) by motion frame 0, 1, ... 14 (from 14 on: the top, 0.95 clears).
        constexpr float kDiveBottom[] = {0.15f, 0.15f, 0.15f, 0.15f, 0.15f, 0.17f, 0.21f, 0.27f,
                                         0.40f, 0.62f, 0.75f, 0.84f, 0.92f, 0.97f, 1.00f};
        uint32_t g_liftPos = 0u; // where the y was raised (0 = not raised)
        float g_liftWas = 0.0f;  // and what it was
        float g_liftNow = 0.0f;  // the lift drawn (eased toward the target)

        float diveClears(const uint8_t *rdram, uint32_t gene)
        {
            if (rd32(rdram, gene + kMotion) != kJumpMotions[0])
                return kDiveClears; // the flying kick
            const float frame = rdf(rdram, gene + kMotionFrame);
            constexpr int last = static_cast<int>(sizeof(kDiveBottom) / sizeof(kDiveBottom[0])) - 1;
            if (!(frame >= 0.0f) || frame >= static_cast<float>(last))
                return kDiveClears;
            const int i = static_cast<int>(frame);
            const float t = frame - static_cast<float>(i);
            const float bottom = kDiveBottom[i] + (kDiveBottom[i + 1] - kDiveBottom[i]) * t;
            return bottom - (kDiveBottom[last] - kDiveClears); // the top clears 0.95, as the run-up shows
        }

        void liftStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_liftPos = 0u;
            const uint32_t gene = GPR_U32(ctx, 17); // s1
            const bool jumping = inRam(gene, 0x600u) && moveKind(rdram, gene) == kJump;
            const uint32_t pos = jumping ? rd32(rdram, gene + game::GameObj::kPos) : 0u;
            if (!jumping || !inRam(pos, 12u))
            {
                g_liftNow = 0.0f;
                return;
            }
            const float px = rdf(rdram, pos), py = rdf(rdram, pos + 4u), pz = rdf(rdram, pos + 8u);
            const uint16_t room = rd16(rdram, game::GameWork::address(game::GameWork::kRoom));
            const float clears = diveClears(rdram, gene);
            float target = 0.0f;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const auto &[key, list] : g_windows)
                {
                    if (key.first != room)
                        continue;
                    for (const Window &w : list)
                    {
                        if (!(w.passes & kJump) || std::fabs(w.y - py) > kWindowFloor || !facesThrough(w, rdram, gene))
                            continue;
                        const float cx = w.x + w.dx * w.offset, cz = w.z + w.dz * w.offset;
                        const float along = (px - cx) * w.dx + (pz - cz) * w.dz;
                        const float across = (px - cx) * -w.dz + (pz - cz) * w.dx;
                        const float near = std::min(0.0f, w.depth), far = std::max(0.0f, w.depth);
                        const float out = across < near ? near - across : across > far ? across - far : 0.0f;
                        if (std::fabs(along) > 0.5f * w.width || out > kWindowMargin)
                            continue;
                        float t = 1.0f - out / kWindowMargin;
                        t = t * t * (3.0f - 2.0f * t); // eased in and out
                        target = std::max(target, (w.y + w.sill - py - clears) * t);
                    }
                }
            }
            const float vblanks = static_cast<float>(std::max<uint8_t>(1u, rdram[game::GameWork::address(game::GameWork::kVblankDivider)]));
            const float step = kLiftStep * vblanks;
            g_liftNow += std::clamp(target - g_liftNow, -step, step);
            if (g_liftNow <= 0.0f)
            {
                g_liftNow = 0.0f;
                return;
            }
            g_liftPos = pos;
            g_liftWas = py;
            wrf(rdram, pos + 4u, py + g_liftNow);
        }

        void liftEnd(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            if (g_liftPos)
                wrf(rdram, g_liftPos + 4u, g_liftWas);
            g_liftPos = 0u;
        }

        // Windows linked to a door (K, Nov 7): r503's windows look past a locked door whose far side belongs to the
        // next room (r509) and has no collision. A dive through one runs the door's own trigger when Gene lands (K's
        // choice: the landing spot is solid, what lies past it isn't), so the game makes the jump exactly as when he
        // walks through the door: its fade, exit callback (the SAVE / CONTINUE screen) and landing point. Gene keeps a
        // key he holds (K: an immersive-sim spare; claude/later.md has the fallback should a spare break something).
        // The doors: cSceAtManager D_005FEA60, its unit list at +0x5C (tagged links: bit 31 = a unit, 0xFFFFFFFF ends,
        // cSceAtManager_getNextUnit); type +0x35 indexes the handler table D_003C2650, type 1 = func_002C2078 (the door
        // trigger: cDoor_getJumpData(&D_00583EC0, +0x79, +0x5E, 2, 0, 0), returns at once while +0x7A (lock) is 1,
        // installs the exit callback +0x74 / +0x70, cDoor_setDoorJump). The area (cArea) at +0x04: type +0x05,
        // y +0x08, corners (x, z) from +0x14 (one, the centre, for a circle).
        constexpr uint32_t kSceAt = 0x5FEA60u, kSceAtList = 0x5Cu;
        constexpr uint32_t kUnitType = 0x35u, kUnitRoom = 0x5Eu, kUnitPoint = 0x79u, kUnitLock = 0x7Au;
        constexpr uint8_t kDoorType = 1;
        constexpr uint32_t kDoorTrigger = 0x2C2078u, kGetJumpData = 0x2004C8u, kSetDoorJump = 0x1FFF80u;
        constexpr uint32_t kDoorJump = 0x583EC0u; // D_00583EC0, the door jump's cDoor
        constexpr int kLandWait = 90; // vblanks: run the door anyway if the dive has not ended by then

        std::mutex g_doorMutex;
        std::vector<Door> g_doors; // this room's (GS thread reads)
        std::vector<std::pair<uint32_t, Door>> g_doorUnits; // game thread: (unit, door)
        struct Pending
        {
            uint16_t room = 0xFFFFu;
            int doorRoom = -1, doorPoint = -1;
            int wait = 0;
        } g_pending;
        std::vector<float> g_lastAcross; // per linked window this frame: how far past its near face Gene was

        void scanDoors(const uint8_t *rdram)
        {
            g_doorUnits.clear();
            uint32_t x = rd32(rdram, kSceAt + kSceAtList);
            for (int guard = 0; x != 0xFFFFFFFFu && guard < 4096; ++guard)
            {
                const uint32_t u = x & 0x7FFFFFFFu;
                if (!inRam(u, 0x9Cu))
                    break;
                if ((x & 0x80000000u) && rdram[(u + kUnitType) & 0x01FFFFFFu] == kDoorType)
                {
                    Door d;
                    d.room = rdram[(u + kUnitRoom) & 0x01FFFFFFu];
                    d.point = rdram[(u + kUnitPoint) & 0x01FFFFFFu];
                    d.locked = rdram[(u + kUnitLock) & 0x01FFFFFFu] == 1u;
                    const uint8_t shape = rdram[(u + 0x05u) & 0x01FFFFFFu];
                    const int corners = shape == 1u ? 4 : 1;
                    for (int c = 0; c < corners; ++c)
                    {
                        d.x += rdf(rdram, u + 0x14u + 8u * c) / corners;
                        d.z += rdf(rdram, u + 0x18u + 8u * c) / corners;
                    }
                    d.y = rdf(rdram, u + 0x08u);
                    g_doorUnits.emplace_back(u, d);
                }
                x = rd32(rdram, u);
            }
            std::lock_guard<std::mutex> lock(g_doorMutex);
            g_doors.clear();
            for (const auto &e : g_doorUnits)
                g_doors.push_back(e.second);
        }

        // A guest function run to its end on a copy of the registers (frame_patches.cpp callGuest); v0 back.
        uint32_t callGuest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t address,
                           std::initializer_list<uint32_t> args)
        {
            if (!runtime || !runtime->hasFunction(address))
                return 0u;
            PS2Runtime::RecompiledFunction fn = runtime->lookupFunction(address);
            if (!fn)
                return 0u;
            const R5900Context saved = *ctx;
            int r = 4;
            for (uint32_t a : args)
                SET_GPR_U32(ctx, r++, a);
            ctx->pc = address; // a hooked function finds its hook by the pc
            runtime->eeScheduler().beginHostCall();
            fn(rdram, ctx, runtime);
            runtime->eeScheduler().endHostCall();
            const uint32_t v0 = GPR_U32(ctx, 2);
            *ctx = saved;
            return v0;
        }

        // Run the door: its own trigger (the lock let go for the call: the window is the way in), or, with no trigger
        // unit for it in the room, the jump the trigger would make (no exit callback then).
        void runDoor(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int doorRoom, int doorPoint)
        {
            for (const auto &[u, d] : g_doorUnits)
            {
                if (d.room != doorRoom || d.point != doorPoint)
                    continue;
                uint8_t &lock = rdram[(u + kUnitLock) & 0x01FFFFFFu];
                const uint8_t was = lock;
                lock = 0u;
                callGuest(rdram, ctx, runtime, kDoorTrigger, {u});
                lock = was;
                const bool jumping = (rd32(rdram, game::GameWork::address(game::GameWork::kFlags5E4)) & game::Sta::kRoomJump) != 0u;
                KM_INFO(Game, "busta-chan jive: window -> door " << std::hex << doorRoom << " " << doorPoint << std::dec
                                                                   << " (trigger, lock " << int(was) << ")"
                                                                   << (jumping ? "" : ": no jump started"));
                return;
            }
            const uint32_t found = callGuest(rdram, ctx, runtime, kGetJumpData,
                                             {kDoorJump, static_cast<uint32_t>(doorPoint), static_cast<uint32_t>(doorRoom), 2u, 0u, 0u});
            if (found)
                callGuest(rdram, ctx, runtime, kSetDoorJump, {kDoorJump});
            KM_INFO(Game, "busta-chan jive: window -> door " << std::hex << doorRoom << " " << doorPoint << std::dec
                                                               << (found ? " (no trigger in the room: the jump only)" : ": no such door, nothing done"));
        }

        bool canRunDoor(const uint8_t *rdram)
        {
            using namespace game;
            const uint32_t sta = rd32(rdram, GameWork::address(GameWork::kFlags5E4));
            if (sta & (Sta::kEvent | Sta::kRoomJump | Sta::kTalk | Sta::kSoftEvent | Sta::kSubScr))
                return false;
            return (rd32(rdram, GameWork::address(GameWork::kFlags5D8)) & kFlags5D8PlayerOff) == 0u;
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
        ps2xRegisterHostHook("km_trav_lift", &liftStart);
        ps2xRegisterHostHook("km_trav_lift_end", &liftEnd);
        ps2xSetPatchFlag("sprint_traversal", g_enabled);
        KM_INFO(Game, "sprint attack traversal: " << (g_enabled ? "on" : "off") << ", " << g_marks.size() << " groups marked, "
                                                   << [] { size_t n = 0; for (const auto &w : g_windows) n += w.second.size(); return n; }()
                                                   << " windows");
    }

    bool enabled() { return g_enabled; }

    std::vector<Door> doors()
    {
        std::lock_guard<std::mutex> lock(g_doorMutex);
        return g_doors;
    }

    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_enabled || !rdram || !ctx)
            return;
        scanDoors(rdram);
        const uint16_t room = rd16(rdram, game::GameWork::address(game::GameWork::kRoom));
        const uint32_t gene = rd32(rdram, game::GameWork::address(game::GameWork::kPlayerObj));
        if (g_pending.room != 0xFFFFu && g_pending.room != room)
            g_pending = Pending{}; // left the room some other way
        if (!inRam(gene, 0x600u))
            return;
        const int kind = moveKind(rdram, gene);
        // Through a linked window and landed (the dive over): run its door.
        if (g_pending.room != 0xFFFFu)
        {
            const uint8_t vblanks = std::max<uint8_t>(1u, rdram[game::GameWork::address(game::GameWork::kVblankDivider)]);
            g_pending.wait += vblanks;
            if ((kind == 0 || g_pending.wait >= kLandWait) && canRunDoor(rdram))
            {
                const Pending p = g_pending;
                g_pending = Pending{};
                runDoor(rdram, ctx, runtime, p.doorRoom, p.doorPoint);
            }
            return;
        }
        // Gene crossing a linked window's far face in a jump move (from its near side the frame before).
        Windows linked;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &[key, list] : g_windows)
                if (key.first == room)
                    for (const Window &w : list)
                        if (w.linked() && (w.passes & kJump))
                            linked.push_back(w);
        }
        g_lastAcross.resize(linked.size(), -1e9f);
        const uint32_t pos = rd32(rdram, gene + game::GameObj::kPos);
        if (linked.empty() || !inRam(pos, 12u))
            return;
        const float px = rdf(rdram, pos), py = rdf(rdram, pos + 4u), pz = rdf(rdram, pos + 8u);
        for (size_t i = 0; i < linked.size(); ++i)
        {
            const Window &w = linked[i];
            const float cx = w.x + w.dx * w.offset, cz = w.z + w.dz * w.offset;
            const float along = (px - cx) * w.dx + (pz - cz) * w.dz;
            // Across, measured the way the window lets Gene through; near face 0, far face `far`.
            const float across = ((px - cx) * -w.dz + (pz - cz) * w.dx) * static_cast<float>(w.side);
            const float far = std::max(0.0f, w.depth * static_cast<float>(w.side));
            const bool in = kind == kJump && std::fabs(along) <= 0.5f * w.width && std::fabs(w.y - py) <= kWindowFloor &&
                            across >= -kWindowMargin && across <= far + kWindowMargin;
            const float last = g_lastAcross[i];
            g_lastAcross[i] = in ? across : -1e9f;
            if (in && last > -1e8f && last <= far && across > far)
            {
                g_pending.room = room;
                g_pending.doorRoom = w.doorRoom;
                g_pending.doorPoint = w.doorPoint;
                g_pending.wait = 0;
                std::fill(g_lastAcross.begin(), g_lastAcross.end(), -1e9f);
                KM_INFO(Game, "busta-chan jive: through a door's window, the door runs when Gene lands");
                break;
            }
        }
    }

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

    bool restoreShipped()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_userMarks.clear();
        g_userWindows.clear();
        g_marks = g_shippedMarks;
        mergeWindows();
        g_windowsDirty = false;
        ++g_version;
        return save();
    }

    bool flushWindows()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return !g_windowsDirty || save();
    }
}
