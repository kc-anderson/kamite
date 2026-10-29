// The test mode: cases the testers record in play, runs the port measures itself (claude/test-mode.md).
//
// Three things live here, sharing one per-frame logger and one pad path:
//
// 1. The case recorder (always on; the RecordCase hotkey, the right stick press by default): press once and the port
//    saves a state of its own (never the players' slots) and starts keeping the pad; press again and everything goes into one file,
//    cases\<date-time>.case: the state, the pad (one 32-byte status per game frame), a live per-frame log of what the
//    game did while the tester played, and a few lines of metadata. Nothing to type, nothing to edit.
// 2. The case replay (KAMITE_REPLAY_CASE=<file>, --replay-case <file> [--fps60 0|1]): loads the case's state, plays
//    its pad back frame by frame at the chosen rate, logs every frame to <case>.30.csv / <case>.60.csv (the
//    collision pool to <case>.<rate>.shapes.csv) and quits. Replay-Cases.cmd runs both rates for every new case;
//    tools/fps60/case_report.py compares them (the 30 run is the ground truth) and checks the 60 replay against the
//    live log (the replay is faithful when the two agree frame for frame).
// 3. A scripted run (KAMITE_TEST=<script>, --test; [Run] Test): the same machinery driven by a key = value script
//    (state, mode = record | play, input, frames in 30 FPS game frames, csv, fps60, enemies), for runs without a case.
//
// The pad goes through ps2x::pad's state tap on the IOP host's pad read (runtime/ps2_pad_tap.h): PADMAN, running on
// the emulated IOP, polls the virtual DualShock once a vblank, and the host state it is given passes through here (on
// the IOP thread, hence the mutex). The recorder keeps the last poll of each game frame as a 32-byte DS2 status (the
// file format); a replay rewrites the state from the recording's sample for the same real time. A recording made at 30
// and played at 60 holds each sample for two game frames, so every input lasts as long as it did; one made at 60 and
// played at 30 merges each pair of samples (a button pressed in either counts, the sticks take the later), so no tap
// is lost.
#include "kamite/test_mode.h"
#include "kamite/frame_patches.h"
#include "kamite/game_layout.h"
#include "kamite/git_commit.h" // KAMITE_GIT_COMMIT (generated at build time)
#include "kamite/log.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_hotkeys.h"
#include "runtime/ps2_pad_tap.h"
#include "runtime/ps2_save_state.h"
#include "ps2_runtime_macros.h" // GPR_U32

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace kamite::test_mode
{
    namespace
    {
        namespace fs = std::filesystem;
        namespace GameObj = game::GameObj;
        namespace GameWork = game::GameWork;

        constexpr uint32_t kSample = 32u;                     // a DS2 status (the libpad layout) per game frame
        std::mutex g_mutex;                                   // the tap (IOP thread) against the frame hook (game thread)

        // PadInputState <-> the DS2 status bytes the recording keeps: +2..3 buttons (active low), +4..7 rx ry lx ly,
        // +8..19 the pressures in PadPressure order.
        void toStatus(const ps2x::iop::PadInputState &state, uint8_t *status)
        {
            std::memset(status, 0, kSample);
            status[0] = 0x01u;
            status[1] = 0x79u;
            const uint16_t inverted = static_cast<uint16_t>(~state.buttons);
            status[2] = static_cast<uint8_t>(inverted & 0xFFu);
            status[3] = static_cast<uint8_t>(inverted >> 8);
            status[4] = state.rightX;
            status[5] = state.rightY;
            status[6] = state.leftX;
            status[7] = state.leftY;
            for (size_t i = 0; i < state.pressure.size() && i < 12u; ++i)
                status[8 + i] = state.pressure[i];
        }
        void fromStatus(const uint8_t *status, ps2x::iop::PadInputState &state)
        {
            state.connected = true;
            state.buttons = static_cast<uint16_t>(~(status[2] | (status[3] << 8)) & 0xFFFFu);
            state.rightX = status[4];
            state.rightY = status[5];
            state.leftX = status[6];
            state.leftY = status[7];
            for (size_t i = 0; i < state.pressure.size() && i < 12u; ++i)
                state.pressure[i] = status[8 + i];
        }
        constexpr uint32_t kEnemyList = 0x5864F0u + 0x500u;   // cEmManage: the first used slot (debug_tools.cpp)
        constexpr uint32_t kCollisionManager = 0x5E8040u;     // D_005E8040 (hitbox_overlay.cpp has the layout)
        constexpr uint32_t kHitStop = 0x574380u + 0xACD0u;    // the damage manager's hit stop word (fps60-notes 4b)
        constexpr uint32_t kObjHpMax = 0x548u, kObjHp = 0x54Au, kObjGuard = 0x54Cu, kObjSpeedRate = 0x5A8u;
        constexpr uint32_t kObjFlags = 0x250u, kObjKind = 0x2FEu, kObjSeq = 0x3ACu, kObjMotionFrame = 0x41Cu,
                           kObjMotion = 0x428u, kObjMotionLeft = 0x438u;
        constexpr uint32_t kPlTakeUnit = 0x678u;
        constexpr double kLoadTimeoutSeconds = 20.0;
        constexpr double kSaveTimeoutSeconds = 10.0;
        constexpr uint32_t kMaxCaseVblanks = 60u * 120u;      // a case stops itself after 2 minutes
        constexpr uint32_t kDefaultEnemies = 4u;

        fs::path g_exeDir;

        // ---- guest memory ----------------------------------------------------------------------------------------
        bool inRam(uint32_t address, uint32_t size)
        {
            const uint32_t a = address & 0x1FFFFFFFu;
            return a >= 0x100000u && a + size <= PS2_RAM_SIZE;
        }
        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t v = 0;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }
        uint16_t rd16(const uint8_t *rdram, uint32_t address)
        {
            uint16_t v = 0;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 2);
            return v;
        }
        int16_t rds16(const uint8_t *rdram, uint32_t address) { return static_cast<int16_t>(rd16(rdram, address)); }
        uint8_t rd8(const uint8_t *rdram, uint32_t address) { return rdram[address & 0x01FFFFFFu]; }
        float rdf(const uint8_t *rdram, uint32_t address)
        {
            float v = 0.0f;
            std::memcpy(&v, rdram + (address & 0x01FFFFFFu), 4);
            return v;
        }

        std::string trim(std::string text)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }

        bool readFile(const fs::path &path, std::string &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            std::ostringstream buffer;
            buffer << in.rdbuf();
            out = buffer.str();
            return true;
        }

        bool writeFile(const fs::path &path, const std::string &data)
        {
            std::error_code ec;
            if (path.has_parent_path())
                fs::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out.write(data.data(), static_cast<std::streamsize>(data.size()));
            return static_cast<bool>(out);
        }

        std::FILE *openOut(const fs::path &path)
        {
            std::error_code ec;
            if (path.has_parent_path())
                fs::create_directories(path.parent_path(), ec);
            return std::fopen(path.string().c_str(), "wb");
        }

        // ---- the pad recording (text: "kamite-pad 1", "divider N", "frames N", then 64 hex characters a frame) ----
        // One sample per pad poll. PADMAN polls the DualShock once a vblank whatever the game's frame rate, so poll n
        // after the state is vblank n in both runs: a recording maps onto real time by itself at 30 and at 60, and the
        // game reads the latest poll when its own pad read comes, as with a real pad (version 1 files kept one sample
        // per game frame; they are expanded on reading).
        struct Recording
        {
            std::vector<uint8_t> samples; // polls x kSample
            uint32_t divider = 2u;        // the frame rate it was recorded at (vblanks per game frame), for the report
            uint32_t polls() const { return static_cast<uint32_t>(samples.size() / kSample); }
        };

        bool parseRecording(const std::string &text, Recording &rec, std::string &error)
        {
            rec.samples.clear();
            std::istringstream in(text);
            std::string line;
            int version = 1;
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.rfind("kamite-pad", 0) == 0)
                {
                    version = std::atoi(line.c_str() + 10);
                    continue;
                }
                if (line.empty() || line[0] == '#' || line.rfind("frames", 0) == 0 || line.rfind("polls", 0) == 0)
                    continue;
                if (line.rfind("divider", 0) == 0)
                {
                    rec.divider = static_cast<uint32_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
                    continue;
                }
                if (line.size() != kSample * 2u)
                {
                    error = "a pad sample line has " + std::to_string(line.size()) + " characters, not 64";
                    return false;
                }
                for (uint32_t i = 0; i < kSample; ++i)
                    rec.samples.push_back(static_cast<uint8_t>(std::strtoul(line.substr(i * 2u, 2u).c_str(), nullptr, 16)));
            }
            if (rec.divider != 1u && rec.divider != 2u)
            {
                error = "the pad's divider must be 1 or 2";
                return false;
            }
            if (rec.samples.empty())
            {
                error = "the pad has no samples";
                return false;
            }
            if (version < 2 && rec.divider > 1u) // one sample per game frame: each one for each of its vblanks
            {
                std::vector<uint8_t> polls;
                polls.reserve(rec.samples.size() * rec.divider);
                for (size_t at = 0; at < rec.samples.size(); at += kSample)
                    for (uint32_t v = 0; v < rec.divider; ++v)
                        polls.insert(polls.end(), rec.samples.begin() + static_cast<std::ptrdiff_t>(at),
                                     rec.samples.begin() + static_cast<std::ptrdiff_t>(at + kSample));
                rec.samples.swap(polls);
            }
            return true;
        }

        std::string formatRecording(const Recording &rec)
        {
            std::string out = "kamite-pad 2\ndivider " + std::to_string(rec.divider) + "\npolls " + std::to_string(rec.polls()) + "\n";
            char hex[3];
            for (uint32_t f = 0; f < rec.polls(); ++f)
            {
                for (uint32_t i = 0; i < kSample; ++i)
                {
                    std::snprintf(hex, sizeof(hex), "%02X", rec.samples[f * kSample + i]);
                    out += hex;
                }
                out += '\n';
            }
            return out;
        }

        // ---- the case file: "KAMITE-CASE 1", then "section <name> <bytes>" + the bytes + "\n", then "end" ---------
        struct CaseFile
        {
            std::string meta, state, pad, live;
            std::string host; // the Modern controls' camera and turns, a line a frame (HostTrack)
            std::string reads; // what the game's pad reads returned, a line a read (ReadTrack)
        };

        std::string packCase(const CaseFile &c)
        {
            std::string out = "KAMITE-CASE 1\n";
            const auto section = [&](const char *name, const std::string &data) {
                out += "section ";
                out += name;
                out += ' ';
                out += std::to_string(data.size());
                out += '\n';
                out += data;
                out += '\n';
            };
            section("meta", c.meta);
            section("pad", c.pad);
            section("live", c.live);
            if (!c.host.empty())
                section("host", c.host);
            if (!c.reads.empty())
                section("reads", c.reads);
            section("state", c.state); // last: the big one
            out += "end\n";
            return out;
        }

        bool unpackCase(const std::string &data, CaseFile &c, std::string &error)
        {
            if (data.rfind("KAMITE-CASE 1\n", 0) != 0)
            {
                error = "not a case file";
                return false;
            }
            size_t at = 14u;
            while (at < data.size())
            {
                const size_t eol = data.find('\n', at);
                if (eol == std::string::npos)
                    break;
                const std::string line = data.substr(at, eol - at);
                at = eol + 1u;
                if (line == "end")
                    return true;
                if (line.rfind("section ", 0) != 0)
                {
                    error = "unexpected line in the case file: " + line;
                    return false;
                }
                const size_t space = line.find(' ', 8u);
                if (space == std::string::npos)
                {
                    error = "bad section header: " + line;
                    return false;
                }
                const std::string name = line.substr(8u, space - 8u);
                const size_t size = static_cast<size_t>(std::strtoull(line.c_str() + space + 1u, nullptr, 10));
                if (at + size > data.size())
                {
                    error = "section " + name + " runs past the end of the file";
                    return false;
                }
                std::string *target = name == "meta" ? &c.meta : name == "state" ? &c.state : name == "pad" ? &c.pad : name == "live" ? &c.live : name == "host" ? &c.host : name == "reads" ? &c.reads : nullptr;
                if (target)
                    target->assign(data, at, size);
                at += size + 1u; // the newline after the bytes
            }
            error = "no end marker";
            return false;
        }

        std::string metaValue(const std::string &meta, const char *key)
        {
            std::istringstream in(meta);
            std::string line;
            const std::string prefix = std::string(key) + "=";
            while (std::getline(in, line))
                if (line.rfind(prefix, 0) == 0)
                    return trim(line.substr(prefix.size()));
            return {};
        }

        // ---- the per-frame log -------------------------------------------------------------------------------------
        struct Logger
        {
            std::FILE *csv = nullptr;
            std::FILE *shapes = nullptr; // optional
            uint32_t enemies = kDefaultEnemies;
        };

        void writeHeaders(Logger &log)
        {
            std::fputs("frame,div,reads,sample,buttons,lx,ly,rx,ry,held,room,stp,sta,hitstop,"
                       "pl_x,pl_y,pl_z,pl_heading,pl_mode,pl_phase,pl_step,pl_arg,pl_motion,pl_mframe,pl_mleft,pl_seq,pl_hp,pl_guard,pl_speed,"
                       "pl_w568,pl_w5F8,pl_w5FC,pl_b61D,pl_b621,pl_b622,pl_b623,pl_b624,pl_b625,pl_b626,pl_b627,pl_b628,pl_h648,"
                       "pl_f15D0,pl_h15F4,pl_b1600,pl_b1601,pl_b1602,pl_b1603,pl_h1616,pl_h1618,pl_hurt,"
                       "shapes,active,attack_on,hurt_on,enemies",
                       log.csv);
            for (uint32_t i = 0; i < log.enemies; ++i)
                std::fprintf(log.csv, ",em%u_addr,em%u_kind,em%u_hp,em%u_mode,em%u_phase,em%u_step,em%u_motion,em%u_mframe,"
                                      "em%u_x,em%u_y,em%u_z,em%u_heading,em%u_speed,em%u_guard,em%u_flags",
                             i, i, i, i, i, i, i, i, i, i, i, i, i, i, i);
            std::fputc('\n', log.csv);
            if (log.shapes)
                std::fputs("frame,rec,group,active,kind,x,y,z,a,b,c,owner\n", log.shapes);
        }

        // Gene's take unit: the pool records of its nodes, in list order (as the dodge probe read them).
        void takeNodes(const uint8_t *rdram, uint32_t obj, std::vector<uint32_t> &records, std::string &bits)
        {
            records.clear();
            bits.clear();
            const uint32_t unit = rd32(rdram, obj + kPlTakeUnit);
            if (!inRam(unit, 0x40u))
                return;
            uint32_t node = rd32(rdram, unit + 0x3Cu);
            for (int guard = 0; guard < 16 && inRam(node, 0x30u); ++guard, node = rd32(rdram, node + 0x24u))
            {
                const uint32_t data = rd32(rdram, node + 0x28u);
                records.push_back(data);
                bits += inRam(data, 4u) && (rd32(rdram, data) & 2u) ? '1' : '0';
            }
        }

        void objectPosition(const uint8_t *rdram, uint32_t obj, float &x, float &y, float &z, float &heading)
        {
            x = y = z = heading = 0.0f;
            const uint32_t pos = rd32(rdram, obj + GameObj::kPos);
            if (inRam(pos, 12u))
            {
                x = rdf(rdram, pos);
                y = rdf(rdram, pos + 4u);
                z = rdf(rdram, pos + 8u);
            }
            heading = rdf(rdram, obj + GameObj::kRot + 4u);
        }

        // One row: the state at the start of a game frame (= after the previous frame's update). `sample` is the pad
        // status the game saw in the previous frame (nullptr: none yet), `sampleIndex` the recording index (-1: live).
        void logFrame(Logger &log, const uint8_t *rdram, uint32_t frame, uint32_t divider, uint32_t reads, int64_t sampleIndex,
                      const uint8_t *sample)
        {
            const unsigned buttons = sample ? (~(sample[2] | (sample[3] << 8)) & 0xFFFFu) : 0u;
            std::fprintf(log.csv, "%u,%u,%u,%lld,%04X,%u,%u,%u,%u,%08X,%u,%08X,%08X,%u", frame, divider, reads,
                         static_cast<long long>(sampleIndex), buttons, sample ? sample[6] : 0x80u, sample ? sample[7] : 0x80u,
                         sample ? sample[4] : 0x80u, sample ? sample[5] : 0x80u, rd32(rdram, GameWork::address(GameWork::kPadHeld)),
                         rd16(rdram, GameWork::address(GameWork::kRoom)), rd32(rdram, GameWork::address(GameWork::kFlags5D8)),
                         rd32(rdram, GameWork::address(GameWork::kFlags5E4)), rd32(rdram, kHitStop));

            std::vector<uint32_t> plRecords;
            std::string plHurt;
            const uint32_t pl = rd32(rdram, GameWork::address(GameWork::kPlayerObj));
            if (inRam(pl, 0x1700u))
            {
                float x, y, z, heading;
                objectPosition(rdram, pl, x, y, z, heading);
                takeNodes(rdram, pl, plRecords, plHurt);
                std::fprintf(log.csv, ",%.3f,%.3f,%.3f,%.4f,%u,%u,%u,%u,%X,%.3f,%.3f,%04X,%d,%.3f,%.3f", x, y, z, heading,
                             rd8(rdram, pl + GameObj::kMode), rd8(rdram, pl + GameObj::kMode + 1u), rd8(rdram, pl + GameObj::kMode + 2u),
                             rd8(rdram, pl + GameObj::kMode + 3u), rd32(rdram, pl + kObjMotion), rdf(rdram, pl + kObjMotionFrame),
                             rdf(rdram, pl + kObjMotionLeft), rd16(rdram, pl + kObjSeq), rds16(rdram, pl + kObjHp), rdf(rdram, pl + kObjGuard),
                             rdf(rdram, pl + kObjSpeedRate));
                std::fprintf(log.csv, ",%u,%u,%u,%u", rd32(rdram, pl + 0x568u), rd32(rdram, pl + 0x5F8u), rd32(rdram, pl + 0x5FCu),
                             rd8(rdram, pl + 0x61Du));
                for (uint32_t off = 0x621u; off <= 0x628u; ++off)
                    std::fprintf(log.csv, ",%u", rd8(rdram, pl + off));
                std::fprintf(log.csv, ",%u,%.3f,%04X,%u,%u,%u,%u,%u,%u,%s", rd16(rdram, pl + 0x648u), rdf(rdram, pl + 0x15D0u),
                             rd16(rdram, pl + 0x15F4u), rd8(rdram, pl + 0x1600u), rd8(rdram, pl + 0x1601u), rd8(rdram, pl + 0x1602u),
                             rd8(rdram, pl + 0x1603u), rd16(rdram, pl + 0x1616u), rd16(rdram, pl + 0x1618u), plHurt.c_str());
            }
            else
                for (int i = 0; i < 37; ++i) // the player columns
                    std::fputc(',', log.csv);

            // The collision pool: every record in use, to the shapes file; the counts here.
            const uint32_t records = rd32(rdram, kCollisionManager);
            const uint32_t count = rd32(rdram, kCollisionManager + 4u);
            const uint32_t stride = rd32(rdram, kCollisionManager + 8u);
            uint32_t used = 0, active = 0, attackOn = 0, hurtOn = 0;
            if (stride >= 0x100u && stride <= 0x1000u && count <= 2048u && inRam(records, count * stride))
            {
                for (uint32_t i = 0; i < count; ++i)
                {
                    const uint32_t rec = records + i * stride;
                    const uint32_t flags = rd32(rdram, rec);
                    if ((flags & 0x601u) != 1u)
                        continue;
                    ++used;
                    const bool on = (flags & 2u) != 0u;
                    const uint32_t group = rd32(rdram, rec + 0x10u);
                    const bool attack = group >= 1u && group <= 8u && (group & 1u) != 0u;
                    const bool hurt = group >= 1u && group <= 8u && (group & 1u) == 0u;
                    if (on)
                    {
                        ++active;
                        attackOn += attack ? 1u : 0u;
                        hurtOn += hurt ? 1u : 0u;
                    }
                    if (!log.shapes)
                        continue;
                    uint32_t shape = rd32(rdram, rec + 0x34u);
                    if (!inRam(shape, 0x120u))
                        shape = rec + 0xE0u;
                    const uint32_t kind = rd32(rdram, shape);
                    const uint32_t pos = rd32(rdram, shape + 0xC0u);
                    float x = 0.0f, y = 0.0f, z = 0.0f;
                    if (inRam(pos, 12u))
                    {
                        x = rdf(rdram, pos);
                        y = rdf(rdram, pos + 4u);
                        z = rdf(rdram, pos + 8u);
                    }
                    const float a = rdf(rdram, shape + 0x110u), b = rdf(rdram, shape + 0x114u), c = rdf(rdram, shape + 0x118u);
                    const char *owner = "";
                    for (uint32_t r : plRecords)
                        if (r == rec)
                            owner = "pl";
                    std::fprintf(log.shapes, "%u,%u,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s\n", frame, i, group, on ? 1u : 0u, kind, x, y,
                                 z, a, kind == 0u ? 0.0f : b, kind == 4u ? c : 0.0f, owner);
                }
            }

            // The enemies, in cEmManage's list order.
            std::vector<uint32_t> enemies;
            uint32_t slot = rd32(rdram, kEnemyList);
            for (int guard = 0; guard < 64 && inRam(slot, 16u); ++guard)
            {
                const uint32_t em = rd32(rdram, slot + 8u);
                slot = rd32(rdram, slot + 4u);
                if (!inRam(em, 0x600u))
                    continue;
                const uint16_t kind = rd16(rdram, em + kObjKind);
                if (kind < 0x200u || kind >= 0x280u)
                    continue;
                enemies.push_back(em);
            }
            std::fprintf(log.csv, ",%u,%u,%u,%u,%u", used, active, attackOn, hurtOn, static_cast<unsigned>(enemies.size()));
            for (uint32_t i = 0; i < log.enemies; ++i)
            {
                if (i >= enemies.size())
                {
                    for (int k = 0; k < 15; ++k) // the enemy's columns
                        std::fputc(',', log.csv);
                    continue;
                }
                const uint32_t em = enemies[i];
                float x, y, z, heading;
                objectPosition(rdram, em, x, y, z, heading);
                std::fprintf(log.csv, ",%X,%03X,%d,%u,%u,%u,%X,%.3f,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%08X", em, rd16(rdram, em + kObjKind),
                             rds16(rdram, em + kObjHp), rd8(rdram, em + GameObj::kMode), rd8(rdram, em + GameObj::kMode + 1u),
                             rd8(rdram, em + GameObj::kMode + 2u), rd32(rdram, em + kObjMotion), rdf(rdram, em + kObjMotionFrame), x, y, z,
                             heading, rdf(rdram, em + kObjSpeedRate), rdf(rdram, em + kObjGuard), rd32(rdram, em + kObjFlags));
            }
            std::fputc('\n', log.csv);
        }

        void closeLogger(Logger &log)
        {
            if (log.csv)
                std::fclose(log.csv);
            if (log.shapes)
                std::fclose(log.shapes);
            log.csv = log.shapes = nullptr;
        }

        // ---- the host track: "kamite-host 1", "divider N", then "<frame> <free> <yaw> <pitch> <gene yaw | ->" ----------
        struct HostEntry
        {
            uint32_t frame = 0; // the recorder's frame counter when the camera hook ran (frame k's update: k + 1)
            test_mode::HostCamera camera;
        };
        struct HostTrack
        {
            uint32_t divider = 2u;
            std::vector<HostEntry> entries;
        };

        std::string formatHost(const HostTrack &track)
        {
            std::string out = "kamite-host 1\ndivider " + std::to_string(track.divider) + "\n";
            char line[128];
            for (const HostEntry &e : track.entries)
            {
                char turn[32] = "-";
                if (e.camera.turned)
                    std::snprintf(turn, sizeof(turn), "%.9g", static_cast<double>(e.camera.geneYaw));
                std::snprintf(line, sizeof(line), "%u %d %.9g %.9g %s\n", e.frame, e.camera.free ? 1 : 0,
                              static_cast<double>(e.camera.yaw), static_cast<double>(e.camera.pitch), turn);
                out += line;
            }
            return out;
        }

        bool parseHost(const std::string &text, HostTrack &track)
        {
            std::istringstream in(text);
            std::string line;
            if (!std::getline(in, line) || trim(line) != "kamite-host 1")
                return false;
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.rfind("divider ", 0) == 0)
                {
                    track.divider = static_cast<uint32_t>(std::strtoul(line.c_str() + 8, nullptr, 10));
                    continue;
                }
                std::istringstream fields(line);
                HostEntry e;
                int free = 0;
                std::string turn;
                if (!(fields >> e.frame >> free >> e.camera.yaw >> e.camera.pitch >> turn))
                    continue;
                e.camera.free = free != 0;
                e.camera.turned = turn != "-";
                if (e.camera.turned)
                    e.camera.geneYaw = std::strtof(turn.c_str(), nullptr);
                track.entries.push_back(e);
            }
            return track.divider == 1u || track.divider == 2u;
        }

        // ---- the read track: "kamite-reads 1", "divider N", then "<frame> <n> <hex bytes 2..19>" a pad read ----------
        // What scePadRead (0x397D20) handed the game for pad 1, every read (n = the read's number within the frame). The
        // game reads the latest poll PADMAN wrote, and in live play a poll sometimes lands just before the read and
        // sometimes just after (Oct 29: 189 of 4575 frames read the next poll; at 18 s the fight went otherwise). A
        // replay hands the game these bytes at the same read instead, so it reads exactly what the tester's game read.
        constexpr uint32_t kReadFirst = 2u, kReadBytes = 18u; // buttons, sticks, pressures (status bytes 2..19)
        struct ReadEntry
        {
            uint32_t frame = 0, n = 0;
            uint8_t bytes[kReadBytes] = {};
        };
        struct ReadTrack
        {
            uint32_t divider = 2u;
            std::vector<ReadEntry> entries;
        };

        std::string formatReads(const ReadTrack &track)
        {
            std::string out = "kamite-reads 1\ndivider " + std::to_string(track.divider) + "\n";
            char line[96];
            for (const ReadEntry &e : track.entries)
            {
                int at = std::snprintf(line, sizeof(line), "%u %u ", e.frame, e.n);
                for (uint32_t i = 0; i < kReadBytes; ++i)
                    at += std::snprintf(line + at, sizeof(line) - static_cast<size_t>(at), "%02X", e.bytes[i]);
                out += line;
                out += "\n";
            }
            return out;
        }

        bool parseReads(const std::string &text, ReadTrack &track)
        {
            std::istringstream in(text);
            std::string line;
            if (!std::getline(in, line) || trim(line) != "kamite-reads 1")
                return false;
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.rfind("divider ", 0) == 0)
                {
                    track.divider = static_cast<uint32_t>(std::strtoul(line.c_str() + 8, nullptr, 10));
                    continue;
                }
                std::istringstream fields(line);
                ReadEntry e;
                std::string hex;
                if (!(fields >> e.frame >> e.n >> hex) || hex.size() != kReadBytes * 2u)
                    continue;
                for (uint32_t i = 0; i < kReadBytes; ++i)
                    e.bytes[i] = static_cast<uint8_t>(std::strtoul(hex.substr(i * 2u, 2u).c_str(), nullptr, 16));
                track.entries.push_back(e);
            }
            return track.divider == 1u || track.divider == 2u;
        }

        // ============================================================================================================
        // A run: a script (KAMITE_TEST) or a case replay (KAMITE_REPLAY_CASE). One at a time; the recorder is off
        // while one is running.
        // ============================================================================================================
        struct Run
        {
            enum class Phase
            {
                Off,
                Boot,    // waiting for the game loop to settle, then the state is requested
                Loading, // waiting for the state
                Running,
                Done
            };
            Phase phase = Phase::Off;
            bool failed = false;
            bool record = false;      // script mode = record: the pad is kept and written at the end
            fs::path state;           // the state to load (empty: start at boot)
            fs::path stateIsTemp;     // a state unpacked from a case: deleted once loaded
            fs::path output;          // the pad file (record) or the CSV (play)
            fs::path csv;
            bool withShapes = true;
            int fps60 = -1;           // -1: kamite.ini's
            uint32_t vblanks = 0;     // the length in vblanks (real time x 60)
            uint32_t enemies = kDefaultEnemies;
            Recording rec;            // play: the recording; record: filled as it runs
            Logger log;
            HostTrack host;           // play: the case's host track (empty: none, the controls run live)
            size_t hostNext = 0;      // the first entry not yet played
            std::FILE *ram = nullptr; // play: <csv stem>.ram.bin, the RAM dump (writeRamFrame)
            std::FILE *snap = nullptr; // play: <csv stem>.snap.bin, the whole RAM once a second (writeSnapshot)
            std::vector<uint32_t> snapPrev;
            ReadTrack reads;          // play: the case's read track (empty: the polls alone feed the game)
            size_t readNext = 0;      // the first entry not yet reached
            uint32_t readFrame = ~0u, readN = 0; // the frame of the last read and the reads in it

            uint64_t hookFrames = 0;
            uint32_t frame = 0, runFrames = 0, divider = 2u;
            uint32_t loadsBefore = 0;
            std::chrono::steady_clock::time_point loadRequested;
            uint32_t readsThisFrame = 0, readsLastFrame = 0;
            int64_t sampleUsed = -1;  // the last poll's sample (play: injected; record: kept)
            bool polling = false;     // the state has loaded: polls count from here
            uint32_t polls = 0;
        };
        Run g_run;

        // Poll n after the state loaded takes sample n (the case recorder kept poll n after the state saved).
        void runTap(ps2x::iop::PadInputState &state)
        {
            Run &run = g_run;
            ++run.readsThisFrame;
            const uint32_t index = run.polls++;
            if (run.record)
            {
                if (index < run.rec.polls())
                {
                    toStatus(state, run.rec.samples.data() + static_cast<size_t>(index) * kSample);
                    run.sampleUsed = index;
                }
                return;
            }
            const uint32_t last = run.rec.polls() - 1u;
            const uint32_t use = index < last ? index : last;
            fromStatus(run.rec.samples.data() + static_cast<size_t>(use) * kSample, state);
            run.sampleUsed = use;
        }

        // The RAM dump of a replay (dev-only; tools/fps60/ram_counters.py): a record a frame, the frame number (u32) then
        // Gene's object (0x1800 bytes), GameWork (0x800) and the save data (0x200, *D_00569B70), zeros where a pointer is
        // not valid. For finding what counts game frames without knowing where it lives (the God Hand gauge, Oct 29).
        constexpr uint32_t kRamGene = 0x1800u, kRamGameWork = 0x800u, kRamSave = 0x200u;
        void writeRamFrame(std::FILE *out, const uint8_t *rdram, uint32_t frame)
        {
            if (!out)
                return;
            static std::vector<uint8_t> buffer(4u + kRamGene + kRamGameWork + kRamSave);
            std::fill(buffer.begin(), buffer.end(), 0u);
            std::memcpy(buffer.data(), &frame, 4u);
            const auto copy = [&](uint32_t at, uint32_t address, uint32_t size) {
                if (address >= 0x100000u && address + size <= 0x2000000u)
                    std::memcpy(buffer.data() + at, rdram + address, size);
            };
            copy(4u, rd32(rdram, GameWork::address(GameWork::kPlayerObj)), kRamGene);
            copy(4u + kRamGene, GameWork::kAddress, kRamGameWork);
            copy(4u + kRamGene + kRamGameWork, rd32(rdram, 0x569B70u), kRamSave);
            std::fwrite(buffer.data(), 1u, buffer.size(), out);
        }

        // The whole of EE RAM once a second of game time (dev-only; tools/fps60/ram_snapshots.py), as the words that
        // changed since the last snapshot (the first: every non-zero word): "KSNP" then per snapshot u32 frame, u32
        // count, count x (u32 word index, u32 value). For a value that lives in no object the per-frame dump covers
        // (the God Hand meter, Oct 29).
        void writeSnapshot(Run &run, const uint8_t *rdram)
        {
            if (!run.snap)
                return;
            constexpr uint32_t kWords = 0x2000000u / 4u;
            if (run.snapPrev.empty())
            {
                run.snapPrev.assign(kWords, 0u);
                std::fwrite("KSNP", 1u, 4u, run.snap);
            }
            std::vector<uint32_t> out;
            const uint32_t *words = reinterpret_cast<const uint32_t *>(rdram);
            for (uint32_t i = 0x100000u / 4u; i < kWords; ++i)
                if (words[i] != run.snapPrev[i])
                {
                    out.push_back(i);
                    out.push_back(words[i]);
                    run.snapPrev[i] = words[i];
                }
            const uint32_t header[2] = {run.frame, static_cast<uint32_t>(out.size() / 2u)};
            std::fwrite(header, 4u, 2u, run.snap);
            if (!out.empty())
                std::fwrite(out.data(), 4u, out.size(), run.snap);
        }

        void finishRun(PS2Runtime *runtime, bool ok)
        {
            Run &run = g_run;
            run.phase = Run::Phase::Done;
            if (ok && run.record)
            {
                if (run.polls < run.rec.polls())
                    run.rec.samples.resize(static_cast<size_t>(std::max(run.polls, 1u)) * kSample);
                if (writeFile(run.output, formatRecording(run.rec)))
                    KM_INFO(Game, "test: recorded " << run.rec.polls() << " polls (divider " << run.rec.divider << ") to " << run.output.string());
                else
                {
                    KM_ERROR(Game, "test: cannot write " << run.output.string());
                    ok = false;
                }
            }
            closeLogger(run.log);
            if (run.ram)
            {
                std::fclose(run.ram);
                run.ram = nullptr;
            }
            if (run.snap)
            {
                std::fclose(run.snap);
                run.snap = nullptr;
            }
            if (!ok)
                run.failed = true;
            KM_INFO(Game, "test: " << (ok ? "done" : "failed") << " after " << run.frame << " frames; stopping the run");
            if (runtime)
                runtime->requestStop();
        }

        void runFrame(uint8_t *rdram, PS2Runtime *runtime)
        {
            Run &run = g_run;
            ++run.hookFrames;
            switch (run.phase)
            {
            case Run::Phase::Boot:
                if (run.hookFrames < 3u) // the loop is up; a frame rate switch has been applied
                    return;
                run.polling = false;
                run.polls = 0;
                if (run.record) // neutral until polled
                {
                    run.rec.divider = fps60() ? 1u : 2u;
                    run.rec.samples.assign(static_cast<size_t>(run.vblanks) * kSample, 0u);
                    for (uint32_t f = 0; f < run.vblanks; ++f)
                    {
                        uint8_t *sample = run.rec.samples.data() + static_cast<size_t>(f) * kSample;
                        sample[2] = sample[3] = 0xFFu;
                        sample[4] = sample[5] = sample[6] = sample[7] = 0x80u;
                    }
                }
                if (run.state.empty())
                {
                    run.phase = Run::Phase::Loading; // falls into the start below on this frame
                    run.loadsBefore = ps2x::savestate::loadCount() - 1u;
                }
                else
                {
                    std::error_code ec;
                    if (!fs::exists(run.state, ec))
                    {
                        KM_ERROR(Game, "test: no state " << run.state.string());
                        finishRun(runtime, false);
                        return;
                    }
                    run.loadsBefore = ps2x::savestate::loadCount();
                    ps2x::savestate::requestLoad(run.state);
                    run.loadRequested = std::chrono::steady_clock::now();
                    run.phase = Run::Phase::Loading;
                    return;
                }
                [[fallthrough]];
            case Run::Phase::Loading:
            {
                if (ps2x::savestate::loadCount() == run.loadsBefore)
                {
                    if (!run.state.empty() &&
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - run.loadRequested).count() > kLoadTimeoutSeconds)
                    {
                        KM_ERROR(Game, "test: the state did not load within " << kLoadTimeoutSeconds << " s");
                        finishRun(runtime, false);
                    }
                    return;
                }
                if (!run.stateIsTemp.empty())
                {
                    std::error_code ec;
                    fs::remove(run.stateIsTemp, ec);
                }
                // Loaded: this is test frame 0.
                run.divider = fps60() ? 1u : 2u;
                run.runFrames = run.vblanks / run.divider;
                run.frame = 0;
                run.readsLastFrame = 0;
                run.log.enemies = run.enemies;
                run.log.csv = openOut(run.csv);
                run.log.shapes = run.withShapes ? openOut(run.csv.string().substr(0, run.csv.string().size() - 4) + ".shapes.csv") : nullptr;
                if (!run.log.csv || (run.withShapes && !run.log.shapes))
                {
                    KM_ERROR(Game, "test: cannot write " << run.csv.string());
                    finishRun(runtime, false);
                    return;
                }
                writeHeaders(run.log);
                if (!run.record)
                {
                    const std::string stem = run.csv.string().substr(0, run.csv.string().size() - 4);
                    run.ram = openOut(stem + ".ram.bin");
                    run.snap = openOut(stem + ".snap.bin");
                }
                run.phase = Run::Phase::Running;
                KM_INFO(Game, "test: running " << run.runFrames << " frames at " << (run.divider == 1u ? 60 : 30) << " FPS"
                                               << (run.record ? " (recording the pad)" : " (playing the recording)"));
                [[fallthrough]];
            }
            case Run::Phase::Running:
            {
                run.readsLastFrame = run.readsThisFrame;
                run.readsThisFrame = 0;
                const uint8_t *sample = run.sampleUsed >= 0 ? run.rec.samples.data() + static_cast<size_t>(run.sampleUsed) * kSample : nullptr;
                logFrame(run.log, rdram, run.frame, run.divider, run.readsLastFrame, run.sampleUsed, sample);
                writeRamFrame(run.ram, rdram, run.frame);
                if ((run.frame * run.divider) % 60u == 0u)
                    writeSnapshot(run, rdram);
                if (++run.frame >= run.runFrames)
                    finishRun(runtime, true);
                return;
            }
            default:
                return;
            }
        }

        // ---- the script (KAMITE_TEST) ----------------------------------------------------------------------------
        bool readScript(const fs::path &path, Run &run, std::string &error)
        {
            std::ifstream in(path);
            if (!in)
            {
                error = "cannot open " + path.string();
                return false;
            }
            const auto resolve = [&](const std::string &value) {
                const fs::path p(value);
                return p.is_absolute() ? p : g_exeDir / p;
            };
            std::string line;
            int number = 0;
            uint32_t frames30 = 300u;
            fs::path input;
            while (std::getline(in, line))
            {
                ++number;
                if (const size_t hash = line.find('#'); hash != std::string::npos)
                    line.erase(hash);
                line = trim(line);
                if (line.empty())
                    continue;
                size_t split = line.find('=');
                if (split == std::string::npos)
                    split = line.find_first_of(" \t");
                if (split == std::string::npos)
                {
                    error = "line " + std::to_string(number) + ": expected key = value";
                    return false;
                }
                const std::string key = trim(line.substr(0, split));
                const std::string value = trim(line.substr(split + 1));
                if (key == "state")
                    run.state = resolve(value);
                else if (key == "input")
                    input = resolve(value);
                else if (key == "csv")
                    run.csv = resolve(value);
                else if (key == "mode")
                {
                    if (value == "record")
                        run.record = true;
                    else if (value == "play")
                        run.record = false;
                    else
                    {
                        error = "line " + std::to_string(number) + ": mode is record or play";
                        return false;
                    }
                }
                else if (key == "frames")
                    frames30 = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                else if (key == "fps60")
                    run.fps60 = value == "1" || value == "true" || value == "on" ? 1 : 0;
                else if (key == "enemies")
                    run.enemies = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                else
                {
                    error = "line " + std::to_string(number) + ": unknown key " + key;
                    return false;
                }
            }
            if (input.empty())
            {
                error = "no input file";
                return false;
            }
            if (frames30 == 0u)
            {
                error = "frames must be above 0";
                return false;
            }
            run.vblanks = frames30 * 2u;
            run.output = input;
            if (run.csv.empty())
                run.csv = input.parent_path() / (input.stem().string() + (run.record ? "_record.csv" : "_play.csv"));
            if (!run.record)
            {
                std::string text;
                if (!readFile(input, text))
                {
                    error = "cannot open " + input.string();
                    return false;
                }
                if (!parseRecording(text, run.rec, error))
                {
                    error = input.string() + ": " + error;
                    return false;
                }
            }
            return true;
        }

        // ---- a case replay (KAMITE_REPLAY_CASE) -------------------------------------------------------------------
        bool readCaseForReplay(const fs::path &path, Run &run, std::string &error)
        {
            std::string data;
            if (!readFile(path, data))
            {
                error = "cannot open " + path.string();
                return false;
            }
            CaseFile c;
            if (!unpackCase(data, c, error))
                return false;
            if (!parseRecording(c.pad, run.rec, error))
                return false;
            if (!c.host.empty() && !parseHost(c.host, run.host))
            {
                error = "bad host section";
                return false;
            }
            if (!c.reads.empty() && !parseReads(c.reads, run.reads))
            {
                error = "bad reads section";
                return false;
            }
            if (c.state.empty())
            {
                error = "the case has no state";
                return false;
            }
            run.state = path.string() + ".state.tmp";
            if (!writeFile(run.state, c.state))
            {
                error = "cannot write " + run.state.string();
                return false;
            }
            run.stateIsTemp = run.state;
            run.record = false;
            run.vblanks = run.rec.polls();
            if (const std::string enemies = metaValue(c.meta, "enemies"); !enemies.empty())
                run.enemies = static_cast<uint32_t>(std::strtoul(enemies.c_str(), nullptr, 10));
            if (const char *fps = std::getenv("KAMITE_REPLAY_FPS60"); fps && *fps)
                run.fps60 = *fps == '1' ? 1 : 0;
            const bool at60 = run.fps60 >= 0 ? run.fps60 != 0 : fps60();
            run.csv = path.string() + (at60 ? ".60.csv" : ".30.csv");
            return true;
        }

        // ============================================================================================================
        // The case recorder: the RecordCase hotkey (a pad button read from the pad data here, or a key the window
        // loop fires), in play, whenever no run is active.
        // ============================================================================================================
        struct Recorder
        {
            enum class Phase
            {
                Idle,
                Saving,   // the state was requested
                Recording
            };
            Phase phase = Phase::Idle;
            std::string stamp;
            fs::path stateFile, liveFile;
            uint32_t savesBefore = 0;
            std::chrono::steady_clock::time_point saveRequested;
            uint32_t divider = 2u;
            uint32_t frame = 0;
            uint16_t room = 0;
            Recording rec;            // poll n after the state saved = sample n
            Logger log;
            HostTrack host;           // the Modern controls' camera and turns, from frame 0
            ReadTrack reads;          // the game's pad reads, from frame 0
            uint32_t readFrame = ~0u, readN = 0;
            bool polling = false;     // the state has saved: every poll is kept
            uint32_t readsThisFrame = 0, readsLastFrame = 0;
            uint16_t prevPressed = 0;   // the pad's pressed bits at the last read (for the hotkey's edge)
            bool toggle = false;        // a press of the hotkey, taken at the next frame hook
            uint32_t casesWritten = 0;
        };
        Recorder g_rec;

        void recorderTap(const ps2x::iop::PadInputState &state)
        {
            Recorder &rec = g_rec;
            const uint16_t pressed = state.buttons; // PadButton bits, as the hotkey binding names them
            const uint16_t padBit = ps2x::hotkeys::binding(ps2x::hotkeys::Action::RecordCase).pad;
            if (padBit != 0u && (pressed & padBit) != 0u && (rec.prevPressed & padBit) == 0u)
                rec.toggle = true;
            rec.prevPressed = pressed;
            if (rec.phase == Recorder::Phase::Saving && !rec.polling && ps2x::savestate::saveCount() != rec.savesBefore)
                rec.polling = true; // the first poll after the state: the replay's first poll after the load
            if (!rec.polling)
                return;
            ++rec.readsThisFrame;
            uint8_t sample[kSample];
            toStatus(state, sample);
            rec.rec.samples.insert(rec.rec.samples.end(), sample, sample + kSample);
        }

        std::string timeStamp()
        {
            const std::time_t now = std::time(nullptr);
            char text[32] = {};
            std::strftime(text, sizeof(text), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
            return text;
        }

        void recorderStart(uint8_t *rdram)
        {
            Recorder &rec = g_rec;
            rec.stamp = timeStamp();
            const fs::path dir = g_exeDir / "cases";
            std::error_code ec;
            fs::create_directories(dir, ec);
            rec.stateFile = dir / (rec.stamp + ".state.tmp");
            rec.liveFile = dir / (rec.stamp + ".live.tmp");
            rec.room = rd16(rdram, GameWork::address(GameWork::kRoom));
            rec.divider = fps60() ? 1u : 2u;
            rec.rec.divider = rec.divider;
            rec.rec.samples.clear();
            rec.host = HostTrack{};
            rec.host.divider = rec.divider;
            rec.reads = ReadTrack{};
            rec.reads.divider = rec.divider;
            rec.readFrame = ~0u;
            rec.polling = false;
            rec.readsThisFrame = 0;
            rec.savesBefore = ps2x::savestate::saveCount();
            rec.saveRequested = std::chrono::steady_clock::now();
            ps2x::savestate::requestSave(rec.stateFile);
            rec.phase = Recorder::Phase::Saving;
            ps2x::menu::notify("Case: saving the state...", 3.0f);
            KM_INFO(Game, "case: recording " << rec.stamp << " (state requested)");
        }

        void recorderAbort(const char *why)
        {
            Recorder &rec = g_rec;
            closeLogger(rec.log);
            std::error_code ec;
            fs::remove(rec.stateFile, ec);
            fs::remove(rec.liveFile, ec);
            rec.phase = Recorder::Phase::Idle;
            rec.polling = false;
            ps2x::menu::notify(std::string("Case: ") + why, 4.0f);
            KM_ERROR(Game, "case: " << why);
        }

        void recorderStop()
        {
            Recorder &rec = g_rec;
            closeLogger(rec.log);
            CaseFile c;
            if (!readFile(rec.stateFile, c.state) || !readFile(rec.liveFile, c.live))
            {
                recorderAbort("could not read the recording's files");
                return;
            }
            c.pad = formatRecording(rec.rec);
            c.host = formatHost(rec.host);
            c.reads = formatReads(rec.reads);
            c.meta = "created=" + rec.stamp + "\ndivider=" + std::to_string(rec.divider) + "\nframes=" + std::to_string(rec.frame) +
                     "\npolls=" + std::to_string(rec.rec.polls()) + "\nseconds=" + std::to_string(static_cast<double>(rec.rec.polls()) / 60.0) +
                     "\nroom=0x";
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%03X", rec.room);
            c.meta += hex;
            c.meta += "\nenemies=" + std::to_string(kDefaultEnemies) + "\n";
            // The build that recorded it (kamite.exe's size and time): a replay with another build may differ from the
            // live log where a fix changed the game, and the report says so.
            {
                std::error_code buildError;
                const fs::path exe = g_exeDir / "kamite.exe";
                const auto size = fs::file_size(exe, buildError);
                const auto time = fs::last_write_time(exe, buildError);
                if (!buildError)
                    c.meta += "build=" + std::to_string(size) + ":" +
                              std::to_string(static_cast<long long>(time.time_since_epoch().count())) + "\n";
            }
            c.meta += std::string("commit=") + KAMITE_GIT_COMMIT + "\n"; // the build's source commit
            const fs::path caseFile = g_exeDir / "cases" / (rec.stamp + ".case");
            std::error_code ec;
            if (!writeFile(caseFile, packCase(c)))
            {
                recorderAbort("could not write the case file");
                return;
            }
            fs::remove(rec.stateFile, ec);
            fs::remove(rec.liveFile, ec);
            rec.phase = Recorder::Phase::Idle;
            rec.polling = false;
            ++rec.casesWritten;
            ps2x::menu::notify("Case saved: " + rec.stamp + " (" + std::to_string(rec.rec.polls() / 60u) + " s)", 4.0f);
            KM_INFO(Game, "case: wrote " << caseFile.string() << ": " << rec.frame << " frames, " << rec.rec.polls() << " polls at " << (rec.divider == 1u ? 60 : 30)
                                         << " FPS, " << c.state.size() / 1024u << " KB of state");
        }

        void recorderFrame(uint8_t *rdram)
        {
            Recorder &rec = g_rec;
            const bool toggle = rec.toggle || ps2x::hotkeys::take(ps2x::hotkeys::Action::RecordCase);
            rec.toggle = false;
            switch (rec.phase)
            {
            case Recorder::Phase::Idle:
                if (toggle)
                    recorderStart(rdram);
                return;
            case Recorder::Phase::Saving:
                if (ps2x::savestate::saveCount() == rec.savesBefore)
                {
                    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - rec.saveRequested).count() > kSaveTimeoutSeconds)
                        recorderAbort("the state did not save (see the log)");
                    return;
                }
                // Saved: this is frame 0 of the case (the tap has been keeping polls since the save).
                rec.frame = 0;
                rec.readsLastFrame = 0;
                rec.log.enemies = kDefaultEnemies;
                rec.log.csv = openOut(rec.liveFile);
                rec.log.shapes = nullptr; // the replays write the shapes; the live log stays small
                if (!rec.log.csv)
                {
                    recorderAbort("could not write the live log");
                    return;
                }
                writeHeaders(rec.log);
                rec.phase = Recorder::Phase::Recording;
                ps2x::menu::notify("Recording case " + rec.stamp + "... (press again to stop)", 2.0f);
                [[fallthrough]];
            case Recorder::Phase::Recording:
            {
                rec.readsLastFrame = rec.readsThisFrame;
                rec.readsThisFrame = 0;
                const int64_t last = static_cast<int64_t>(rec.rec.polls()) - 1;
                logFrame(rec.log, rdram, rec.frame, rec.divider, rec.readsLastFrame, last,
                         last >= 0 ? rec.rec.samples.data() + static_cast<size_t>(last) * kSample : nullptr);
                ++rec.frame;
                if (rec.frame % 60u == 0u)
                {
                    char text[64];
                    std::snprintf(text, sizeof(text), "Recording case... %.0f s (press again to stop)", static_cast<double>(rec.frame * rec.divider) / 60.0);
                    ps2x::menu::notify(text, 1.6f);
                }
                if (toggle || rec.rec.polls() >= kMaxCaseVblanks)
                {
                    // The stopping press is not part of the case: drop the polls at the end that hold it.
                    const uint16_t padBit = ps2x::hotkeys::binding(ps2x::hotkeys::Action::RecordCase).pad;
                    while (toggle && padBit != 0u && rec.rec.polls() > 1u)
                    {
                        const uint8_t *end = rec.rec.samples.data() + rec.rec.samples.size() - kSample;
                        if (((~(end[2] | (end[3] << 8))) & padBit) == 0u)
                            break;
                        rec.rec.samples.resize(rec.rec.samples.size() - kSample);
                    }
                    recorderStop();
                }
                return;
            }
            }
        }

        // ---- the one pad tap (IOP thread) --------------------------------------------------------------------------
        void padTap(uint32_t port, uint32_t slot, ps2x::iop::PadInputState &state)
        {
            if (port != 0u || slot != 0u)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            Run &run = g_run;
            if (run.phase == Run::Phase::Off)
            {
                recorderTap(state);
                return;
            }
            if (run.phase != Run::Phase::Loading && run.phase != Run::Phase::Running)
                return;
            if (!run.polling && ps2x::savestate::loadCount() != run.loadsBefore)
                run.polling = true; // the first poll after the load: sample 0
            if (run.polling)
                runTap(state);
        }
    }

    namespace
    {
        uint32_t g_readPort = ~0u, g_readSlot = ~0u; // scePadRead's arguments (the entry hook)

        // 0x397D20 daddu a3,a0 (scePadRead's first instruction, the hook runs before it): its port and slot.
        void padReadEntry(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            g_readPort = GPR_U32(ctx, 4);
            g_readSlot = GPR_U32(ctx, 5);
        }

        // 0x397D74 daddu a0,s0 (before the copy to the caller, func_3A5148(data, sp, len)): the status the read got is
        // at sp. Recording: keep it. Replay: the recorded one instead.
        void padReadCopy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_readPort != 0u || g_readSlot != 0u)
                return;
            uint8_t *status = rdram + ((GPR_U32(ctx, 29) + kReadFirst) & 0x1FFFFFFu);
            std::lock_guard<std::mutex> lock(g_mutex);
            Run &run = g_run;
            if (run.phase == Run::Phase::Off)
            {
                Recorder &rec = g_rec;
                if (rec.phase != Recorder::Phase::Recording)
                    return;
                if (rec.readFrame != rec.frame)
                {
                    rec.readFrame = rec.frame;
                    rec.readN = 0;
                }
                ReadEntry e;
                e.frame = rec.frame;
                e.n = rec.readN++;
                std::memcpy(e.bytes, status, kReadBytes);
                rec.reads.entries.push_back(e);
                return;
            }
            if (run.phase != Run::Phase::Running || run.reads.entries.empty())
                return;
            if (run.readFrame != run.frame)
            {
                run.readFrame = run.frame;
                run.readN = 0;
            }
            const uint32_t n = run.readN++;
            // The recorded frame at or before this one in time (the other rate: frames x divider), and its read n (or
            // its last).
            const std::vector<ReadEntry> &entries = run.reads.entries;
            const uint64_t now = static_cast<uint64_t>(run.frame) * run.divider;
            while (run.readNext + 1u < entries.size() && static_cast<uint64_t>(entries[run.readNext + 1u].frame) * run.reads.divider <= now)
                ++run.readNext;
            size_t at = run.readNext;
            if (static_cast<uint64_t>(entries[at].frame) * run.reads.divider > now)
                return; // before the first recorded read: leave the poll's
            const uint32_t frame = entries[at].frame;
            while (at > 0u && entries[at - 1u].frame == frame)
                --at; // read 0 of that frame
            size_t pick = at;
            while (pick + 1u < entries.size() && entries[pick + 1u].frame == frame && entries[pick].n < n)
                ++pick;
            std::memcpy(status, entries[pick].bytes, kReadBytes);
        }
    }

    void install(PS2Runtime &, const fs::path &exeDir)
    {
        g_exeDir = exeDir;
        ps2xRegisterHostHook("km_test_pad_read_entry", &padReadEntry);
        ps2xRegisterHostHook("km_test_pad_read_copy", &padReadCopy);
        ps2xSetPatchFlag("test_cases", true); // always: the hooks check the recorder / replay state themselves
        ps2x::pad::setStateTap(&padTap);
        std::string error;
        if (const char *value = std::getenv("KAMITE_REPLAY_CASE"); value && *value)
        {
            const fs::path path = fs::path(value).is_absolute() ? fs::path(value) : exeDir / value;
            if (!readCaseForReplay(path, g_run, error))
            {
                KM_ERROR(Game, "test: " << path.string() << ": " << error);
                g_run.failed = true;
                g_run.phase = Run::Phase::Done;
                return;
            }
            KM_INFO(Game, "test: replaying " << path.string() << ": " << g_run.rec.polls() << " polls recorded at "
                                            << (g_run.rec.divider == 1u ? 60 : 30) << " FPS, log " << g_run.csv.string());
        }
        else if (const char *value = std::getenv("KAMITE_TEST"); value && *value)
        {
            const fs::path path = fs::path(value).is_absolute() ? fs::path(value) : exeDir / value;
            if (!readScript(path, g_run, error))
            {
                KM_ERROR(Game, "test: " << path.string() << ": " << error);
                g_run.failed = true;
                g_run.phase = Run::Phase::Done;
                return;
            }
            KM_INFO(Game, "test: " << path.string() << ": " << (g_run.record ? "record to " : "play ") << g_run.output.string() << ", "
                                   << g_run.vblanks / 2u << " frames at 30 FPS" << (g_run.state.empty() ? "" : ", state " + g_run.state.string())
                                   << ", log " << g_run.csv.string());
        }
        else
            return;
        if (g_run.fps60 >= 0)
            requestFps60(g_run.fps60 != 0);
        g_run.phase = Run::Phase::Boot;
    }

    void onFrame(uint8_t *rdram, R5900Context *, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_mutex); // the tap on the IOP thread waits while a frame is handled
        if (g_run.phase == Run::Phase::Off)
            recorderFrame(rdram);
        else if (g_run.phase != Run::Phase::Done)
            runFrame(rdram, runtime);
    }

    bool failed()
    {
        return g_run.failed;
    }

    bool recording()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_rec.phase != Recorder::Phase::Idle;
    }

    bool replayHost(HostCamera &out)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Run &run = g_run;
        if (run.phase != Run::Phase::Running || run.host.entries.empty())
            return false;
        // Frame counters are taken at the same point at either rate (frame k's update sees k + 1); the other rate's
        // entries are matched by time (frames x divider = vblanks). Camera: the last entry at or before now; a turn:
        // the last one since the previous frame, so none is lost at 30 and none repeats at 60.
        const uint64_t now = static_cast<uint64_t>(run.frame) * run.divider;
        const std::vector<HostEntry> &entries = run.host.entries;
        const HostEntry *camera = nullptr;
        const HostEntry *turn = nullptr;
        while (run.hostNext < entries.size() && static_cast<uint64_t>(entries[run.hostNext].frame) * run.host.divider <= now)
        {
            const HostEntry &e = entries[run.hostNext++];
            camera = &e;
            if (e.camera.turned)
                turn = &e;
        }
        if (!camera)
            camera = run.hostNext > 0 ? &entries[run.hostNext - 1] : &entries.front();
        out = camera->camera;
        out.turned = turn != nullptr;
        out.geneYaw = turn ? turn->camera.geneYaw : 0.0f;
        return true;
    }

    void recordHost(const HostCamera &in)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Recorder &rec = g_rec;
        if (g_run.phase != Run::Phase::Off || rec.phase != Recorder::Phase::Recording)
            return;
        HostEntry e;
        e.frame = rec.frame;
        e.camera = in;
        rec.host.entries.push_back(e);
    }
}
