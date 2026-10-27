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
        struct Saved
        {
            uint32_t address = 0;
            std::array<float, 16> original{}, mirrored{};
        };
        Saved g_saved[2]; // the two hair bones
        bool g_applied = false;

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
                            g_strands[0].q, g_saved[0]);
            if (hair.side.bone >= 0)
                placeHinged(rdram, parts, bind, hair.side.bone, 4, Vec{hair.side.pivot[0], hair.side.pivot[1], hair.side.pivot[2]},
                            g_strands[1].q, g_saved[1]);
        }

        void onGetMeshPtr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 31) != kTransCallSite)
                return;
            const uint32_t model = GPR_U32(ctx, 4);
            if (model != rd32(rdram, GameWork::address(GameWork::kPlayerObj)))
                return;
            if (characters::hair().back.bone < 0 && characters::hair().side.bone < 0)
                return;
            uint32_t parts[32];
            if (playerParts(rdram, parts) != 32)
                return;
            Vec bind[32];
            bindPositions(rdram, parts, 32, bind);
            placeHair(rdram, parts, bind);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kGetMeshPtr))
            return;
        FunctionHooks::instance().observeEntry(kGetMeshPtr, "bone_tweaks_pack", &onGetMeshPtr);
        const characters::Hair &h = characters::hair();
        g_tune[0].store(h.stiffness);
        g_tune[1].store(h.damping);
        g_tune[2].store(h.gravity);
        g_tune[3].store(h.wind);
        if (hasHair())
            KM_INFO(Game, "bone tweaks: hair on");
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

    bool hasHair() { return characters::hair().back.bone >= 0 || characters::hair().side.bone >= 0; }
    float tuning(Tune which)
    {
        const int i = static_cast<int>(which);
        return g_tune[i].load(std::memory_order_relaxed);
    }
    void setTuning(Tune which, float value)
    {
        const int i = static_cast<int>(which);
        g_tune[i].store(value, std::memory_order_relaxed);
    }

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
