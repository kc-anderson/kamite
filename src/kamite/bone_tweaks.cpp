#include "kamite/bone_tweaks.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <array>
#include <atomic>
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
        struct Saved
        {
            uint32_t address = 0;
            std::array<float, 16> original{}, mirrored{};
        };
        Saved g_saved[4]; // the thighs and shins
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
            for (const auto &leg : kLegs)
            {
                Bone thigh = readBone(rdram, parts[leg[0]]), shin = readBone(rdram, parts[leg[1]]), foot = readBone(rdram, parts[leg[2]]);
                const Vec hip = thigh.pos(), knee = shin.pos(), ankle = foot.pos();
                const Vec axis = unit(sub(ankle, hip));
                const Vec v = sub(knee, hip);
                const Vec par = mulf(axis, dot(v, axis)), perp = sub(v, par);
                g_saved[saved].address = parts[leg[0]];
                g_saved[saved].original = thigh.m;
                g_saved[saved + 1].address = parts[leg[1]];
                g_saved[saved + 1].original = shin.m;
                if (length(perp) > kStraight && length(axis) > 0.5f)
                {
                    const Vec knee2 = add(hip, sub(par, perp));
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

        void onGetMeshPtr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (!g_knees.load(std::memory_order_relaxed) || GPR_U32(ctx, 31) != kTransCallSite)
                return;
            const uint32_t model = GPR_U32(ctx, 4);
            if (model != rd32(rdram, GameWork::address(GameWork::kPlayerObj)))
                return;
            mirrorKnees(rdram);
        }
    }

    void install(PS2Runtime &runtime)
    {
        const char *knees = std::getenv("KAMITE_BACKWARD_KNEES");
        g_knees.store(knees && *knees && std::strcmp(knees, "0") != 0);
        if (!runtime.hasFunction(kGetMeshPtr))
            return;
        FunctionHooks::instance().observeEntry(kGetMeshPtr, "bone_tweaks_pack", &onGetMeshPtr);
        KM_INFO(Game, "bone tweaks: backward knees " << (g_knees.load() ? "on" : "off") << " ([Mods] BackwardKnees; F2 > Debug)");
    }

    void onFrame(uint8_t *rdram, R5900Context *, PS2Runtime *)
    {
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
