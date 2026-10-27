#include "kamite/bone_tweaks.h"
#include "kamite/character_mods.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace kamite::bone_tweaks
{
    namespace
    {
        using namespace kamite::game;

        constexpr uint32_t kGetMeshPtr = 0x0014B6F8u;    // cModel_getMeshPtr(model, index)
        constexpr uint32_t kTransCallSite = 0x0014A4C0u; // the return address of its first call in cModel_Trans
        constexpr uint32_t kPartsFirst = 0x144u, kPartNext = 0x144u, kPartMtx = 0x80u, kPartNum = 0x2B4u;
        constexpr uint32_t kMeshHead = 0x25Cu, kNodeNext = 0x404u, kNodeFlags = 0x380u, kNodeInfo = 0x414u, kInfoName = 0x8u;
        constexpr int kLegs[2][3] = {{18, 19, 20}, {22, 23, 24}}; // thigh, shin, foot: right, left
        constexpr float kStraight = 0.01f;                        // metres the knee must be off the hip-ankle line

        std::atomic<bool> g_knees{false};
        std::atomic<float> g_kneeEase[2] = {45.0f, 100.0f}; // degrees of mirrored thigh-back where the knee starts / ends swinging home
        struct Saved
        {
            uint32_t address = 0;
            std::array<float, 16> original{}, mirrored{};
        };
        Saved g_saved[7]; // the thighs and shins, the snake's jaw bone, the two hair bones
        bool g_applied = false;
        float g_jaw = 0.0f;          // the snake jaw's angle now (radians), eased towards open / closed
        uint32_t g_fistNode = 0;     // the LH_01 mesh node, once found

        uint32_t rd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + (a & 0x01FFFFFFu), 4); return v; }
        bool inRam(uint32_t a, uint32_t size = 4u) { a &= 0x01FFFFFFu; return a >= 0x100000u && a + size <= 0x02000000u; }

        using Vec = std::array<float, 3>;
        using Mat = std::array<float, 9>; // row-major, column vectors: v' = M v
        Vec sub(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
        Vec add(const Vec &a, const Vec &b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
        Vec mulf(const Vec &a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }
        float dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
        Vec cross(const Vec &a, const Vec &b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
        float length(const Vec &a) { return std::sqrt(dot(a, a)); }
        Vec unit(const Vec &a) { const float l = length(a); return l > 1e-9f ? mulf(a, 1.0f / l) : Vec{0, 0, 0}; }
        Mat mul(const Mat &a, const Mat &b)
        {
            Mat r{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
            return r;
        }
        // The least rotation taking unit vector a onto unit vector b (Rodrigues).
        Mat between(const Vec &a, const Vec &b)
        {
            const Vec k = cross(a, b);
            const float c = dot(a, b);
            Mat r{1, 0, 0, 0, 1, 0, 0, 0, 1};
            if (c < -0.9999f)
                return r; // opposite: not reached for a knee (the mirror never turns a bone right round)
            const Mat kx{0, -k[2], k[1], k[2], 0, -k[0], -k[1], k[0], 0};
            const Mat kx2 = mul(kx, kx);
            const float f = 1.0f / (1.0f + c);
            for (int i = 0; i < 9; ++i)
                r[i] += kx[i] + kx2[i] * f;
            return r;
        }

        // The game's matrix: rows are the bone's axes (row vectors), row 3 the position.
        struct Bone
        {
            std::array<float, 16> m{};
            Mat rot() const { return {m[0], m[4], m[8], m[1], m[5], m[9], m[2], m[6], m[10]}; } // the transpose
            Vec pos() const { return {m[12], m[13], m[14]}; }
            void set(const Mat &r, const Vec &p)
            {
                m[0] = r[0]; m[4] = r[1]; m[8] = r[2];
                m[1] = r[3]; m[5] = r[4]; m[9] = r[5];
                m[2] = r[6]; m[6] = r[7]; m[10] = r[8];
                m[12] = p[0]; m[13] = p[1]; m[14] = p[2];
            }
        };
        Bone readBone(const uint8_t *rdram, uint32_t part) { Bone b; std::memcpy(b.m.data(), rdram + ((part + kPartMtx) & 0x01FFFFFFu), 64); return b; }
        void writeBone(uint8_t *rdram, uint32_t part, const Bone &b) { std::memcpy(rdram + ((part + kPartMtx) & 0x01FFFFFFu), b.m.data(), 64); }

        // The player's cParts in bone order (0 on failure).
        int playerParts(const uint8_t *rdram, uint32_t parts[32])
        {
            const uint32_t model = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
            if (!inRam(model, 0x2E4u))
                return 0;
            const int n = rdram[(model + kPartNum) & 0x01FFFFFFu];
            uint32_t p = rd32(rdram, model + kPartsFirst);
            int i = 0;
            for (; i < n && i < 32 && inRam(p, 0x160u); ++i)
            {
                parts[i] = p;
                p = rd32(rdram, p + kPartNext);
            }
            return i;
        }

        void mirrorKnees(uint8_t *rdram)
        {
            uint32_t parts[32];
            if (playerParts(rdram, parts) < 26)
                return;
            // Already mirrored this frame (a second pack of the same matrices): leave them.
            if (g_applied && g_saved[0].address == parts[18])
            {
                Bone now = readBone(rdram, parts[18]);
                if (std::memcmp(now.m.data(), g_saved[0].mirrored.data(), 64) == 0)
                    return;
            }
            int saved = 0;
            // The pelvis: its side axis (row 0, her left) tells sideways from forward, its up (row 1) and forward
            // (row 2) measure how far back a thigh would point.
            const Bone pelvis = readBone(rdram, parts[17]);
            const Vec side{pelvis.m[0], pelvis.m[1], pelvis.m[2]}, up{pelvis.m[4], pelvis.m[5], pelvis.m[6]},
                fwd{pelvis.m[8], pelvis.m[9], pelvis.m[10]};
            const float e1 = g_kneeEase[0].load(std::memory_order_relaxed), e2 = g_kneeEase[1].load(std::memory_order_relaxed);
            for (int li = 0; li < 2; ++li)
            {
                const auto &leg = kLegs[li];
                Bone thigh = readBone(rdram, parts[leg[0]]), shin = readBone(rdram, parts[leg[1]]), foot = readBone(rdram, parts[leg[2]]);
                const Vec hip = thigh.pos(), knee = shin.pos(), ankle = foot.pos();
                const Vec axis = unit(sub(ankle, hip));
                const Vec v = sub(knee, hip);
                const Vec par = mulf(axis, dot(v, axis));
                const Vec sideP = unit(sub(side, mulf(axis, dot(side, axis)))); // the side axis across the leg
                const Vec out = li == 0 ? mulf(sideP, -1.0f) : sideP;             // outward: her right leg goes -x
                const Vec whole = sub(v, par);
                const Vec perp = sub(whole, mulf(sideP, dot(whole, sideP)));    // the forward part
                g_saved[saved].address = parts[leg[0]];
                g_saved[saved].original = thigh.m;
                g_saved[saved + 1].address = parts[leg[1]];
                g_saved[saved + 1].original = shin.m;
                if (length(perp) > kStraight && length(axis) > 0.5f)
                {
                    // The knee on its circle round the hip-ankle line (fixed bone lengths, feet and hips stay): psi
                    // from the forward direction towards the outward side. The original knee is at psi0, the mirror
                    // (forward offset flipped, sideways kept) at pi - psi0. A deep bend mirrored points the thigh far
                    // back past what the hip's skin can take (the sprint slide, kicks, crouches: Oct 27); from e1 to
                    // e2 degrees of that, the knee swings from the mirror round the outward side back to the
                    // original, smoothly, so deep bends keep the game's own knee.
                    const Vec u = unit(perp);
                    const float r = length(whole);
                    const float psi0 = std::atan2(dot(whole, out), dot(whole, u)), psim = 3.14159265f - psi0;
                    const Vec centre = add(hip, par);
                    auto at = [&](float psi) { return add(centre, add(mulf(u, r * std::cos(psi)), mulf(out, r * std::sin(psi)))); };
                    const Vec th = unit(sub(at(psim), hip));
                    const float back = -std::atan2(dot(th, fwd), -dot(th, up)) * 57.2957795f; // degrees behind straight down
                    float t = std::clamp((back - e1) / std::max(1.0f, e2 - e1), 0.0f, 1.0f);
                    t = t * t * (3.0f - 2.0f * t);
                    const Vec knee2 = at(psim - t * (psim - psi0));
                    const Mat q1 = between(unit(sub(knee, hip)), unit(sub(knee2, hip)));
                    const Mat q2 = between(unit(sub(ankle, knee)), unit(sub(ankle, knee2)));
                    thigh.set(mul(q1, thigh.rot()), hip);
                    shin.set(mul(q2, shin.rot()), knee2);
                    writeBone(rdram, parts[leg[0]], thigh);
                    writeBone(rdram, parts[leg[1]], shin);
                }
                g_saved[saved].mirrored = thigh.m;
                g_saved[saved + 1].mirrored = shin.m;
                saved += 2;
            }
            g_applied = true;
        }

        Mat axisAngle(const Vec &k, float a)
        {
            const float c = std::cos(a), s = std::sin(a), t = 1 - c;
            return {t * k[0] * k[0] + c, t * k[0] * k[1] - s * k[2], t * k[0] * k[2] + s * k[1],
                    t * k[0] * k[1] + s * k[2], t * k[1] * k[1] + c, t * k[1] * k[2] - s * k[0],
                    t * k[0] * k[2] - s * k[1], t * k[1] * k[2] + s * k[0], t * k[2] * k[2] + c};
        }
        Vec applyRot(const Mat &m, const Vec &v)
        {
            return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
        }

        // Gene's fist mesh LH_01 is shown (the game swaps hand meshes by node visibility; Xavier keeps the nodes empty).
        bool fistShown(const uint8_t *rdram, uint32_t model)
        {
            auto nodeIs = [&](uint32_t node, const char *name) {
                const uint32_t info = rd32(rdram, node + kNodeInfo);
                return inRam(info, 0x10u) && std::memcmp(rdram + ((info + kInfoName) & 0x01FFFFFFu), name, 6) == 0;
            };
            if (!inRam(g_fistNode, 0x418u) || !nodeIs(g_fistNode, "LH_01"))
            {
                g_fistNode = 0;
                uint32_t node = rd32(rdram, model + kMeshHead);
                for (int i = 0; i < 64 && inRam(node, 0x418u) && !g_fistNode; ++i, node = rd32(rdram, node + kNodeNext))
                    if (nodeIs(node, "LH_01"))
                        g_fistNode = node;
                if (!g_fistNode)
                    return false;
            }
            return (rd32(rdram, g_fistNode + kNodeFlags) & 1u) == 0u;
        }

        // The snake's jaw: its bone (31, a head child the game never moves) placed under the hand (bone 16) with the
        // jaw turned about its hinge: shut while the fist mesh shows, open otherwise (swapped at once, as the game swaps the hand). The
        // game skins with mtx * (bind pose)^-1 and the bind rotations are identity, so for a vertex p (bind space):
        // world = R16 (pivot + R (p - GW16 - pivot)) + t16 = A (p - GW31) + b with A = R16 R, b = R16 (R (d - pivot) + pivot) + t16,
        // d = GW31 - GW16 (bind positions, the part offsets summed up the chain).
        void placeSnakeJaw(uint8_t *rdram, uint32_t model, const uint32_t parts[32], const Vec bind[32]);
        // The bind positions (the part offsets summed up the chain; the bind rotations are identity).
        void bindPositions(const uint8_t *rdram, const uint32_t parts[32], int count, Vec bind[32])
        {
            for (int i = 0; i < count; ++i)
            {
                float t[3];
                std::memcpy(t, rdram + ((parts[i] + 0xE0u) & 0x01FFFFFFu), 12);
                const uint32_t parent = rd32(rdram, parts[i] + 0x148u);
                int pi = -1;
                for (int j = 0; j < i; ++j)
                    if (parts[j] == parent)
                        pi = j;
                bind[i] = pi >= 0 ? add(bind[pi], Vec{t[0], t[1], t[2]}) : Vec{t[0], t[1], t[2]};
            }
        }

        // A spare bone placed as its parent turned by q about a pivot (both in the parent's bind frame, pivot
        // relative to the parent's bind position): world = Rp (pivot + q (p - Bp - pivot)) + tp for a bind-space p, so
        // mtx = [Rp q | Rp (q (d - pivot) + pivot) + tp] with d = Bbone - Bp.
        void placeHinged(uint8_t *rdram, const uint32_t parts[32], const Vec bind[32], int bone, int parent, const Vec &pivot,
                         const Mat &q, Saved &s)
        {
            const Bone p = readBone(rdram, parts[parent]);
            Bone b = readBone(rdram, parts[bone]);
            if (!(s.address == parts[bone] && std::memcmp(b.m.data(), s.mirrored.data(), 64) == 0))
            {
                s.address = parts[bone];
                s.original = b.m;
            }
            const Mat rp = p.rot();
            const Vec d = sub(bind[bone], bind[parent]);
            b.set(mul(rp, q), add(applyRot(rp, add(applyRot(q, sub(d, pivot)), pivot)), p.pos()));
            writeBone(rdram, parts[bone], b);
            s.mirrored = b.m;
            g_applied = true;
        }

        void placeSnakeJaw(uint8_t *rdram, uint32_t model, const uint32_t parts[32], const Vec bind[32])
        {
            const characters::HingedBone &jaw = characters::snakeJaw();
            if (jaw.bone < 0 || jaw.bone > 31 || jaw.parent < 0 || jaw.parent > 31)
                return;
            g_jaw = fistShown(rdram, model) ? jaw.closed : 0.0f; // a swap, like the game's fist mesh: no easing (K)
            placeHinged(rdram, parts, bind, jaw.bone, jaw.parent, Vec{jaw.pivot[0], jaw.pivot[1], jaw.pivot[2]},
                        axisAngle(unit(Vec{jaw.axis[0], jaw.axis[1], jaw.axis[2]}), g_jaw), g_saved[4]);
        }

        // ---- hair: each strand's tip a point mass on a rod from its root, pulled back to the rest shape by a spring,
        // damped against the root's own motion, pushed by gravity when the head tilts and by a gusting wind; the
        // bone turned to point at it within its swing limits. Stepped once per game frame.
        std::atomic<float> g_tune[4] = {40.0f, 5.0f, 0.6f, 1.2f}; // stiffness, damping, gravity, wind (F2 > Debug)
        uint32_t g_frame = 0, g_stepped = ~0u;
        std::chrono::steady_clock::time_point g_lastStep{};
        float g_clock = 0.0f;
        struct Strand
        {
            bool live = false;
            Vec tip{}, vel{}, root{};
            Mat q{1, 0, 0, 0, 1, 0, 0, 0, 1};
        };
        Strand g_strands[2];

        Mat rotX(float a) { const float c = std::cos(a), s = std::sin(a); return {1, 0, 0, 0, c, -s, 0, s, c}; }
        Mat rotZ(float a) { const float c = std::cos(a), s = std::sin(a); return {c, -s, 0, s, c, 0, 0, 0, 1}; }
        Mat transpose(const Mat &m) { return {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; }

        void stepStrand(Strand &st, const characters::HairStrand &cfg, const Bone &head, float dt, const characters::Hair &lim)
        {
            const Mat rh = head.rot();
            const Vec pivot{cfg.pivot[0], cfg.pivot[1], cfg.pivot[2]}, rest = unit(Vec{cfg.dir[0], cfg.dir[1], cfg.dir[2]});
            const Vec root = add(head.pos(), applyRot(rh, pivot));
            const Vec restTip = add(root, mulf(applyRot(rh, rest), cfg.length));
            if (!st.live || length(sub(root, st.root)) > 1.0f)
            {
                st.live = true;
                st.tip = restTip;
                st.vel = {0, 0, 0};
                st.root = root;
            }
            const Vec rootVel = mulf(sub(root, st.root), 1.0f / dt);
            st.root = root;
            const float k = g_tune[0].load(std::memory_order_relaxed), c = g_tune[1].load(std::memory_order_relaxed),
                        g = g_tune[2].load(std::memory_order_relaxed) * 9.8f, w = g_tune[3].load(std::memory_order_relaxed);
            const Vec down{0, -1, 0};
            const Vec gdev = mulf(sub(down, applyRot(rh, down)), g); // nothing while the head is upright
            const float gust = 0.6f + 0.4f * std::sin(g_clock * 1.3f) + 0.3f * std::sin(g_clock * 3.7f + 1.0f);
            const Vec wind = mulf(unit(Vec{1.0f, 0.0f, 0.35f}), w * gust);
            Vec acc = add(add(mulf(sub(restTip, st.tip), k), gdev), wind);
            acc = sub(acc, mulf(sub(st.vel, rootVel), c));
            st.vel = add(st.vel, mulf(acc, dt));
            st.tip = add(st.tip, mulf(st.vel, dt));
            st.tip = add(root, mulf(unit(sub(st.tip, root)), cfg.length));
            // The bone's turn, in the head's frame: pitch (towards her front +z) and roll (towards her left +x),
            // each within its limits; a hinged strand only pitches.
            const Vec dl = applyRot(transpose(rh), unit(sub(st.tip, root)));
            constexpr float kRad = 3.14159265f / 180.0f;
            float pitch = std::atan2(dl[2], -dl[1]) - std::atan2(rest[2], -rest[1]);
            float roll = std::asin(std::clamp(dl[0], -1.0f, 1.0f)) - std::asin(std::clamp(rest[0], -1.0f, 1.0f));
            if (cfg.hinge)
            {
                pitch = std::clamp(pitch, -lim.sideSwing * kRad, lim.sideSwing * kRad);
                roll = 0.0f;
            }
            else
            {
                pitch = std::clamp(pitch, -lim.backBack * kRad, lim.backForward * kRad);
                roll = std::clamp(roll, -lim.backSide * kRad, lim.backSide * kRad);
            }
            st.q = mul(rotZ(roll), rotX(-pitch));
            st.tip = add(root, mulf(applyRot(rh, applyRot(st.q, rest)), cfg.length)); // the state keeps to the limits
        }

        void placeHair(uint8_t *rdram, const uint32_t parts[32], const Vec bind[32])
        {
            const characters::Hair &hair = characters::hair();
            if (g_stepped != g_frame)
            {
                const auto now = std::chrono::steady_clock::now();
                float dt = g_stepped == ~0u ? 1.0f / 60.0f : std::chrono::duration<float>(now - g_lastStep).count();
                dt = std::clamp(dt, 1.0f / 120.0f, 1.0f / 20.0f);
                g_lastStep = now;
                g_stepped = g_frame;
                g_clock += dt;
                const Bone head = readBone(rdram, parts[4]);
                if (hair.back.bone >= 0)
                    stepStrand(g_strands[0], hair.back, head, dt, hair);
                if (hair.side.bone >= 0)
                    stepStrand(g_strands[1], hair.side, head, dt, hair);
            }
            if (hair.back.bone >= 0)
                placeHinged(rdram, parts, bind, hair.back.bone, 4, Vec{hair.back.pivot[0], hair.back.pivot[1], hair.back.pivot[2]},
                            g_strands[0].q, g_saved[5]);
            if (hair.side.bone >= 0)
                placeHinged(rdram, parts, bind, hair.side.bone, 4, Vec{hair.side.pivot[0], hair.side.pivot[1], hair.side.pivot[2]},
                            g_strands[1].q, g_saved[6]);
        }

        void onGetMeshPtr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 31) != kTransCallSite)
                return;
            const uint32_t model = GPR_U32(ctx, 4);
            if (model != rd32(rdram, GameWork::address(GameWork::kPlayerObj)))
                return;
            if (g_knees.load(std::memory_order_relaxed))
                mirrorKnees(rdram);
            if (characters::snakeJaw().bone < 0 && characters::hair().back.bone < 0 && characters::hair().side.bone < 0)
                return;
            uint32_t parts[32];
            if (playerParts(rdram, parts) != 32)
                return;
            Vec bind[32];
            bindPositions(rdram, parts, 32, bind);
            placeSnakeJaw(rdram, model, parts, bind);
            placeHair(rdram, parts, bind);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *knees = std::getenv("KAMITE_BACKWARD_KNEES");
        g_knees.store(knees && *knees && std::strcmp(knees, "0") != 0);
        if (!runtime.hasFunction(kGetMeshPtr))
            return;
        FunctionHooks::instance().observeEntry(kGetMeshPtr, "bone_tweaks_pack", &onGetMeshPtr);
        const characters::Hair &h = characters::hair();
        g_tune[0].store(h.stiffness);
        g_tune[1].store(h.damping);
        g_tune[2].store(h.gravity);
        g_tune[3].store(h.wind);
        KM_INFO(Game, "bone tweaks: backward knees " << (g_knees.load() ? "on" : "off") << " ([Mods] BackwardKnees; F2 > Debug)"
                                                     << (characters::snakeJaw().bone >= 0 ? "; snake jaw on" : ""));
    }

    void onFrame(uint8_t *rdram, R5900Context *, PS2Runtime *)
    {
        ++g_frame;
        if (!g_applied)
            return;
        g_applied = false;
        for (Saved &s : g_saved)
        {
            if (!inRam(s.address, 0x160u))
                continue;
            Bone now = readBone(rdram, s.address);
            if (std::memcmp(now.m.data(), s.mirrored.data(), 64) == 0) // still ours: the game's own back
                std::memcpy(rdram + ((s.address + kPartMtx) & 0x01FFFFFFu), s.original.data(), 64);
            s.address = 0;
        }
    }

    bool backwardKnees() { return g_knees.load(std::memory_order_relaxed); }
    bool hasHair() { return characters::hair().back.bone >= 0 || characters::hair().side.bone >= 0; }
    float tuning(Tune which)
    {
        const int i = static_cast<int>(which);
        return i < 4 ? g_tune[i].load(std::memory_order_relaxed) : g_kneeEase[i - 4].load(std::memory_order_relaxed);
    }
    void setTuning(Tune which, float value)
    {
        const int i = static_cast<int>(which);
        if (i < 4)
            g_tune[i].store(value, std::memory_order_relaxed);
        else
            g_kneeEase[i - 4].store(value, std::memory_order_relaxed);
    }
    void setBackwardKnees(bool on) { g_knees.store(on, std::memory_order_relaxed); }

    std::string handNodes(const uint8_t *rdram)
    {
        const uint32_t model = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
        if (!inRam(model, 0x2E4u))
            return "(no player)";
        std::string out;
        uint32_t node = rd32(rdram, model + kMeshHead);
        for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
        {
            const uint32_t info = rd32(rdram, node + kNodeInfo);
            if (inRam(info, 0x10u))
            {
                char name[9] = {};
                std::memcpy(name, rdram + ((info + kInfoName) & 0x01FFFFFFu), 8);
                const bool hidden = (rd32(rdram, node + kNodeFlags) & 1u) != 0u;
                if (std::strstr(name, "H_") && !hidden)
                    out += std::string(out.empty() ? "" : " ") + name;
            }
            node = rd32(rdram, node + kNodeNext);
        }
        return out.empty() ? "(none)" : out;
    }
}
