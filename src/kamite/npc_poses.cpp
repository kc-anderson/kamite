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
        constexpr uint32_t kSlotSize = 0x1000u;
        constexpr uint32_t kSlotCount = (0x000FC000u - kSlotBase) / kSlotSize; // 120
        constexpr uint32_t kMaxMotion = 0x4000u;
        constexpr float kDeg = 3.14159265358979f / 180.0f;

        struct Pose
        {
            bool lean = false;
            float waist = 0.0f;   // degrees
            float sway = 100.0f;  // percent
            float height = 0.0f;  // metres
            float forward = 0.0f; // metres
            float side = 0.0f;    // metres
            float turn = 0.0f;    // degrees
        };

        struct Key
        {
            uint16_t room = 0;
            int x = 0, z = 0; // centimetres
        };

        struct Npc
        {
            int slot = -1;
            float spawn[3] = {};
            std::vector<uint8_t> original; // its own idle, as the game has it
            bool dirty = true;
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

        std::vector<uint8_t> leanMotion(const Pose &p)
        {
            std::vector<uint8_t> m(std::begin(data::kRailLoop), std::end(data::kRailLoop));
            shift(m, 1, 3, p.waist * kDeg);
            const float s = p.sway / 100.0f;
            scale(m, 0, 1, s);
            for (int c = 3; c <= 5; ++c)
                scale(m, 17, c, s);
            // Turn: the hips' heading, and the feet's targets (constants here) turned round her middle.
            const float a = p.turn * kDeg, ca = std::cos(a), sa = std::sin(a);
            shift(m, 0, 4, a);
            for (int bone : {20, 24})
            {
                const float x = constant(m, bone, 0), z = constant(m, bone, 2);
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

        void writeSlot(uint8_t *rdram, Npc &npc)
        {
            const Pose *pose = poseFor(g_room, npc.spawn);
            const std::vector<uint8_t> bytes = (pose && pose->lean) ? leanMotion(*pose) : npc.original;
            if (bytes.size() <= kSlotSize)
                std::memcpy(rdram + kSlotBase + kSlotSize * static_cast<uint32_t>(npc.slot), bytes.data(), bytes.size());
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
                g_slotUsed[it->second.slot] = false; // a new actor in the old one's memory
                g_npcs.erase(it);
                it = g_npcs.end();
            }
            if (it == g_npcs.end())
            {
                const auto free = std::find(g_slotUsed.begin(), g_slotUsed.end(), false);
                if (free == g_slotUsed.end())
                    return;
                Npc npc;
                npc.slot = static_cast<int>(free - g_slotUsed.begin());
                *free = true;
                std::copy(here, here + 3, npc.spawn);
                it = g_npcs.emplace(actor, std::move(npc)).first;
            }
            Npc &npc = it->second;
            if (npc.original.size() != length || fnv(npc.original.data(), length) != hash)
            {
                npc.original.assign(bytes, bytes + length); // its idle (19 and 29 alternate)
                npc.dirty = true;
            }
            if (npc.dirty)
                writeSlot(rdram, npc);
            SET_GPR_U32(ctx, 5, kSlotBase + kSlotSize * static_cast<uint32_t>(npc.slot));
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
                   "# height (m), forward (m), sideways (m, + = her left), turn (degrees, + = to her left)\r\n";
            for (const auto &[key, pose] : g_poses)
            {
                char line[160];
                std::snprintf(line, sizeof(line), "%s %d %.1f %.0f %.3f %.3f %.3f %.1f\r\n", keyText(key).c_str(), pose.lean ? 1 : 0,
                              pose.waist, pose.sway, pose.height, pose.forward, pose.side, pose.turn);
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
                int lean = pose.lean ? 1 : 0;
                if (ui->checkbox("Lean on the counter##lean", &lean))
                {
                    pose.lean = lean != 0;
                    changed = true;
                }
                ui->begin_disabled(!pose.lean);
                const auto slider = [&](const char *label, float &value, float low, float high, const char *format) {
                    ui->set_next_width(340.0f);
                    if (ui->slider_float(label, &value, low, high, format))
                        changed = true;
                };
                slider("Bend at the waist##waist", pose.waist, -30.0f, 40.0f, "%.1f deg");
                slider("Hip sway##sway", pose.sway, 0.0f, 300.0f, "%.0f%%");
                slider("Height##height", pose.height, -0.3f, 0.3f, "%.3f m");
                slider("Forward##forward", pose.forward, -0.6f, 0.6f, "%.3f m");
                slider("Sideways (+ = her left)##side", pose.side, -0.6f, 0.6f, "%.3f m");
                slider("Turn (+ = to her left)##turn", pose.turn, -180.0f, 180.0f, "%.1f deg");
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
                    for (auto &[actor, npc] : g_npcs)
                        npc.dirty = true;
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
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            load();
        }
        const bool hooked = FunctionHooks::instance().observeEntry(kStartMotion, "npc_poses_start_motion", &onStartMotion);
        KM_INFO(Game, "NPC poses: " << g_poses.size() << " set in npc_poses.txt" << (hooked ? "" : " (could not hook the motion start)"));
#if defined(KAMITE_HAS_GS_MENU)
        ps2x::menu::addPanel("NPCs", &drawPanel);
#endif
    }

    void onFrame(uint8_t *rdram)
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
            if (npc.dirty)
                writeSlot(rdram, npc);
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
