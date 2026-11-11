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
// (body, punish, motion) and reused; the area starts over when full, the list after a save state load (saveState).
//
// Gene's pose (K, Nov 8: his arms through the victim where the original keeps clear; Cobra Twist's arms round the
// neck on women and fat men; Gorilla Suplex's arms folded too far in on the fat men): Gene's tracks also carry his torso
// (bone 1 turned in bone 0's space: x, z) and each arm (the upper arm, 13 / 7, turned in the clavicle's space: raise
// about z, swing about y; the forearm, 14 / 8, about y in the upper arm's), put in front of the motion's own rotation
// every frame (mot_edit preRotate); the shoulder helpers 12 / 6, which follow the upper arms at about half in the game's
// motions, get half. Offline the same pre-rotations are measured (fit5.py gene_xform).
//
// Size (K, Nov 8). Enemies of one model come in many sizes (stage 1: 1.03, 1.05, 1.07 ... 1.10), so a pair is not
// fitted per size: it is scaled. Every actor's model, root travel (cObjBase_addNullSpeed: the motion's delta x +0x110 /
// +0x114 / +0x118) and the pair's placement (x enemy +0x110 / +0x118) scale with it, so Gene and the enemy scaled by
// one factor are the fitted pair, uniformly bigger or smaller. Each fit is for an enemy size (scale=: 1.0, 1.3); a
// body whose fits are all larger also has the game's own pairing at 1.0. At the catch the fit whose size needs the
// least change is taken (sizes marked off are not), and Gene and the enemy meet halfway: both by k = sqrt(fit size /
// the enemy's size), Gene up, the enemy down (or the reverse), easing in over kRampVblanks and back after the punish.
// Each changes by at most 12% (K, Nov 8: 6% at first; 12% lets Gene spank the stage 1 demon, size 1.20): past that a
// Buck Breaker pick doesn't fit (the enemy keeps its own punish), and the enemy's own punish plays with k held there. The game's placement calls run with the enemy at the fit's size x k.
// size= is the fit's own victim size change (0.95..1.05), on top. Bodies with no fits (other skeletons) are untouched.
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
#include "runtime/ps2_save_state.h"

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
#include <iterator>
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
        constexpr int kRampVblanks = 12; // the size changes ease in (and out) over this many vblanks (K: quick)
        constexpr float kMaxStep = 1.12f; // each of the two changes size by at most this factor (K: 12%, for the stage 1 demon)
        constexpr int kValues = 13;
        constexpr uint32_t kParts = 0x144u, kPartCount = 0x2B4u, kPartOffset = 0xE0u, kPartParent = 0x148u;
        constexpr uint32_t kMotionPlaying = 0x428u, kMotionFrame = 0x430u;

        // A curve: keys (frame, values). Gene: forward (m), side (m), turn (degrees), height (m), torso x, torso z,
        // left raise, left swing, left elbow, right raise, right swing, right elbow, left forearm twist (degrees);
        // victim: height (m), bend (degrees).
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

        // The fit a pair plays at enemy size s (see the top): fit nullptr = the game's own pairing at 1.0.
        struct Anchor
        {
            bool any = false;    // a fit (or the game's own) to scale to
            bool within = false; // reached with each of the two changing by at most kMaxStep
            const Fit *fit = nullptr;
            float scale = 1.0f; // the fit's enemy size
            float k = 1.0f;     // Gene's factor; the enemy's: scale x k / s
        };
        Anchor anchorFor(const Fits &fits, float s)
        {
            Anchor best;
            float bestD = 1e9f;
            const auto consider = [&](const Fit *f, float a) {
                const float d = std::fabs(std::log(s / a));
                if (d < bestD)
                {
                    bestD = d;
                    best.any = true;
                    best.fit = f;
                    best.scale = a;
                }
            };
            if (fits.front()->scale > 1.0f)
                consider(nullptr, 1.0f);
            for (const auto &f : fits)
                if (!f->off)
                    consider(f.get(), f->scale);
            if (best.any)
            {
                best.within = bestD <= 2.0f * std::log(kMaxStep) + 1e-4f;
                best.k = std::clamp(std::sqrt(s / best.scale), 1.0f / kMaxStep, kMaxStep);
            }
            return best;
        }

        // The pair playing (EE thread only).
        struct Pair
        {
            int punish = -1;
            uint32_t gene = 0, enemy = 0, body = 0;
            const Fit *fit = nullptr; // the fit played (nullptr: the game's own pairing, scaled, or no fit)
            int sizeKey = 100;        // the fit's size x 100 (the copies are per fit)
            float baseScale[3] = {1.0f, 1.0f, 1.0f};  // the enemy's own
            float placeScale[3] = {1.0f, 1.0f, 1.0f}; // the enemy at the fit's size x k: what placement scales by
            bool live = false;
            uint32_t geneMotionPtr = 0; // the pointer Gene's current punish motion was started with (ours or the game's)
            const Track *geneTrack = nullptr;
            float applied[3] = {}; // forward, side, turn (radians) put in so far
        };

        // An actor's size change (Gene's, the victim's), easing in while the pair plays and out after it (EE thread only).
        struct Resize
        {
            uint32_t actor = 0, number = 0;
            float base[3] = {1.0f, 1.0f, 1.0f};
            float now = 1.0f, target = 1.0f, written = 0.0f;
            bool active = false;
        };

        uint8_t *g_ram = nullptr;
        bool g_on = false;
        PS2Runtime::RecompiledFunction g_align = nullptr;
        fs::path g_file, g_shippedFile;
        std::map<uint64_t, Fits> g_fits; // punish << 32 | body, by size; read at start, then only read
        std::map<uint32_t, std::string> g_bodyNames; // body -> its type ("men", ...), punish_fit.txt's body lines
        Pair g_pair;
        Resize g_resize[2]; // the victim, Gene
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

        // The enemy's model file: +0x2FE's low byte (func_002CFE68 builds "em%02x" from it; the kind +0x564 does not fix
        // the model).
        constexpr uint32_t kModelId = 0x2FEu;
        std::string enemyName(const uint8_t *rdram, uint32_t enemy)
        {
            uint16_t id;
            std::memcpy(&id, rdram + ((enemy + kModelId) & (PS2_RAM_SIZE - 1u)), sizeof(id));
            char text[8];
            std::snprintf(text, sizeof(text), "em%02x", id & 0xFFu);
            return text;
        }

        // The enemy's skeleton hashed (FNV-1a): part count, then each part after the root: its parent's index and its
        // offset's bits (tools/models/punish_fit/common.py runtime_key computes the same from the file). 0 when it
        // cannot be read.
        // The enemy's parts (root first); their count, 0 if they can't be read.
        int readParts(const uint8_t *rdram, uint32_t enemy, uint32_t parts[64])
        {
            const int n = rdram[(enemy + kPartCount) & (PS2_RAM_SIZE - 1u)];
            if (n < 2 || n > 64)
                return 0;
            uint32_t p = rd32(rdram, enemy + kParts);
            for (int i = 0; i < n; ++i)
            {
                if (!inRam(p, 0x150u))
                    return 0;
                parts[i] = p & 0x1FFFFFFu;
                p = rd32(rdram, p + kParts);
            }
            return n;
        }

        // The hash of the first n parts (common.runtime_key offline).
        uint32_t hashParts(const uint8_t *rdram, const uint32_t *parts, int n)
        {
            uint32_t h = 2166136261u;
            const auto mix = [&h](const void *data, size_t size) {
                const auto *b = static_cast<const uint8_t *>(data);
                for (size_t i = 0; i < size; ++i)
                    h = (h ^ b[i]) * 16777619u;
            };
            mix(&n, sizeof(n));
            for (int i = 1; i < n; ++i)
            {
                int8_t index = -1;
                const uint32_t parent = rd32(rdram, parts[i] + kPartParent) & 0x1FFFFFFu;
                for (int j = 0; j < n; ++j)
                    if (parts[j] == parent && j != i)
                        index = static_cast<int8_t>(j);
                mix(&index, 1);
                mix(rdram + parts[i] + kPartOffset, 12);
            }
            return h ? h : 1u;
        }

        // The body. A skeleton in no body line whose body bones (parts 0-25) match a known body's plays that body's
        // fits: the head's extra bones (26..) vary between identical bodies (punish_fit.txt's core lines). A body
        // still unknown is logged once with its parts (parent: offset), so it can be added offline.
        constexpr int kCoreParts = 26;
        std::map<uint32_t, uint32_t> g_core; // hash of parts 0-25 -> a body with fits
        std::set<uint32_t> g_seenBodies;
        uint32_t bodyOf(const uint8_t *rdram, uint32_t enemy)
        {
            uint32_t parts[64];
            const int n = readParts(rdram, enemy, parts);
            if (!n)
                return 0x80000000u | (rd32(rdram, enemy + kNumber) & 0xFFFFu);
            const uint32_t body = hashParts(rdram, parts, n);
            if (g_bodyNames.empty() || g_bodyNames.count(body))
                return body;
            const uint32_t core = n >= kCoreParts ? hashParts(rdram, parts, kCoreParts) : 0u;
            const auto it = g_core.find(core);
            const uint32_t alias = it != g_core.end() ? it->second : 0u;
            if (g_seenBodies.insert(body).second)
            {
                std::ostringstream text;
                text << "punish fit: body " << std::hex << body << " (" << enemyName(rdram, enemy) << ", " << std::dec << n
                     << " parts, core " << std::hex << core << std::dec << ") ";
                if (alias)
                    text << "plays body " << std::hex << alias << std::dec << "'s fits";
                else
                {
                    text << "unknown:";
                    for (int i = 1; i < n; ++i)
                    {
                        const uint32_t parent = rd32(rdram, parts[i] + kPartParent) & 0x1FFFFFFu;
                        int index = -1;
                        for (int j = 0; j < n; ++j)
                            if (parts[j] == parent && j != i)
                                index = j;
                        float t[3];
                        std::memcpy(t, rdram + parts[i] + kPartOffset, sizeof(t));
                        char item[64];
                        std::snprintf(item, sizeof(item), " %d:%d:%.4f,%.4f,%.4f", i, index, t[0], t[1], t[2]);
                        text << item;
                    }
                }
                KM_INFO(Game, text.str());
            }
            return alias ? alias : body;
        }

        const Fits *fitsFor(int punish, uint32_t body)
        {
            const auto it = g_fits.find(fitKey(punish, body));
            return it == g_fits.end() || it->second.empty() ? nullptr : &it->second;
        }

        // Enemies whose prompt offers a punish Buck Breaker changed (setChanged; the last few enemies seen).
        struct Changed
        {
            uint32_t enemy = 0u;
            bool changed = false;
        };
        Changed g_changed[16];
        size_t g_changedNext = 0u;
        bool changedFor(uint32_t enemy)
        {
            enemy &= 0x1FFFFFFu;
            for (const Changed &c : g_changed)
                if (c.enemy == enemy)
                    return c.changed;
            return false;
        }
        // The fits for a pair Buck Breaker made; nullptr for one the game already had (it plays as the game made it).
        const Fits *fitsForPair(const uint8_t *, uint32_t enemy, int punish, uint32_t body)
        {
            return changedFor(enemy) ? fitsFor(punish, body) : nullptr;
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
                                         << enemyName(g_ram, enemy) << ", size "
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

        void writeScale(uint8_t *rdram, uint32_t actor, const float base[3], float f)
        {
            for (uint32_t i = 0; i < 3; ++i)
                wrF(rdram, actor + kScaleX + 4u * i, base[i] * f);
        }
        // The size change's actor still there, its scale still what we last wrote.
        bool resizeOurs(const uint8_t *rdram, const Resize &r)
        {
            return inRam(r.actor, 0x700u) && rd32(rdram, r.actor + kNumber) == r.number && rdF(rdram, r.actor + kScaleY) == r.written;
        }
        void finishResizes(uint8_t *rdram)
        {
            for (Resize &r : g_resize)
            {
                if (r.active && resizeOurs(rdram, r))
                    writeScale(rdram, r.actor, r.base, 1.0f);
                r.active = false;
            }
        }
        void startResize(uint8_t *rdram, Resize &r, uint32_t actor, const float base[3], float target)
        {
            r = Resize{};
            if (target == 1.0f)
                return;
            r.actor = actor;
            r.number = rd32(rdram, actor + kNumber);
            std::memcpy(r.base, base, sizeof(r.base));
            r.target = target;
            r.written = rdF(rdram, actor + kScaleY);
            r.active = true;
        }
        void stepResizes()
        {
            for (Resize &r : g_resize)
            {
                if (!r.active)
                    continue;
                if (!resizeOurs(g_ram, r))
                {
                    r.active = false;
                    continue;
                }
                const float target = g_pair.live && (r.actor == g_pair.enemy || r.actor == g_pair.gene) ? r.target : 1.0f;
                const float step = std::fabs(r.target - 1.0f) / kRampVblanks;
                r.now = r.now < target ? std::min(target, r.now + step) : std::max(target, r.now - step);
                writeScale(g_ram, r.actor, r.base, r.now);
                r.written = rdF(g_ram, r.actor + kScaleY);
                if (target == 1.0f && r.now == 1.0f)
                    r.active = false;
            }
        }
        // The actor's own size: its scale, or what it was before our size change.
        void ownScale(const uint8_t *rdram, uint32_t actor, float out[3])
        {
            const Resize *mine = nullptr;
            for (const Resize &r : g_resize)
                if (r.active && r.actor == actor)
                    mine = &r;
            for (uint32_t i = 0; i < 3; ++i)
                out[i] = mine ? mine->base[i] : rdF(rdram, actor + kScaleX + 4u * i);
        }
        int sizeKeyOf(float s) { return static_cast<int>(std::lround(s * 100.0f)); }

        // The fit for the enemy's punish at its size: the pair's, or its anchor's for a motion started before the pair's
        // placement call; nullptr when there is none (or it doesn't fit).
        const Fit *fitNow(const uint8_t *rdram, uint32_t enemy, int punish, int &sizeKey)
        {
            if (g_pair.enemy == enemy && g_pair.punish == punish && g_pair.live)
            {
                sizeKey = g_pair.sizeKey;
                return g_pair.fit;
            }
            const Fits *fits = fitsForPair(rdram, enemy, punish, bodyOf(rdram, enemy));
            if (!fits)
                return nullptr;
            float scale[3];
            ownScale(rdram, enemy, scale);
            const Anchor an = anchorFor(*fits, scale[1]);
            if (!an.any)
                return nullptr;
            sizeKey = sizeKeyOf(an.scale);
            return an.fit;
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
                // A new pair: the fit whose size needs the least change, Gene and the enemy scaled to meet it (any size
                // change still easing out ends first).
                finishResizes(rdram);
                ownScale(rdram, enemy, g_pair.baseScale);
                std::memcpy(g_pair.placeScale, g_pair.baseScale, sizeof(g_pair.placeScale));
                const Fits *fits = fitsForPair(rdram, enemy, call->punish, body);
                const Anchor an = fits ? anchorFor(*fits, g_pair.baseScale[1]) : Anchor{};
                g_pair.fit = an.fit;
                g_pair.sizeKey = sizeKeyOf(an.scale);
                g_pair.live = an.any;
                if (an.any)
                {
                    const float e = an.scale * an.k / g_pair.baseScale[1]; // the enemy's factor (before the fit's size=)
                    for (int i = 0; i < 3; ++i)
                        g_pair.placeScale[i] = g_pair.baseScale[i] * e;
                    startResize(rdram, g_resize[0], enemy, g_pair.baseScale, e * (an.fit ? an.fit->size : 1.0f));
                    float geneBase[3];
                    ownScale(rdram, gene, geneBase);
                    startResize(rdram, g_resize[1], gene, geneBase, an.k);
                }
                if (first)
                {
                    char what[96];
                    if (!an.any)
                        std::snprintf(what, sizeof(what), "%s", fits ? "doesn't fit" : "no fit (the game's own placement)");
                    else
                        std::snprintf(what, sizeof(what), "%s at %.2f%s, Gene x%.3f, enemy x%.3f",
                                      an.fit ? "fitted" : "the game's own pairing", an.scale,
                                      an.within ? "" : " (past the cap: held there)", an.k,
                                      an.scale * an.k / g_pair.baseScale[1] * (an.fit ? an.fit->size : 1.0f));
                    logPair(call->punish, body, enemy, what, g_pair.baseScale[1]);
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
            // The game's placement with the enemy at the fit's size x k (not its own, nor mid-ease).
            float held[3];
            for (uint32_t i = 0; i < 3; ++i)
                held[i] = rdF(rdram, enemy + kScaleX + 4u * i);
            const float one = 1.0f;
            if (g_pair.live)
                writeScale(rdram, enemy, g_pair.placeScale, one);
            g_align(rdram, ctx, runtime);
            if (g_pair.live)
                writeScale(rdram, enemy, held, one);
            const float facing = wrap(rdF(rdram, gene + 0x104u) - (mode == 1u ? 0.0f : turn));
            moveGene(rdram, gene, g_pair.placeScale, v[1], v[0], mode == 1u ? turn : 0.0f, facing);

            g_pair.punish = call->punish;
            g_pair.gene = gene;
            g_pair.enemy = enemy;
            g_pair.body = body;
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
            std::vector<mot::Mat> twist; // the left forearm about its length: half on bone 15, half on the hand (16)
            bool anyHeight = false, anyBend = false, anyPose = false, anyTwist = false;
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
                twist.push_back(mot::rotX(v[12] * kDegrees * 0.5f));
                anyTwist = anyTwist || v[12] != 0.0f;
            }
            if (!anyHeight && !anyBend && !anyPose && !anyTwist)
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
            if (anyTwist)
                for (int bone : {15, 16})
                    mot::preRotate(m, bone, {}, 1, twist);
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

        // Save states (K, Nov 8: a state loaded after playing other punishes played wrong motions in wrong places). The
        // copies live in guest RAM, which a load puts back as it was when the state was saved, so the copy list made
        // since pointed at whatever the state holds there. A state keeps the area's fill point and the pair in play (a
        // state saved mid-punish goes on with it); after every load the list starts empty and each copy is made again
        // past the state's own. A state without them (older builds): the area from its start, no pair.
        struct SavedPair
        {
            int32_t punish = -1, fitIndex = -1, sizeKey = 100;
            uint32_t gene = 0, enemy = 0, body = 0, geneMotionPtr = 0;
            float baseScale[3] = {}, placeScale[3] = {}, applied[3] = {};
            uint16_t trackMotion = 0;
            uint8_t live = 0, hasTrack = 0;
        };
        bool g_stateLoaded = false; // the state carried ours (until the load's end)

        void saveState(ps2x::state::Writer &out)
        {
            uint32_t version = 1u, areaAt = g_areaAt;
            SavedPair s;
            s.punish = g_pair.punish;
            s.gene = g_pair.gene;
            s.enemy = g_pair.enemy;
            s.body = g_pair.body;
            s.sizeKey = g_pair.sizeKey;
            s.geneMotionPtr = g_pair.geneMotionPtr;
            std::memcpy(s.baseScale, g_pair.baseScale, sizeof(s.baseScale));
            std::memcpy(s.placeScale, g_pair.placeScale, sizeof(s.placeScale));
            std::memcpy(s.applied, g_pair.applied, sizeof(s.applied));
            s.live = g_pair.live ? 1u : 0u;
            if (const Fits *fits = g_pair.punish >= 0 ? fitsFor(g_pair.punish, g_pair.body) : nullptr)
                for (size_t i = 0; i < fits->size(); ++i)
                    if ((*fits)[i].get() == g_pair.fit)
                        s.fitIndex = static_cast<int32_t>(i);
            if (g_pair.geneTrack)
            {
                s.hasTrack = 1u;
                s.trackMotion = g_pair.geneTrack->motion;
            }
            ps2x::state::fields(out, version, areaAt, s, g_resize[0], g_resize[1]);
        }

        void loadState(ps2x::state::Reader &in)
        {
            uint32_t version = 0u, areaAt = kRewriteArea;
            SavedPair s;
            Resize resize[2];
            ps2x::state::fields(in, version, areaAt, s, resize[0], resize[1]);
            g_copies.clear();
            g_areaAt = areaAt >= kRewriteArea && areaAt <= kRewriteAreaEnd ? areaAt : kRewriteArea;
            g_pair = Pair{};
            g_pair.punish = s.punish;
            g_pair.gene = s.gene;
            g_pair.enemy = s.enemy;
            g_pair.body = s.body;
            g_pair.sizeKey = s.sizeKey;
            g_pair.geneMotionPtr = s.geneMotionPtr;
            std::memcpy(g_pair.baseScale, s.baseScale, sizeof(s.baseScale));
            std::memcpy(g_pair.placeScale, s.placeScale, sizeof(s.placeScale));
            std::memcpy(g_pair.applied, s.applied, sizeof(s.applied));
            const Fits *fits = s.punish >= 0 && s.punish < kPunishCount ? fitsFor(s.punish, s.body) : nullptr;
            const bool fitOk = s.fitIndex < 0 || (fits && static_cast<size_t>(s.fitIndex) < fits->size());
            if (fits && s.fitIndex >= 0 && fitOk)
                g_pair.fit = (*fits)[static_cast<size_t>(s.fitIndex)].get();
            if (g_pair.fit && s.hasTrack)
                g_pair.geneTrack = g_pair.fit->find(s.trackMotion, true);
            g_pair.live = s.live && fitOk && s.punish >= 0 && s.punish < kPunishCount;
            g_resize[0] = resize[0];
            g_resize[1] = resize[1];
            g_stateLoaded = true;
        }

        void afterLoad()
        {
            if (!g_stateLoaded)
            {
                g_copies.clear();
                g_areaAt = kRewriteArea;
                g_pair = Pair{};
                g_resize[0] = Resize{};
                g_resize[1] = Resize{};
            }
            g_stateLoaded = false;
        }

        // EE thread, each vblank: Gene follows his placement curve while the punish plays.
        void onVblank()
        {
            if (!g_ram)
                return;
            stepResizes();
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
            moveGene(g_ram, gene, g_pair.placeScale, want[1] - g_pair.applied[1], want[0] - g_pair.applied[0],
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
        //   body <body hex>[,<body hex>...] <type>   (the enemy labels' body type)
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
                else if (kind == "body")
                {
                    std::string bodies, name;
                    fields >> bodies >> name;
                    std::istringstream keys(bodies);
                    std::string body;
                    while (std::getline(keys, body, ','))
                        if (const uint32_t key = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16)))
                            g_bodyNames[key] = name;
                }
                else if (kind == "core")
                {
                    std::string core, body;
                    fields >> core >> body;
                    const auto key = static_cast<uint32_t>(std::strtoul(core.c_str(), nullptr, 16));
                    const auto to = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16));
                    if (key && to)
                        g_core[key] = to;
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

    void setChanged(uint32_t enemy, bool changed)
    {
        enemy &= 0x1FFFFFFu;
        for (Changed &c : g_changed)
            if (c.enemy == enemy)
            {
                c.changed = changed;
                return;
            }
        g_changed[g_changedNext] = Changed{enemy, changed};
        g_changedNext = (g_changedNext + 1u) % std::size(g_changed);
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
        return !anchorFor(*fits, scale[1]).within;
    }

    bool suplexAsGorilla(const uint8_t *rdram, uint32_t enemy)
    {
        if (!g_on || !inRam(enemy, 0x700u))
            return false;
        const Fit *fit = fitFor(Suplex, bodyOf(rdram, enemy & 0x1FFFFFFu));
        return fit && fit->gorilla;
    }

    void describe(const uint8_t *rdram, uint32_t enemy, std::string &label, std::string &status)
    {
        label.clear();
        status.clear();
        enemy &= 0x1FFFFFFu;
        if (!inRam(enemy, 0x700u))
            return;
        float scale[3];
        ownScale(rdram, enemy, scale);
        const uint32_t body = bodyOf(rdram, enemy);
        const auto name = g_bodyNames.find(body);
        char text[64];
        std::snprintf(text, sizeof(text), "%s | %s | %.2f", enemyName(rdram, enemy).c_str(),
                      name != g_bodyNames.end() ? name->second.c_str() : "?", scale[1]);
        label = text;
        // In Gene's punish: which one, and what the fit does.
        const uint32_t gene = player(rdram);
        if (!inRam(gene, 0x700u) || (rd32(rdram, gene + kCaught) & 0x1FFFFFFu) != enemy || rdram[gene + game::GameObj::kMode] != 0u)
            return;
        const uint8_t phase = rdram[gene + game::GameObj::kMode + 1u];
        for (int p = 0; p < kPunishCount; ++p)
            if (phase == kGenePhase[p])
            {
                const Fits *fits = g_on ? fitsForPair(rdram, enemy, p, body) : nullptr;
                const Anchor an = fits ? anchorFor(*fits, scale[1]) : Anchor{};
                char what[96];
                if (!an.any)
                    std::snprintf(what, sizeof(what), "%s", fits ? "doesn't fit" : "no fit (the game's placement)");
                else
                    std::snprintf(what, sizeof(what), "%s %.2f, pair x%.3f%s", an.fit ? "fit" : "game's own at", an.scale,
                                  an.k, an.within ? "" : " (held at the cap)");
                status = std::string(kNames[p]) + ": " + what;
                return;
            }
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
        ps2x::savestate::registerExtension("kamite.punish_fit", &saveState, &loadState);
        ps2x::savestate::onLoaded(&afterLoad);
    }
}
