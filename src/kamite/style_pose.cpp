#include "kamite/style_pose.h"

#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

namespace kamite::style_pose
{
    namespace
    {
        // func_00158328(obj): the pose of the motion at +0x428 (an mtb3 block) at the clock +0x430 into every part's
        // Euler +0x100..+0x108 (parts at +0x278, count +0x2B4), root outputs +0x310 / +0x340 / +0x370 (frame_patches'
        // motionPoseSmooth and game_options' roulette replay wrap it too; this wrapper chains with them). After the
        // game's pose, the arm and shoulder parts are slerped toward Gene's quick turn (motion 30 of his pack, +0x304:
        // u32 count, then offsets from the pack) at the swing's clock, decoded here from the motion in RAM (as
        // tools/models/mot.py). Running the game's pose function on the turn instead (948a609) moved more than the
        // arms: it writes translations (hips, the leg IK targets 20 / 24) through part pointers (+0xD0) outside what
        // was put back: K saw his legs freeze and the model shift.
        constexpr uint32_t kMotionPose = 0x158328u;
        constexpr uint32_t kPlayer = 0x7474A0u + 0x5F4u; // GameWork's player
        constexpr uint32_t kTurnMotion = 30u;
        constexpr uint32_t kFirstBone = 5u, kLastBone = 16u; // clavicles, deltoids, arms, twists, hands (R 5-10, L 11-16)
        constexpr int64_t kCancelMs = 100;

        PS2Runtime::RecompiledFunction g_inner = nullptr;
        uint8_t *g_ram = nullptr;

        // Tuning (F2, temporary while K dials it in): frames of motion 30, speed, strength.
        std::atomic<float> g_from{0.0f}, g_to{15.0f}, g_speed{1.0f}, g_strength{1.0f}, g_easeIn{150.0f}, g_easeOut{150.0f};

        std::atomic<bool> g_request{false};
        bool g_active = false;
        int64_t g_startMs = 0, g_cancelMs = 0;

        int64_t nowMs()
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }
        bool inRam(uint32_t a, uint32_t size) { return a >= 0x100000u && a + size <= 0x2000000u; }
        uint32_t rd32(uint32_t a)
        {
            uint32_t v = 0;
            std::memcpy(&v, g_ram + (a & 0x01FFFFFFu), 4);
            return v;
        }
        void wrF(uint32_t a, float f) { std::memcpy(g_ram + (a & 0x01FFFFFFu), &f, 4); }
        float rdF(uint32_t a)
        {
            float f = 0.0f;
            std::memcpy(&f, g_ram + (a & 0x01FFFFFFu), 4);
            return f;
        }
        // Free: mode 0 (+0x2F4) and phase (+0x2F5) 0..3, the only phases that call the action chooser func_00123938
        // with a1 = 0 (idle 0x10BE18, 0x10C300, 0x10C5A8, 0x10C930: standing, walking, running); every other phase
        // (attacks, dodges, hits, the roulette...) calls it with 1 or not at all.
        bool isFree(uint32_t gene) { return g_ram[gene + 0x2F4u] == 0u && g_ram[gene + 0x2F5u] <= 3u; }
        uint32_t turnMotion(uint32_t gene)
        {
            const uint32_t pack = rd32(gene + 0x304u);
            if (!inRam(pack, 8u) || rd32(pack) <= kTurnMotion)
                return 0u;
            const uint32_t motion = pack + rd32(pack + 4u + 4u * kTurnMotion);
            return inRam(motion, 8u) && std::memcmp(g_ram + motion, "mtb3", 4) == 0 ? motion : 0u;
        }

        // ---- the motion's curves (mtb3: u16 frames, u8 records, u8 loop; 12-byte records: u8 bone, u8 channel (high
        // nibble 0x00 constant f32 payload / 0x10 Hermite curve at motion + payload; low nibble 0..5 = t xyz, r xyz),
        // u16 key count, u32 flags, u32 payload; curve: six half floats (value, value step, in tangent, its step, out
        // tangent, its step), then 4-byte keys (frames since the last, value step, in step, out step)) ----
        float half(uint16_t v)
        {
            const uint32_t sign = (v >> 15) & 1u, expo = (v >> 9) & 0x3Fu, mant = v & 0x1FFu;
            if (expo == 0u && mant == 0u)
                return 0.0f;
            const uint32_t bits = (sign << 31) | ((expo - 47u + 127u) << 23) | (mant << 14);
            float f = 0.0f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        uint16_t rd16(uint32_t a)
        {
            uint16_t v = 0;
            std::memcpy(&v, g_ram + (a & 0x01FFFFFFu), 2);
            return v;
        }
        // The rotation of `bone` at frame t: out[c] set where the motion keys component c (found[c]).
        void sampleRotation(uint32_t motion, uint32_t bone, float t, float out[3], bool found[3])
        {
            found[0] = found[1] = found[2] = false;
            const uint32_t records = g_ram[motion + 6u];
            for (uint32_t i = 0; i < records; ++i)
            {
                const uint32_t r = motion + 8u + 12u * i;
                const uint32_t channel = g_ram[r + 1u];
                if (channel == 0xFFu)
                    break;
                const uint32_t component = channel & 0x0Fu;
                if (g_ram[r] != bone || component < 3u || component > 5u)
                    continue;
                const uint32_t keys = rd16(r + 2u), payload = rd32(r + 8u);
                float value = 0.0f;
                if ((channel & 0xF0u) == 0x00u)
                    std::memcpy(&value, &payload, 4);
                else if ((channel & 0xF0u) == 0x10u && keys > 0u && inRam(motion + payload, 12u + 4u * keys))
                {
                    const uint32_t c = motion + payload;
                    const float p = half(rd16(c)), dp = half(rd16(c + 2u)), m0 = half(rd16(c + 4u)),
                                dm0 = half(rd16(c + 6u)), m1 = half(rd16(c + 8u)), dm1 = half(rd16(c + 10u));
                    float f0 = 0.0f, v0 = 0.0f, out0 = 0.0f;
                    uint32_t frame = 0u;
                    bool done = false;
                    for (uint32_t k = 0; k < keys && !done; ++k)
                    {
                        const uint8_t *key = g_ram + c + 12u + 4u * k;
                        frame += key[0];
                        const float f = static_cast<float>(frame), v = p + dp * key[1], in = m0 + dm0 * key[2],
                                    o = m1 + dm1 * key[3];
                        if (k == 0u)
                            value = v;
                        if (k > 0u && f > t && f > f0)
                        {
                            const float u = (t - f0) / (f - f0), u2 = u * u, u3 = u2 * u;
                            value = (2 * u3 - 3 * u2 + 1) * v0 + (u3 - 2 * u2 + u) * out0 + (-2 * u3 + 3 * u2) * v +
                                    (u3 - u2) * in;
                            done = true;
                        }
                        else if (f <= t)
                            value = v; // at or past this key (the last key holds)
                        f0 = f, v0 = v, out0 = o;
                    }
                }
                else
                    continue; // an encoding not known here: leave the component to the game's pose
                out[component - 3u] = value;
                found[component - 3u] = true;
            }
        }

        // ---- rotations (as frame_patches.cpp's half-frame poses: Euler R = Rz Ry Rx, radians) ----
        struct Quat
        {
            float w, x, y, z;
        };
        void eulerMatrix(const float e[3], float r[3][3])
        {
            const float cx = std::cos(e[0]), sx = std::sin(e[0]), cy = std::cos(e[1]), sy = std::sin(e[1]),
                        cz = std::cos(e[2]), sz = std::sin(e[2]);
            r[0][0] = cy * cz, r[0][1] = sx * sy * cz - cx * sz, r[0][2] = cx * sy * cz + sx * sz;
            r[1][0] = cy * sz, r[1][1] = sx * sy * sz + cx * cz, r[1][2] = cx * sy * sz - sx * cz;
            r[2][0] = -sy, r[2][1] = sx * cy, r[2][2] = cx * cy;
        }
        Quat matrixQuat(const float r[3][3])
        {
            Quat q;
            q.w = std::sqrt(std::max(0.0f, 1.0f + r[0][0] + r[1][1] + r[2][2])) * 0.5f;
            q.x = std::copysign(std::sqrt(std::max(0.0f, 1.0f + r[0][0] - r[1][1] - r[2][2])) * 0.5f, r[2][1] - r[1][2]);
            q.y = std::copysign(std::sqrt(std::max(0.0f, 1.0f - r[0][0] + r[1][1] - r[2][2])) * 0.5f, r[0][2] - r[2][0]);
            q.z = std::copysign(std::sqrt(std::max(0.0f, 1.0f - r[0][0] - r[1][1] + r[2][2])) * 0.5f, r[1][0] - r[0][1]);
            const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
            return n > 0.0f ? Quat{q.w / n, q.x / n, q.y / n, q.z / n} : Quat{1.0f, 0.0f, 0.0f, 0.0f};
        }
        void quatMatrix(const Quat &q, float r[3][3])
        {
            const float w = q.w, x = q.x, y = q.y, z = q.z;
            r[0][0] = 1 - 2 * (y * y + z * z), r[0][1] = 2 * (x * y - w * z), r[0][2] = 2 * (x * z + w * y);
            r[1][0] = 2 * (x * y + w * z), r[1][1] = 1 - 2 * (x * x + z * z), r[1][2] = 2 * (y * z - w * x);
            r[2][0] = 2 * (x * z - w * y), r[2][1] = 2 * (y * z + w * x), r[2][2] = 1 - 2 * (x * x + y * y);
        }
        Quat quatSlerp(const Quat &a, Quat b, float u)
        {
            float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
            if (d < 0.0f)
                b = {-b.w, -b.x, -b.y, -b.z}, d = -d;
            float ka = 1.0f - u, kb = u;
            if (d < 0.9995f)
            {
                const float th = std::acos(d), s = std::sin(th);
                ka = std::sin((1.0f - u) * th) / s, kb = std::sin(u * th) / s;
            }
            Quat q{ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z};
            const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
            return Quat{q.w / n, q.x / n, q.y / n, q.z / n};
        }
        float windNear(float a, float near)
        {
            constexpr float kTurn = 6.2831853f;
            return a + kTurn * std::round((near - a) / kTurn);
        }
        bool eulerNear(const float r[3][3], const float near[3], float out[3])
        {
            const float x0 = std::atan2(r[2][1], r[2][2]);
            const float xs[3] = {x0, x0 + 3.14159265f, near[0]};
            float best = 1e30f;
            for (const float x : xs)
            {
                const float c = std::cos(x), s = std::sin(x);
                const float p01 = r[0][1] * c - r[0][2] * s, p11 = r[1][1] * c - r[1][2] * s;
                const float p20 = r[2][0], p22 = r[2][1] * s + r[2][2] * c;
                const float e[3] = {windNear(x, near[0]), windNear(std::atan2(-p20, p22), near[1]),
                                    windNear(std::atan2(-p01, p11), near[2])};
                float check[3][3];
                eulerMatrix(e, check);
                float err = 0.0f;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        err = std::max(err, std::fabs(check[i][j] - r[i][j]));
                const float distance = std::fabs(e[0] - near[0]) + std::fabs(e[1] - near[1]) + std::fabs(e[2] - near[2]);
                if (err < 1e-3f && distance < best)
                {
                    best = distance;
                    std::copy(e, e + 3, out);
                }
            }
            return best < 1e30f;
        }

        void poseWrapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t obj = GPR_U32(ctx, 4);
            if (g_request.exchange(false, std::memory_order_relaxed))
            {
                const uint32_t player = rd32(kPlayer);
                if (inRam(player, 0x1700u) && isFree(player))
                    g_active = true, g_startMs = nowMs(), g_cancelMs = 0;
            }
            if (!g_active || ctx->pc != kMotionPose || obj != rd32(kPlayer) || !inRam(obj, 0x1700u))
                return g_inner(rdram, ctx, runtime);

            // The swing's clock and weight (host time: the swing is drawn only).
            const int64_t now = nowMs();
            const float from = g_from.load(std::memory_order_relaxed), to = std::max(from + 1.0f, g_to.load(std::memory_order_relaxed));
            const float speed = std::max(0.1f, g_speed.load(std::memory_order_relaxed));
            const float lengthMs = (to - from) / 30.0f / speed * 1000.0f;
            const float t = static_cast<float>(now - g_startMs);
            if (!isFree(obj) && g_cancelMs == 0)
                g_cancelMs = now; // he does something else: fade out (the swing never holds him)
            float weight = g_strength.load(std::memory_order_relaxed);
            {
                // eased (smoothstep) in and out
                float k = std::clamp(std::min(t / std::max(1.0f, g_easeIn.load(std::memory_order_relaxed)),
                                              (lengthMs - t) / std::max(1.0f, g_easeOut.load(std::memory_order_relaxed))),
                                     0.0f, 1.0f);
                weight *= k * k * (3.0f - 2.0f * k);
            }
            if (g_cancelMs)
                weight *= std::clamp(1.0f - static_cast<float>(now - g_cancelMs) / kCancelMs, 0.0f, 1.0f);
            if (t >= lengthMs || (g_cancelMs && now - g_cancelMs >= kCancelMs))
                g_active = false;
            const uint32_t turn = turnMotion(obj), parts = rd32(obj + 0x278u), count = g_ram[obj + 0x2B4u];
            if (!(weight > 0.001f) || !turn || count <= kLastBone || !inRam(parts, 4u * count))
                return g_inner(rdram, ctx, runtime);
            uint32_t part[kLastBone + 1];
            for (uint32_t i = kFirstBone; i <= kLastBone; ++i)
                if (!inRam(part[i] = rd32(parts + 4u * i), 0x10Cu))
                    return g_inner(rdram, ctx, runtime);
            g_inner(rdram, ctx, runtime); // the game's pose, untouched

            const float swingClock = std::min(from + t / 1000.0f * 30.0f * speed, to);
            for (uint32_t i = kFirstBone; i <= kLastBone; ++i)
            {
                float e[3], target[3];
                bool found[3];
                std::memcpy(e, g_ram + part[i] + 0x100u, 12);
                if (!std::isfinite(e[0]) || !std::isfinite(e[1]) || !std::isfinite(e[2]))
                    continue;
                sampleRotation(turn, i, swingClock, target, found);
                if (!found[0] && !found[1] && !found[2])
                    continue;
                for (int c = 0; c < 3; ++c)
                    if (!found[c])
                        target[c] = e[c]; // not keyed by the turn: the game's
                float ra[3][3], rb[3][3], rc[3][3], ec[3];
                eulerMatrix(e, ra);
                eulerMatrix(target, rb);
                quatMatrix(quatSlerp(matrixQuat(ra), matrixQuat(rb), weight), rc);
                if (eulerNear(rc, e, ec))
                    std::memcpy(g_ram + part[i] + 0x100u, ec, 12);
            }
        }
    }

    void trigger()
    {
        g_request.store(true, std::memory_order_relaxed);
    }

    void install(PS2Runtime &runtime)
    {
        g_ram = runtime.memory().getRDRAM();
        if (!g_ram || !runtime.hasFunction(kMotionPose) || !(g_inner = runtime.lookupFunction(kMotionPose)) ||
            !FunctionHooks::instance().replace(kMotionPose, "motion pose (Signal Style Switch arm swing)", &poseWrapped,
                                               ReplacementStatus::Native, "the quick turn's arm swing on a style switch, drawn only"))
        {
            g_inner = nullptr;
            KM_WARN(Game, "styles: the motion pose (0x158328) is not hookable: no arm swing");
        }
    }

    void drawTuning(const void *uiPointer)
    {
#if defined(KAMITE_HAS_GS_MENU)
        const Pcsx2GsUi *ui = static_cast<const Pcsx2GsUi *>(uiPointer);
        if (!ui || !g_inner)
            return;
        // Temporary (K, Nov 11): the swing's frames of the quick turn (motion 30, 21 frames), speed and strength.
        float from = g_from.load(), to = g_to.load(), speed = g_speed.load(), strength = g_strength.load();
        if (ui->slider_float("Arm swing from frame##swingfrom", &from, 0.0f, 19.0f, "%.1f"))
            g_from.store(from);
        if (ui->slider_float("Arm swing to frame##swingto", &to, 1.0f, 20.0f, "%.1f"))
            g_to.store(to);
        if (ui->slider_float("Arm swing speed##swingspeed", &speed, 0.25f, 2.0f, "%.2f"))
            g_speed.store(speed);
        if (ui->slider_float("Arm swing strength##swingstrength", &strength, 0.0f, 1.0f, "%.2f"))
            g_strength.store(strength);
        float easeIn = g_easeIn.load(), easeOut = g_easeOut.load();
        if (ui->slider_float("Arm swing ease in (ms)##swingin", &easeIn, 0.0f, 400.0f, "%.0f"))
            g_easeIn.store(easeIn);
        if (ui->slider_float("Arm swing ease out (ms)##swingout", &easeOut, 0.0f, 400.0f, "%.0f"))
            g_easeOut.store(easeOut);
        if (ui->button("Play arm swing##swingplay"))
            trigger();
#else
        (void)uiPointer;
#endif
    }
}
