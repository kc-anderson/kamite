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
#include "kamite/hitbox_overlay.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_pcsx2_backend.h"

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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

        void collect(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            g_collected = true;
            if (!g_enabled.load(std::memory_order_relaxed) || menuOpen(rdram))
            {
                clear();
                return;
            }
            if (!g_haveMatrix.load(std::memory_order_relaxed))
                return;
            const bool dump = g_dumpRequest.exchange(false);
            const bool showAttack = g_showAttack.load(std::memory_order_relaxed);
            const bool showHurt = g_showHurt.load(std::memory_order_relaxed);
            const bool showInactive = g_showInactive.load(std::memory_order_relaxed);
            g_lines.clear();
            g_colors.clear();

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
#else
        (void)ui;
#endif
    }
}
