// Buck Breaker's punish fits (K, Nov 8): a punish made for one body leaves Gene too far from (or into) another, e.g.
// Stinger, made for the fat men, leaves a gap on a thug whose bent-over head is also 15-20 cm lower. Each body is
// fitted offline to the original's touch points (tools/models/punish_fit, claude/punish-fit-offline.md): per punish
// and body, curves over the punish's motions of Gene's placement (forward, side, turn), Gene's height and the
// victim's height (the hips up or down; the game's leg IK bends the knees, the feet stay put) and waist bend.
//
// Placement. Each paired punish places Gene with func_001299F0(a0 = Gene, a1 = the enemy, a2 = &vec{x, 0, z, 1} on the
// caller's stack, a3 = mode, f12 = yaw): mode 0 turns Gene to face the enemy, the enemy's heading (+0x104) = Gene's +
// pi, then Gene's += f12; mode 1 only sets the enemy's heading = Gene's + pi. Then Gene = enemy.pos + RotY(enemy
// heading) . (x * enemy +0x110, 0, z * enemy +0x118). After that each of the two moves by its own motion's root
// travel, never by the other's position, so Gene moved in the middle of the punish stays moved. The wrapper
// (FunctionHooks::replace on 0x1299F0, only the four punishes' calls, by return address) puts the curve's first
// values in: Turn in f12 (mode 0), or for mode 1 off Gene's heading for the call and back after; then Gene is moved
// by RotY(his facing without Turn) . (Side * enemy +0x110, 0, Forward * enemy +0x118); RotY as the game's
// (sceVu0RotMatrixY): x' = x cos + z sin, z' = -x sin + z cos; +z forward, +x the left. Each vblank while the punish
// plays, Gene's motion (+0x428) and frame (+0x430, half steps at 60 FPS) give the curve's values, and Gene moves by
// the difference from what was applied.
//
// Heights and bend. func_002A8578 (actor, motion, events, ...) starts every motion. When Gene starts one of the
// punish's motions (pointers from the caught enemy's table, +0x694 -> +0x304) or the caught enemy one of its own,
// the motion is copied into kRewriteArea with bone 0's height (Gene / victim) and bone 1's x rotation (the victim's
// waist) changed by the curve (mot_edit addCurve, npc_poses' way), and started from there. Copies are kept per
// (body, punish, motion) and reused; the area starts over when full.
//
// Gene's pose (K, Nov 8: his arms through the victim where the original keeps clear; Cobra Twist's arms round the
// neck on women and fat men; Gorilla Suplex's arms folded too far in on the fat men): Gene's tracks also carry his torso
// (bone 1 turned in bone 0's space: x, z) and each arm (the upper arm, 13 / 7, turned in the clavicle's space: raise
// about z, swing about y; the forearm, 14 / 8, about y in the upper arm's), put in front of the motion's own rotation
// every frame (mot_edit preRotate); the shoulder helpers 12 / 6, which follow the upper arms at about half in the game's
// motions, get half. Offline the same pre-rotations are measured (fit5.py gene_xform).
//
// Size. Each fit is for an enemy size (scale=, the enemy's +0x114 at the catch); between two the values are
// interpolated, past the ends held; a body whose smallest fit is above 1.0 (the original pairing, fitted only when
// scaled up) is unchanged at 1.0 and blends in above it. A fit marked off at a size: from halfway there the pairing
// doesn't fit (Buck Breaker keeps the enemy's own punish), and below that it is left out of the blend. size= is the victim's own size change for the pairing
// (0.95..1.05, K: unnoticed in play): the enemy's scale (+0x110 / +0x114 / +0x118) eases to it over a third of a
// second after the catch and back after the punish; the game's placement calls see the enemy's own scale.
//
// The body: the enemy's skeleton (its cModel's parts, list at +0x144, count at +0x2B4; each part's offset +0xE0 and
// parent +0x148), hashed from the parts after the root: recolours share it. Hashed at every use (an enemy number does
// not fix its model). Values in the file are at size 1; placement is scaled by the enemy's size, heights are the
// model's own (the actor's scale applies).

#include "kamite/punish_fit.h"

#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/mot_edit.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_pnach.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace kamite::punish_fit
{
    namespace
    {
        namespace work = game::GameWork;

        constexpr uint32_t kAlign = 0x001299F0u, kStartMotion = 0x002A8578u;
        constexpr float kPi = 3.14159265358979f, kDegrees = kPi / 180.0f;
        constexpr uint32_t kEm00Entries = 3998u;

        // The punishes' motions (em00 entries; Gene's come from the caught enemy's table, cEm00_GetPlMotion).
        struct Motions
        {
            uint16_t gene[4];
            uint16_t victim[4];
        };
        constexpr Motions kMotions[kPunishCount] = {
            {{333, 0, 0, 0}, {329, 331, 0, 0}},
            {{811, 813, 815, 0}, {805, 807, 809, 0}},
            {{1551, 1553, 1557, 1555}, {1543, 1545, 1549, 1547}},
            {{1337, 1339, 1341, 0}, {1327, 1329, 1331, 0}},
            {{3840, 0, 0, 0}, {3838, 0, 0, 0}},
        };
        // func_001299F0's calls by the four punishes, by return address, and the Gene motion each one starts.
        struct Call
        {
            uint32_t ra;
            Punish punish;
            uint16_t geneMotion;
        };
        constexpr Call kCalls[] = {{0x0011770Cu, Suplex, 333},     {0x00117AC8u, Stinger, 811}, {0x00117B90u, Stinger, 813},
                                   {0x00117E40u, Stinger, 815},    {0x0011825Cu, CobraTwist, 1551}, {0x00118790u, Spanking, 1337},
                                   {0x0011806Cu, GorillaSuplex, 3840}};
        constexpr uint8_t kGenePhase[kPunishCount] = {57u, 59u, 61u, 62u, 60u};
        const char *const kNames[kPunishCount] = {"Suplex", "Stinger", "Cobra Twist", "Spanking", "Gorilla Suplex"};
        const char *const kFileNames[kPunishCount] = {"Suplex", "Stinger", "CobraTwist", "Spanking", "GorillaSuplex"};

        constexpr uint32_t kNumber = 0x564u, kScaleX = 0x110u, kScaleY = 0x114u, kScaleZ = 0x118u, kCaught = 0x694u;
        constexpr int kRampVblanks = 20; // the victim's size change eases in (and out) over this many vblanks
        constexpr int kValues = 12;
        constexpr uint32_t kParts = 0x144u, kPartCount = 0x2B4u, kPartOffset = 0xE0u, kPartParent = 0x148u;
        constexpr uint32_t kMotionPlaying = 0x428u, kMotionFrame = 0x430u;

        // A curve: keys (frame, values). Gene: forward (m), side (m), turn (degrees), height (m), torso x, torso z,
        // left raise, left swing, left elbow, right raise, right swing, right elbow (degrees); victim: height (m), bend
        // (degrees).
        struct Key
        {
            float frame = 0.0f;
            float v[kValues] = {};
        };
        struct Track
        {
            uint16_t motion = 0;
            bool gene = false;
            std::vector<Key> keys;
            void at(float frame, float out[kValues]) const
            {
                for (int i = 0; i < kValues; ++i)
                    out[i] = 0.0f;
                if (keys.empty())
                    return;
                if (frame <= keys.front().frame || keys.size() == 1)
                {
                    std::memcpy(out, keys.front().v, sizeof(Key::v));
                    return;
                }
                if (frame >= keys.back().frame)
                {
                    std::memcpy(out, keys.back().v, sizeof(Key::v));
                    return;
                }
                size_t i = 1;
                while (keys[i].frame < frame)
                    ++i;
                const Key &a = keys[i - 1], &b = keys[i];
                const float t = (frame - a.frame) / (b.frame - a.frame);
                for (int c = 0; c < kValues; ++c)
                    out[c] = a.v[c] + (b.v[c] - a.v[c]) * t;
            }
        };
        struct Fit
        {
            bool off = false;
            bool gorilla = false; // a Suplex fit: the Suplex pick plays the Gorilla Suplex on this body
            float scale = 1.0f;   // the enemy size it was fitted at
            float size = 1.0f;    // the victim's size change for the pairing
            std::vector<Track> tracks;
            const Track *find(uint16_t motion, bool gene) const
            {
                for (const Track &t : tracks)
                    if (t.motion == motion && t.gene == gene)
                        return &t;
                return nullptr;
            }
        };

        using Fits = std::vector<std::shared_ptr<Fit>>; // one body's fits for one punish, by enemy size

        // Whether the pairing fits at enemy size s: the fit nearest that size decides (a body whose fits are all for
        // larger sizes is unchanged at 1.0, which fits).
        bool offAt(const Fits &fits, float s)
        {
            const Fit *best = nullptr;
            float d = fits.front()->scale > 1.0f ? std::fabs(s - 1.0f) : 1e9f;
            for (const auto &f : fits)
                if (std::fabs(f->scale - s) < d)
                {
                    d = std::fabs(f->scale - s);
                    best = f.get();
                }
            return best && best->off;
        }

        // The fits at enemy size s (see the top); the sizes marked off are left out.
        Fit blend(const Fits &all, float s)
        {
            static const Fit none;
            Fits fits;
            for (const auto &f : all)
                if (!f->off)
                    fits.push_back(f);
            if (fits.empty())
                return none;
            const Fit *lo = nullptr, *hi = nullptr;
            if (fits.front()->scale > 1.0f)
            {
                if (s <= 1.0f)
                    return none; // the original pairing at its own size: unchanged
                lo = &none;
            }
            for (const auto &f : fits)
            {
                if (f->scale <= s)
                    lo = f.get();
                if (f->scale >= s && !hi)
                    hi = f.get();
            }
            if (!lo)
                return *fits.front();
            if (!hi)
                return *fits.back();
            if (lo == hi || hi->scale <= lo->scale)
                return *hi;
            const float t = (s - lo->scale) / (hi->scale - lo->scale);
            Fit out = *hi;
            out.scale = s;
            out.size = lo->size + (hi->size - lo->size) * t;
            for (Track &track : out.tracks)
            {
                const Track *a = lo->find(track.motion, track.gene);
                for (Key &k : track.keys)
                {
                    float va[kValues] = {};
                    if (a)
                        a->at(k.frame, va);
                    for (int c = 0; c < kValues; ++c)
                        k.v[c] = va[c] + (k.v[c] - va[c]) * t;
                }
            }
            return out;
        }

        // The pair playing (EE thread only).
        struct Pair
        {
            int punish = -1;
            uint32_t gene = 0, enemy = 0, body = 0;
            Fit own;                // the body's fit at the enemy's size
            const Fit *fit = nullptr; // &own while fitted
            int sizeKey = 100;      // the enemy's size x 100 (the copies are per size)
            float baseScale[3] = {1.0f, 1.0f, 1.0f};
            bool live = false;
            uint32_t geneMotionPtr = 0; // the pointer Gene's current punish motion was started with (ours or the game's)
            const Track *geneTrack = nullptr;
            float applied[3] = {}; // forward, side, turn (radians) put in so far
        };

        // The victim's size change, easing in while the pair plays and out after it (EE thread only).
        struct Resize
        {
            uint32_t enemy = 0, number = 0;
            float base[3] = {1.0f, 1.0f, 1.0f};
            float now = 1.0f, target = 1.0f, written = 0.0f;
            bool active = false;
        };

        uint8_t *g_ram = nullptr;
        bool g_on = false;
        PS2Runtime::RecompiledFunction g_align = nullptr;
        fs::path g_file, g_shippedFile;
        std::map<uint64_t, Fits> g_fits; // punish << 32 | body, by size; read at start, then only read
        Pair g_pair;
        Resize g_resize;
        std::map<std::tuple<uint32_t, int, uint16_t, bool, int>, uint32_t> g_copies; // (body, punish, motion, gene, size) -> copy
        uint32_t g_areaAt = kRewriteArea;
        std::set<uint64_t> g_logged;

        uint64_t fitKey(int punish, uint32_t body) { return (static_cast<uint64_t>(punish) << 32) | body; }

        bool inRam(uint32_t p, uint32_t size)
        {
            p &= 0x1FFFFFFFu;
            return p >= 0x100000u && p < PS2_RAM_SIZE && size <= PS2_RAM_SIZE - p;
        }
        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 1u)), sizeof(v));
            return v;
        }
        float rdF(const uint8_t *rdram, uint32_t a)
        {
            float v;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 1u)), sizeof(v));
            return v;
        }
        void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & (PS2_RAM_SIZE - 1u)), &v, sizeof(v)); }

        float wrap(float a)
        {
            while (a > kPi)
                a -= 2.0f * kPi;
            while (a < -kPi)
                a += 2.0f * kPi;
            return a;
        }

        std::string enemyName(uint16_t number)
        {
            char text[8];
            std::snprintf(text, sizeof(text), "em%02x", number & 0xFFu);
            return text;
        }

        // The enemy's skeleton hashed (FNV-1a): part count, then each part after the root: its parent's index and its
        // offset's bits (tools/models/punish_fit/common.py runtime_key computes the same from the file). 0 when it
        // cannot be read.
        uint32_t skeletonHash(const uint8_t *rdram, uint32_t enemy)
        {
            const int n = rdram[(enemy + kPartCount) & (PS2_RAM_SIZE - 1u)];
            if (n < 2 || n > 64)
                return 0u;
            uint32_t parts[64];
            uint32_t p = rd32(rdram, enemy + kParts);
            for (int i = 0; i < n; ++i)
            {
                if (!inRam(p, 0x150u))
                    return 0u;
                parts[i] = p & 0x1FFFFFFu;
                p = rd32(rdram, p + kParts);
            }
            uint32_t h = 2166136261u;
            const auto mix = [&h](const void *data, size_t size) {
                const auto *b = static_cast<const uint8_t *>(data);
                for (size_t i = 0; i < size; ++i)
                    h = (h ^ b[i]) * 16777619u;
            };
            mix(&n, sizeof(n));
            for (int i = 1; i < n; ++i)
            {
                const uint32_t parent = rd32(rdram, parts[i] + kPartParent) & 0x1FFFFFFu;
                int8_t index = -1;
                for (int j = 0; j < n; ++j)
                    if (parts[j] == parent && j != i)
                        index = static_cast<int8_t>(j);
                mix(&index, 1);
                mix(rdram + parts[i] + kPartOffset, 12);
            }
            return h ? h : 1u;
        }

        uint32_t bodyOf(const uint8_t *rdram, uint32_t enemy)
        {
            const uint32_t body = skeletonHash(rdram, enemy);
            return body ? body : 0x80000000u | (rd32(rdram, enemy + kNumber) & 0xFFFFu);
        }

        const Fits *fitsFor(int punish, uint32_t body)
        {
            const auto it = g_fits.find(fitKey(punish, body));
            return it == g_fits.end() || it->second.empty() ? nullptr : &it->second;
        }
        const Fit *fitFor(int punish, uint32_t body) // its flags (off, gorilla)
        {
            const Fits *fits = fitsFor(punish, body);
            return fits ? fits->front().get() : nullptr;
        }

        // The motion `index` of an enemy's em00-style table (pack + its offset, u32 wrap like the game), 0 if none.
        uint32_t entry(const uint8_t *rdram, uint32_t enemy, uint16_t index)
        {
            const uint32_t pack = rd32(rdram, enemy + game::GameObj::kMotionData) & 0x1FFFFFFu;
            if (!inRam(pack, 4u + 4u * kEm00Entries) || rd32(rdram, pack) != kEm00Entries)
                return 0u;
            const uint32_t offset = rd32(rdram, pack + 4u + 4u * index);
            return offset ? (pack + offset) & 0x1FFFFFFu : 0u;
        }

        uint32_t player(const uint8_t *rdram) { return rd32(rdram, work::address(work::kPlayerObj)) & 0x1FFFFFFu; }

        void logPair(int punish, uint32_t body, uint32_t enemy, const char *what, float size)
        {
            const int sizeKey = static_cast<int>(std::lround(size * 100.0f));
            if (!g_logged.insert(fitKey(punish, body) ^ (static_cast<uint64_t>(sizeKey & 0xFFFF) << 40)).second)
                return;
            KM_INFO(Game, "punish fit: " << kNames[punish] << " on body " << std::hex << body << std::dec << " ("
                                         << enemyName(static_cast<uint16_t>(rd32(g_ram, enemy + kNumber))) << ", size "
                                         << size << "): " << what);
        }

        // Moves Gene by (side, forward) in his facing without Turn (scaled by the enemy's own size) and turns him.
        void moveGene(uint8_t *rdram, uint32_t gene, const float scale[3], float side, float forward, float turn, float facing)
        {
            const uint32_t pos = rd32(rdram, gene + game::GameObj::kPos);
            if (!inRam(pos, 12u))
                return;
            const float x = side * scale[0], z = forward * scale[2];
            const float c = std::cos(facing), s = std::sin(facing);
            if (x != 0.0f || z != 0.0f)
            {
                wrF(rdram, pos, rdF(rdram, pos) + x * c + z * s);
                wrF(rdram, pos + 8u, rdF(rdram, pos + 8u) - x * s + z * c);
            }
            if (turn != 0.0f)
                wrF(rdram, gene + 0x104u, wrap(rdF(rdram, gene + 0x104u) + turn));
        }

        void writeScale(uint8_t *rdram, uint32_t enemy, const float base[3], float f)
        {
            for (uint32_t i = 0; i < 3; ++i)
                wrF(rdram, enemy + kScaleX + 4u * i, base[i] * f);
        }
        // The size change's enemy still there, its scale still what we last wrote.
        bool resizeOurs(const uint8_t *rdram)
        {
            const Resize &r = g_resize;
            return inRam(r.enemy, 0x700u) && rd32(rdram, r.enemy + kNumber) == r.number && rdF(rdram, r.enemy + kScaleY) == r.written;
        }
        void finishResize(uint8_t *rdram)
        {
            if (g_resize.active && resizeOurs(rdram))
                writeScale(rdram, g_resize.enemy, g_resize.base, 1.0f);
            g_resize.active = false;
        }
        void stepResize()
        {
            Resize &r = g_resize;
            if (!r.active)
                return;
            if (!resizeOurs(g_ram))
            {
                r.active = false;
                return;
            }
            const float target = g_pair.live && g_pair.enemy == r.enemy ? r.target : 1.0f;
            const float step = std::fabs(r.target - 1.0f) / kRampVblanks;
            r.now = r.now < target ? std::min(target, r.now + step) : std::max(target, r.now - step);
            writeScale(g_ram, r.enemy, r.base, r.now);
            r.written = rdF(g_ram, r.enemy + kScaleY);
            if (target == 1.0f && r.now == 1.0f)
                r.active = false;
        }
        // The enemy's own size: its scale, or what it was before our size change.
        void ownScale(const uint8_t *rdram, uint32_t enemy, float out[3])
        {
            for (uint32_t i = 0; i < 3; ++i)
                out[i] = g_resize.active && g_resize.enemy == enemy ? g_resize.base[i] : rdF(rdram, enemy + kScaleX + 4u * i);
        }
        int sizeKeyOf(float s) { return static_cast<int>(std::lround(s * 100.0f)); }

        // The fit for the enemy's punish at its size: the pair's, or blended for a motion started before the pair's
        // placement call; nullptr when there is none (or it doesn't fit).
        const Fit *fitNow(const uint8_t *rdram, uint32_t enemy, int punish, int &sizeKey)
        {
            if (g_pair.fit && g_pair.enemy == enemy && g_pair.punish == punish)
            {
                sizeKey = g_pair.sizeKey;
                return g_pair.fit;
            }
            const Fits *fits = fitsFor(punish, bodyOf(rdram, enemy));
            float scale[3];
            ownScale(rdram, enemy, scale);
            if (!fits || offAt(*fits, scale[1]))
                return nullptr;
            static Fit scratch;
            scratch = blend(*fits, scale[1]);
            sizeKey = sizeKeyOf(scale[1]);
            return &scratch;
        }

        // func_001299F0, wrapped.
        void alignHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const Call *call = nullptr;
            for (const Call &c : kCalls)
                if (c.ra == ra)
                    call = &c;
            const uint32_t gene = GPR_U32(ctx, 4) & 0x1FFFFFFu, enemy = GPR_U32(ctx, 5) & 0x1FFFFFFu;
            if (!call || !inRam(gene, 0x700u) || !inRam(enemy, 0x700u))
            {
                g_align(rdram, ctx, runtime);
                return;
            }
            const uint32_t body = bodyOf(rdram, enemy);
            const bool first = call->geneMotion == kMotions[call->punish].gene[0];
            if (first || g_pair.enemy != enemy || g_pair.punish != call->punish || g_pair.gene != gene)
            {
                // A new pair: the fit at the enemy's own size (a size change still easing out ends first).
                finishResize(rdram);
                ownScale(rdram, enemy, g_pair.baseScale);
                const Fits *fits = fitsFor(call->punish, body);
                const bool off = fits && offAt(*fits, g_pair.baseScale[1]);
                g_pair.fit = nullptr;
                if (fits && !off)
                {
                    g_pair.own = blend(*fits, g_pair.baseScale[1]);
                    g_pair.fit = &g_pair.own;
                }
                g_pair.sizeKey = sizeKeyOf(g_pair.baseScale[1]);
                if (first)
                    logPair(call->punish, body, enemy,
                            !fits ? "no fit (the game's own placement)" : off ? "doesn't fit at this size"
                            : g_pair.own.tracks.empty() ? "unchanged at this size" : "fitted",
                            g_pair.baseScale[1]);
                if (g_pair.fit && g_pair.fit->size != 1.0f)
                {
                    g_resize = Resize{};
                    g_resize.enemy = enemy;
                    g_resize.number = rd32(rdram, enemy + kNumber);
                    std::memcpy(g_resize.base, g_pair.baseScale, sizeof(g_resize.base));
                    g_resize.target = g_pair.fit->size;
                    g_resize.written = rdF(rdram, enemy + kScaleY);
                    g_resize.active = true;
                }
            }
            const Fit *fit = g_pair.fit;
            const Track *track = fit ? fit->find(call->geneMotion, true) : nullptr;
            float v[kValues] = {};
            if (track)
                track->at(0.0f, v);
            const uint32_t mode = GPR_U32(ctx, 7);
            const float turn = v[2] * kDegrees;
            if (mode == 1u)
            {
                if (turn != 0.0f)
                    wrF(rdram, gene + 0x104u, wrap(rdF(rdram, gene + 0x104u) - turn));
            }
            else
                ctx->f[12] += turn;
            // The game's placement with the enemy's own size (not our size change).
            const bool resized = g_resize.active && g_resize.enemy == enemy && resizeOurs(rdram);
            if (resized)
                writeScale(rdram, enemy, g_resize.base, 1.0f);
            g_align(rdram, ctx, runtime);
            if (resized)
                writeScale(rdram, enemy, g_resize.base, g_resize.now);
            const float facing = wrap(rdF(rdram, gene + 0x104u) - (mode == 1u ? 0.0f : turn));
            moveGene(rdram, gene, g_pair.baseScale, v[1], v[0], mode == 1u ? turn : 0.0f, facing);

            g_pair.punish = call->punish;
            g_pair.gene = gene;
            g_pair.enemy = enemy;
            g_pair.body = body;
            g_pair.live = fit != nullptr;
            g_pair.geneTrack = track;
            g_pair.geneMotionPtr = 0u;
            g_pair.applied[0] = v[0];
            g_pair.applied[1] = v[1];
            g_pair.applied[2] = turn;
        }

        // A copy of the motion at `motion` with the track's heights (and the victim's bend), in kRewriteArea; 0 if
        // nothing changes or it does not fit.
        uint32_t rewrite(uint8_t *rdram, uint32_t motion, const Track &track, uint32_t body, int punish, int sizeKey)
        {
            const auto key = std::make_tuple(body, punish, track.motion, track.gene, sizeKey);
            if (const auto it = g_copies.find(key); it != g_copies.end())
                return it->second;
            const uint32_t length = mot::motionLength(rdram + motion, PS2_RAM_SIZE - motion);
            if (length == 0u || length > 0x10000u)
                return 0u;
            std::vector<uint8_t> m(rdram + motion, rdram + motion + length);
            uint16_t frames;
            std::memcpy(&frames, m.data() + 4, 2);
            std::vector<float> height(frames), bend(frames);
            // Gene's pose: the torso (bone 1), upper arms (13 left, 7 right), their helpers (12, 6: half), forearms (14, 8).
            constexpr int kPoseBones[7] = {1, 13, 12, 7, 6, 14, 8};
            std::vector<mot::Mat> pose[7];
            bool anyHeight = false, anyBend = false, anyPose = false;
            for (int f = 0; f < frames; ++f)
            {
                float v[kValues];
                track.at(static_cast<float>(f), v);
                height[f] = track.gene ? v[3] : v[0];
                bend[f] = track.gene ? 0.0f : v[1] * kDegrees;
                anyHeight = anyHeight || height[f] != 0.0f;
                anyBend = anyBend || bend[f] != 0.0f;
                if (!track.gene)
                    continue;
                float a[8];
                for (int i = 0; i < 8; ++i)
                {
                    a[i] = v[4 + i] * kDegrees;
                    anyPose = anyPose || a[i] != 0.0f;
                }
                pose[0].push_back(mot::mul(mot::rotZ(a[1]), mot::rotX(a[0])));
                pose[1].push_back(mot::mul(mot::rotZ(a[2]), mot::rotY(a[3])));
                pose[2].push_back(mot::mul(mot::rotZ(a[2] * 0.5f), mot::rotY(a[3] * 0.5f)));
                pose[3].push_back(mot::mul(mot::rotZ(-a[5]), mot::rotY(-a[6])));
                pose[4].push_back(mot::mul(mot::rotZ(-a[5] * 0.5f), mot::rotY(-a[6] * 0.5f)));
                pose[5].push_back(mot::rotY(a[4]));
                pose[6].push_back(mot::rotY(-a[7]));
            }
            if (!anyHeight && !anyBend && !anyPose)
            {
                g_copies[key] = 0u;
                return 0u;
            }
            if (anyHeight)
                mot::addCurve(m, 0, 1, height, 1);
            if (anyBend)
                mot::addCurve(m, 1, 3, bend, 1);
            if (anyPose)
                for (int i = 0; i < 7; ++i)
                    mot::preRotate(m, kPoseBones[i], pose[i], 1);
            const uint32_t size = (static_cast<uint32_t>(m.size()) + 0x3Fu) & ~0x3Fu;
            if (size > kRewriteAreaEnd - kRewriteArea)
                return 0u;
            if (g_areaAt + size > kRewriteAreaEnd)
            {
                g_copies.clear(); // start over (the motions of an earlier pair are not playing any more)
                g_areaAt = kRewriteArea;
            }
            const uint32_t at = g_areaAt;
            std::memcpy(rdram + at, m.data(), m.size());
            g_areaAt += size;
            g_copies[key] = at;
            return at;
        }

        // func_002A8578 (a0 = the actor, a1 = its motion, a2 = the motion's events): the pair's punish motions start
        // from their rewritten copies.
        void onStartMotion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t actor = GPR_U32(ctx, 4) & 0x1FFFFFFu, motion = GPR_U32(ctx, 5) & 0x1FFFFFFu;
            if (!inRam(actor, 0x700u) || !inRam(motion, 8u))
                return;
            const uint32_t gene = player(rdram);
            if (!inRam(gene, 0x700u))
                return;
            const bool isGene = actor == gene;
            const uint32_t enemy = isGene ? rd32(rdram, gene + kCaught) & 0x1FFFFFFu : actor;
            if (!inRam(enemy, 0x700u) || (!isGene && (rd32(rdram, gene + kCaught) & 0x1FFFFFFu) != actor))
                return;
            for (int p = 0; p < kPunishCount; ++p)
            {
                const uint16_t *list = isGene ? kMotions[p].gene : kMotions[p].victim;
                for (int i = 0; i < 4 && list[i]; ++i)
                {
                    if (entry(rdram, enemy, list[i]) != motion)
                        continue;
                    const uint32_t body = bodyOf(rdram, enemy);
                    int sizeKey = 100;
                    const Fit *fit = fitNow(rdram, enemy, p, sizeKey);
                    const Track *track = fit ? fit->find(list[i], isGene) : nullptr;
                    uint32_t started = motion;
                    if (track)
                        if (const uint32_t copy = rewrite(rdram, motion, *track, body, p, sizeKey))
                        {
                            started = copy;
                            SET_GPR_U32(ctx, 5, copy);
                        }
                    if (isGene && g_pair.live && g_pair.enemy == enemy && g_pair.punish == p)
                    {
                        g_pair.geneTrack = track;
                        g_pair.geneMotionPtr = started;
                    }
                    return;
                }
            }
        }

        // EE thread, each vblank: Gene follows his placement curve while the punish plays.
        void onVblank()
        {
            if (!g_ram)
                return;
            stepResize();
            if (!g_pair.live)
                return;
            const uint32_t gene = player(g_ram);
            if (gene != g_pair.gene || !inRam(gene, 0x700u) || g_ram[gene + game::GameObj::kMode] != 0u ||
                g_ram[gene + game::GameObj::kMode + 1u] != kGenePhase[g_pair.punish] ||
                (rd32(g_ram, gene + kCaught) & 0x1FFFFFFu) != g_pair.enemy)
            {
                g_pair.live = false;
                return;
            }
            if (!g_pair.geneTrack || (rd32(g_ram, gene + kMotionPlaying) & 0x1FFFFFFu) != g_pair.geneMotionPtr)
                return;
            float v[kValues];
            g_pair.geneTrack->at(rdF(g_ram, gene + kMotionFrame), v);
            const float want[3] = {v[0], v[1], v[2] * kDegrees};
            if (want[0] == g_pair.applied[0] && want[1] == g_pair.applied[1] && want[2] == g_pair.applied[2])
                return;
            const float facing = wrap(rdF(g_ram, gene + 0x104u) - g_pair.applied[2]);
            moveGene(g_ram, gene, g_pair.baseScale, want[1] - g_pair.applied[1], want[0] - g_pair.applied[0],
                     want[2] - g_pair.applied[2], facing);
            std::memcpy(g_pair.applied, want, sizeof(want));
        }

        // punish_fit.txt:
        //   fit <Suplex|Stinger|CobraTwist|Spanking|GorillaSuplex> <body hex>[,<body hex>...] [off] [gorilla] [scale=<s>]
        //       [size=<s>]   (the lines after it, until the next fit, are its; gorilla on a Suplex fit: that body plays
        //       the Gorilla Suplex for a Suplex pick; scale: the enemy size it is for, 1 if not given)
        //   gene <motion> <frame>:<forward>,<side>,<turn>,<height>[,<torso x>,<torso z>,<left raise>,<left swing>,
        //       <left elbow>,<right raise>,<right swing>,<right elbow>] ...
        //   victim <motion> <frame>:<height>,<bend> ...
        void load()
        {
            std::error_code error;
            const fs::path &file = fs::exists(g_file, error) ? g_file : g_shippedFile;
            std::ifstream in(file);
            std::string line;
            Fit *fit = nullptr;
            size_t count = 0;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream fields(line);
                std::string kind;
                fields >> kind;
                if (kind == "fit")
                {
                    std::string name, bodies, token;
                    fields >> name >> bodies;
                    int punish = -1;
                    for (int i = 0; i < kPunishCount; ++i)
                        if (name == kFileNames[i])
                            punish = i;
                    auto made = std::make_shared<Fit>();
                    while (fields >> token && token[0] != '#')
                    {
                        if (token == "off")
                            made->off = true;
                        else if (token == "gorilla")
                            made->gorilla = true;
                        else if (token.rfind("scale=", 0) == 0)
                            made->scale = std::strtof(token.c_str() + 6, nullptr);
                        else if (token.rfind("size=", 0) == 0)
                            made->size = std::clamp(std::strtof(token.c_str() + 5, nullptr), 0.9f, 1.1f);
                    }
                    fit = nullptr;
                    std::istringstream keys(bodies);
                    std::string body;
                    while (punish >= 0 && std::getline(keys, body, ','))
                        if (const uint32_t key = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16)))
                        {
                            g_fits[fitKey(punish, key)].push_back(made);
                            fit = made.get();
                        }
                    count += fit != nullptr;
                }
                else if (fit && (kind == "gene" || kind == "victim"))
                {
                    Track track;
                    track.gene = kind == "gene";
                    unsigned motion = 0;
                    fields >> motion;
                    track.motion = static_cast<uint16_t>(motion);
                    std::string item;
                    while (fields >> item)
                    {
                        Key k;
                        const size_t colon = item.find(':');
                        if (colon == std::string::npos)
                            continue;
                        k.frame = std::strtof(item.c_str(), nullptr);
                        const char *at = item.c_str() + colon + 1;
                        for (int c = 0; c < kValues && *at; ++c)
                        {
                            char *end = nullptr;
                            k.v[c] = std::strtof(at, &end);
                            at = *end == ',' ? end + 1 : end;
                        }
                        track.keys.push_back(k);
                    }
                    if (!track.keys.empty())
                        fit->tracks.push_back(std::move(track));
                }
            }
            for (auto &[key, fits] : g_fits)
                std::stable_sort(fits.begin(), fits.end(), [](const auto &a, const auto &b) { return a->scale < b->scale; });
            KM_INFO(Game, "punish fit: " << count << " fits for " << g_fits.size() << " bodies and punishes from "
                                         << file.filename().string()
                                         << (file == g_shippedFile ? " (shipped)" : " (yours, next to the exe)"));
        }

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v && *v != '0';
        }
    }

    bool doesntFit(const uint8_t *rdram, uint32_t enemy, int punish)
    {
        if (!g_on || punish < 0 || punish >= kPunishCount || !inRam(enemy, 0x700u))
            return false;
        const Fits *fits = fitsFor(punish, bodyOf(rdram, enemy & 0x1FFFFFFu));
        if (!fits)
            return false;
        float scale[3];
        ownScale(rdram, enemy & 0x1FFFFFFu, scale);
        return offAt(*fits, scale[1]);
    }

    bool suplexAsGorilla(const uint8_t *rdram, uint32_t enemy)
    {
        if (!g_on || !inRam(enemy, 0x700u))
            return false;
        const Fit *fit = fitFor(Suplex, bodyOf(rdram, enemy & 0x1FFFFFFu));
        return fit && fit->gorilla;
    }

    void install(PS2Runtime &runtime, const fs::path &exeDir)
    {
        g_file = exeDir / "punish_fit.txt";
        g_shippedFile = exeDir / "resources" / "punish_fit.txt";
        g_on = envOn("KAMITE_BUCK_BREAKER");
        if (!g_on)
            return;
        g_ram = runtime.memory().getRDRAM();
        if (!g_ram || !runtime.hasFunction(kAlign) || !(g_align = runtime.lookupFunction(kAlign)) ||
            !FunctionHooks::instance().replace(kAlign, "punish placement (Buck Breaker's punish fit)", &alignHook,
                                               ReplacementStatus::Native, "[Game] BuckBreaker: per-body punish fits"))
        {
            g_on = false;
            KM_WARN(Game, "punish fit: func_001299F0 is not hookable; no punish fits");
            return;
        }
        if (!FunctionHooks::instance().observeEntry(kStartMotion, "punish fit: motion start", &onStartMotion))
            KM_WARN(Game, "punish fit: func_002A8578 is not hookable; placement only, no heights or bend");
        load();
        ps2x::pnach::addFrameHook(&onVblank);
    }
}
