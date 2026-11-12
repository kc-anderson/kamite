// The gameplay camera's field of view and framing, and the enemies' facing cones that follow what it shows.
//
// Camera (cPlCamera, s1 in the camera hooks). It looks at a point on Gene (+0x200, 1.06 m up) from an orbit around it
// (eye +0x210): pitch +0x410 (radians, > 0 = from above), yaw +0x414, distance +0x420 (cPlCamera_SetNormalZoomOutMode:
// 4 m, 4 degrees; the battle presets at 0x3BD548 / 0x3BD5F0 / 0x3BD698 that it eases to while Gene has a live target:
// 3.08 m and 10 degrees, or 4.38 m and 6 degrees, swung 20-24 degrees over the shoulder). It eases its fov (degrees,
// +0x250) toward a target every frame, fov = fov*0.9 + target*0.1, at two places in func_00139FF8 (called by
// cPlCamera_update), and resets it to 31 in cPlCamera_setCamUpdate. +0x51C counts down from 15 while the battle (or
// another preset) camera is on.
//
// The fov hooks replace those three stores so the eased value goes to target*m, where m eases (at the camera's own
// rate) toward FieldOfView outside fights and FightFieldOfView in them: everything the game derives from the fov
// follows (projection, drawing clip, LOD); cutscene and other cameras keep theirs. The orbit hooks replace the loads of
// the pitch and the distance where the eye is placed (both update paths and setCamUpdate):
//   distance * (1 - c + c * tan(fov/m / 2) / tan(fov / 2))   c = CameraDistance / 100: 1 keeps Gene the same size
//   pitch + CameraTilt * (m - 1) / (FieldOfView - 1)          the camera rises (looking at the same point) with the
//                                                            extra width, none at m = 1
// The hooks are always on: at 100% they compute exactly the game's values, and the F2 menu can change the settings
// while playing.
//
// Enemies (docs/enemy-ai.md). They never test the camera; they go by the angle between Gene's facing and the enemy
// (+0x768). With [Game] EnemiesFollowView, the angles they compare it with grow by how much further round Gene the
// picture now reaches: at the enemy's distance r (+0x618 = r squared), the screen's edge passes Gene's facing at
//   edge(r) = phi + asin(D sin(phi) / r)   (180 degrees when D sin(phi) >= r)
// with phi = atan(A tan(fov/2)) (A = the picture's aspect) and D = the camera's distance on the floor. Every angle is
// scaled by edge_now / edge_ps2 (the PS2: the game's own fov and distance, 4:3), so an enemy keeps the place it had
// relative to the screen's edge; at 100% and 4:3 nothing changes. The compares:
//   func_00211C08 0x2126C4, 0x2127D0  c.le.s $f0,$f1: $f0 = 60 degrees (the wedge), $f1 = the angle
//   func_00262AA8 0x263CF4 ... 0x267FC8 (9)  c.lt.s $f0,$f1: $f0 = the angle, $f1 = the level's lunge cone
// The hooks run before the compare and scale the cone's register (every path after it loads both registers again).
#include "kamite/camera.h"
#include "kamite/hitbox_overlay.h"
#include "kamite/camera_presets.h"
#include "kamite/ini_file.h"
#include "kamite/modern_controls.h"
#include "kamite/near_geometry.h"
#include "kamite/log.h"
#include "kamite/game_layout.h"
#include "kamite/function_hooks.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h" // GPR_U32
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/ps2_game_menu.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

namespace kamite
{
    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kDegToRad = kPi / 180.0f;
        constexpr float kNativeAspect = 4.0f / 3.0f;

        // Settings: written by the F2 menu (GS thread), read by the hooks (game thread).
        std::atomic<float> g_fovPercent{100.0f};
        std::atomic<float> g_fightPercent{100.0f};
        std::atomic<float> g_closerPercent{100.0f};
        std::atomic<float> g_tiltDegrees{0.0f};
        std::atomic<bool> g_enemyView{true};
        std::atomic<bool> g_depthOfField{true}; // [Video] DepthOfField (PS2X_PCSX2_DOF), the Graphics tab
        std::atomic<bool> g_preciseDepth{true}; // [Video] PreciseDepth (PS2X_PCSX2_PRECISE_DEPTH), the Graphics tab
        std::atomic<bool> g_perspectiveShading{true}; // [Video] PerspectiveShading (PS2X_PCSX2_PERSPECTIVE_SHADING)
        float g_aspect = kNativeAspect;
        std::filesystem::path g_iniPath;

        // The camera as the hooks saw it last (game thread; the menu reads them for its readout).
        float g_mul = 1.0f;                      // m
        std::atomic<float> g_camFov{31.0f};      // the camera's fov now, degrees
        std::atomic<float> g_eyeDistance{4.0f};  // eye to look point on the floor, metres
        std::atomic<float> g_dolly{1.0f};        // our distance / the game's
        std::atomic<bool> g_battle{false};       // +0x51C > 0

        float fovFactor() { return g_fovPercent.load(std::memory_order_relaxed) / 100.0f; }
        float fightFactor() { return g_fightPercent.load(std::memory_order_relaxed) / 100.0f; }

        uint32_t cameraAt(R5900Context *ctx, uint32_t offset) { return (GPR_U32(ctx, 17) + offset) & (PS2_RAM_SIZE - 1u); }

        float loadFloat(const uint8_t *rdram, uint32_t address)
        {
            float value;
            std::memcpy(&value, rdram + (address & (PS2_RAM_SIZE - 1u)), sizeof(value));
            return value;
        }

        void storeFov(uint8_t *rdram, R5900Context *ctx, float value)
        {
            std::memcpy(rdram + cameraAt(ctx, 0x250u), &value, sizeof(value));
        }

        float g_lastDistance = 4.0f; // the eye's distance from the look point last update (orbitDistance)

        // Depth range. cCamera_update builds the projection with sceVu0ViewScreenMatrix (func_0030AA40, called at
        // 0x138864): zmin 1, zmax 2031584 / fov (65535 at the player camera's 31 degrees), near 0.1, far 600, into a
        // 16-bit depth buffer (Z16S): depth falls as 1/distance, coarse in the distance (the renderer makes it precise
        // again from Q: pcsx2gs, GSRendererHW::ApplyPreciseDepth). A wider fov shrinks zmax with it, and that is left
        // alone: every projection of the frame has to share one depth scale. Two fixes were tried and undone:
        // - the near plane moved out: the game culls what comes nearer than it, so walls near the camera vanished;
        // - zmax put back for the player camera (a hook before 0x13884C): the stage is drawn with its own projection,
        //   built the same way from the camera's fov (zmax 2031584 / fov at 0x2A3A88, also 0x29E9A8 and Fil_clut_set),
        //   so it kept the widened fov's smaller range while the models had the PS2's, and walls lost the depth test to
        //   models behind them (dump gif_11967: Z = 4285 Q for a wall, 6569 Q for the cabinet behind it).
        // Some passes use a fixed zmax of 65535 (func_0014C7E8, func_0014D0E8, trails, most 2D): at a wider fov they sit
        // a little nearer in depth than the scene, as they already did at the game's own wider battle fovs.

        // Once per camera update: m eases toward the fight or the exploration value, and the readout is taken.
        // The camera preset the battle camera eases to (D_003BD540, an index into 0x3BD548/0x3BD5F0/0x3BD698): 0, 1
        // the usual battle framings; 2, 5, 6, 7, 10, 11, 12 bosses'; 3, 4, 8, 9, 13 the close-ups of grabs and
        // special moves (the game's own moment: the Modern controls step aside).
        constexpr uint32_t kCameraPreset = 0x3BD540u;

        // The player camera (cPlCamera, s1 in its own hooks; s4 in cCamera_update) and when its own logic last updated
        // it: the see-through oval below works from these.
        std::atomic<uint32_t> g_playerCamera{0};
        std::atomic<int64_t> g_playerCameraMs{0};

        int64_t steadyMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        void easeMultiplier(const uint8_t *rdram, R5900Context *ctx, bool secondPath)
        {
            int32_t preset;
            std::memcpy(&preset, rdram + cameraAt(ctx, 0x51Cu), sizeof(preset));
            const bool battle = preset > 0;
            int32_t mode;
            std::memcpy(&mode, rdram + kCameraPreset, sizeof(mode));
            const bool closeUp = battle && (mode == 3 || mode == 4 || mode == 8 || mode == 9 || mode == 13);
            modern::noteCamera(rdram, battle, secondPath || closeUp);
            g_playerCamera.store(GPR_U32(ctx, 17), std::memory_order_relaxed);
            g_playerCameraMs.store(steadyMs(), std::memory_order_relaxed);
            g_mul += ((battle ? fightFactor() : fovFactor()) - g_mul) * 0.1f;
            g_battle.store(battle, std::memory_order_relaxed);
            g_camFov.store(loadFloat(rdram, cameraAt(ctx, 0x250u)), std::memory_order_relaxed);
            const float dx = loadFloat(rdram, cameraAt(ctx, 0x210u)) - loadFloat(rdram, cameraAt(ctx, 0x200u));
            const float dz = loadFloat(rdram, cameraAt(ctx, 0x218u)) - loadFloat(rdram, cameraAt(ctx, 0x208u));
            const float floor = std::sqrt(dx * dx + dz * dz);
            if (floor > 0.1f && floor < 50.0f)
                g_eyeDistance.store(floor, std::memory_order_relaxed);
        }

        // 0x13A6E4 swc1 $f1,0x250(s1): $f1 = fov*0.9 + $f20*0.1
        void fovEaseA(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            easeMultiplier(rdram, ctx, false);
            storeFov(rdram, ctx, ctx->f[1] + 0.1f * (g_mul - 1.0f) * ctx->f[20]);
        }
        // 0x13B368 swc1 $f0,0x250(s1): $f0 = fov*0.9 + $f3*0.1
        void fovEaseB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            easeMultiplier(rdram, ctx, true);
            storeFov(rdram, ctx, ctx->f[0] + 0.1f * (g_mul - 1.0f) * ctx->f[3]);
        }
        // 0x13BBB8 swc1 $f0,0x250(s1): $f0 = 31.0, the player camera's starting fov (setCamUpdate also clears +0x51C)
        void fovReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            g_mul = fovFactor();
            modern::resetCamera();
            storeFov(rdram, ctx, ctx->f[0] * g_mul);
        }

        float orbitDistance(const uint8_t *rdram, R5900Context *ctx)
        {
            const float distance = loadFloat(rdram, cameraAt(ctx, 0x420u));
            const float fov = loadFloat(rdram, cameraAt(ctx, 0x250u));
            const float closer = g_closerPercent.load(std::memory_order_relaxed) / 100.0f;
            float dolly = 1.0f;
            if (closer > 0.0f && fov > 1.0f && fov < 170.0f && std::fabs(g_mul - 1.0f) >= 0.0005f)
            {
                const float same = std::tan(fov / g_mul * (kDegToRad * 0.5f)) / std::tan(fov * (kDegToRad * 0.5f));
                dolly = 1.0f - closer + closer * same;
            }
            g_dolly.store(dolly, std::memory_order_relaxed);
            g_lastDistance = distance * dolly;
            return distance * dolly;
        }

        float g_freePitch = 0.0f; // the free camera's extra pitch (Modern controls), from the last yaw hook
        bool g_freeNow = false;   // the free camera has the view
        float g_freeYaw = 0.0f;   // ... and its yaw (Gene's convention), for the radar

        // 0x13A2B8 lwc1 $f2,0x414(s1): the camera's yaw offset from Gene's facing ($f1 = his yaw, loaded just before;
        // the two are added at 0x13A2C4). With the Modern controls' free camera, the offset that puts the camera at
        // the free camera's own yaw instead.
        void orbitYaw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            static auto s_last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - s_last).count();
            s_last = now;
            const float offset = loadFloat(rdram, cameraAt(ctx, 0x414u));
            const modern::FreeCamera free = modern::updateCamera(rdram, ctx->f[1] + offset, ctx->f[1], dt);
            g_freePitch = free.free ? free.pitch : 0.0f;
            g_freeNow = free.free;
            g_freeYaw = free.yaw;
            if (!free.free)
            {
                ctx->f[2] = offset;
                return;
            }
            float relative = free.yaw - ctx->f[1];
            while (relative > kPi)
                relative -= 2.0f * kPi;
            while (relative < -kPi)
                relative += 2.0f * kPi;
            ctx->f[2] = relative;
        }

        // The radar (top right) turns its enemy dots by Gene's facing: func_00143D20 copies his rotation (+0x100..0x108,
        // yaw +0x104) to 128(sp), negates it and turns each enemy's offset by it. While the Modern free camera has the
        // view, before that copy is read back (0x143F58 lqc2 $vf4,128(sp)) the yaw becomes the camera's, so up on the
        // radar is where the camera looks, as it is with the camera behind Gene.
        void radarYaw(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_freeNow)
                return;
            const uint32_t at = (GPR_U32(ctx, 29) + 128u + 4u) & (PS2_RAM_SIZE - 1u);
            std::memcpy(rdram + at, &g_freeYaw, sizeof(g_freeYaw));
        }

        float orbitPitch(const uint8_t *rdram, R5900Context *ctx)
        {
            float pitch = loadFloat(rdram, cameraAt(ctx, 0x410u));
            const float tilt = g_tiltDegrees.load(std::memory_order_relaxed) * kDegToRad;
            const float fov = fovFactor();
            if (tilt != 0.0f && std::fabs(fov - 1.0f) >= 0.0005f)
                pitch += tilt * std::fmin(std::fmax((g_mul - 1.0f) / (fov - 1.0f), 0.0f), 1.0f);
            // The free camera's pitch, kept so the eye stays 0.35 m above Gene's feet: the look point is the look
            // offset's height (+0x404) plus +0x510 above them, the eye that plus distance * sin(pitch).
            constexpr float kClearance = 0.35f;
            const float lookHeight = loadFloat(rdram, cameraAt(ctx, 0x404u)) + loadFloat(rdram, cameraAt(ctx, 0x510u));
            const float lowest = std::asin(std::fmin(std::fmax((kClearance - lookHeight) / std::fmax(g_lastDistance, 0.5f), -1.0f), 1.0f));
            modern::setPitchFloor(lowest - pitch);
            return pitch + std::fmax(g_freePitch, std::fmin(lowest - pitch, 0.0f));
        }

        // lwc1 $f4/$f0,0x410(s1) and lwc1 $f0,0x420(s1) where the eye is placed
        void orbitPitchF4(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[4] = orbitPitch(rdram, ctx); }  // 0x13A2A8, 0x13BACC
        void orbitPitchF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitPitch(rdram, ctx); }  // 0x13B12C
        void orbitDistanceF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *) { ctx->f[0] = orbitDistance(rdram, ctx); } // 0x13A2F4, 0x13B15C, 0x13BB18

        float length3(const float *v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

        // ---- See-through oval -----------------------------------------------------------------------------------------
        //
        // Things between the camera and Gene (walls, pillars, the stalls in town) dissolve in an oval round him, so he
        // and the enemies about him stay in view. Every frame, once cCamera_update has the player camera's world-to-screen
        // matrix (+0x00 = view-screen * view, 0x1388F0, s4 = the camera), Gene's middle (0.95 m above his feet, +0xF0 of
        // the player) is projected to the screen; the oval's radii are SeeThroughWidth / SeeThroughHeight metres at his
        // distance. The renderer gets the eye and the inverse of the camera's matrix, rebuilds each pixel's place in
        // the world, and dissolves what lies inside the oval and nearer the eye than Gene by SeeThroughMargin (full
        // 0.3 m further), the ground below his middle excepted (pcsx2gs: pcsx2gs_set_option "see_through",
        // GSRendererHW::ApplySeeThrough, tfx.fx SeeThroughHides: an ordered dither, densest in the middle, thinning
        // over the soft edge). Gene and the enemies round him are at his distance or beyond; the HUD has no depth test.
        // Oct 2026: the cut was a depth (two points projected in front of him) with a 1 m margin and a 0.8 m ramp:
        // a wall less than ~1.8 m in front of him stayed (mostly) whole, and the floor before his feet dissolved in
        // stripes of dither as its depth changed down the screen.
        // Only while the player camera's own logic runs (gameplay: not in cutscenes); the DLL turns the oval off by
        // itself when no frame brings one. Nov 6 (K): on only while a sight line to Gene or an enemy in the oval is
        // blocked by the stage's collision (sightBlocked, faded in and out), and the ground that stays is what lies
        // below his feet + 0.3 m (was his middle: sills and ledges stayed whole).
        constexpr uint32_t kPlayerPointer = game::GameWork::address(game::GameWork::kPlayerObj); // GameWork.playerObj
        std::atomic<bool> g_seeThrough{true};
        std::atomic<float> g_seeWidth{2.3f};     // metres, half-width (the most: the oval fits what is hidden)
        std::atomic<float> g_seeHeight{2.8f};    // metres, half-height (the most)
        std::atomic<float> g_seeMargin{0.7f};    // metres in front of Gene
        std::atomic<float> g_seeStrength{100.0f}; // percent
        std::atomic<float> g_seeSoftness{33.0f};  // percent of the radius
        bool g_seeSent = false;                   // game thread: an oval went to the renderer last frame
        std::atomic<float> g_seeReadout[4] = {};  // centre x, y, radii x, y (GS pixels), for the menu

        struct Clip
        {
            float x, y, z, w;
        };

        // Inverse of the world-to-screen matrix m (column-major, as project uses it), row-major out; false if singular.
        bool invert(const float *m, float *out)
        {
            double a[4][8];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                {
                    a[r][c] = m[c * 4 + r];
                    a[r][c + 4] = r == c ? 1.0 : 0.0;
                }
            for (int c = 0; c < 4; ++c)
            {
                int pivot = c;
                for (int r = c + 1; r < 4; ++r)
                    if (std::fabs(a[r][c]) > std::fabs(a[pivot][c]))
                        pivot = r;
                if (!(std::fabs(a[pivot][c]) > 1e-12))
                    return false;
                if (pivot != c)
                    for (int k = 0; k < 8; ++k)
                        std::swap(a[c][k], a[pivot][k]);
                const double inv = 1.0 / a[c][c];
                for (int k = 0; k < 8; ++k)
                    a[c][k] *= inv;
                for (int r = 0; r < 4; ++r)
                    if (r != c && a[r][c] != 0.0)
                    {
                        const double f = a[r][c];
                        for (int k = 0; k < 8; ++k)
                            a[r][k] -= f * a[c][k];
                    }
            }
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                {
                    out[r * 4 + c] = static_cast<float>(a[r][c + 4]);
                    if (!std::isfinite(out[r * 4 + c]))
                        return false;
                }
            return true;
        }

        Clip project(const float *m, const float *p)
        {
            Clip c;
            c.x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
            c.y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
            c.z = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
            c.w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
            return c;
        }

        // ---- See-through trigger (K, Nov 6): only while the stage's walls block the camera's view ---------------------
        // Sight lines from the eye to three points on Gene (head, chest, hips) and two on each living enemy
        // inside the oval on screen are tested against the room's collision polygons (cCollisionScroll 0x5CAE50, as
        // hitbox_overlay reads it: +4 groups, +0xC vertices (0xC stride), +0x14 polygons (0x14: +0..+6 u16 vertex ids,
        // +0x10 flags bit 0 quad, bit 1 off); header +0x28 u16 polygon count). Any blocked line turns the oval on: it
        // fades in over 0.15 s, stays 0.3 s after the last block and fades out over 0.3 s (steady clock, so the same at
        // 30 and 60). Each line stops 0.25 m short of its point (a wall Gene leans on is not in the way). Collision only:
        // a visible piece with no collision behind it doesn't turn the oval on alone (it still dissolves once on).
        constexpr uint32_t kCollScroll = 0x5CAE50u, kEmListHead = 0x5864F0u + 0x500u;
        constexpr float kSeeFadeIn = 0.15f, kSeeHold = 0.3f, kSeeFadeOut = 0.3f, kSeeShort = 0.25f;
        float g_seeLevel = 0.0f;       // 0..1, game thread
        float g_seeFit[4] = {};        // the oval sent: centre x, y, radii x, y (GS pixels), game thread
        int64_t g_seeBlockedMs = -100000, g_seeLevelMs = 0;

        struct Segment
        {
            float a[3], d[3]; // from a, a + d
        };
        bool inGuestRam(uint32_t a, uint32_t bytes) { return a >= 0x100000u && a < PS2_RAM_SIZE && bytes <= PS2_RAM_SIZE - a; }
        uint32_t rdWord(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 1u)), 4);
            return v & 0x1FFFFFFFu;
        }
        uint16_t rdHalf(const uint8_t *rdram, uint32_t a)
        {
            uint16_t v = 0;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 1u)), 2);
            return v;
        }
        // Moller-Trumbore on the segment (t in 0..1).
        bool segmentHitsTriangle(const Segment &s, const float *a, const float *b, const float *c)
        {
            const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float pv[3] = {s.d[1] * e2[2] - s.d[2] * e2[1], s.d[2] * e2[0] - s.d[0] * e2[2], s.d[0] * e2[1] - s.d[1] * e2[0]};
            const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
            if (std::fabs(det) < 1e-9f)
                return false;
            const float inv = 1.0f / det;
            const float tv[3] = {s.a[0] - a[0], s.a[1] - a[1], s.a[2] - a[2]};
            const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
            if (u < 0.0f || u > 1.0f)
                return false;
            const float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0]};
            const float v = (s.d[0] * qv[0] + s.d[1] * qv[1] + s.d[2] * qv[2]) * inv;
            if (v < 0.0f || u + v > 1.0f)
                return false;
            const float t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
            return t > 0.0f && t < 1.0f;
        }
        // Which of the segments cross a collision polygon that is on (hit[i]); true if any.
        bool sightBlocked(const uint8_t *rdram, const Segment *segs, int count, bool *hit)
        {
            for (int i = 0; i < count; ++i)
                hit[i] = false;
            int left = count;
            const uint32_t header = rdWord(rdram, kCollScroll);
            const uint32_t verts = rdWord(rdram, kCollScroll + 0x0Cu), polys = rdWord(rdram, kCollScroll + 0x14u);
            if (count <= 0 || !inGuestRam(header, 0x30u) || !inGuestRam(verts, 0x0Cu))
                return false;
            const uint32_t polyCount = rdHalf(rdram, header + 0x28u);
            if (polyCount == 0u || !inGuestRam(polys, polyCount * 0x14u))
                return false;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; // all the segments' box
            for (int i = 0; i < count; ++i)
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = std::fmin(lo[k], std::fmin(segs[i].a[k], segs[i].a[k] + segs[i].d[k]));
                    hi[k] = std::fmax(hi[k], std::fmax(segs[i].a[k], segs[i].a[k] + segs[i].d[k]));
                }
            for (uint32_t p = 0; p < polyCount; ++p)
            {
                const uint32_t rec = polys + p * 0x14u;
                uint32_t flags = 0;
                std::memcpy(&flags, rdram + rec + 0x10u, 4);
                if (flags & 2u)
                    continue; // switched off (a broken object's, or ours)
                const int corners = (flags & 1u) ? 4 : 3;
                float v[4][3];
                float plo[3] = {1e30f, 1e30f, 1e30f}, phi[3] = {-1e30f, -1e30f, -1e30f};
                bool ok = true;
                for (int c = 0; c < corners && ok; ++c)
                {
                    const uint32_t at = verts + rdHalf(rdram, rec + 2u * c) * 0x0Cu;
                    ok = inGuestRam(at, 12u);
                    if (!ok)
                        break;
                    std::memcpy(v[c], rdram + at, 12);
                    for (int k = 0; k < 3; ++k)
                    {
                        ok = ok && std::isfinite(v[c][k]);
                        plo[k] = std::fmin(plo[k], v[c][k]);
                        phi[k] = std::fmax(phi[k], v[c][k]);
                    }
                }
                if (!ok || plo[0] > hi[0] || phi[0] < lo[0] || plo[1] > hi[1] || phi[1] < lo[1] || plo[2] > hi[2] || phi[2] < lo[2])
                    continue;
                for (int i = 0; i < count; ++i)
                    if (!hit[i] && (segmentHitsTriangle(segs[i], v[0], v[1], v[2]) ||
                                    (corners == 4 && segmentHitsTriangle(segs[i], v[0], v[2], v[3]))))
                    {
                        hit[i] = true;
                        if (--left == 0)
                            return true;
                    }
            }
            return left != count;
        }

        void seeThroughOff()
        {
            if (g_seeSent)
                GSPcsx2Backend::SetOption("see_through", "0");
            g_seeSent = false;
        }

        // The game hides whole walls and pillars that come between the camera and Gene (func_00307B28: rays from the
        // camera to him, every mesh hit hidden for the frame; the game skips it while STP_CAMTHRU, game::Stp::kCamThru,
        // is set). The oval replaces that: while it is on, the meshes stay (toml patch at 0x308650, flag
        // "keep_walls"), and they dissolve round him instead.
        // Only while the player's camera runs (K, Nov 12): when the game takes the camera (punishes and other camera
        // moves) the oval is off (cameraMatrixReady), so the game's own wall hiding comes back until the player's camera
        // is back (it built within the last 100 ms).
        bool g_keepWallsSet = false, g_keepWalls = false;
        void updateKeepWalls()
        {
            const bool want = g_seeThrough.load(std::memory_order_relaxed) &&
                              steadyMs() - g_playerCameraMs.load(std::memory_order_relaxed) <= 100;
            if (g_keepWallsSet && want == g_keepWalls)
                return;
            g_keepWallsSet = true;
            g_keepWalls = want;
            ps2xSetPatchFlag("keep_walls", want);
        }

        // The camera fade of map objects lets go when it stops running (Nov 2, K: with the Modern camera an object Gene
        // picked up stayed invisible, carried and thrown; case 2026-11-02_11-38-52). func_002A87E8, called every frame
        // from an object's update while it lies on the ground, sets its alpha +0x24C from the camera's distance when
        // it is nearer the camera than Gene (the translucent flag +0x250 0x10 with it; under 0.15: alpha 0, flag clear),
        // else 1.0. The carried and flying updates never call it, so an object picked up while faded kept its last
        // alpha (0.97 .. 0.17, then 0.0 at frames 390-398, and 0.0 to the end). The PS2's camera seldom comes that
        // close; the Modern one does. Now: an object whose fade did not run for two frames, with the alpha the fade
        // left still in place (no other code wrote it: break and respawn fades keep theirs), and the same class
        // (+0x214), gets 1.0 and the flag cleared, as the fade gives an object that isn't in the way.
        struct CameraFade
        {
            uint32_t frame = 0, vtable = 0, alphaBits = 0;
        };
        std::unordered_map<uint32_t, CameraFade> g_cameraFade;
        uint32_t gameFrame(const uint8_t *rdram)
        {
            uint32_t frame = 0;
            std::memcpy(&frame, rdram + game::GameWork::address(game::GameWork::kFrameCount), sizeof(frame));
            return frame;
        }
        // 0x2A87E8, the fade's entry (a0 = the object).
        void cameraFadeCalled(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (obj < 0x100000u || obj >= PS2_RAM_SIZE - 0x600u)
                return;
            CameraFade &f = g_cameraFade[obj];
            f.frame = gameFrame(rdram);
            std::memcpy(&f.vtable, rdram + obj + 0x214u, sizeof(f.vtable));
        }
        // Once a frame (cameraMatrixReady).
        void cameraFadeSweep(uint8_t *rdram)
        {
            const uint32_t now = gameFrame(rdram);
            for (auto it = g_cameraFade.begin(); it != g_cameraFade.end();)
            {
                const uint32_t obj = it->first;
                CameraFade &f = it->second;
                uint32_t alpha = 0, vtable = 0;
                std::memcpy(&alpha, rdram + obj + 0x24Cu, sizeof(alpha));
                std::memcpy(&vtable, rdram + obj + 0x214u, sizeof(vtable));
                if (vtable != f.vtable || now - f.frame > 600u)
                {
                    it = g_cameraFade.erase(it);
                    continue;
                }
                if (now - f.frame < 2u)
                {
                    f.alphaBits = alpha; // what the fade left (read a frame or less after it ran)
                    ++it;
                    continue;
                }
                if (alpha == f.alphaBits && alpha != 0x3F800000u)
                {
                    const uint32_t one = 0x3F800000u;
                    uint32_t flags = 0;
                    std::memcpy(rdram + obj + 0x24Cu, &one, sizeof(one));
                    std::memcpy(&flags, rdram + obj + 0x250u, sizeof(flags));
                    flags &= ~0x10u;
                    std::memcpy(rdram + obj + 0x250u, &flags, sizeof(flags));
                }
                it = g_cameraFade.erase(it);
            }
        }

        // 0x1388F0 (cCamera_update, lui at,0x4233 after MtxMultiply; s4 = the camera)
        void cameraMatrixReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            updateKeepWalls();
            cameraFadeSweep(rdram);
            const uint32_t camera = GPR_U32(ctx, 20);
            if (camera >= 0x100000u && camera < PS2_RAM_SIZE - 0x1000u)
                hitbox_overlay::noteScreenMatrix(reinterpret_cast<const float *>(rdram + camera)); // any camera: the last built wins
            if (camera != g_playerCamera.load(std::memory_order_relaxed))
                return;
            const auto valid = [](uint32_t p) { return p >= 0x100000u && p < PS2_RAM_SIZE - 0x1000u; };
            if (!g_seeThrough.load(std::memory_order_relaxed) || !valid(camera) ||
                steadyMs() - g_playerCameraMs.load(std::memory_order_relaxed) > 100)
            {
                seeThroughOff();
                return;
            }
            uint32_t player, at;
            std::memcpy(&player, rdram + kPlayerPointer, sizeof(player));
            if (!valid(player))
            {
                seeThroughOff();
                return;
            }
            std::memcpy(&at, rdram + player + 0xF0u, sizeof(at));
            if (!valid(at))
            {
                seeThroughOff();
                return;
            }
            float m[16];
            std::memcpy(m, rdram + camera, sizeof(m));
            const float centre[3] = {loadFloat(rdram, at), loadFloat(rdram, at + 4u) + 0.95f, loadFloat(rdram, at + 8u)};
            float eye[3];
            for (int i = 0; i < 3; ++i)
                eye[i] = loadFloat(rdram, camera + 0x210u + 4u * i);
            float toEye[3] = {eye[0] - centre[0], eye[1] - centre[1], eye[2] - centre[2]};
            const float distance = length3(toEye);
            const float margin = g_seeMargin.load(std::memory_order_relaxed);
            const Clip c = project(m, centre);
            if (!(c.w > 0.2f) || !(distance > margin + 0.2f) || !std::isfinite(c.x + c.y + c.z))
            {
                seeThroughOff();
                return;
            }
            for (float &v : toEye)
                v /= distance;
            const float flat = std::sqrt(toEye[0] * toEye[0] + toEye[2] * toEye[2]);
            const float side[3] = {flat > 1e-3f ? -toEye[2] / flat : 1.0f, 0.0f, flat > 1e-3f ? toEye[0] / flat : 0.0f};
            const float sidePoint[3] = {centre[0] + side[0], centre[1], centre[2] + side[2]};
            const float upPoint[3] = {centre[0], centre[1] + 1.0f, centre[2]};
            const Clip cs = project(m, sidePoint), cu = project(m, upPoint);
            if (!(cs.w > 0.05f) || !(cu.w > 0.05f))
            {
                seeThroughOff();
                return;
            }
            const float cx = c.x / c.w, cy = c.y / c.w;
            const float perMetreX = std::hypot(cs.x / cs.w - cx, cs.y / cs.w - cy);
            // Both radii at the same scale (metres at Gene's distance, across the view): measured with the world's up
            // the height shrank as the camera looked down, and the oval came out wide and short.
            (void)cu;
            const float perMetreY = perMetreX;
            const float rx = g_seeWidth.load(std::memory_order_relaxed) * perMetreX;
            const float ry = g_seeHeight.load(std::memory_order_relaxed) * perMetreY;
            float inverse[16];
            if (!(rx > 1.0f) || !(ry > 1.0f) || rx > 20000.0f || ry > 20000.0f || !invert(m, inverse))
            {
                seeThroughOff();
                return;
            }
            // The trigger: sight lines from the eye to Gene (head, chest, hips: not his feet, which a low railing or
            // step in front of him hides while he is plainly in view: K, Nov 6) and to the enemies inside the oval
            // (middle, head). A target counts as hidden when any of its lines is blocked.
            struct Target
            {
                float base[3]; // feet
                int first, count;
            };
            Segment segs[3 + 2 * 16];
            Target targets[1 + 16];
            int count = 0, targetCount = 0;
            const auto aim = [&](const float *point) {
                Segment &sg = segs[count];
                float d[3] = {point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
                const float len = length3(d);
                if (!(len > kSeeShort + 0.05f))
                    return;
                const float k = (len - kSeeShort) / len;
                for (int i = 0; i < 3; ++i)
                {
                    sg.a[i] = eye[i];
                    sg.d[i] = d[i] * k;
                }
                ++count;
            };
            const float feet = centre[1] - 0.95f;
            {
                Target &t = targets[targetCount++];
                t = {{centre[0], feet, centre[2]}, count, 0};
                const float genePoints[3][3] = {{centre[0], feet + 1.7f, centre[2]},
                                                {centre[0], feet + 1.3f, centre[2]},
                                                {centre[0], feet + 0.95f, centre[2]}};
                for (const auto &pt : genePoints)
                    aim(pt);
                t.count = count - t.first;
            }
            {
                uint32_t slot = rdWord(rdram, kEmListHead);
                for (int guard = 0; guard < 64 && inGuestRam(slot, 16u) && targetCount < 17 &&
                                    count + 2 <= static_cast<int>(sizeof(segs) / sizeof(segs[0]));
                     ++guard)
                {
                    const uint32_t em = rdWord(rdram, slot + 8u);
                    slot = rdWord(rdram, slot + 4u);
                    if (!inGuestRam(em, 0x1800u))
                        continue;
                    const uint16_t kind = rdHalf(rdram, em + 0x2FEu);
                    int16_t hp = 0;
                    std::memcpy(&hp, rdram + em + 0x54Au, 2);
                    uint32_t flags = 0;
                    std::memcpy(&flags, rdram + em + 0x250u, 4);
                    const uint32_t ep = rdWord(rdram, em + 0xF0u);
                    if (kind < 0x200u || kind >= 0x280u || hp <= 0 || static_cast<int32_t>(flags) < 0 || !inGuestRam(ep, 12u))
                        continue;
                    const float base[3] = {loadFloat(rdram, ep), loadFloat(rdram, ep + 4u), loadFloat(rdram, ep + 8u)};
                    const float mid[3] = {base[0], base[1] + 0.95f, base[2]}, top[3] = {base[0], base[1] + 1.6f, base[2]};
                    const Clip ce = project(m, mid);
                    if (!(ce.w > 0.2f))
                        continue;
                    const float ox = (ce.x / ce.w - cx) / rx, oy = (ce.y / ce.w - cy) / ry;
                    if (ox * ox + oy * oy > 1.0f)
                        continue; // outside the oval: not what it would uncover
                    Target &t = targets[targetCount++];
                    t = {{base[0], base[1], base[2]}, count, 0};
                    aim(mid);
                    aim(top);
                    t.count = count - t.first;
                }
            }
            bool hit[sizeof(segs) / sizeof(segs[0])] = {};
            const int64_t now = steadyMs();
            const bool blocked = sightBlocked(rdram, segs, count, hit);
            // The oval fits what is hidden (K, Nov 6: one fixed oval dissolved a wall beside Gene while only a railing
            // hid his feet): the screen box round each hidden target (0.5 m to either side, feet to 1.9 m up, 0.3 m
            // spare all round), an ellipse through its corners, centred on it, never bigger than the oval set in F2
            // (width / height = the most). Eased toward it (~0.1 s), and kept as it was while the oval fades out.
            if (blocked)
            {
                g_seeBlockedMs = now;
                float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
                for (int ti = 0; ti < targetCount; ++ti)
                {
                    const Target &t = targets[ti];
                    bool hidden = false;
                    for (int k = t.first; k < t.first + t.count; ++k)
                        hidden = hidden || hit[k];
                    if (!hidden)
                        continue;
                    for (int corner = 0; corner < 4; ++corner)
                    {
                        const float across = (corner & 1) ? 0.8f : -0.8f, up = (corner & 2) ? 2.2f : -0.3f;
                        const float pt[3] = {t.base[0] + side[0] * across, t.base[1] + up, t.base[2] + side[2] * across};
                        const Clip cc = project(m, pt);
                        if (!(cc.w > 0.05f))
                            continue;
                        lo[0] = std::fmin(lo[0], cc.x / cc.w);
                        lo[1] = std::fmin(lo[1], cc.y / cc.w);
                        hi[0] = std::fmax(hi[0], cc.x / cc.w);
                        hi[1] = std::fmax(hi[1], cc.y / cc.w);
                    }
                }
                if (hi[0] > lo[0] && hi[1] > lo[1])
                {
                    const float want[4] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]),
                                           std::fmin(rx, 0.70710678f * (hi[0] - lo[0])), std::fmin(ry, 0.70710678f * (hi[1] - lo[1]))};
                    const float dtFit = std::fmin(0.25f, std::fmax(0.0f, static_cast<float>(now - g_seeLevelMs) / 1000.0f));
                    const float k = g_seeLevel <= 0.0f ? 1.0f : 1.0f - std::exp(-dtFit / 0.1f);
                    for (int i = 0; i < 4; ++i)
                        g_seeFit[i] += (want[i] - g_seeFit[i]) * k;
                }
            }
            const float dt = std::fmin(0.25f, std::fmax(0.0f, static_cast<float>(now - g_seeLevelMs) / 1000.0f));
            g_seeLevelMs = now;
            if (static_cast<float>(now - g_seeBlockedMs) / 1000.0f <= kSeeHold)
                g_seeLevel = std::fmin(1.0f, g_seeLevel + dt / kSeeFadeIn);
            else
                g_seeLevel = std::fmax(0.0f, g_seeLevel - dt / kSeeFadeOut);
            if (g_seeLevel <= 0.0f || !(g_seeFit[2] > 1.0f) || !(g_seeFit[3] > 1.0f))
            {
                seeThroughOff();
                return;
            }
            const float ox = g_seeFit[0], oy = g_seeFit[1], orx = g_seeFit[2], ory = g_seeFit[3];
            const float start = distance - margin;
            const float ramp = std::fmin(0.3f, std::fmax(start - 0.15f, 0.05f));
            // The ground that stays (up / down-facing surfaces, the shader's last value): below Gene's feet + 0.3 m
            // (K, Nov 6; was his middle, which kept window sills and ledges sticking out of the walls whole).
            const float groundBelow = feet + 0.3f;
            char text[640];
            int length = std::snprintf(text, sizeof(text), "%.2f %.2f %.2f %.2f %.4f %.4f %.3f %.3f %.5f %.5f %.5f %.5f", ox, oy, orx, ory,
                                       start, ramp, g_seeStrength.load(std::memory_order_relaxed) / 100.0f * g_seeLevel,
                                       g_seeSoftness.load(std::memory_order_relaxed) / 100.0f, eye[0], eye[1], eye[2], groundBelow);
            for (int i = 0; i < 16 && length > 0 && length < static_cast<int>(sizeof(text)); ++i)
                length += std::snprintf(text + length, sizeof(text) - static_cast<size_t>(length), " %.9g", inverse[i]);
            if (!g_seeSent)
                KM_TRACE(Game, "see-through on: " << text);
            g_seeSent = GSPcsx2Backend::SetOption("see_through", text);
            g_seeReadout[0].store(ox - 2048.0f, std::memory_order_relaxed);
            g_seeReadout[1].store(oy - 2048.0f, std::memory_order_relaxed);
            g_seeReadout[2].store(orx, std::memory_order_relaxed);
            g_seeReadout[3].store(ory, std::memory_order_relaxed);
        }

        // ---- Enemies' cones ---------------------------------------------------------------------------------------

        // Angle from Gene's facing (radians) at which the screen's edge passes at distance r: a camera D behind him
        // (on the floor) whose half-width is atan(halfWidth).
        float edgeAngle(float r, float D, float halfWidth)
        {
            const float phi = std::atan(halfWidth);
            const float x = D * std::sin(phi) / r;
            return x >= 1.0f ? kPi : phi + std::asin(x);
        }

        // edge_now / edge_ps2 at distance r (1 when the setting is off).
        float coneScale(float r)
        {
            if (!g_enemyView.load(std::memory_order_relaxed))
                return 1.0f;
            const float fov = g_camFov.load(std::memory_order_relaxed);
            const float mul = g_mul > 0.1f ? g_mul : 1.0f;
            const float dolly = g_dolly.load(std::memory_order_relaxed);
            const float eye = g_eyeDistance.load(std::memory_order_relaxed);
            if (!(fov > 1.0f && fov < 170.0f) || !(dolly > 0.05f))
                return 1.0f;
            r = std::fmax(r, 0.3f);
            const float now = edgeAngle(r, eye, g_aspect * std::tan(fov * (kDegToRad * 0.5f)));
            const float ps2 = edgeAngle(r, eye / dolly, kNativeAspect * std::tan(fov / mul * (kDegToRad * 0.5f)));
            return std::fmin(std::fmax(now / ps2, 0.5f), 3.0f);
        }

        float scaledCone(float cone, float r)
        {
            const float scaled = cone * coneScale(r);
            constexpr float kMax = 150.0f * kDegToRad; // never all the way round behind him
            return cone >= kMax ? cone : std::fmin(scaled, kMax);
        }

        float enemyDistance(const uint8_t *rdram, R5900Context *ctx)
        {
            const float squared = loadFloat(rdram, GPR_U32(ctx, 18) + 0x618u); // s2 = the enemy
            return std::sqrt(std::fmax(squared, 0.0f));
        }

        // With the Modern free camera turned away from Gene, "in front" means in front of the camera. The enemies'
        // common update (func_00208BE0, and its twin near 0x2096A0) works out once a frame Turn_dest(Gene's position,
        // the enemy's, Gene's yaw): +0x764 signed and +0x768 = |angle| from his facing to the enemy, and every AI
        // decision builds on those (the idle wedge, the circling side, the guard/approach value, the lunges). Swapping
        // the camera's yaw in for his there (just after it is loaded into $f12) keeps all of them in agreement. e6f98f4
        // swapped it at the compares only: the rest still saw Gene's back turned, and enemies in view froze in guard
        // (K, Oct 29). Gene's own readers of +0x768 (func_001269E0, the grab prompt) see the camera's too.
        // The same swap where the AI works the angle out again from Gene's +0x104 instead of reading +0x768 (Oct 30,
        // K's case 2026-10-30_08-15-59: enemies right behind Gene, in the free camera's view, stood idle): the attack
        // decision func_00262AA8 (0x263524: the per-kind rules that keep enemies at his back from attacking) and the
        // common sub-steps 13, 17 and 18 (0x216984, 0x2175D4 / 0x217770 / 0x21784C, 0x217D98 / 0x2187A0); all hooked
        // after their `lwc1 $f12,0x104(v0)`. Left on his real facing: the back-grab prompt (func_00271148/00271298,
        // a stunned enemy at his back) and cEmManage's func_00292F70.
        // Modern rule (K, Oct 30): an enemy is judged from the camera's view, so on screen = in front. Within the game's
        // point-blank range (2.5 m, 2 m on Level 1: Level 1 = the level gauge's points, save +0x1C, below the first
        // threshold at 0x3BF160, as cCoreSave_getGameLevel reads them) Gene facing it counts too: there the yaw that
        // gives the smaller angle (his or the camera's) is used, so an enemy he swings at right in front of him fights
        // back even with the camera behind it. Classic (no free camera): untouched.
        float facingYawFor(const uint8_t *rdram, float ex, float ez)
        {
            uint32_t gene = 0, pos = 0;
            std::memcpy(&gene, rdram + 0x747A94u, 4);
            gene &= 0x1FFFFFFFu;
            if (gene < 0x100000u || gene + 0x200u > PS2_RAM_SIZE)
                return g_freeYaw;
            std::memcpy(&pos, rdram + gene + 0xF0u, 4);
            pos &= 0x1FFFFFFFu;
            if (pos < 0x100000u || pos + 12u > PS2_RAM_SIZE)
                return g_freeYaw;
            const float dx = ex - loadFloat(rdram, pos), dz = ez - loadFloat(rdram, pos + 8u);
            uint32_t save = 0, first = 0;
            std::memcpy(&save, rdram + 0x569B70u, 4);
            std::memcpy(&first, rdram + 0x3BF160u, 4);
            save &= 0x1FFFFFFFu;
            int16_t points = 0x7FFF;
            if (save >= 0x100000u && save + 0x20u <= PS2_RAM_SIZE)
                std::memcpy(&points, rdram + save + 0x1Cu, 2);
            const float pointBlank = static_cast<int32_t>(points) < static_cast<int32_t>(first) ? 2.0f : 2.5f;
            if (dx * dx + dz * dz >= pointBlank * pointBlank)
                return g_freeYaw;
            const float geneYaw = loadFloat(rdram, gene + 0x104u);
            const auto off = [&](float yaw) {
                float a = std::atan2(dx, dz) - yaw;
                while (a > kPi)
                    a -= 2.0f * kPi;
                while (a < -kPi)
                    a += 2.0f * kPi;
                return std::fabs(a);
            };
            return off(geneYaw) < off(g_freeYaw) ? geneYaw : g_freeYaw;
        }

        bool facingSwap() { return g_freeNow && g_enemyView.load(std::memory_order_relaxed); }

        // The enemy's position from its object (GPR `reg` = the enemy; +0xF0 -> its position).
        template <int reg>
        void enemyFacing(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!facingSwap())
                return;
            uint32_t pos = 0;
            std::memcpy(&pos, rdram + ((GPR_U32(ctx, reg) + 0xF0u) & (PS2_RAM_SIZE - 1u)), 4);
            pos &= 0x1FFFFFFFu;
            ctx->f[12] = pos >= 0x100000u && pos + 12u <= PS2_RAM_SIZE ? facingYawFor(rdram, loadFloat(rdram, pos), loadFloat(rdram, pos + 8u))
                                                                        : g_freeYaw;
        }
        // The twin at 0x2096D8 passes a position on the stack (a1 = sp).
        void enemyFacingSp(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!facingSwap())
                return;
            const uint32_t at = GPR_U32(ctx, 29);
            ctx->f[12] = facingYawFor(rdram, loadFloat(rdram, at), loadFloat(rdram, at + 8u));
        }

        // The queue's walk-to spots (+0x16A0, func_0020F3C0) are offsets in Gene's own frame put into the world by his
        // matrix (Gene +0x80, sceVu0ApplyMatrix at 0x20F82C: the "bone"), so they lay in front of his real facing: with
        // him turned to the camera, the enemies in view were sent round him, off the screen. Hooked when the call
        // returns (0x20F834, s4 = the spot): the offset from the matrix's translation is turned by the camera's yaw
        // less his (x' = x cos d + z sin d, z' = z cos d - x sin d: the game's yaw turns (0, 1) to (sin, cos)).
        void enemySpot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_freeNow || !g_enemyView.load(std::memory_order_relaxed))
                return;
            uint32_t gene = 0;
            std::memcpy(&gene, rdram + 0x747A94u, 4);
            gene &= 0x1FFFFFFFu;
            const uint32_t spot = GPR_U32(ctx, 20) & 0x1FFFFFFFu;
            if (gene < 0x100000u || gene + 0x100u > PS2_RAM_SIZE || spot < 0x100000u || spot + 12u > PS2_RAM_SIZE)
                return;
            const float delta = g_freeYaw - loadFloat(rdram, gene + 0x104u);
            const float tx = loadFloat(rdram, gene + 0xB0u), tz = loadFloat(rdram, gene + 0xB8u);
            const float x = loadFloat(rdram, spot) - tx, z = loadFloat(rdram, spot + 8u) - tz;
            const float c = std::cos(delta), s = std::sin(delta);
            const float out[2] = {tx + x * c + z * s, tz + z * c - x * s};
            std::memcpy(rdram + spot, &out[0], 4);
            std::memcpy(rdram + spot + 8u, &out[1], 4);
        }

        // TEMPORARY (Oct 29, strip once K's free-camera fights are right): once a second while the free camera has the
        // view, what the enemies compared: the camera's yaw, Gene's, and per enemy the angle (from the camera's
        // facing) and the cone.
        struct ConeNote
        {
            uint32_t enemy;
            char site;
            float angle, cone;
        };
        ConeNote g_coneNotes[12];
        int g_coneCount = 0;
        std::chrono::steady_clock::time_point g_coneLogAt{};
        void noteCone(const uint8_t *rdram, R5900Context *ctx, char site, float angle, float cone)
        {
            if (!g_freeNow)
                return;
            const uint32_t enemy = GPR_U32(ctx, 18);
            for (int i = 0; i < g_coneCount; ++i)
                if (g_coneNotes[i].enemy == enemy && g_coneNotes[i].site == site)
                {
                    g_coneNotes[i] = {enemy, site, angle, cone};
                    goto logged;
                }
            if (g_coneCount < 12)
                g_coneNotes[g_coneCount++] = {enemy, site, angle, cone};
        logged:
            const auto now = std::chrono::steady_clock::now();
            if (now - g_coneLogAt < std::chrono::seconds(1))
                return;
            g_coneLogAt = now;
            uint32_t gene = 0;
            std::memcpy(&gene, rdram + 0x747A94u, 4);
            const float geneYaw = (gene & 0x1FFFFFFFu) >= 0x100000u ? loadFloat(rdram, gene + 0x104u) : 0.0f;
            char line[1024];
            int n = std::snprintf(line, sizeof(line), "enemy cones (deg): camera yaw %.0f, Gene %.0f;", g_freeYaw / kDegToRad,
                                  geneYaw / kDegToRad);
            for (int i = 0; i < g_coneCount && n < static_cast<int>(sizeof(line)) - 80; ++i)
            {
                const ConeNote &c = g_coneNotes[i];
                n += std::snprintf(line + n, sizeof(line) - static_cast<size_t>(n), " %x %c %.0f /%.0f", c.enemy & 0xFFFFFFu, c.site,
                                   c.angle / kDegToRad, c.cone / kDegToRad);
            }
            KM_INFO(Game, line);
            g_coneCount = 0;
        }

        // Before c.le.s $f0,$f1 in func_00211C08 ($f0 = the 60-degree wedge, $f1 = the angle)
        void enemyWedge(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[0] = scaledCone(ctx->f[0], enemyDistance(rdram, ctx));
            noteCone(rdram, ctx, 'w', ctx->f[1], ctx->f[0]);
        }
        // The attack decision's own facing cones (func_00262AA8, f21 = |angle from the facing| worked out at 0x263524):
        // the default rule's hold by level (c.lt.s $f1,$f21 at 0x263608, $f1 = 60/90/135/180 deg: outside it the
        // enemy waits) and the per-kind 30-degree attack cones (c.lt.s $f21,$f0, 12 compares 0x266628..0x267B6C) widen
        // with the view like the wedge and the lunges (K, Oct 30: an enemy visible at the side at FOV 153% was held).
        void enemyHoldF1(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[1] = scaledCone(ctx->f[1], enemyDistance(rdram, ctx));
            noteCone(rdram, ctx, 'h', ctx->f[21], ctx->f[1]);
        }
        void enemyHoldF0(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[0] = scaledCone(ctx->f[0], enemyDistance(rdram, ctx));
            noteCone(rdram, ctx, 'a', ctx->f[21], ctx->f[0]);
        }
        // Before c.lt.s $f0,$f1 in func_00262AA8 ($f0 = the angle, $f1 = the level's lunge cone)
        void enemyLunge(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            ctx->f[1] = scaledCone(ctx->f[1], enemyDistance(rdram, ctx));
            noteCone(rdram, ctx, 'l', ctx->f[0], ctx->f[1]);
        }

        // ---- Camera shake -----------------------------------------------------------------------------------------

        // Steady camera while Gene stands idle ([Video] SteadyIdleCamera, off by default). His idle stomps about:
        // the player camera follows his position and rotation (cPlCamera_update 0x139FA8: the look point is his
        // position plus an offset turned by his rotation, eased toward), so the view rocked with him, and the stomps
        // start the camera's shake. While he is idle (mode 0, phase 0 or 10: IsStateByteIdle_10B5F0) the camera's
        // update sees him where his idle started (his position put back after it), and the player camera shows no
        // shake. Should he really move off (half a metre), that becomes the new place. His heading is never held: the
        // idle motion doesn't turn him (+0x104 stays put through it), but turning on the spot does (the stick, 6
        // degrees a frame), and a held heading let the camera lag and jump 20 degrees at a time (K, Nov 4: Gene looked
        // to snap while turning; case spinning_broken).
        constexpr uint32_t kPlCameraUpdate = 0x139FA8u;
        constexpr uint32_t kCameraUpdate = 0x138740u;
        constexpr uint32_t kShakeChannels[] = {0x254u, 0x268u}; // cCamVib: amplitude, speed, decay, phase, frames left
        PS2Runtime::RecompiledFunction g_plCameraUpdate = nullptr;
        PS2Runtime::RecompiledFunction g_cameraUpdate = nullptr;
        std::atomic<bool> g_steadyIdle{false};
        bool g_holding = false;
        float g_heldPos[3] = {};

        uint32_t idlePlayer(const uint8_t *rdram)
        {
            if (!g_steadyIdle.load(std::memory_order_relaxed))
                return 0;
            uint32_t player;
            std::memcpy(&player, rdram + kPlayerPointer, sizeof(player));
            player &= PS2_RAM_SIZE - 1u;
            if (player == 0 || player + 0x300u > PS2_RAM_SIZE)
                return 0;
            const uint8_t mode = rdram[player + game::GameObj::kMode], phase = rdram[player + game::GameObj::kMode + 1u];
            return mode == 0 && (phase == 0 || phase == 10) ? player : 0;
        }

        void plCameraUpdateSteady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t player = idlePlayer(rdram);
            uint32_t pos = 0;
            if (player)
            {
                std::memcpy(&pos, rdram + player + game::GameObj::kPos, sizeof(pos));
                pos &= PS2_RAM_SIZE - 1u;
                if (pos == 0 || pos + 12u > PS2_RAM_SIZE)
                    pos = 0;
            }
            if (!pos)
            {
                g_holding = false;
                g_plCameraUpdate(rdram, ctx, runtime);
                return;
            }
            float now[3];
            std::memcpy(now, rdram + pos, sizeof(now));
            const float dx = now[0] - g_heldPos[0], dz = now[2] - g_heldPos[2];
            if (!g_holding || dx * dx + dz * dz > 0.25f)
            {
                std::memcpy(g_heldPos, now, sizeof(now));
                g_holding = true;
            }
            std::memcpy(rdram + pos, g_heldPos, sizeof(g_heldPos));
            g_plCameraUpdate(rdram, ctx, runtime);
            std::memcpy(rdram + pos, now, sizeof(now));
        }

        // cCamera_update adds the camera's two shake channels to its angles as amplitude * sin(phase): for the player
        // camera while Gene is idle, the amplitudes are zero for the update and put back after it (the shake's own
        // course, its decay and length, stays the game's).
        void cameraUpdateSteady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t camera = GPR_U32(ctx, 4) & (PS2_RAM_SIZE - 1u);
            const bool still = camera != 0 && camera == (g_playerCamera.load(std::memory_order_relaxed) & (PS2_RAM_SIZE - 1u)) &&
                               camera + 0x280u <= PS2_RAM_SIZE && idlePlayer(rdram) != 0;
            float amplitude[2] = {};
            if (still)
                for (int i = 0; i < 2; ++i)
                {
                    std::memcpy(&amplitude[i], rdram + camera + kShakeChannels[i], 4);
                    std::memset(rdram + camera + kShakeChannels[i], 0, 4);
                }
            g_cameraUpdate(rdram, ctx, runtime);
            if (still)
                for (int i = 0; i < 2; ++i)
                    std::memcpy(rdram + camera + kShakeChannels[i], &amplitude[i], 4);
        }

        // ---- Settings in GHR.ini ------------------------------------------------------------------------------

        float envValue(const char *name, float fallback, float low, float high)
        {
            const char *text = std::getenv(name);
            const float value = (text && *text) ? static_cast<float>(std::atof(text)) : fallback;
            return std::fmin(std::fmax(value, low), high);
        }

        float iniValue(const IniFile &ini, const char *section, const char *key, float fallback, float low, float high)
        {
            const std::string text = ini.get(section, key, "");
            const float value = text.empty() ? fallback : static_cast<float>(std::atof(text.c_str()));
            return std::fmin(std::fmax(value, low), high);
        }

        std::string number(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", std::round(value * 100.0f) / 100.0f);
            return text;
        }

        std::string presetLine()
        {
            return "{\"Name\", " + number(g_fovPercent.load()) + "f, " + number(g_fightPercent.load()) + "f, " +
                   number(100.0f - g_closerPercent.load()) + "f, " + number(g_tiltDegrees.load()) + "f},";
        }

        std::mutex g_statusMutex;
        std::string g_status; // the menu's last Save / Revert result

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        // [Video] SeeThrough*: read straight from GHR.ini (at install and on Revert).
        void loadExtras(const IniFile &ini)
        {
            g_seeThrough = ini.getBool("Video", "SeeThrough", true);
            g_seeWidth = iniValue(ini, "Video", "SeeThroughWidth", 2.3f, 0.3f, 6.0f);
            g_seeHeight = iniValue(ini, "Video", "SeeThroughHeight", 2.8f, 0.3f, 6.0f);
            g_seeMargin = iniValue(ini, "Video", "SeeThroughMargin", 0.7f, 0.1f, 5.0f);
            g_seeStrength = iniValue(ini, "Video", "SeeThroughStrength", 100.0f, 0.0f, 100.0f);
            g_seeSoftness = iniValue(ini, "Video", "SeeThroughSoftness", 33.0f, 2.0f, 100.0f);
        }

        void saveToIni()
        {
            IniFile ini;
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                setStatus("Could not read GHR.ini.");
                return;
            }
            ini.set("Video", "FieldOfView", number(g_fovPercent.load()));
            ini.set("Video", "FightFieldOfView", number(g_fightPercent.load()));
            ini.set("Video", "CameraZoomOut", number(100.0f - g_closerPercent.load()));
            ini.set("Video", "CameraTilt", number(g_tiltDegrees.load()));
            ini.setBool("Video", "SteadyIdleCamera", g_steadyIdle.load());
            ini.setBool("Game", "EnemiesFollowView", g_enemyView.load());
            ini.setBool("Video", "DepthOfField", g_depthOfField.load());
            ini.setBool("Video", "PreciseDepth", g_preciseDepth.load());
            ini.setBool("Video", "PerspectiveShading", g_perspectiveShading.load());
            ini.setBool("Video", "SeeThrough", g_seeThrough.load());
            ini.set("Video", "SeeThroughWidth", number(g_seeWidth.load()));
            ini.set("Video", "SeeThroughHeight", number(g_seeHeight.load()));
            ini.set("Video", "SeeThroughMargin", number(g_seeMargin.load()));
            ini.set("Video", "SeeThroughStrength", number(g_seeStrength.load()));
            ini.set("Video", "SeeThroughSoftness", number(g_seeSoftness.load()));
            if (!ini.save())
            {
                setStatus("Could not write GHR.ini.");
                return;
            }
            KM_INFO(Game, "camera saved to GHR.ini; as a preset (camera_presets.h): " << presetLine());
            setStatus("Saved to GHR.ini.");
        }

        void revertToIni()
        {
            IniFile ini;
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                setStatus("Could not read GHR.ini.");
                return;
            }
            g_fovPercent = iniValue(ini, "Video", "FieldOfView", 100.0f, 50.0f, 200.0f);
            g_fightPercent = iniValue(ini, "Video", "FightFieldOfView", 100.0f, 50.0f, 200.0f);
            g_closerPercent = ini.get("Video", "CameraZoomOut").empty()
                                  ? iniValue(ini, "Video", "CameraDistance", 100.0f, 0.0f, 100.0f)
                                  : 100.0f - iniValue(ini, "Video", "CameraZoomOut", 0.0f, 0.0f, 100.0f);
            g_tiltDegrees = iniValue(ini, "Video", "CameraTilt", 0.0f, -30.0f, 30.0f);
            g_steadyIdle = ini.getBool("Video", "SteadyIdleCamera", false);
            g_enemyView = ini.getBool("Game", "EnemiesFollowView", true);
            g_depthOfField = ini.getBool("Video", "DepthOfField", true);
            g_preciseDepth = ini.getBool("Video", "PreciseDepth", true);
            g_perspectiveShading = ini.getBool("Video", "PerspectiveShading", true);
            loadExtras(ini);
            GSPcsx2Backend::SetOption("precise_depth", g_preciseDepth.load() ? "1" : "0");
            GSPcsx2Backend::SetOption("perspective_shading", g_perspectiveShading.load() ? "1" : "0");
            GSPcsx2Backend::SetOption("dof", g_depthOfField.load() ? "1" : "0");
            setStatus("Back to the values in GHR.ini.");
        }

        // ---- F2 menu: the Camera tab ------------------------------------------------------------------------------

#if defined(KAMITE_HAS_GS_MENU)
        void slider(const Pcsx2GsUi *ui, const char *label, std::atomic<float> &setting, float low, float high, const char *format)
        {
            float value = setting.load(std::memory_order_relaxed);
            ui->set_next_width(340.0f);
            if (ui->slider_float(label, &value, low, high, format))
                setting.store(value, std::memory_order_relaxed);
        }

        void drawCameraPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 3u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again to get the camera controls.");
                return;
            }
            ui->text_wrapped("Changes apply at once. Turn on \"Keep playing\" above to walk around and fight while you "
                             "tune with the mouse.");
            ui->separator_text("Presets");
            for (const CameraPreset &preset : kCameraPresets)
            {
                if (ui->button(preset.name))
                {
                    g_fovPercent = preset.fieldOfView;
                    g_fightPercent = preset.fightFieldOfView;
                    g_closerPercent = 100.0f - preset.cameraZoomOut;
                    g_tiltDegrees = preset.cameraTilt;
                }
                ui->same_line();
            }
            ui->text("");

            ui->separator_text("Camera");
            slider(ui, "Field of view##fov", g_fovPercent, 50.0f, 200.0f, "%.0f%%");
            slider(ui, "Field of view in fights##fight", g_fightPercent, 50.0f, 200.0f, "%.0f%%");
            {
                // Shown as the distance (K, Oct 30): 0% = Gene keeps his size, 100% = the view looks zoomed out.
                float zoomOut = 100.0f - g_closerPercent.load(std::memory_order_relaxed);
                ui->set_next_width(340.0f);
                if (ui->slider_float("Camera distance##zoomout", &zoomOut, 0.0f, 100.0f, "%.0f%%"))
                    g_closerPercent.store(100.0f - zoomOut, std::memory_order_relaxed);
            }
            ui->text_disabled("0% = Gene keeps his size; 100% = the camera stays back (the view looks zoomed out).");
            slider(ui, "Camera tilt##tilt", g_tiltDegrees, -15.0f, 30.0f, "%.1f deg");
            ui->text_disabled("How much the camera rises and looks down at Gene at the field of view above.");
            int steady = g_steadyIdle.load() ? 1 : 0;
            if (ui->checkbox("Steady camera while Gene idles##steadyidle", &steady))
                g_steadyIdle = steady != 0;
            ui->text_disabled("The camera stops rocking and shaking with his idle's stomps (handy for posing NPCs).");

            ui->separator_text("Enemies");
            int follow = g_enemyView.load() ? 1 : 0;
            if (ui->checkbox("Enemies follow the wider view##enemies", &follow))
                g_enemyView = follow != 0;
            ui->text_disabled("Their facing cones widen by as much as the picture reaches further round Gene, so the "
                              "ones you can see at the edges join in as they did at the PS2's edges.");

            ui->separator_text("See Through");
            int see = g_seeThrough.load() ? 1 : 0;
            if (ui->checkbox("Dissolve what stands between the camera and Gene##see", &see))
                g_seeThrough = see != 0;
            ui->begin_disabled(see == 0);
            slider(ui, "Oval half-width (largest)##seew", g_seeWidth, 0.3f, 4.0f, "%.2f m");
            slider(ui, "Oval half-height (largest)##seeh", g_seeHeight, 0.3f, 4.0f, "%.2f m");
            slider(ui, "Starts this far in front of Gene##seem", g_seeMargin, 0.1f, 3.0f, "%.2f m");
            slider(ui, "Strength##sees", g_seeStrength, 0.0f, 100.0f, "%.0f%%");
            slider(ui, "Soft edge##seesoft", g_seeSoftness, 2.0f, 100.0f, "%.0f%%");
            ui->end_disabled();
            ui->text_disabled("Sizes are in metres round Gene's middle. Strength 100% = gone in the middle; the soft "
                              "edge is the part of the oval over which it fades out.");

            ui->separator_text("Now");
            char line[256];
            const float scale = coneScale(3.0f);
            std::snprintf(line, sizeof(line), "%s camera, fov %.1f deg (x%.2f), camera %.2f m from Gene (x%.2f of the game's)",
                          g_battle.load() ? "Battle" : "Exploring", g_camFov.load(), g_mul, g_eyeDistance.load(), g_dolly.load());
            ui->text(line);
            std::snprintf(line, sizeof(line), "Enemies at 3 m: cones x%.2f (the 60 deg wedge is %.0f deg, DIE's 90 deg lunge %.0f deg)",
                          scale, scaledCone(60.0f * kDegToRad, 3.0f) / kDegToRad, scaledCone(90.0f * kDegToRad, 3.0f) / kDegToRad);
            ui->text(line);
            if (g_seeThrough.load())
            {
                std::snprintf(line, sizeof(line), "See-through oval: centre %.0f, %.0f, radii %.0f x %.0f (game pixels, 512 x 448)",
                              g_seeReadout[0].load() + 256.0f, g_seeReadout[1].load() + 224.0f, g_seeReadout[2].load(), g_seeReadout[3].load());
                ui->text_disabled(line);
            }

            ui->separator_text("GHR.ini");
            if (ui->button("Save"))
                saveToIni();
            ui->same_line();
            if (ui->button("Revert"))
                revertToIni();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                {
                    ui->same_line();
                    ui->text(g_status.c_str());
                }
            }
            const std::string preset = "As a preset: " + presetLine();
            ui->text_disabled(preset.c_str());
        }

        // The Graphics tab: switches the renderer applies at once (pcsx2gs_set_option).
        void drawGraphicsPanel(const Pcsx2GsUi *ui)
        {
            int dof = g_depthOfField.load() ? 1 : 0;
            if (ui->checkbox("Depth of field##dof", &dof))
            {
                g_depthOfField = dof != 0;
                if (!GSPcsx2Backend::SetOption("dof", dof ? "1" : "0"))
                    setStatus("This pcsx2gs.dll cannot switch it while playing: build again.");
            }
            ui->text_disabled("The game's blur of the distance: a small blurred copy of the picture blended over it by "
                              "depth. At high internal resolutions its edges show as dark, jagged fringes that "
                              "shimmer as the camera moves.");
            int precise = g_preciseDepth.load() ? 1 : 0;
            if (ui->checkbox("Precise depth##precise", &precise))
            {
                g_preciseDepth = precise != 0;
                if (!GSPcsx2Backend::SetOption("precise_depth", precise ? "1" : "0"))
                    setStatus("This pcsx2gs.dll cannot switch it while playing: build again.");
            }
            ui->text_disabled("The game's 16-bit depth is coarse in the distance, so surfaces that meet there cut into "
                              "each other in jagged, shifting slivers (on the PS2 too, under a pixel). On: the renderer "
                              "takes each surface's exact depth from its texture perspective instead. Off: as the PS2.");
            int perspective = g_perspectiveShading.load() ? 1 : 0;
            if (ui->checkbox("Perspective shading##perspshade", &perspective))
            {
                g_perspectiveShading = perspective != 0;
                if (!GSPcsx2Backend::SetOption("perspective_shading", perspective ? "1" : "0"))
                    setStatus("This pcsx2gs.dll cannot switch it while playing: build again.");
            }
            ui->text_disabled("The PS2 spreads a surface's lighting and shadows evenly across the screen instead of along "
                              "the surface, so the floor's shading near the camera shifts, darker and lighter, as the "
                              "camera turns. On: shading follows the surface like its texture does. Off: as the PS2.");
            {
                uint64_t patched = 0, cut = 0, cutNear = 0, noStitch = 0;
                nearGeometryStats(patched, cut, cutNear, noStitch);
                char line[256];
                std::snprintf(line, sizeof(line),
                              "Near geometry (on with See-through, at start): %llu renderer tests patched; "
                              "%llu triangles cut at the screen's edges, %llu at the camera, %llu behind it left out "
                              "(no stitch information).",
                              static_cast<unsigned long long>(patched), static_cast<unsigned long long>(cut),
                              static_cast<unsigned long long>(cutNear), static_cast<unsigned long long>(noStitch));
                ui->text_wrapped(line);
            }
            ui->separator_text("GHR.ini");
            if (ui->button("Save##graphics"))
                saveToIni();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                {
                    ui->same_line();
                    ui->text(g_status.c_str());
                }
            }
        }
#endif
    }

    void setCameraIniPath(const std::filesystem::path &path)
    {
        g_iniPath = path;
    }

    const std::filesystem::path &cameraIniPath()
    {
        return g_iniPath;
    }

    void installSteadyIdleCamera(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kPlCameraUpdate) || !(g_plCameraUpdate = runtime.lookupFunction(kPlCameraUpdate)) ||
            !FunctionHooks::instance().replace(kPlCameraUpdate, "cPlCamera_update (steady idle camera)", &plCameraUpdateSteady,
                                               ReplacementStatus::Native, "[Video] SteadyIdleCamera: Gene held still for the camera"))
            KM_WARN(Game, "camera: cPlCamera_update 0x139FA8 is not hookable; the camera rocks with Gene's idle");
        if (!runtime.hasFunction(kCameraUpdate) || !(g_cameraUpdate = runtime.lookupFunction(kCameraUpdate)) ||
            !FunctionHooks::instance().replace(kCameraUpdate, "cCamera_update (steady idle camera)", &cameraUpdateSteady,
                                               ReplacementStatus::Native, "[Video] SteadyIdleCamera: no shake while Gene idles"))
            KM_WARN(Game, "camera: cCamera_update 0x138740 is not hookable; the camera shakes with Gene's idle");
    }

    void installCamera(float aspect)
    {
        g_aspect = aspect;
        // [Video] FieldOfView / FightFieldOfView: percent of the game's own (100 = the PS2's), 50..200;
        // CameraZoomOut: 0..100 (0 = Gene keeps his size on screen; the older CameraDistance = 100 - it, read when
        // CameraZoomOut is missing); CameraTilt: degrees, -30..30;
        // [Game] EnemiesFollowView: 0/1 (default 1)
        g_fovPercent = envValue("KAMITE_FOV", 100.0f, 50.0f, 200.0f);
        g_fightPercent = envValue("KAMITE_FIGHT_FOV", 100.0f, 50.0f, 200.0f);
        {
            const char *zoomOut = std::getenv("KAMITE_CAMERA_ZOOM_OUT");
            g_closerPercent = zoomOut && *zoomOut ? 100.0f - envValue("KAMITE_CAMERA_ZOOM_OUT", 0.0f, 0.0f, 100.0f)
                                                  : envValue("KAMITE_CAMERA_DISTANCE", 100.0f, 0.0f, 100.0f);
        }
        g_tiltDegrees = envValue("KAMITE_CAMERA_TILT", 0.0f, -30.0f, 30.0f);
        {
            const char *steady = std::getenv("KAMITE_STEADY_IDLE_CAMERA");
            g_steadyIdle = steady && *steady && *steady != '0';
        }
        {
            const char *text = std::getenv("KAMITE_ENEMY_VIEW");
            g_enemyView = !(text && *text == '0');
            const char *precise = std::getenv("PS2X_PCSX2_PRECISE_DEPTH");
            g_preciseDepth = !(precise && *precise == '0'); // the DLL reads the same variable when it opens
            const char *shading = std::getenv("PS2X_PCSX2_PERSPECTIVE_SHADING");
            g_perspectiveShading = !(shading && *shading == '0'); // the DLL reads the same variable when it opens
            const char *dof = std::getenv("PS2X_PCSX2_DOF");
            g_depthOfField = !(dof && *dof == '0'); // the DLL reads the same variable when it opens
        }
        {
            IniFile ini;
            if (!g_iniPath.empty() && ini.load(g_iniPath))
                loadExtras(ini);
        }
        g_mul = fovFactor();
        ps2xRegisterHostHook("km_radar_yaw", &radarYaw);
        ps2xRegisterHostHook("km_cam_matrix", &cameraMatrixReady);
        ps2xRegisterHostHook("km_cam_fade_called", &cameraFadeCalled);
        ps2xRegisterHostHook("km_fov_ease_a", &fovEaseA);
        ps2xRegisterHostHook("km_fov_ease_b", &fovEaseB);
        ps2xRegisterHostHook("km_fov_reset", &fovReset);
        ps2xRegisterHostHook("km_cam_pitch_f4", &orbitPitchF4);
        ps2xRegisterHostHook("km_cam_pitch_f0", &orbitPitchF0);
        ps2xRegisterHostHook("km_cam_distance", &orbitDistanceF0);
        ps2xRegisterHostHook("km_cam_yaw", &orbitYaw);
        ps2xRegisterHostHook("km_em_wedge", &enemyWedge);
        ps2xRegisterHostHook("km_em_lunge", &enemyLunge);
        ps2xRegisterHostHook("km_em_facing", &enemyFacing<18>);    // s2 = the enemy
        ps2xRegisterHostHook("km_em_facing_s1", &enemyFacing<17>); // s1 = the enemy
        ps2xRegisterHostHook("km_em_facing_sp", &enemyFacingSp);
        ps2xRegisterHostHook("km_em_hold_f0", &enemyHoldF0);
        ps2xRegisterHostHook("km_em_hold_f1", &enemyHoldF1);
        ps2xRegisterHostHook("km_em_spot", &enemySpot);
        ps2xSetPatchFlag("fov", true);
        ps2xSetPatchFlag("keep_walls", g_seeThrough.load());
        ps2xSetPatchFlag("enemy_view", true);
        KM_INFO(Game, "camera: field of view " << g_fovPercent.load() << "%, " << g_fightPercent.load()
                                               << "% in fights; camera distance " << 100.0f - g_closerPercent.load() << "%, tilt "
                                               << g_tiltDegrees.load() << " degrees; enemies follow the view: "
                                               << (g_enemyView.load() ? "on" : "off") << "; see-through: "
                                               << (g_seeThrough.load() ? "on" : "off"));
        modern::install(); // the Control Style (its Controls tab comes after Camera and Graphics)
#if defined(KAMITE_HAS_GS_MENU)
        static bool s_panel = false;
        if (!s_panel)
        {
            s_panel = true;
            ps2x::menu::setTitle("God Hand: Rearmed", "F2 closes this menu. The game keeps running behind it.");
            ps2x::menu::addPanel("Camera", &drawCameraPanel);
            ps2x::menu::addPanel("Graphics", &drawGraphicsPanel);
        }
#endif
    }
}
