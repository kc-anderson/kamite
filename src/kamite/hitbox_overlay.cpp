// The hitbox overlay: the preview disc's DBG_COLL_AT_DISP (SLPM_665.50 0x2721F0 walked the collision manager's list
// and called debug-draw primitives retail no longer has), done on the host from the same data.
//
// The data (retail SLUS_215.03; decomp names where it has them, cDamageUnit.h, src/cod/001f60d8.c, 002bb2d8.c):
//   cCollisionManager D_005E8040: the pool of every damage shape in the game, +0 records, +4 count, +8 stride (0x270),
//     +0x10 the used list (records linked at +4), +0x18 vtable. Its per-frame update func_002B9D58 is the first call
//     of the game's Move(): it rebuilds each live shape's world matrix from its owner's bone (UpdateObjWithVtable
//     Dispatch_2BB1F0) and runs the hit tests, so what it leaves is exactly what the game tested this frame.
//   record (PoolObj): +0 flags (bit 0 in use, bit 1 active - cDamageUnit_SetDamageCollActive; 0x600 being released),
//     +0x10 group (func_001FD908: 1..8 from the owning cDamageUnit's type +0x48 and give/take byte +0x49: odd = a
//     damage-give unit, an attack; even = damage-take, a hurt volume), +0x34 -> the shape, embedded at +0xE0.
//   shape (cCollisionShape, the decomp's XformNode): +0 kind (0 sphere, 2 cylinder, 4 box: the three factories
//     0x2B9DF0 / 0x2B9EF0 / 0x2BA020), +4 flags, +8 parent matrix, +0x10 local matrix, +0x70 -> the matrix in use
//     (world, local or the parent's: func_002BB2D8), +0x80 world matrix, +0xC0 -> its position (row 3 of that
//     matrix), +0x100 vtable (0x44BD60 sphere, 0x44BCA0 cylinder, 0x44BB20 box). Sphere: radius +0x110. Cylinder:
//     height +0x110, radius +0x114; the preview drew it as a capsule from pos to pos + height * the matrix's Y axis
//     with a sphere at each end. Box: size +0x110 (x, y, z), the eight corners (+-x/2, 0..y, +-z/2) through the
//     matrix: it stands on its origin.
//   The preview's colours: give red (0x808080FF), take green; here attack red, hurt blue, inactive dimmed.
//
// Collection: the hook km_hitbox_collect at 0x30EF34, the instruction after Move()'s call of func_002B9D58, on the
// game thread. Each shape becomes line segments in world space, projected with the last world-to-screen matrix
// cCamera_update built (camera.cpp's km_cam_matrix hook: the previous frame's, which is the camera that drew the
// poses these shapes were built from - the game's hit tests run one frame behind its picture, and so does the
// overlay, on purpose), clipped at the near plane in clip space, and sent to pcsx2gs as GS pixel coordinates
// (pcsx2gs_set_overlay); the DLL subtracts the scene's XYOFFSET and maps the frame onto the window at each present.
// "Dump this frame" writes the shape list to godhand.log once (world space), for diffing a 30 fps frame against a
// 60 fps one.
//
// Stage collision (same overlay, same hook): the room's wall/floor polygons, cCollisionScroll D_005CAE50 (the decomp
// matches SetLayerCollEnable 0x133330; the rest read from func_001331B8 and the preview's COLL_SCR_VIEW draw
// func_001325A0, which retail keeps behind DBG word 2 0x08000000). +0 -> header (+0x20 u16 group count, +0x28 u16
// polygon count), +4 -> named groups (0x14 each: +8 u16 first polygon, +0xA u16 count, +0xC name, up to 8 chars),
// +8 -> attribute records (0x10 each; +2 u16 flags the debug draw colours by, +0xD the layer byte
// SetLayerCollEnable matches), +0xC -> vertices (3 floats), +0x14 -> polygons (0x14 each: +0..+6 u16 vertex indices,
// +0xC u16 attribute record, +0x10 flags: bit 0 a quad, bit 1 switched off). Objects switch the group named like
// themselves (their name at +0x540) through cOmBase_setScrSollEnable 0x1B7680; the panel's ticks do the same write.
#include "kamite/hitbox_overlay.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/sprint_traversal.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_pcsx2_backend.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace kamite::hitbox_overlay
{
    namespace
    {
        constexpr uint32_t kCollisionManager = 0x5E8040u; // D_005E8040
        constexpr uint32_t kRecordStride = 0x270u;        // func_002BA178(mgr, 0x26C, 2)
        constexpr uint32_t kMaxRecords = 2048u;
        constexpr uint32_t kRecFlags = 0x00u, kRecGroup = 0x10u, kRecShape = 0x34u, kRecShapeEmbedded = 0xE0u;
        constexpr uint32_t kShapeKind = 0x00u, kShapeMatrix = 0x70u, kShapePos = 0xC0u;
        constexpr uint32_t kSphereRadius = 0x110u, kCylinderHeight = 0x110u, kCylinderRadius = 0x114u, kBoxSize = 0x110u;
        constexpr uint32_t kKindSphere = 0u, kKindCylinder = 2u, kKindBox = 4u;
        constexpr float kNearW = 0.05f;
        constexpr int kCircleSegments = 16;

        // Colours as the DLL's UI takes them: R in the low byte.
        constexpr uint32_t kAttack = 0xFF3030FFu, kHurt = 0xFFFF9040u, kOther = 0xFFB0B0B0u;
        constexpr uint32_t kDimAlpha = 0x50000000u;

        std::atomic<bool> g_enabled{false};
        std::atomic<bool> g_showAttack{true};
        std::atomic<bool> g_showHurt{true};
        std::atomic<bool> g_showInactive{false};
        std::atomic<bool> g_dumpRequest{false};
        std::atomic<int> g_shapesSeen{0};   // game thread -> UI: live shapes this frame
        std::atomic<int> g_shapesDrawn{0};
        std::atomic<int> g_unknownKinds{0};
        std::atomic<bool> g_dllTooOld{false};
        std::atomic<bool> g_haveMatrix{false};
        // Stage collision.
        constexpr uint32_t kScroll = 0x5CAE50u; // cCollisionScroll D_005CAE50
        constexpr uint32_t kScrHeader = 0x00u, kScrGroups = 0x04u, kScrAttrs = 0x08u, kScrVerts = 0x0Cu, kScrPolys = 0x14u;
        constexpr uint32_t kHdrGroupCount = 0x20u, kHdrPolyCount = 0x28u;
        constexpr uint32_t kGroupStride = 0x14u, kGroupFirst = 0x08u, kGroupCount = 0x0Au, kGroupName = 0x0Cu;
        constexpr uint32_t kPolyStride = 0x14u, kPolyAttr = 0x0Cu, kPolyFlags = 0x10u, kVertStride = 0x0Cu, kAttrStride = 0x10u;
        constexpr uint32_t kPolyQuad = 1u, kPolyOff = 2u;
        constexpr size_t kMaxLines = 30000u;
        constexpr uint32_t kWall = 0xFF2090FFu, kFloor = 0xFF40D040u, kNamed = 0xFFFFD040u, kOff = 0x70A0A0A0u,
                           kHighlight = 0xFFFF40FFu, kWindowColor = 0xFFFFFFFFu;
        constexpr float kMarkReach = 3.0f;  // Mark window: the wall must be this close ahead of Gene
        constexpr float kMarkHeight = 1.0f; // ... at this height above his feet

        struct GroupInfo
        {
            std::string name;
            uint16_t first = 0, count = 0;
            int state = 0; // 0 on, 1 off, 2 partly off
            bool nearGene = false;
        };
        std::atomic<bool> g_collEnabled{false};
        std::atomic<bool> g_collAllGroups{false};
        std::atomic<float> g_collRadius{8.0f};
        std::atomic<int> g_collHighlight{-1};
        std::atomic<bool> g_collDump{false};
        std::atomic<bool> g_saveFailed{false};
        std::atomic<bool> g_markWindow{false}; // GS thread -> game thread: mark the wall ahead of Gene as a window
        std::atomic<int> g_markResult{0};      // 0 nothing yet, 1 marked, 2 no wall ahead, 3 could not save
        std::atomic<int> g_collPolys{0}, g_collDrawn{0}, g_collRoom{-1};
        std::mutex g_collMutex;                     // guards the two below (game thread writes, GS thread reads)
        std::vector<GroupInfo> g_groupsShared;      // the room's named groups
        std::vector<std::pair<int, bool>> g_toggles; // GS thread -> game thread: switch group i on / off
        std::vector<int16_t> g_polyGroup;           // game thread: polygon -> its named group (-1 none)
        uint64_t g_collSignature = 0;               // game thread: the room the table above was built for

        bool g_sentClear = true; // game thread: the DLL holds no lines
        bool g_collected = false; // game thread: collect() ran since the last frame start

        float g_matrix[16] = {};       // game thread: the last world-to-screen matrix (column-major as camera.cpp uses it)
        std::vector<float> g_lines;    // game thread: x0 y0 x1 y1 per line, GS pixels
        std::vector<uint32_t> g_colors;

        struct Vec3
        {
            float x, y, z;
        };
        Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
        Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

        bool inRam(uint32_t address, uint32_t size)
        {
            const uint32_t a = address & 0x1FFFFFFFu;
            return a >= 0x100000u && a + size <= PS2_RAM_SIZE;
        }
        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }
        uint16_t rd16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t v = 0;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 2);
            return v;
        }
        float rdf(const uint8_t *rdram, uint32_t address)
        {
            float v = 0.0f;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }
        Vec3 rdv(const uint8_t *rdram, uint32_t address)
        {
            return {rdf(rdram, address), rdf(rdram, address + 4u), rdf(rdram, address + 8u)};
        }
        bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        struct Clip
        {
            float x, y, z, w;
        };
        Clip project(const float *m, Vec3 p)
        {
            return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                    m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14], m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15]};
        }

        // A world-space segment -> a GS-pixel line, cut at the near plane (w = kNearW) in clip space.
        void line(Vec3 a, Vec3 b, uint32_t color)
        {
            Clip ca = project(g_matrix, a), cb = project(g_matrix, b);
            if (ca.w < kNearW && cb.w < kNearW)
                return;
            if (ca.w < kNearW || cb.w < kNearW)
            {
                Clip &in = ca.w < kNearW ? cb : ca;
                Clip &out = ca.w < kNearW ? ca : cb;
                const float t = (kNearW - in.w) / (out.w - in.w);
                out = {in.x + (out.x - in.x) * t, in.y + (out.y - in.y) * t, in.z + (out.z - in.z) * t, kNearW};
            }
            const float x0 = ca.x / ca.w, y0 = ca.y / ca.w, x1 = cb.x / cb.w, y1 = cb.y / cb.w;
            if (!std::isfinite(x0 + y0 + x1 + y1))
                return;
            g_lines.insert(g_lines.end(), {x0, y0, x1, y1});
            g_colors.push_back(color);
        }

        // A circle of radius r round c in the plane spanned by the unit vectors u and v.
        void circle(Vec3 c, Vec3 u, Vec3 v, float r, uint32_t color, int from = 0, int to = kCircleSegments)
        {
            constexpr float kTau = 6.2831853f;
            Vec3 prev{};
            for (int i = from; i <= to; ++i)
            {
                const float a = kTau * static_cast<float>(i) / kCircleSegments;
                const Vec3 p = c + u * (r * std::cos(a)) + v * (r * std::sin(a));
                if (i > from)
                    line(prev, p, color);
                prev = p;
            }
        }

        Vec3 normalized(Vec3 v, Vec3 fallback)
        {
            const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            return l > 1e-6f ? v * (1.0f / l) : fallback;
        }
        Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
        // Two unit vectors perpendicular to the unit axis n.
        void basis(Vec3 n, Vec3 &u, Vec3 &v)
        {
            const Vec3 helper = std::fabs(n.y) < 0.9f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            u = normalized(cross(helper, n), {1.0f, 0.0f, 0.0f});
            v = cross(n, u);
        }

        void sphere(Vec3 c, float r, uint32_t color)
        {
            circle(c, {1, 0, 0}, {0, 1, 0}, r, color);
            circle(c, {1, 0, 0}, {0, 0, 1}, r, color);
            circle(c, {0, 1, 0}, {0, 0, 1}, r, color);
        }

        void capsule(Vec3 p0, Vec3 p1, float r, uint32_t color)
        {
            const Vec3 n = normalized(p1 + p0 * -1.0f, {0.0f, 1.0f, 0.0f});
            Vec3 u, v;
            basis(n, u, v);
            circle(p0, u, v, r, color);
            circle(p1, u, v, r, color);
            for (int k = 0; k < 4; ++k)
            {
                const float a = 1.5707963f * static_cast<float>(k);
                const Vec3 side = u * (r * std::cos(a)) + v * (r * std::sin(a));
                line(p0 + side, p1 + side, color);
            }
            // The caps: half circles in the two planes that hold the axis.
            const int half = kCircleSegments / 2;
            circle(p1, u, n, r, color, 0, half);       // u -> n -> -u over the top
            circle(p1, v, n, r, color, 0, half);
            circle(p0, u, n * -1.0f, r, color, 0, half); // under the bottom
            circle(p0, v, n * -1.0f, r, color, 0, half);
        }

        void box(const float *m, Vec3 size, uint32_t color)
        {
            const Vec3 ax{m[0], m[1], m[2]}, ay{m[4], m[5], m[6]}, az{m[8], m[9], m[10]}, origin{m[12], m[13], m[14]};
            const float hx = 0.5f * size.x, hz = 0.5f * size.z;
            Vec3 c[8];
            for (int i = 0; i < 8; ++i)
            {
                const float x = (i & 1) ? -hx : hx, y = (i & 4) ? 0.0f : size.y, z = (i & 2) ? -hz : hz;
                c[i] = origin + ax * x + ay * y + az * z;
            }
            static constexpr int kEdges[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7},
                                                  {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
            for (const auto &e : kEdges)
                line(c[e[0]], c[e[1]], color);
        }

        // After func_002B9D58 (the collision manager's update) in Move(): the shapes as the game just tested them.
        void clear()
        {
            if (!g_sentClear)
            {
                GSPcsx2Backend::SetOverlay(nullptr, nullptr, 0u);
                g_sentClear = true;
            }
        }

        // A game menu (pause, save/load, shop) is open: the shapes stay in the pool but are not drawn over it.
        bool menuOpen(const uint8_t *rdram)
        {
            if (rd32(rdram, game::GameWork::address(game::GameWork::kFlags5E4)) & game::Sta::kSubScr)
                return true;
            const uint32_t subScr = rd32(rdram, game::SubScr::kPointer);
            return inRam(subScr, 0x60u) && static_cast<int32_t>(rd32(rdram, subScr + game::SubScr::kKind)) != -1;
        }

        void gatherHitboxes(const uint8_t *rdram)
        {
            const bool dump = g_dumpRequest.exchange(false);
            const bool showAttack = g_showAttack.load(std::memory_order_relaxed);
            const bool showHurt = g_showHurt.load(std::memory_order_relaxed);
            const bool showInactive = g_showInactive.load(std::memory_order_relaxed);

            const uint32_t records = rd32(rdram, kCollisionManager);
            const uint32_t count = rd32(rdram, kCollisionManager + 4u);
            const uint32_t stride = rd32(rdram, kCollisionManager + 8u);
            int seen = 0, drawn = 0, unknown = 0;
            if (dump)
                KM_INFO(Game, "hitbox dump: " << count << " records of 0x" << std::hex << stride << " at 0x" << records << std::dec);
            if (stride >= 0x100u && stride <= 0x1000u && count <= kMaxRecords && inRam(records, count * stride))
            {
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t rec = records + i * stride;
                    const uint32_t flags = rd32(rdram, rec + kRecFlags);
                    if ((flags & 0x601u) != 1u) // free, or being released
                        continue;
                    const bool active = (flags & 2u) != 0u;
                    uint32_t shape = rd32(rdram, rec + kRecShape);
                    if (!inRam(shape, 0x120u))
                        shape = rec + kRecShapeEmbedded;
                    const uint32_t group = rd32(rdram, rec + kRecGroup);
                    const uint32_t kind = rd32(rdram, shape + kShapeKind);
                    const uint32_t matrix = rd32(rdram, shape + kShapeMatrix);
                    const uint32_t pos = rd32(rdram, shape + kShapePos);
                    ++seen;
                    const bool attack = group >= 1u && group <= 8u && (group & 1u) != 0u;
                    const bool hurt = group >= 1u && group <= 8u && (group & 1u) == 0u;
                    uint32_t color = attack ? kAttack : hurt ? kHurt : kOther;
                    if (!active)
                        color = (color & 0x00FFFFFFu) | kDimAlpha;
                    const bool show = (active || showInactive) && (attack ? showAttack : hurt ? showHurt : true);
                    if (!inRam(pos, 12u))
                        continue;
                    const Vec3 p = rdv(rdram, pos);
                    if (!finite(p))
                        continue;
                    float m[16] = {};
                    const bool haveMatrix = inRam(matrix, 64u);
                    if (haveMatrix)
                        std::memcpy(m, rdram + (matrix & 0x01FFFFFFu), sizeof(m));
                    if (dump)
                    {
                        char text[256];
                        std::snprintf(text, sizeof(text), "  #%u rec 0x%06X group %u %s kind %u %s pos %.3f %.3f %.3f", i, rec, group,
                                      attack ? "attack" : hurt ? "hurt" : "other", kind, active ? "active" : "idle", p.x, p.y, p.z);
                        std::string row = text;
                        if (kind == kKindSphere)
                            std::snprintf(text, sizeof(text), " r %.3f", rdf(rdram, shape + kSphereRadius));
                        else if (kind == kKindCylinder)
                            std::snprintf(text, sizeof(text), " r %.3f h %.3f axis %.3f %.3f %.3f", rdf(rdram, shape + kCylinderRadius),
                                          rdf(rdram, shape + kCylinderHeight), m[4], m[5], m[6]);
                        else if (kind == kKindBox)
                            std::snprintf(text, sizeof(text), " size %.3f %.3f %.3f", rdf(rdram, shape + kBoxSize),
                                          rdf(rdram, shape + kBoxSize + 4u), rdf(rdram, shape + kBoxSize + 8u));
                        else
                            std::snprintf(text, sizeof(text), " (unknown kind)");
                        KM_INFO(Game, "hitbox dump:" << row << text);
                    }
                    if (!show)
                        continue;
                    if (kind == kKindSphere)
                    {
                        const float r = rdf(rdram, shape + kSphereRadius);
                        if (std::isfinite(r) && r > 0.0f && r < 100.0f)
                            sphere(p, r, color), ++drawn;
                    }
                    else if (kind == kKindCylinder)
                    {
                        const float r = rdf(rdram, shape + kCylinderRadius), h = rdf(rdram, shape + kCylinderHeight);
                        if (haveMatrix && std::isfinite(r) && std::isfinite(h) && r > 0.0f && r < 100.0f && std::fabs(h) < 100.0f)
                        {
                            const Vec3 p1 = p + Vec3{m[4], m[5], m[6]} * h;
                            capsule(p, p1, r, color);
                            ++drawn;
                        }
                    }
                    else if (kind == kKindBox)
                    {
                        const Vec3 size = rdv(rdram, shape + kBoxSize);
                        if (haveMatrix && finite(size) && std::fabs(size.x) + std::fabs(size.y) + std::fabs(size.z) < 300.0f)
                            box(m, size, color), ++drawn;
                    }
                    else
                        ++unknown;
                }
            }
            g_shapesSeen.store(seen, std::memory_order_relaxed);
            g_shapesDrawn.store(drawn, std::memory_order_relaxed);
            g_unknownKinds.store(unknown, std::memory_order_relaxed);
            if (dump)
                KM_INFO(Game, "hitbox dump: " << seen << " shapes, " << drawn << " drawn, " << g_lines.size() / 4 << " lines");
        }

        // The room's collision tables, or false when no room is loaded.
        struct Scroll
        {
            uint32_t header, groups, attrs, verts, polys, groupCount, polyCount;
        };
        bool readScroll(const uint8_t *rdram, Scroll &s)
        {
            s.header = rd32(rdram, kScroll + kScrHeader);
            if (!inRam(s.header, 0x30u))
                return false;
            s.groups = rd32(rdram, kScroll + kScrGroups);
            s.attrs = rd32(rdram, kScroll + kScrAttrs);
            s.verts = rd32(rdram, kScroll + kScrVerts);
            s.polys = rd32(rdram, kScroll + kScrPolys);
            s.groupCount = rd16(rdram, s.header + kHdrGroupCount);
            s.polyCount = rd16(rdram, s.header + kHdrPolyCount);
            if (s.polyCount == 0u || !inRam(s.polys, s.polyCount * kPolyStride) || !inRam(s.verts, kVertStride))
                return false;
            if (!inRam(s.groups, s.groupCount * kGroupStride))
                s.groupCount = 0u;
            return true;
        }

        // The panel's ticks: the write func_001331B8 makes (the polygons' bit 1), on the game thread.
        void applyToggles(uint8_t *rdram)
        {
            std::vector<std::pair<int, bool>> toggles;
            {
                std::lock_guard<std::mutex> lock(g_collMutex);
                toggles.swap(g_toggles);
            }
            Scroll s{};
            if (toggles.empty() || !readScroll(rdram, s))
                return;
            for (const auto &[index, on] : toggles)
            {
                if (index < 0 || static_cast<uint32_t>(index) >= s.groupCount)
                    continue;
                const uint32_t g = s.groups + static_cast<uint32_t>(index) * kGroupStride;
                const uint32_t first = rd16(rdram, g + kGroupFirst), count = rd16(rdram, g + kGroupCount);
                for (uint32_t p = first; p < first + count && p < s.polyCount; ++p)
                {
                    const uint32_t f = s.polys + p * kPolyStride + kPolyFlags;
                    const uint32_t flags = rd32(rdram, f);
                    const uint32_t next = on ? (flags & ~kPolyOff) : (flags | kPolyOff);
                    std::memcpy(rdram + (f & 0x01FFFFFFu), &next, 4);
                }
                KM_INFO(Game, "stage collision: group " << index << " switched " << (on ? "on" : "off") << " from the Debug tab");
            }
        }

        void gatherCollision(const uint8_t *rdram)
        {
            const bool dump = g_collDump.exchange(false);
            Scroll s{};
            const uint32_t player = rd32(rdram, game::GameWork::address(game::GameWork::kPlayerObj));
            const uint32_t playerPos = inRam(player, 0x110u) ? rd32(rdram, player + game::GameObj::kPos) : 0u;
            if (!readScroll(rdram, s) || !inRam(playerPos, 12u))
            {
                g_collPolys.store(0, std::memory_order_relaxed);
                return;
            }
            const Vec3 gene = rdv(rdram, playerPos);
            const float radius = g_collRadius.load(std::memory_order_relaxed);
            const int highlight = g_collHighlight.load(std::memory_order_relaxed);
            const int room = static_cast<int>(rd16(rdram, game::GameWork::address(game::GameWork::kRoom)));

            // Polygon -> group, rebuilt when the room's tables change.
            const uint64_t signature = (static_cast<uint64_t>(s.polys) << 32) ^ (static_cast<uint64_t>(s.groups) << 8) ^
                                       (s.polyCount << 16) ^ s.groupCount;
            std::vector<GroupInfo> groups(s.groupCount);
            if (signature != g_collSignature)
            {
                g_collSignature = signature;
                g_polyGroup.assign(s.polyCount, -1);
                g_collHighlight.store(-1, std::memory_order_relaxed);
            }
            for (uint32_t i = 0; i < s.groupCount; ++i)
            {
                const uint32_t g = s.groups + i * kGroupStride;
                char name[9] = {};
                std::memcpy(name, rdram + ((g + kGroupName) & 0x01FFFFFFu), 8);
                groups[i].name = name[0] ? name : "(no name)";
                groups[i].first = rd16(rdram, g + kGroupFirst);
                groups[i].count = rd16(rdram, g + kGroupCount);
                for (uint32_t p = groups[i].first; p < uint32_t(groups[i].first) + groups[i].count && p < s.polyCount; ++p)
                    g_polyGroup[p] = static_cast<int16_t>(i);
            }

            if (dump)
            {
                KM_INFO(Game, "collision dump: room r" << std::hex << room << " header 0x" << s.header << " polys 0x" << s.polys
                                                       << " verts 0x" << s.verts << " attrs 0x" << s.attrs << std::dec << ", "
                                                       << s.polyCount << " polygons, " << s.groupCount << " groups; Gene "
                                                       << gene.x << " " << gene.y << " " << gene.z);
                for (uint32_t i = 0; i < s.groupCount; ++i)
                    KM_INFO(Game, "collision dump: group " << i << " '" << groups[i].name << "' polygons " << groups[i].first
                                                           << ".." << (groups[i].first + groups[i].count));
            }

            // Mark window: the nearest wall polygon straight ahead of Gene, at hip height.
            const bool mark = g_markWindow.exchange(false);
            const float heading = rdf(rdram, player + game::GameObj::kRot + 4u);
            const Vec3 rayFrom{gene.x, gene.y + kMarkHeight, gene.z}, rayDir{std::sin(heading), 0.0f, std::cos(heading)};
            struct WallHit
            {
                float t;
                int group;
                Vec3 normal;
            };
            std::vector<WallHit> wallHits; // every wall the ray crosses within reach + 3 m (the far face of a thick wall)
            auto rayTriangle = [&](Vec3 a, Vec3 b, Vec3 c) -> float { // Moller-Trumbore; < 0 = miss
                const Vec3 e1{b.x - a.x, b.y - a.y, b.z - a.z}, e2{c.x - a.x, c.y - a.y, c.z - a.z};
                const Vec3 pv{rayDir.y * e2.z - rayDir.z * e2.y, rayDir.z * e2.x - rayDir.x * e2.z, rayDir.x * e2.y - rayDir.y * e2.x};
                const float det = e1.x * pv.x + e1.y * pv.y + e1.z * pv.z;
                if (std::fabs(det) < 1e-6f)
                    return -1.0f;
                const Vec3 tv{rayFrom.x - a.x, rayFrom.y - a.y, rayFrom.z - a.z};
                const float u = (tv.x * pv.x + tv.y * pv.y + tv.z * pv.z) / det;
                if (u < 0.0f || u > 1.0f)
                    return -1.0f;
                const Vec3 qv{tv.y * e1.z - tv.z * e1.y, tv.z * e1.x - tv.x * e1.z, tv.x * e1.y - tv.y * e1.x};
                const float v = (rayDir.x * qv.x + rayDir.y * qv.y + rayDir.z * qv.z) / det;
                if (v < 0.0f || u + v > 1.0f)
                    return -1.0f;
                return (e2.x * qv.x + e2.y * qv.y + e2.z * qv.z) / det;
            };

            std::vector<int> onCount(s.groupCount, 0), offCount(s.groupCount, 0);
            int drawn = 0;
            for (uint32_t p = 0; p < s.polyCount; ++p)
            {
                const uint32_t rec = s.polys + p * kPolyStride;
                const uint32_t flags = rd32(rdram, rec + kPolyFlags);
                const bool off = (flags & kPolyOff) != 0u;
                const int group = g_polyGroup[p];
                if (group >= 0)
                    (off ? offCount : onCount)[group]++;
                const int corners = (flags & kPolyQuad) ? 4 : 3;
                Vec3 v[4] = {};
                bool ok = true;
                Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
                for (int c = 0; c < corners && ok; ++c)
                {
                    const uint32_t a = s.verts + rd16(rdram, rec + 2u * c) * kVertStride;
                    ok = inRam(a, 12u);
                    if (!ok)
                        break;
                    v[c] = rdv(rdram, a);
                    ok = finite(v[c]);
                    lo = {std::min(lo.x, v[c].x), std::min(lo.y, v[c].y), std::min(lo.z, v[c].z)};
                    hi = {std::max(hi.x, v[c].x), std::max(hi.y, v[c].y), std::max(hi.z, v[c].z)};
                }
                if (!ok)
                    continue;
                // The nearest point of the polygon's box to Gene, within the radius.
                const float dx = std::max({lo.x - gene.x, 0.0f, gene.x - hi.x});
                const float dy = std::max({lo.y - gene.y, 0.0f, gene.y - hi.y});
                const float dz = std::max({lo.z - gene.z, 0.0f, gene.z - hi.z});
                if (dx * dx + dy * dy + dz * dz > radius * radius)
                    continue;
                if (group >= 0)
                    groups[group].nearGene = true;
                const Vec3 e1{v[1].x - v[0].x, v[1].y - v[0].y, v[1].z - v[0].z};
                const Vec3 e2{v[2].x - v[0].x, v[2].y - v[0].y, v[2].z - v[0].z};
                const Vec3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
                const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                const bool flat = len > 0.0f && std::fabs(n.y) > 0.7f * len;
                if (mark && !flat && len > 0.0f)
                {
                    float t = rayTriangle(v[0], v[1], v[2]);
                    if (corners == 4)
                    {
                        const float t2 = rayTriangle(v[0], v[2], v[3]);
                        if (t2 >= 0.0f && (t < 0.0f || t2 < t))
                            t = t2;
                    }
                    if (t >= 0.0f && t < kMarkReach + 3.0f)
                        wallHits.push_back({t, group, n});
                }
                if (dump)
                {
                    const uint32_t attr = s.attrs + rd16(rdram, rec + kPolyAttr) * kAttrStride;
                    char text[512];
                    int at = std::snprintf(text, sizeof(text), "collision dump: poly %u %s '%s' flags %08X attr %u [", p,
                                           flat ? "floor" : "wall", group >= 0 ? groups[group].name.c_str() : "-", flags,
                                           rd16(rdram, rec + kPolyAttr));
                    for (uint32_t b = 0; b < kAttrStride && inRam(attr, kAttrStride) && at < 400; ++b)
                        at += std::snprintf(text + at, sizeof(text) - at, "%02X", rdram[(attr + b) & 0x01FFFFFFu]);
                    at += std::snprintf(text + at, sizeof(text) - at, "]");
                    for (int c = 0; c < corners && at < 480; ++c)
                        at += std::snprintf(text + at, sizeof(text) - at, " (%.2f %.2f %.2f)", v[c].x, v[c].y, v[c].z);
                    KM_INFO(Game, text);
                }
                if (g_colors.size() >= kMaxLines)
                    continue;
                uint32_t color = off ? kOff : group >= 0 ? kNamed : flat ? kFloor : kWall;
                if (group >= 0 && group == highlight)
                    color = kHighlight;
                for (int c = 0; c < corners; ++c)
                    line(v[c], v[(c + 1) % corners], color);
                ++drawn;
            }
            if (mark)
            {
                std::sort(wallHits.begin(), wallHits.end(), [](const WallHit &a, const WallHit &b) { return a.t < b.t; });
                const bool hit = !wallHits.empty() && wallHits[0].t <= kMarkReach;
                const Vec3 markNormal = hit ? wallHits[0].normal : Vec3{0.0f, 0.0f, 0.0f};
                const int markGroup = hit ? wallHits[0].group : -2;
                const float markT = hit ? wallHits[0].t : 0.0f;
                const float h = std::sqrt(markNormal.x * markNormal.x + markNormal.z * markNormal.z);
                if (!hit || h <= 0.0f)
                    g_markResult.store(2, std::memory_order_relaxed);
                else
                {
                    sprint_traversal::Window w;
                    w.x = rayFrom.x + rayDir.x * markT;
                    w.y = gene.y;
                    w.z = rayFrom.z + rayDir.z * markT;
                    w.dx = -markNormal.z / h; // along the wall
                    w.dz = markNormal.x / h;
                    // The wall's far face: the next parallel wall the ray crosses within 3 m (a thick wall).
                    for (size_t k = 1; k < wallHits.size(); ++k)
                    {
                        const WallHit &far = wallHits[k];
                        const float fh = std::sqrt(far.normal.x * far.normal.x + far.normal.z * far.normal.z);
                        if (far.t <= markT + 0.05f || far.t > markT + 3.0f || fh <= 0.0f)
                            continue;
                        if (std::fabs((far.normal.x * markNormal.x + far.normal.z * markNormal.z) / (fh * h)) < 0.9f)
                            continue;
                        w.depth = (far.t - markT) * (rayDir.x * -w.dz + rayDir.z * w.dx);
                        break;
                    }
                    const std::string name = markGroup >= 0 ? groups[markGroup].name : "-";
                    g_markResult.store(sprint_traversal::addWindow(static_cast<uint16_t>(room), name, w) ? 1 : 3,
                                       std::memory_order_relaxed);
                }
            }
            // The room's windows (white): the opening's rectangle and its diagonals.
            for (const auto &ref : sprint_traversal::windows(static_cast<uint16_t>(room)))
            {
                const auto &w = ref.window;
                const float cx = w.x + w.dx * w.offset, cz = w.z + w.dz * w.offset, half = 0.5f * w.width;
                const float y0 = w.y + w.sill, y1 = w.y + w.sill + w.height;
                const Vec3 a0{cx - w.dx * half, y0, cz - w.dz * half}, b0{cx + w.dx * half, y0, cz + w.dz * half};
                const Vec3 a1{a0.x, y1, a0.z}, b1{b0.x, y1, b0.z};
                if (g_colors.size() + 10u > kMaxLines)
                    break;
                line(a0, b0, kWindowColor);
                line(b0, b1, kWindowColor);
                line(b1, a1, kWindowColor);
                line(a1, a0, kWindowColor);
                line(a0, b1, kWindowColor);
                line(b0, a1, kWindowColor);
                if (std::fabs(w.depth) > 0.01f) // the far face
                {
                    const Vec3 off{-w.dz * w.depth, 0.0f, w.dx * w.depth};
                    line(a0 + off, b0 + off, kWindowColor);
                    line(b0 + off, b1 + off, kWindowColor);
                    line(b1 + off, a1 + off, kWindowColor);
                    line(a1 + off, a0 + off, kWindowColor);
                }
            }
            for (uint32_t i = 0; i < s.groupCount; ++i)
                groups[i].state = offCount[i] == 0 ? 0 : onCount[i] == 0 ? 1 : 2;
            g_collPolys.store(static_cast<int>(s.polyCount), std::memory_order_relaxed);
            g_collDrawn.store(drawn, std::memory_order_relaxed);
            g_collRoom.store(room, std::memory_order_relaxed);
            if (dump)
                KM_INFO(Game, "collision dump: " << drawn << " polygons within " << radius << " m");
            std::lock_guard<std::mutex> lock(g_collMutex);
            g_groupsShared.swap(groups);
        }

        void collect(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            g_collected = true;
            applyToggles(rdram);
            const bool hitboxes = g_enabled.load(std::memory_order_relaxed);
            const bool collision = g_collEnabled.load(std::memory_order_relaxed);
            if ((!hitboxes && !collision) || menuOpen(rdram))
            {
                clear();
                return;
            }
            if (!g_haveMatrix.load(std::memory_order_relaxed))
                return;
            g_lines.clear();
            g_colors.clear();
            if (hitboxes)
                gatherHitboxes(rdram);
            if (collision)
                gatherCollision(rdram);
            const bool sent = GSPcsx2Backend::SetOverlay(g_lines.empty() ? nullptr : g_lines.data(),
                                                         g_colors.empty() ? nullptr : g_colors.data(),
                                                         static_cast<uint32_t>(g_colors.size()));
            g_dllTooOld.store(!sent, std::memory_order_relaxed);
            g_sentClear = g_lines.empty();
        }
    }

    void onFrame()
    {
        // Move() did not run last frame (title screen, a menu that stops the game): the DLL would keep the last lines.
        if (!g_collected)
            clear();
        g_collected = false;
    }

    void noteScreenMatrix(const float *m16)
    {
        if (!m16)
            return;
        std::memcpy(g_matrix, m16, sizeof(g_matrix));
        g_haveMatrix.store(true, std::memory_order_relaxed);
    }

    void install()
    {
        ps2xRegisterHostHook("km_hitbox_collect", &collect);
    }

    void drawPanel(const Pcsx2GsUi *ui)
    {
#if defined(KAMITE_HAS_GS_MENU)
        if (ui->version < 4u)
            return;
        ui->separator_text("Hitboxes");
        auto toggle = [&](const char *label, std::atomic<bool> &setting) {
            int on = setting.load() ? 1 : 0;
            if (ui->checkbox(label, &on))
                setting = on != 0;
        };
        toggle("Show hitboxes (red = attack, blue = hurt; where the game tested them, one frame behind the picture)##hitboxes",
               g_enabled);
        if (g_enabled.load())
        {
            ui->indent();
            toggle("Attack volumes (red)##hbattack", g_showAttack);
            toggle("Hurt volumes (blue)##hbhurt", g_showHurt);
            toggle("Inactive shapes too (dimmed)##hbidle", g_showInactive);
            ui->unindent();
            char text[128];
            std::snprintf(text, sizeof(text), "%d shapes in use, %d drawn%s.", g_shapesSeen.load(), g_shapesDrawn.load(),
                          g_unknownKinds.load() ? " (some of an unknown kind: not drawn)" : "");
            ui->text_disabled(text);
            if (ui->button("Dump this frame to godhand.log##hbdump"))
                g_dumpRequest.store(true);
            if (g_dllTooOld.load())
                ui->text_colored(0xFF4040FFu, "This pcsx2gs.dll has no overlay (older than the game): build again.");
            else if (!g_haveMatrix.load())
                ui->text_disabled("Waiting for the game's camera.");
        }

        ui->separator_text("Stage collision");
        toggle("Show stage collision near Gene (orange walls, green floors, cyan named groups, grey switched off, magenta "
               "the highlighted group)##stagecoll",
               g_collEnabled);
        if (!g_collEnabled.load())
            return;
        ui->indent();
        float radius = g_collRadius.load();
        if (ui->slider_float("Radius##collradius", &radius, 3.0f, 40.0f, "%.0f m"))
            g_collRadius = radius;
        toggle("List every named group, not only the ones near Gene##collall", g_collAllGroups);
        ui->unindent();
        char text[160];
        std::snprintf(text, sizeof(text), "Room r%x: %d polygons, %d drawn.", g_collRoom.load() < 0 ? 0 : g_collRoom.load(),
                      g_collPolys.load(), g_collDrawn.load());
        ui->text_disabled(text);
        if (ui->button("Dump the polygons near Gene to godhand.log##colldump"))
            g_collDump.store(true);
        std::vector<GroupInfo> groups;
        {
            std::lock_guard<std::mutex> lock(g_collMutex);
            groups = g_groupsShared;
        }
        ui->text_wrapped("Named groups. First tick = solid (untick lets everyone through until the room reloads). Jump / "
                         "Slide = the sprint attacks that pass the group with \"Sprint attack traversal\" on: jump for "
                         "sprint + X / Y, slide for sprint + A (saved to sprint_traversal.txt at once).");
        if (!sprint_traversal::enabled())
            ui->text_disabled("Sprint attack traversal is off in the launcher: the marks save but do nothing in play.");
        if (g_saveFailed.load())
            ui->text_colored(0xFF4040FFu, "Could not write sprint_traversal.txt.");
        const bool all = g_collAllGroups.load();
        const int highlight = g_collHighlight.load();
        int listed = 0;
        if (ui->begin_child("##collgroups", 0.0f, 220.0f))
        {
            for (size_t i = 0; i < groups.size(); ++i)
            {
                if (!all && !groups[i].nearGene)
                    continue;
                ++listed;
                ui->push_id(static_cast<int>(i));
                int on = groups[i].state == 1 ? 0 : 1;
                char label[96];
                std::snprintf(label, sizeof(label), "%s (%u polygons)%s##grp", groups[i].name.c_str(), groups[i].count,
                              groups[i].state == 2 ? ", partly off" : "");
                if (ui->checkbox(label, &on))
                {
                    std::lock_guard<std::mutex> lock(g_collMutex);
                    g_toggles.emplace_back(static_cast<int>(i), on != 0);
                }
                // Sprint attack traversal marks (sprint_traversal.txt, saved at once).
                const int room = g_collRoom.load();
                if (room >= 0)
                {
                    const uint16_t r = static_cast<uint16_t>(room);
                    const int marks = sprint_traversal::marks(r, groups[i].name);
                    int jump = (marks & sprint_traversal::kJump) ? 1 : 0, slide = (marks & sprint_traversal::kSlide) ? 1 : 0;
                    ui->same_line();
                    const bool a = ui->checkbox("Jump##tj", &jump) != 0;
                    ui->same_line();
                    const bool b = ui->checkbox("Slide##ts", &slide) != 0;
                    if ((a || b) && !sprint_traversal::setMarks(r, groups[i].name, (jump ? sprint_traversal::kJump : 0) |
                                                                                       (slide ? sprint_traversal::kSlide : 0)))
                        g_saveFailed.store(true);
                }
                ui->same_line();
                if (ui->radio("Highlight##hl", highlight == static_cast<int>(i)))
                    g_collHighlight.store(highlight == static_cast<int>(i) ? -1 : static_cast<int>(i));
                ui->pop_id();
            }
            if (!listed)
                ui->text_disabled(groups.empty() ? "This room has no named groups." : "None within the radius.");
        }
        ui->end_child();

        // Windows: openings in a wall Gene dives through with sprint + X / Y (sprint_traversal.txt).
        const int room = g_collRoom.load();
        if (room < 0)
            return;
        const uint16_t r = static_cast<uint16_t>(room);
        ui->text_wrapped("Windows (white): sprint + X / Y dive through them with \"Sprint attack traversal\" on. Stand facing "
                         "the window, within 3 m, and mark it; then size and place it with the sliders. A thick wall's far "
                         "side is found by itself (the second rectangle; 0 = none). Any number per wall.");
        if (ui->button("Mark window (the wall Gene faces)##markwin"))
        {
            g_markResult.store(0);
            g_markWindow.store(true);
        }
        const int result = g_markResult.load();
        ui->same_line();
        ui->text_disabled(result == 1   ? "Marked."
                          : result == 2 ? "No wall within 3 m ahead of Gene."
                          : result == 3 ? "Could not write sprint_traversal.txt."
                                        : "");
        const auto list = sprint_traversal::windows(r);
        bool dragging = false;
        if (ui->begin_child("##collwindows", 0.0f, list.empty() ? 30.0f : 230.0f))
        {
            if (list.empty())
                ui->text_disabled("No windows in this room.");
            for (size_t i = 0; i < list.size(); ++i)
            {
                const auto &ref = list[i];
                sprint_traversal::Window w = ref.window;
                ui->push_id(1000 + static_cast<int>(i));
                char label[96];
                std::snprintf(label, sizeof(label), "%s, window %d", ref.group.c_str(), ref.index + 1);
                ui->text(label);
                ui->same_line();
                const bool remove = ui->small_button("Delete##delwin") != 0;
                bool changed = false;
                ui->set_next_width(220.0f);
                changed |= ui->slider_float("Width##ww", &w.width, 0.3f, 6.0f, "%.2f m") != 0;
                dragging |= ui->is_item_active() != 0;
                ui->same_line();
                ui->set_next_width(220.0f);
                changed |= ui->slider_float("Height##wh", &w.height, 0.3f, 4.0f, "%.2f m") != 0;
                dragging |= ui->is_item_active() != 0;
                ui->set_next_width(220.0f);
                changed |= ui->slider_float("Along the wall##wo", &w.offset, -4.0f, 4.0f, "%.2f m") != 0;
                dragging |= ui->is_item_active() != 0;
                ui->same_line();
                ui->set_next_width(220.0f);
                changed |= ui->slider_float("Sill##wsill", &w.sill, 0.0f, 3.0f, "%.2f m") != 0;
                dragging |= ui->is_item_active() != 0;
                ui->set_next_width(220.0f);
                changed |= ui->slider_float("Wall thickness (far side)##wdepth", &w.depth, -3.0f, 3.0f, "%.2f m") != 0;
                dragging |= ui->is_item_active() != 0;
                ui->separator();
                ui->pop_id();
                if (remove)
                {
                    if (!sprint_traversal::removeWindow(r, ref.group, ref.index))
                        g_saveFailed.store(true);
                    break; // the indices moved
                }
                if (changed)
                    sprint_traversal::setWindow(r, ref.group, ref.index, w);
            }
        }
        ui->end_child();
        if (!dragging && !sprint_traversal::flushWindows())
            g_saveFailed.store(true);
#else
        (void)ui;
#endif
    }
}
