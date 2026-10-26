// NPC poses: bystanders who stand at a counter can lean on it.
//
// The motion. Quaternius' Universal Animation Library 2 (CC0) has a rail idle (Idle_Rail_Loop: elbows on a rail, a
// slight sway) and a call (Idle_Rail_Call: the same, waving someone over). tools/models/npc_rail.py retargets them to
// the game's 30-bone skeleton of its women (ema5.dat's bunny girls) and writes them in the game's motion format (MOT,
// "mtb3", tools/models/mot.py) into npc_motions.inc: the spine, arms, neck and head are the rail idle's; the hips, legs
// and feet are the girl's own idle (ema5.dat motion 19), whose feet the game places with its leg IK. The upper body's
// world rotations are carried over relative to each skeleton's T-pose, with the hips held level, so the lean comes from
// the waist (bone 1).
//
// Where it plays. func_002A8578 (a0 = the actor, a1 = its motion, a2 = the motion's events (SEQ), then blend frames and
// speed) starts every actor's motion; the actor's data block (cGameObj.motionData) holds its motions. The men and the
// girls of the casino share ema5.dat's idles (motions 19 and 29), so the hook cannot go by the motion alone: every
// actor that starts one of those idles (matched by the bytes, FNV-1a, so a changed game file is left alone) gets a slot
// of its own, 4 KB of guest memory at 0x84000.. (below the ELF, unused by the game in every dump taken: kernel area
// below 0x82000, ELF from 0x100000), and its idle is started from the slot instead. A slot holds the actor's own idle
// unchanged, or the rail idle when the actor is set to lean, with that actor's adjustments:
//   bend at the waist   bone 1 x rotation (+ = further forward)
//   hip sway            bone 0's height and bone 17's rotations, scaled round their first key
//   height              bone 0's height and the two feet's IK targets (bones 20, 24) together
//   forward / sideways  bone 0's and the feet's x / z, in the actor's own space (+z = her forward, +x = her left)
//   turn                bone 0's y rotation, with the feet's targets turned round her middle
// Since the slot is the motion the game plays, an edit is written into it and shows at once.
//
// Motions: the rail idle built in, MOT files in resources/npc_motions (shipped: K's own table_tease and
// counter_idle) and mods/npc_motions (tools/models/npc_motion.py makes them from a
// .blend / .fbx / .glb; the file's name is the motion's id). On top of the motion: the head (pitch, turn), the upper
// arms (raised round the shoulders' side axis) and the forearms (bent round the upper arms' up axis) are turned in
// front of the motion's own rotation, every frame (preRotate resamples those curves); bottom out tips the pelvis;
// amount scales the motion's own rotations round its first frame.
//
// Which actors lean: npc_poses.txt next to the exe, one line per actor (room, the place it first started the idle in
// centimetres, and the adjustments). The F2 menu's NPCs tab edits the nearest actor and saves the file.
#include "kamite/npc_poses.h"
#include "kamite/npc_motions.inc"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime_macros.h" // GPR_U32 / SET_GPR_U32
#include "runtime/ps2_memory.h"

#include "runtime/ps2_game_menu.h"
#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <optional>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace kamite::npc_poses
{
    namespace
    {
        namespace fs = std::filesystem;
        using namespace kamite::game;

        constexpr uint32_t kStartMotion = 0x002A8578u; // func_002A8578: (actor, motion, events, ...)
        constexpr uint32_t kSlotBase = 0x00084000u;
        constexpr uint32_t kSlotSize = 0x4000u;                                      // one motion
        constexpr uint32_t kSlotCount = (0x000FC000u - kSlotBase) / (2 * kSlotSize); // 15 posed NPCs, two halves each
        constexpr uint32_t kMaxMotion = 0x4000u;
        constexpr float kDeg = 3.14159265358979f / 180.0f;
        // [Game] NpcPoses ("Better shop & casino poses" in the launcher): whether the poses play. Off, every NPC keeps
        // the game's idle (the F2 tab still edits npc_poses.txt and says the poses are off).
        bool g_enabled = false;

        struct Pose
        {
            bool lean = false;
            float waist = 0.0f;   // degrees
            float sway = 100.0f;  // percent
            float height = 0.0f;  // metres
            float forward = 0.0f; // metres
            float side = 0.0f;    // metres
            float turn = 0.0f;    // degrees
            std::string motion = "rail"; // kMotions' id or a file's name in mods/npc_motions
            float headPitch = 0.0f;  // degrees, + = looks down
            float headYaw = 0.0f;    // degrees, + = to her left
            float arms = 0.0f;       // degrees, + = the upper arms raised (the hands come up)
            float elbows = 0.0f;     // degrees, + = the forearms bent further forward
            float butt = 0.0f;       // degrees, + = the pelvis tipped forward (the bottom out)
            float amount = 100.0f;   // percent of the motion's own movement
            float headRoll = 0.0f;   // degrees, + = the head tipped to her right
            float legTurn = 0.0f;    // degrees, + = the legs turned round her middle to her left (she and her feet keep
                                     // facing); past 90 they cross
            float stance = 100.0f;   // percent of the feet's distance apart
        };

        // The motions a pose can play: the rail lean built in, and any MOT files in mods/npc_motions (made with
        // tools/models/npc_rail.py's retargeting; the file's name is the id).
        struct Motion
        {
            std::string id, label;
            std::vector<uint8_t> bytes;
        };
        std::vector<Motion> g_motions;

        struct Key
        {
            uint16_t room = 0;
            int x = 0, z = 0; // centimetres
        };

        struct Npc
        {
            int slot = -1;                 // its slot while it is posed, -1 while it plays the game's own idle
            float spawn[3] = {};
            uint32_t original = 0;         // the game's idle it started (its address in the game's data)
            bool dirty = true;
            std::string playing;           // what the slot holds now: "" for its own idle, else the motion's id
            int half = 0;                  // which of its two halves the game plays
            std::vector<uint8_t> layout;   // the playing motion's record table (curves' key counts and places)
            bool restart = false;          // the motion moved to the other half: start it again from there
            bool commit = false;           // an edit is finished: start the motion again so it surely shows
            // The motion start's other arguments, to start it again (a2 events, a3, t0, t1; f12..f14), and what
            // tells the actor is still the one that started it.
            uint32_t args[4] = {};
            float fargs[3] = {};
            uint32_t motionData = 0;
            uint8_t mode = 0;
        };

        std::mutex g_mutex;
        fs::path g_file;
        std::vector<std::pair<Key, Pose>> g_poses;
        std::map<uint32_t, Npc> g_npcs; // by actor address
        std::vector<bool> g_slotUsed(kSlotCount, false);
        uint16_t g_room = 0xFFFFu;
        uint32_t g_nearest = 0;       // actor nearest Gene, 0 if none
        float g_nearestDistance = 0.0f;
        float g_player[3] = {};
        std::string g_status;
        bool g_dragging = false; // a slider of the NPCs tab was held last frame
        std::optional<std::chrono::steady_clock::time_point> g_stepCommit; // when the last slider steps count as done
        GuestFunction g_start = nullptr; // func_002A8578 as recompiled (the observer is in front of it)

        uint32_t read32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + (address & 0x01FFFFFFu), 4);
            return value;
        }
        float readFloat(const uint8_t *rdram, uint32_t address)
        {
            float value = 0.0f;
            std::memcpy(&value, rdram + (address & 0x01FFFFFFu), 4);
            return value;
        }
        uint16_t read16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t value = 0;
            std::memcpy(&value, rdram + (address & 0x01FFFFFFu), 2);
            return value;
        }
        bool inRam(uint32_t address, uint32_t size)
        {
            const uint32_t a = address & 0x1FFFFFFFu;
            return a >= 0x100000u && a + size <= PS2_RAM_SIZE;
        }

        // PlatinumGames' 16-bit float (mot.py half): sign 1, exponent 6 (bias 47), mantissa 9.
        float halfToFloat(uint16_t v)
        {
            const int expo = (v >> 9) & 0x3F, mant = v & 0x1FF;
            if (expo == 0 && mant == 0)
                return 0.0f;
            const float value = std::ldexp(1.0f + mant / 512.0f, expo - 47);
            return (v & 0x8000u) ? -value : value;
        }
        uint16_t floatToHalf(float x)
        {
            if (x == 0.0f || !std::isfinite(x))
                return 0;
            const uint16_t sign = x < 0.0f ? 0x8000u : 0u;
            const float a = std::fabs(x);
            int e = static_cast<int>(std::floor(std::log2(a)));
            int m = static_cast<int>(std::lround((a / std::ldexp(1.0f, e) - 1.0f) * 512.0f));
            if (m >= 512)
            {
                m = 0;
                ++e;
            }
            const int expo = std::clamp(e + 47, 1, 62);
            return static_cast<uint16_t>(sign | (expo << 9) | (m & 0x1FF));
        }

        // The motion's length from its record table (the last curve block's end), 0 if it is not a motion.
        uint32_t motionLength(const uint8_t *m, uint32_t available)
        {
            if (available < 8 || std::memcmp(m, "mtb3", 4) != 0)
                return 0;
            const uint32_t records = m[6];
            uint32_t end = 8 + 12 * records;
            if (end > available)
                return 0;
            for (uint32_t i = 0; i < records; ++i)
            {
                const uint8_t *r = m + 8 + 12 * i;
                uint16_t count;
                uint32_t payload;
                std::memcpy(&count, r + 2, 2);
                std::memcpy(&payload, r + 8, 4);
                if (r[1] != 0xFF && (r[1] & 0xF0) == 0x10)
                    end = std::max(end, payload + 12u + 4u * count);
            }
            return end <= available ? end : 0;
        }

        uint32_t fnv(const uint8_t *data, uint32_t size)
        {
            uint32_t h = 0x811C9DC5u;
            for (uint32_t i = 0; i < size; ++i)
                h = (h ^ data[i]) * 0x01000193u;
            return h;
        }

        // Every record of `bone` / `component` (0..2 translation, 3..5 rotation) in a motion.
        template <class F> void forRecord(std::vector<uint8_t> &m, int bone, int component, F &&f)
        {
            const uint32_t records = m[6];
            for (uint32_t i = 0; i < records; ++i)
            {
                uint8_t *r = m.data() + 8 + 12 * i;
                if (r[1] != 0xFF && r[0] == bone && (r[1] & 0x0F) == component)
                    f(r);
            }
        }
        float constantOf(const uint8_t *r)
        {
            float v;
            std::memcpy(&v, r + 8, 4);
            return v;
        }
        void setConstant(uint8_t *r, float v)
        {
            std::memcpy(r + 8, &v, 4);
        }
        uint16_t *curveHeader(std::vector<uint8_t> &m, const uint8_t *r)
        {
            uint32_t payload;
            std::memcpy(&payload, r + 8, 4);
            return reinterpret_cast<uint16_t *>(m.data() + payload); // 6 halves: pos, dpos, t0, dt0, t1, dt1
        }

        void shift(std::vector<uint8_t> &m, int bone, int component, float delta)
        {
            forRecord(m, bone, component, [&](uint8_t *r) {
                if ((r[1] & 0xF0) == 0x00)
                    setConstant(r, constantOf(r) + delta);
                else
                {
                    uint16_t *h = curveHeader(m, r);
                    h[0] = floatToHalf(halfToFloat(h[0]) + delta);
                }
            });
        }

        // A curve scaled round its first key: value' = first + s * (value - first).
        void scale(std::vector<uint8_t> &m, int bone, int component, float s)
        {
            forRecord(m, bone, component, [&](uint8_t *r) {
                if ((r[1] & 0xF0) != 0x10)
                    return;
                uint16_t *h = curveHeader(m, r);
                uint32_t payload;
                std::memcpy(&payload, r + 8, 4);
                const float pos = halfToFloat(h[0]), dpos = halfToFloat(h[1]);
                const float first = pos + dpos * m[payload + 12 + 1];
                h[0] = floatToHalf(first + s * (pos - first));
                for (int i = 1; i < 6; ++i)
                    h[i] = floatToHalf(halfToFloat(h[i]) * s);
            });
        }

        float constant(std::vector<uint8_t> &m, int bone, int component)
        {
            float v = 0.0f;
            forRecord(m, bone, component, [&](uint8_t *r) {
                if ((r[1] & 0xF0) == 0x00)
                    v = constantOf(r);
            });
            return v;
        }
        void setConstant(std::vector<uint8_t> &m, int bone, int component, float value)
        {
            forRecord(m, bone, component, [&](uint8_t *r) {
                if ((r[1] & 0xF0) == 0x00)
                    setConstant(r, value);
            });
        }

        // ---- resampling a bone's rotation (for the head, arm and elbow adjustments: a rotation put in front of the
        // motion's own, which no single Euler curve can carry) ----
        struct Rec
        {
            uint8_t bone = 0, channel = 0;
            uint32_t flags = 0;
            uint16_t count = 0;
            float constant = 0.0f;
            std::vector<uint8_t> block; // header (6 halves) and keys, for a curve
        };

        bool parse(const std::vector<uint8_t> &m, std::vector<Rec> &recs)
        {
            if (m.size() < 8)
                return false;
            const uint32_t n = m[6];
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint8_t *r = m.data() + 8 + 12 * i;
                if (r[1] == 0xFF)
                    continue;
                Rec rec;
                rec.bone = r[0];
                rec.channel = r[1];
                std::memcpy(&rec.count, r + 2, 2);
                std::memcpy(&rec.flags, r + 4, 4);
                uint32_t payload;
                std::memcpy(&payload, r + 8, 4);
                if ((r[1] & 0xF0) == 0x10)
                {
                    const uint32_t size = 12u + 4u * rec.count;
                    if (payload + size > m.size())
                        return false;
                    rec.block.assign(m.begin() + payload, m.begin() + payload + size);
                }
                else
                    std::memcpy(&rec.constant, &payload, 4);
                recs.push_back(std::move(rec));
            }
            return true;
        }

        std::vector<uint8_t> serialize(uint16_t frames, uint8_t loop, const std::vector<Rec> &recs)
        {
            const uint32_t n = static_cast<uint32_t>(recs.size()) + 1;
            std::vector<uint8_t> out(8 + 12 * n);
            std::memcpy(out.data(), "mtb3", 4);
            std::memcpy(out.data() + 4, &frames, 2);
            out[6] = static_cast<uint8_t>(n);
            out[7] = loop;
            for (size_t i = 0; i < recs.size(); ++i)
            {
                uint8_t *r = out.data() + 8 + 12 * i;
                const Rec &rec = recs[i];
                r[0] = rec.bone;
                r[1] = rec.channel;
                std::memcpy(r + 2, &rec.count, 2);
                std::memcpy(r + 4, &rec.flags, 4);
                uint32_t payload;
                if ((rec.channel & 0xF0) == 0x10)
                {
                    payload = static_cast<uint32_t>(out.size());
                    out.insert(out.end(), rec.block.begin(), rec.block.end());
                    std::memcpy(out.data() + 8 + 12 * i + 8, &payload, 4);
                }
                else
                {
                    std::memcpy(&payload, &rec.constant, 4);
                    std::memcpy(r + 8, &payload, 4);
                }
            }
            uint8_t *end = out.data() + 8 + 12 * recs.size();
            end[0] = 127;
            end[1] = 0xFF;
            end[2] = end[3] = 0xFF;
            return out;
        }

        float hermite(float p0, float m0, float p1, float m1, float t)
        {
            const float t2 = t * t, t3 = t2 * t;
            return (2 * t3 - 3 * t2 + 1) * p0 + (t3 - 2 * t2 + t) * m0 + (-2 * t3 + 3 * t2) * p1 + (t3 - t2) * m1;
        }

        float sample(const Rec &rec, float t)
        {
            if ((rec.channel & 0xF0) != 0x10)
                return rec.constant;
            const uint16_t *h = reinterpret_cast<const uint16_t *>(rec.block.data());
            const float pos = halfToFloat(h[0]), dpos = halfToFloat(h[1]), t0 = halfToFloat(h[2]), dt0 = halfToFloat(h[3]),
                        t1 = halfToFloat(h[4]), dt1 = halfToFloat(h[5]);
            int frame = 0, prevFrame = 0;
            float prevV = 0, prevOut = 0;
            for (uint16_t k = 0; k < rec.count; ++k)
            {
                const uint8_t *key = rec.block.data() + 12 + 4 * k;
                frame += key[0];
                const float v = pos + dpos * key[1], in = t0 + dt0 * key[2], out = t1 + dt1 * key[3];
                if (k == 0 && t < frame)
                    return v;
                if (k > 0 && t < frame)
                    return frame == prevFrame ? v : hermite(prevV, prevOut, v, in, (t - prevFrame) / (frame - prevFrame));
                prevFrame = frame;
                prevV = v;
                prevOut = out;
            }
            return prevV;
        }

        uint16_t halfAtMost(float x)
        {
            uint16_t h = floatToHalf(x);
            for (int i = 0; i < 8 && halfToFloat(h) > x; ++i)
                h = floatToHalf(x - std::fabs(x) * 0.004f * (i + 1) - 1e-6f);
            return h;
        }

        // pos + delta * k (k 0..255) over [lo, hi]
        void range(float lo, float hi, uint16_t &posBits, uint16_t &deltaBits)
        {
            posBits = halfAtMost(lo);
            const float pos = halfToFloat(posBits);
            deltaBits = 0;
            if (hi - pos <= 1e-7f)
                return;
            float d = (hi - pos) / 255.0f;
            for (int i = 0; i < 16; ++i)
            {
                deltaBits = floatToHalf(d);
                if (pos + halfToFloat(deltaBits) * 255.0f >= hi)
                    break;
                d *= 1.004f;
            }
        }

        uint8_t step(float v, uint16_t posBits, uint16_t deltaBits)
        {
            const float pos = halfToFloat(posBits), d = halfToFloat(deltaBits);
            if (d == 0.0f)
                return 0;
            return static_cast<uint8_t>(std::clamp(std::lround((v - pos) / d), 0L, 255L));
        }

        // A looping curve through `v` (one value a frame), keys every 3 frames (more apart past 120) and on the last.
        Rec encode(uint8_t bone, uint8_t component, const std::vector<float> &v)
        {
            Rec rec;
            rec.bone = bone;
            // Always a curve, never a constant, even when it does not move: the motion's records stay the same
            // whatever the sliders say (edits in place).
            rec.channel = static_cast<uint8_t>(0x10 | component);
            const int n = static_cast<int>(v.size());
            std::vector<int> frames;
            const int every = std::max(3, n / 24); // a long motion keeps its slot size in hand
            for (int f = 0; f < n - 1; f += every)
                frames.push_back(f);
            frames.push_back(n - 1);
            std::vector<float> vals, ins, outs;
            for (size_t i = 0; i < frames.size(); ++i)
            {
                const int f = frames[i];
                const float slope = 0.5f * (v[(f + 1) % n] - v[(f - 1 + n) % n]);
                const int lin = i > 0 ? f - frames[i - 1] : 1, lout = i + 1 < frames.size() ? frames[i + 1] - f : 1;
                vals.push_back(v[f]);
                ins.push_back(slope * lin);
                outs.push_back(slope * lout);
            }
            uint16_t h[6];
            range(*std::min_element(vals.begin(), vals.end()), *std::max_element(vals.begin(), vals.end()), h[0], h[1]);
            range(*std::min_element(ins.begin(), ins.end()), *std::max_element(ins.begin(), ins.end()), h[2], h[3]);
            range(*std::min_element(outs.begin(), outs.end()), *std::max_element(outs.begin(), outs.end()), h[4], h[5]);
            rec.block.resize(12);
            std::memcpy(rec.block.data(), h, 12);
            int prev = 0;
            for (size_t i = 0; i < frames.size(); ++i)
            {
                rec.block.push_back(static_cast<uint8_t>(frames[i] - prev));
                rec.block.push_back(step(vals[i], h[0], h[1]));
                rec.block.push_back(step(ins[i], h[2], h[3]));
                rec.block.push_back(step(outs[i], h[4], h[5]));
                prev = frames[i];
            }
            rec.count = static_cast<uint16_t>(frames.size());
            return rec;
        }

        using Mat = std::array<float, 9>; // row-major
        Mat mul(const Mat &a, const Mat &b)
        {
            Mat r{};
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
            return r;
        }
        Mat rotX(float a) { const float c = std::cos(a), s = std::sin(a); return {1, 0, 0, 0, c, -s, 0, s, c}; }
        Mat rotY(float a) { const float c = std::cos(a), s = std::sin(a); return {c, 0, s, 0, 1, 0, -s, 0, c}; }
        Mat rotZ(float a) { const float c = std::cos(a), s = std::sin(a); return {c, -s, 0, s, c, 0, 0, 0, 1}; }
        // The game's order (mot.py, pose.rot 'XYZ'): R = Rz * Ry * Rx.
        Mat euler(float x, float y, float z) { return mul(rotZ(z), mul(rotY(y), rotX(x))); }
        void toEuler(const Mat &r, float &x, float &y, float &z)
        {
            y = std::asin(std::clamp(-r[6], -1.0f, 1.0f));
            x = std::atan2(r[7], r[8]);
            z = std::atan2(r[3], r[0]);
        }
        float near(float a, float ref)
        {
            constexpr float tau = 6.28318530718f;
            while (a - ref > 3.14159265f) a -= tau;
            while (a - ref < -3.14159265f) a += tau;
            return a;
        }

        // The bone's rotation becomes pre * its own (pre in its parent's space), every frame.
        void preRotate(std::vector<uint8_t> &m, int bone, const Mat &pre)
        {
            std::vector<Rec> recs;
            if (!parse(m, recs))
                return;
            uint16_t frames;
            std::memcpy(&frames, m.data() + 4, 2);
            const uint8_t loop = m[7];
            const Rec *comp[3] = {};
            size_t where = recs.size();
            for (size_t i = 0; i < recs.size(); ++i)
                if (recs[i].bone == bone && (recs[i].channel & 0x0F) >= 3 && (recs[i].channel & 0x0F) <= 5)
                {
                    comp[(recs[i].channel & 0x0F) - 3] = &recs[i];
                    where = std::min(where, i);
                }
            std::vector<float> xs(frames), ys(frames), zs(frames);
            for (int f = 0; f < frames; ++f)
            {
                float a[3];
                for (int c = 0; c < 3; ++c)
                    a[c] = comp[c] ? sample(*comp[c], static_cast<float>(f)) : 0.0f;
                float x, y, z;
                toEuler(mul(pre, euler(a[0], a[1], a[2])), x, y, z);
                if (f > 0)
                {
                    x = near(x, xs[f - 1]);
                    y = near(y, ys[f - 1]);
                    z = near(z, zs[f - 1]);
                }
                xs[f] = x;
                ys[f] = y;
                zs[f] = z;
            }
            std::vector<Rec> out;
            for (size_t i = 0; i < recs.size(); ++i)
            {
                if (i == where)
                {
                    out.push_back(encode(static_cast<uint8_t>(bone), 3, xs));
                    out.push_back(encode(static_cast<uint8_t>(bone), 4, ys));
                    out.push_back(encode(static_cast<uint8_t>(bone), 5, zs));
                }
                const Rec &r = recs[i];
                if (!(r.bone == bone && (r.channel & 0x0F) >= 3 && (r.channel & 0x0F) <= 5))
                    out.push_back(r);
            }
            if (where == recs.size())
            {
                out.push_back(encode(static_cast<uint8_t>(bone), 3, xs));
                out.push_back(encode(static_cast<uint8_t>(bone), 4, ys));
                out.push_back(encode(static_cast<uint8_t>(bone), 5, zs));
            }
            if (out.size() < 254)
                m = serialize(frames, loop, out);
        }

        const Motion *motionFor(const std::string &id)
        {
            for (const Motion &mo : g_motions)
                if (mo.id == id)
                    return &mo;
            return g_motions.empty() ? nullptr : &g_motions.front();
        }

        std::vector<uint8_t> leanMotion(const Pose &p)
        {
            const Motion *mo = motionFor(p.motion);
            if (!mo)
                return {};
            std::vector<uint8_t> m = mo->bytes;
            // How much of the motion's own movement (round its first frame), for the upper body and the hips.
            const float amount = p.amount / 100.0f;
            for (int bone : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 26, 29})
                for (int c = 3; c <= 5; ++c)
                    scale(m, bone, c, amount);
            shift(m, 17, 3, p.butt * kDeg);
            shift(m, 1, 3, p.waist * kDeg);
            const float s = p.sway / 100.0f;
            scale(m, 0, 1, s);
            for (int c = 3; c <= 5; ++c)
                scale(m, 17, c, s);
            // Turn: the hips' heading, and the feet's targets (constants here) turned round her middle, the feet
            // pointing the new way. Legs: the feet's places alone turned round her middle (the leg IK swings the legs
            // round; she keeps facing), the feet still pointing where she faces; past 90 degrees they pass each other
            // and the legs cross. And their distance apart.
            const float a = p.turn * kDeg;
            shift(m, 0, 4, a);
            const float stance = p.stance / 100.0f;
            for (int bone : {20, 24})
            {
                const float place = a + p.legTurn * kDeg, ca = std::cos(place), sa = std::sin(place);
                const float x = constant(m, bone, 0) * stance, z = constant(m, bone, 2);
                setConstant(m, bone, 0, x * ca + z * sa);
                setConstant(m, bone, 2, -x * sa + z * ca);
                shift(m, bone, 4, a);
            }
            for (int bone : {0, 20, 24})
            {
                shift(m, bone, 1, p.height);
                shift(m, bone, 2, p.forward);
                shift(m, bone, 0, p.side);
            }
            // The head (in the neck's space), the upper arms round the shoulders' side axis, the forearms round the
            // upper arms' up axis (a T-pose arm bends forward round it; + on the left is - on the right).
            // Always, even at 0, so the motion's records stay the same whatever the sliders say (edits in place).
            preRotate(m, 4, mul(rotY(p.headYaw * kDeg), mul(rotX(p.headPitch * kDeg), rotZ(p.headRoll * kDeg))));
            for (int bone : {6, 7, 12, 13})
                preRotate(m, bone, rotX(-p.arms * kDeg));
            preRotate(m, 8, rotY(p.elbows * kDeg));
            preRotate(m, 14, rotY(-p.elbows * kDeg));
            return m;
        }

        const Pose *poseFor(uint16_t room, const float spawn[3], Key *keyOut = nullptr)
        {
            const int x = static_cast<int>(std::lround(spawn[0] * 100.0f)), z = static_cast<int>(std::lround(spawn[2] * 100.0f));
            for (auto &[key, pose] : g_poses)
                if (key.room == room && std::abs(key.x - x) <= 20 && std::abs(key.z - z) <= 20)
                {
                    if (keyOut)
                        *keyOut = key;
                    return &pose;
                }
            if (keyOut)
                *keyOut = Key{room, x, z};
            return nullptr;
        }

        // Writes the slot. Between the idle and the lean the motion's records differ (count, bones, keys), and the
        // game keeps its place in each curve while a motion plays: the old place in the new curves made them jerk
        // about, and back from the lean the arms stayed out. So a change of motion starts it again (onFrame).
        uint32_t slotAddress(const Npc &npc) // valid while npc.slot >= 0
        {
            return kSlotBase + 2u * kSlotSize * static_cast<uint32_t>(npc.slot) + kSlotSize * static_cast<uint32_t>(npc.half);
        }

        // The record table without the constants' values: what has to stay the same for an edit in place.
        std::vector<uint8_t> tableLayout(const std::vector<uint8_t> &m)
        {
            if (m.size() < 8)
                return {};
            const uint32_t n = m[6];
            std::vector<uint8_t> t(m.begin() + 4, m.begin() + std::min<size_t>(m.size(), 8 + 12 * n));
            for (uint32_t i = 0; i < n && 4 + 12 * i + 12 <= t.size(); ++i)
                if ((t[4 + 12 * i + 1] & 0xF0) == 0x00)
                    std::fill(t.begin() + 4 + 12 * i + 8, t.begin() + 4 + 12 * i + 12, 0);
            return t;
        }

        void writeSlot(uint8_t *rdram, Npc &npc, bool starting)
        {
            const Pose *pose = poseFor(g_room, npc.spawn);
            std::vector<uint8_t> bytes;
            std::string playing;
            if (g_enabled && pose && pose->lean)
            {
                bytes = leanMotion(*pose);
                playing = motionFor(pose->motion) ? motionFor(pose->motion)->id : std::string();
                if (bytes.size() > kSlotSize)
                {
                    g_status = "That motion with these settings is too large for a slot.";
                    bytes.clear();
                }
            }
            if (!bytes.empty() && npc.slot < 0)
            {
                const auto free = std::find(g_slotUsed.begin(), g_slotUsed.end(), false);
                if (free == g_slotUsed.end())
                {
                    g_status = "All slots are posed in this room: no more NPCs can be.";
                    bytes.clear();
                }
                else
                {
                    *free = true;
                    npc.slot = static_cast<int>(free - g_slotUsed.begin());
                    npc.half = 0;
                    npc.layout.clear();
                    npc.restart = !starting;
                }
            }
            if (bytes.empty())
            {
                // Its own idle again: back to the game's motion, the slot free for another.
                if (npc.slot >= 0)
                {
                    g_slotUsed[npc.slot] = false;
                    npc.slot = -1;
                    npc.restart = !starting;
                }
                npc.playing.clear();
                npc.layout.clear();
                npc.commit = false;
                npc.dirty = false;
                return;
            }
            // The game keeps its place in each curve of a playing motion, and starting the same motion again is
            // ignored, so a motion whose records changed (another motion) goes to the other half and is started
            // there; values only are written in place while a slider is held. A finished edit (the slider let go)
            // moves too, so it shows from a clean start whatever it changed.
            const std::vector<uint8_t> layout = tableLayout(bytes);
            if (!starting && !npc.restart && !npc.layout.empty() && (layout != npc.layout || playing != npc.playing || npc.commit))
            {
                npc.half ^= 1;
                npc.restart = true;
            }
            npc.commit = false;
            npc.layout = layout;
            std::memcpy(rdram + slotAddress(npc), bytes.data(), bytes.size());
            npc.playing = playing;
            npc.dirty = false;
        }

        void forgetRoom()
        {
            g_npcs.clear();
            std::fill(g_slotUsed.begin(), g_slotUsed.end(), false);
            g_nearest = 0;
        }

        void onStartMotion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t actor = GPR_U32(ctx, 4), motion = GPR_U32(ctx, 5);
            if (!inRam(actor, 0x600) || !inRam(motion, 8))
                return;
            const uint32_t available = std::min(kMaxMotion, PS2_RAM_SIZE - (motion & 0x01FFFFFFu));
            const uint8_t *bytes = rdram + (motion & 0x01FFFFFFu);
            const uint32_t length = motionLength(bytes, available);
            if (length == 0 || length > kSlotSize)
                return;
            const uint32_t hash = fnv(bytes, length);
            if (std::find(std::begin(data::kIdleHashes), std::end(data::kIdleHashes), hash) == std::end(data::kIdleHashes))
                return;

            std::lock_guard<std::mutex> lock(g_mutex);
            const uint16_t room = read16(rdram, GameWork::address(GameWork::kRoom));
            if (room != g_room)
            {
                forgetRoom();
                g_room = room;
            }
            const uint32_t posPtr = read32(rdram, actor + GameObj::kPos);
            if (!inRam(posPtr, 12))
                return;
            const float here[3] = {readFloat(rdram, posPtr), readFloat(rdram, posPtr + 4), readFloat(rdram, posPtr + 8)};
            auto it = g_npcs.find(actor);
            if (it != g_npcs.end() && std::hypot(here[0] - it->second.spawn[0], here[2] - it->second.spawn[2]) > 1.0f)
            {
                if (it->second.slot >= 0)
                    g_slotUsed[it->second.slot] = false; // a new actor in the old one's memory
                g_npcs.erase(it);
                it = g_npcs.end();
            }
            if (it == g_npcs.end())
            {
                Npc npc;
                std::copy(here, here + 3, npc.spawn);
                it = g_npcs.emplace(actor, std::move(npc)).first;
            }
            Npc &npc = it->second;
            npc.original = motion; // its idle (19 and 29 alternate)
            if (npc.dirty)
                writeSlot(rdram, npc, true);
            npc.args[0] = GPR_U32(ctx, 6);
            npc.args[1] = GPR_U32(ctx, 7);
            npc.args[2] = GPR_U32(ctx, 8);
            npc.args[3] = GPR_U32(ctx, 9);
            npc.fargs[0] = ctx->f[12];
            npc.fargs[1] = ctx->f[13];
            npc.fargs[2] = ctx->f[14];
            npc.motionData = read32(rdram, actor + GameObj::kMotionData);
            npc.mode = rdram[(actor + GameObj::kMode) & 0x01FFFFFFu];
            npc.restart = false;
            if (npc.slot >= 0)
                SET_GPR_U32(ctx, 5, slotAddress(npc));
        }

        std::string keyText(const Key &k)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "%04X %d %d", k.room, k.x, k.z);
            return line;
        }

        void load()
        {
            g_poses.clear();
            std::ifstream in(g_file);
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream fields(line);
                std::string room;
                Key key;
                Pose pose;
                int lean = 0;
                if (!(fields >> room >> key.x >> key.z >> lean >> pose.waist >> pose.sway >> pose.height >> pose.forward >> pose.side >> pose.turn))
                    continue;
                key.room = static_cast<uint16_t>(std::strtoul(room.c_str(), nullptr, 16));
                pose.lean = lean != 0;
                // Added later (older lines stop here): motion, head pitch and yaw, arms, elbows, bottom, amount.
                std::string motion;
                if (fields >> motion)
                {
                    pose.motion = motion;
                    fields >> pose.headPitch >> pose.headYaw >> pose.arms >> pose.elbows >> pose.butt >> pose.amount;
                    fields >> pose.headRoll >> pose.legTurn >> pose.stance; // added later still
                }
                g_poses.emplace_back(key, pose);
            }
        }

        bool save()
        {
            std::ofstream out(g_file, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out << "# Kamite's NPC poses (the F2 menu's NPCs tab writes this file).\r\n"
                   "# room (hex), where the NPC stands (x z, cm), lean (0/1), bend at the waist (degrees), hip sway (%),\r\n"
                   "# height (m), forward (m), sideways (m, + = her left), turn (degrees, + = to her left), motion,\r\n"
                   "# head pitch and turn, arms raised, elbows bent, bottom out (degrees), amount of movement (%),\r\n"
                   "# head tilt, legs turned (degrees), feet apart (%)\r\n";
            for (const auto &[key, pose] : g_poses)
            {
                char line[320];
                std::snprintf(line, sizeof(line), "%s %d %.1f %.0f %.3f %.3f %.3f %.1f %s %.1f %.1f %.1f %.1f %.1f %.0f %.1f %.1f %.0f\r\n",
                              keyText(key).c_str(), pose.lean ? 1 : 0, pose.waist, pose.sway, pose.height, pose.forward,
                              pose.side, pose.turn, pose.motion.c_str(), pose.headPitch, pose.headYaw, pose.arms, pose.elbows,
                              pose.butt, pose.amount, pose.headRoll, pose.legTurn, pose.stance);
                out << line;
            }
            return static_cast<bool>(out);
        }

#if defined(KAMITE_HAS_GS_MENU)
        void drawPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 3u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again.");
                return;
            }
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_enabled)
                ui->text_colored(0xFF60C0FFu, "Better shop & casino poses is off (launcher: Gameplay), so the poses set "
                                              "here don't play.");
            ui->text_wrapped("Walk up to a bystander who stands at a counter (with \"Keep playing\" on), then set her "
                             "here. Changes show at once; Save keeps them in npc_poses.txt.");
            const auto it = g_npcs.find(g_nearest);
            if (g_nearest == 0 || it == g_npcs.end())
            {
                ui->separator_text("Nearest NPC");
                ui->text_disabled("No NPC with a pose the port can change has started its idle in this room yet.");
            }
            else
            {
                Key key;
                const Pose *found = poseFor(g_room, it->second.spawn, &key);
                Pose pose = found ? *found : Pose{};
                char line[160];
                std::snprintf(line, sizeof(line), "Nearest NPC: %.1f m away (room %04X, at %.2f, %.2f)%s", g_nearestDistance,
                              g_room, it->second.spawn[0], it->second.spawn[2], found ? "" : " - not set yet");
                ui->separator_text("Nearest NPC");
                ui->text(line);
                bool changed = false;
                bool dragging = false, settled = false, stepped = false; // a slider held; a final change; a step
                int lean = pose.lean ? 1 : 0;
                if (ui->checkbox("Play a pose motion##lean", &lean))
                {
                    pose.lean = lean != 0;
                    changed = true;
                    settled = true;
                }
                ui->begin_disabled(!pose.lean);
                {
                    std::vector<const char *> labels;
                    int current = 0;
                    for (size_t i = 0; i < g_motions.size(); ++i)
                    {
                        labels.push_back(g_motions[i].label.c_str());
                        if (g_motions[i].id == pose.motion)
                            current = static_cast<int>(i);
                    }
                    ui->set_next_width(340.0f);
                    if (!labels.empty() && ui->combo("Motion##motion", &current, labels.data(), static_cast<int>(labels.size())))
                    {
                        pose.motion = g_motions[current].id;
                        changed = true;
                        settled = true;
                    }
                }
                const auto slider = [&](const char *label, float &value, float low, float high, const char *format) {
                    ui->set_next_width(340.0f);
                    if (ui->slider_float(label, &value, low, high, format))
                    {
                        changed = true;
                        if (!ui->is_item_active())
                            stepped = true; // a step from the keys / wheel (or typed)
                    }
                    if (ui->is_item_active())
                        dragging = true;
                };
                slider("Bend at the waist##waist", pose.waist, -30.0f, 40.0f, "%.1f deg");
                slider("Hip sway##sway", pose.sway, 0.0f, 300.0f, "%.0f%%");
                slider("Height##height", pose.height, -0.5f, 0.5f, "%.3f m");
                slider("Forward##forward", pose.forward, -2.0f, 2.0f, "%.3f m");
                slider("Sideways (+ = her left)##side", pose.side, -2.0f, 2.0f, "%.3f m");
                slider("Turn (+ = to her left)##turn", pose.turn, -180.0f, 180.0f, "%.1f deg");
                slider("Head: look down (+) / up (-)##hpitch", pose.headPitch, -60.0f, 60.0f, "%.1f deg");
                slider("Head: turn (+ = to her left)##hyaw", pose.headYaw, -90.0f, 90.0f, "%.1f deg");
                slider("Head: tilt (+ = to her right)##hroll", pose.headRoll, -45.0f, 45.0f, "%.1f deg");
                slider("Arms: raise (+) / lower (-)##arms", pose.arms, -60.0f, 60.0f, "%.1f deg");
                slider("Elbows: bend (+) / straighten (-)##elbows", pose.elbows, -60.0f, 60.0f, "%.1f deg");
                slider("Bottom out (+) / in (-)##butt", pose.butt, -40.0f, 40.0f, "%.1f deg");
                slider("Legs: turn (+ = to her left; past 90 they cross)##legturn", pose.legTurn, -100.0f, 100.0f, "%.1f deg");
                slider("Legs: feet apart##stance", pose.stance, 0.0f, 200.0f, "%.0f%%");
                slider("Amount of movement##amount", pose.amount, 0.0f, 150.0f, "%.0f%%");
                ui->end_disabled();
                if (changed)
                {
                    bool stored = false;
                    for (auto &[k, p] : g_poses)
                        if (k.room == key.room && k.x == key.x && k.z == key.z)
                        {
                            p = pose;
                            stored = true;
                        }
                    if (!stored)
                        g_poses.emplace_back(key, pose);
                }
                // Only the NPCs this pose is for. While a slider is held its values go in place; when it is let go
                // (or the change was final at once) the motion starts again from the other half (writeSlot).
                // Steps (wheel, arrows) come one after another: they count as let go half a second after the last.
                const auto now = std::chrono::steady_clock::now();
                if (stepped)
                    g_stepCommit = now + std::chrono::milliseconds(500);
                const bool stepsDone = g_stepCommit && now >= *g_stepCommit;
                if (stepsDone)
                    g_stepCommit.reset();
                const bool released = (g_dragging && !dragging) || stepsDone;
                g_dragging = dragging;
                for (auto &[actor, npc] : g_npcs)
                {
                    Key k;
                    poseFor(g_room, npc.spawn, &k);
                    if (k.room != key.room || k.x != key.x || k.z != key.z)
                        continue;
                    if (changed)
                        npc.dirty = true;
                    if ((changed && settled) || released)
                    {
                        npc.dirty = true;
                        npc.commit = true;
                    }
                }
                if (found && ui->button("Forget this NPC##forget"))
                {
                    g_poses.erase(std::remove_if(g_poses.begin(), g_poses.end(), [&](const auto &e) {
                                      return e.first.room == key.room && e.first.x == key.x && e.first.z == key.z;
                                  }),
                                  g_poses.end());
                    for (auto &[actor, npc] : g_npcs)
                        npc.dirty = true;
                }
            }
            ui->separator_text("npc_poses.txt");
            int set = 0;
            for (const auto &[k, p] : g_poses)
                set += k.room == g_room ? 1 : 0;
            char count[128];
            std::snprintf(count, sizeof(count), "%d NPC(s) set in this room, %d in all; %d NPC(s) seen here.", set,
                          static_cast<int>(g_poses.size()), static_cast<int>(g_npcs.size()));
            ui->text_disabled(count);
            if (ui->button("Save##npcsave"))
                g_status = save() ? "Saved." : "Could not write npc_poses.txt.";
            ui->same_line();
            if (ui->button("Revert##npcrevert"))
            {
                load();
                for (auto &[actor, npc] : g_npcs)
                    npc.dirty = true;
                g_status = "Back to the saved file.";
            }
            if (!g_status.empty())
            {
                ui->same_line();
                ui->text(g_status.c_str());
            }
        }
#endif
    }

    void install(PS2Runtime &, const fs::path &exeDir)
    {
        g_file = exeDir / "npc_poses.txt";
        if (const char *on = std::getenv("KAMITE_NPC_POSES"))
            g_enabled = *on && *on != '0';
        g_motions.push_back(Motion{"rail", "Rail lean (Quaternius, built in)",
                                   std::vector<uint8_t>(std::begin(data::kRailLoop), std::end(data::kRailLoop))});
        {
            // The port's own (resources/npc_motions, shipped with it) and the player's (mods/npc_motions; a file
            // there with the same name replaces the port's).
            std::error_code error;
            std::map<std::string, fs::path> byName;
            for (const fs::path &folder : {exeDir / "resources" / "npc_motions", exeDir / "mods" / "npc_motions"})
                for (const auto &entry : fs::directory_iterator(folder, error))
                    if (entry.is_regular_file(error) && entry.path().extension() == ".mot")
                        byName[entry.path().stem().string()] = entry.path();
            std::vector<fs::path> files;
            for (const auto &[name, path] : byName)
                files.push_back(path);
            for (const fs::path &file : files)
            {
                std::ifstream in(file, std::ios::binary);
                std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (motionLength(bytes.data(), static_cast<uint32_t>(bytes.size())) == 0 || bytes.size() > kSlotSize)
                {
                    KM_WARN(Game, "NPC poses: " << file.filename().string() << " is not a motion that fits a slot; left out");
                    continue;
                }
                std::string id = file.stem().string();
                std::replace(id.begin(), id.end(), ' ', '_'); // npc_poses.txt keeps it as one word
                g_motions.push_back(Motion{id, id, std::move(bytes)});
            }
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            load();
        }
        const bool hooked = FunctionHooks::instance().observeEntry(kStartMotion, "npc_poses_start_motion", &onStartMotion);
        if (hooked)
            g_start = FunctionHooks::instance().original(kStartMotion);
        KM_INFO(Game, "NPC poses: " << g_poses.size() << " set in npc_poses.txt" << (hooked ? "" : " (could not hook the motion start)"));
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("NPCs", &drawPanel);
#endif
    }

    void onFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const uint16_t room = read16(rdram, GameWork::address(GameWork::kRoom));
        if (room != g_room)
        {
            forgetRoom();
            g_room = room;
            return;
        }
        for (auto &[actor, npc] : g_npcs)
        {
            if (npc.dirty)
                writeSlot(rdram, npc, false);
            if (!npc.restart || !g_start || !ctx || !runtime)
                continue;
            npc.restart = false;
            // Only while the actor is still the one that started the idle and still in that state (not hit, not
            // gone: its data block and state byte as they were).
            if (!inRam(actor, 0x600) || read32(rdram, actor + GameObj::kMotionData) != npc.motionData ||
                rdram[(actor + GameObj::kMode) & 0x01FFFFFFu] != npc.mode)
                continue;
            // The motion start called again with the arguments the game gave it, on a copy of the registers.
            const R5900Context saved = *ctx;
            SET_GPR_U32(ctx, 4, actor);
            const uint32_t start = npc.slot >= 0 ? slotAddress(npc) : npc.original;
            if (!start)
            {
                *ctx = saved;
                continue;
            }
            SET_GPR_U32(ctx, 5, start);
            SET_GPR_U32(ctx, 6, npc.args[0]);
            SET_GPR_U32(ctx, 7, npc.args[1]);
            SET_GPR_U32(ctx, 8, npc.args[2]);
            SET_GPR_U32(ctx, 9, npc.args[3]);
            ctx->f[12] = npc.fargs[0];
            ctx->f[13] = npc.fargs[1];
            ctx->f[14] = npc.fargs[2];
            g_start(rdram, ctx, runtime);
            *ctx = saved;
        }
        // The nearest NPC to Gene, by where each started its idle (the actors themselves are not read here: one
        // may have gone since).
        g_nearest = 0;
        const uint32_t player = read32(rdram, GameWork::address(GameWork::kPlayerObj));
        if (!inRam(player, 0x200))
            return;
        const uint32_t posPtr = read32(rdram, player + GameObj::kPos);
        if (!inRam(posPtr, 12))
            return;
        g_player[0] = readFloat(rdram, posPtr);
        g_player[1] = readFloat(rdram, posPtr + 4);
        g_player[2] = readFloat(rdram, posPtr + 8);
        float best = 1e30f;
        for (const auto &[actor, npc] : g_npcs)
        {
            const float d = std::hypot(npc.spawn[0] - g_player[0], npc.spawn[2] - g_player[2]);
            if (d < best)
            {
                best = d;
                g_nearest = actor;
            }
        }
        g_nearestDistance = best;
    }
}
