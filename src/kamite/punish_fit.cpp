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
#include <map>
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

        constexpr uint32_t kNumber = 0x564u, kScaleX = 0x110u, kScaleZ = 0x118u, kCaught = 0x694u;
        constexpr uint32_t kParts = 0x144u, kPartCount = 0x2B4u, kPartOffset = 0xE0u, kPartParent = 0x148u;
        constexpr uint32_t kMotionPlaying = 0x428u, kMotionFrame = 0x430u;

        // A curve: keys (frame, values). Gene: forward (m), side (m), turn (degrees), height (m); victim: height (m),
        // bend (degrees).
        struct Key
        {
            float frame = 0.0f;
            float v[4] = {};
        };
        struct Track
        {
            uint16_t motion = 0;
            bool gene = false;
            std::vector<Key> keys;
            void at(float frame, float out[4]) const
            {
                for (int i = 0; i < 4; ++i)
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
                for (int c = 0; c < 4; ++c)
                    out[c] = a.v[c] + (b.v[c] - a.v[c]) * t;
            }
        };
        struct Fit
        {
            bool off = false;
            bool gorilla = false; // a Suplex fit: the Suplex pick plays the Gorilla Suplex on this body
            std::vector<Track> tracks;
            const Track *find(uint16_t motion, bool gene) const
            {
                for (const Track &t : tracks)
                    if (t.motion == motion && t.gene == gene)
                        return &t;
                return nullptr;
            }
        };

        // The pair playing (EE thread only).
        struct Pair
        {
            int punish = -1;
            uint32_t gene = 0, enemy = 0, body = 0;
            const Fit *fit = nullptr;
            bool live = false;
            uint32_t geneMotionPtr = 0; // the pointer Gene's current punish motion was started with (ours or the game's)
            const Track *geneTrack = nullptr;
            float applied[3] = {}; // forward, side, turn (radians) put in so far
        };

        uint8_t *g_ram = nullptr;
        bool g_on = false;
        PS2Runtime::RecompiledFunction g_align = nullptr;
        fs::path g_file, g_shippedFile;
        std::map<uint64_t, Fit> g_fits; // punish << 32 | body; read at start, then only read
        Pair g_pair;
        std::map<std::tuple<uint32_t, int, uint16_t, bool>, uint32_t> g_copies; // (body, punish, motion, gene) -> copy
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

        const Fit *fitFor(int punish, uint32_t body)
        {
            const auto it = g_fits.find(fitKey(punish, body));
            return it == g_fits.end() ? nullptr : &it->second;
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

        void logPair(int punish, uint32_t body, uint32_t enemy, const Fit *fit)
        {
            if (!g_logged.insert(fitKey(punish, body)).second)
                return;
            KM_INFO(Game, "punish fit: " << kNames[punish] << " on body " << std::hex << body << std::dec << " ("
                                         << enemyName(static_cast<uint16_t>(rd32(g_ram, enemy + kNumber))) << "): "
                                         << (!fit ? "no fit (the game's own placement)" : fit->off ? "doesn't fit" : "fitted"));
        }

        // Moves Gene by (side, forward) in his facing without Turn (scaled by the enemy's size) and turns him.
        void moveGene(uint8_t *rdram, uint32_t gene, uint32_t enemy, float side, float forward, float turn, float facing)
        {
            const uint32_t pos = rd32(rdram, gene + game::GameObj::kPos);
            if (!inRam(pos, 12u))
                return;
            const float x = side * rdF(rdram, enemy + kScaleX), z = forward * rdF(rdram, enemy + kScaleZ);
            const float c = std::cos(facing), s = std::sin(facing);
            if (x != 0.0f || z != 0.0f)
            {
                wrF(rdram, pos, rdF(rdram, pos) + x * c + z * s);
                wrF(rdram, pos + 8u, rdF(rdram, pos + 8u) - x * s + z * c);
            }
            if (turn != 0.0f)
                wrF(rdram, gene + 0x104u, wrap(rdF(rdram, gene + 0x104u) + turn));
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
            const Fit *fit = fitFor(call->punish, body);
            const Track *track = fit && !fit->off ? fit->find(call->geneMotion, true) : nullptr;
            float v[4] = {};
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
            g_align(rdram, ctx, runtime);
            const float facing = wrap(rdF(rdram, gene + 0x104u) - (mode == 1u ? 0.0f : turn));
            moveGene(rdram, gene, enemy, v[1], v[0], mode == 1u ? turn : 0.0f, facing);

            if (call->geneMotion == kMotions[call->punish].gene[0])
                logPair(call->punish, body, enemy, fit);
            g_pair.punish = call->punish;
            g_pair.gene = gene;
            g_pair.enemy = enemy;
            g_pair.body = body;
            g_pair.fit = fit && !fit->off ? fit : nullptr;
            g_pair.live = g_pair.fit != nullptr;
            g_pair.geneTrack = track;
            g_pair.geneMotionPtr = 0u;
            g_pair.applied[0] = v[0];
            g_pair.applied[1] = v[1];
            g_pair.applied[2] = turn;
        }

        // A copy of the motion at `motion` with the track's heights (and the victim's bend), in kRewriteArea; 0 if
        // nothing changes or it does not fit.
        uint32_t rewrite(uint8_t *rdram, uint32_t motion, const Track &track, uint32_t body, int punish)
        {
            const auto key = std::make_tuple(body, punish, track.motion, track.gene);
            if (const auto it = g_copies.find(key); it != g_copies.end())
                return it->second;
            const uint32_t length = mot::motionLength(rdram + motion, PS2_RAM_SIZE - motion);
            if (length == 0u || length > 0x10000u)
                return 0u;
            std::vector<uint8_t> m(rdram + motion, rdram + motion + length);
            uint16_t frames;
            std::memcpy(&frames, m.data() + 4, 2);
            std::vector<float> height(frames), bend(frames);
            bool anyHeight = false, anyBend = false;
            for (int f = 0; f < frames; ++f)
            {
                float v[4];
                track.at(static_cast<float>(f), v);
                height[f] = track.gene ? v[3] : v[0];
                bend[f] = track.gene ? 0.0f : v[1] * kDegrees;
                anyHeight = anyHeight || height[f] != 0.0f;
                anyBend = anyBend || bend[f] != 0.0f;
            }
            if (!anyHeight && !anyBend)
            {
                g_copies[key] = 0u;
                return 0u;
            }
            if (anyHeight)
                mot::addCurve(m, 0, 1, height, 1);
            if (anyBend)
                mot::addCurve(m, 1, 3, bend, 1);
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
                    const Fit *fit = fitFor(p, body);
                    const Track *track = fit && !fit->off ? fit->find(list[i], isGene) : nullptr;
                    uint32_t started = motion;
                    if (track)
                        if (const uint32_t copy = rewrite(rdram, motion, *track, body, p))
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
            if (!g_ram || !g_pair.live)
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
            float v[4];
            g_pair.geneTrack->at(rdF(g_ram, gene + kMotionFrame), v);
            const float want[3] = {v[0], v[1], v[2] * kDegrees};
            if (want[0] == g_pair.applied[0] && want[1] == g_pair.applied[1] && want[2] == g_pair.applied[2])
                return;
            const float facing = wrap(rdF(g_ram, gene + 0x104u) - g_pair.applied[2]);
            moveGene(g_ram, gene, g_pair.enemy, want[1] - g_pair.applied[1], want[0] - g_pair.applied[0],
                     want[2] - g_pair.applied[2], facing);
            std::memcpy(g_pair.applied, want, sizeof(want));
        }

        // punish_fit.txt:
        //   fit <Suplex|Stinger|CobraTwist|Spanking|GorillaSuplex> <body hex> [off|gorilla]   (the lines after it, until the
        //   next fit, are its; gorilla on a Suplex fit: that body plays the Gorilla Suplex for a Suplex pick)
        //   gene <motion> <frame>:<forward>,<side>,<turn>,<height> ...
        //   victim <motion> <frame>:<height>,<bend> ...
        void load()
        {
            std::error_code error;
            const fs::path &file = fs::exists(g_file, error) ? g_file : g_shippedFile;
            std::ifstream in(file);
            std::string line;
            Fit *fit = nullptr;
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
                    std::string name, body, flag;
                    fields >> name >> body >> flag;
                    int punish = -1;
                    for (int i = 0; i < kPunishCount; ++i)
                        if (name == kFileNames[i])
                            punish = i;
                    const uint32_t key = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16));
                    fit = punish >= 0 && key ? &g_fits[fitKey(punish, key)] : nullptr;
                    if (fit)
                    {
                        fit->off = flag == "off";
                        fit->gorilla = flag == "gorilla";
                    }
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
                        for (int c = 0; c < 4 && *at; ++c)
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
            KM_INFO(Game, "punish fit: " << g_fits.size() << " fits from " << file.filename().string()
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
        const Fit *fit = fitFor(punish, bodyOf(rdram, enemy & 0x1FFFFFFu));
        return fit && fit->off;
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
