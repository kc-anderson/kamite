// The test mode: cases the testers record in play, runs the port measures itself (claude/test-mode.md).
//
// Three things live here, sharing one per-frame logger and one pad path:
//
// 1. The case recorder (always on; the RecordCase hotkey, R3 by default): press once and the port saves a state of
//    its own (never the players' slots) and starts keeping the pad; press again and everything goes into one file,
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
// The pad goes through ps2x::pad's read tap on scePadRead, where the host has just filled the 32 status bytes the game
// parses: the recorder keeps the last read of each game frame; a replay writes bytes 2..31 of the recording's sample
// for the same real time over them (status and mode stay the game's). A recording made at 30 and played at 60 holds
// each sample for two game frames, so every input lasts as long as it did; one made at 60 and played at 30 merges each
// pair of samples (a button pressed in either counts, the sticks take the later), so no tap is lost.
#include "kamite/test_mode.h"
#include "kamite/frame_patches.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_hotkeys.h"
#include "runtime/ps2_pad_tap.h"
#include "runtime/ps2_save_state.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
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

        constexpr uint32_t kSample = 32u;                     // scePadRead's status buffer
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
        struct Recording
        {
            std::vector<uint8_t> samples; // frames x kSample
            uint32_t divider = 2u;        // the recording's vblanks per game frame
            uint32_t frames() const { return static_cast<uint32_t>(samples.size() / kSample); }
        };

        bool parseRecording(const std::string &text, Recording &rec, std::string &error)
        {
            rec.samples.clear();
            std::istringstream in(text);
            std::string line;
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.empty() || line[0] == '#' || line.rfind("kamite-pad", 0) == 0 || line.rfind("frames", 0) == 0)
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
            return true;
        }

        std::string formatRecording(const Recording &rec)
        {
            std::string out = "kamite-pad 1\ndivider " + std::to_string(rec.divider) + "\nframes " + std::to_string(rec.frames()) + "\n";
            char hex[3];
            for (uint32_t f = 0; f < rec.frames(); ++f)
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
                std::string *target = name == "meta" ? &c.meta : name == "state" ? &c.state : name == "pad" ? &c.pad : name == "live" ? &c.live : nullptr;
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
            std::vector<uint8_t> haveSample;
            Logger log;

            uint64_t hookFrames = 0;
            uint32_t frame = 0, runFrames = 0, divider = 2u;
            uint32_t loadsBefore = 0;
            std::chrono::steady_clock::time_point loadRequested;
            uint32_t readsThisFrame = 0, readsLastFrame = 0;
            int64_t sampleUsed = -1;
            uint8_t merged[kSample] = {};
        };
        Run g_run;

        // The sample a game frame of this run takes: the one covering the same real time in the recording. Playing a
        // 60 recording at 30: the pair of samples merged (a press in either counts, the sticks from the later one).
        const uint8_t *sampleFor(Run &run, uint32_t frame)
        {
            const uint64_t index = (static_cast<uint64_t>(frame) * run.divider) / run.rec.divider;
            const uint64_t last = run.rec.frames() - 1u;
            const uint64_t first = index < last ? index : last;
            run.sampleUsed = static_cast<int64_t>(first);
            if (run.divider <= run.rec.divider)
                return run.rec.samples.data() + static_cast<size_t>(first) * kSample;
            const uint64_t second = first + 1u < last ? first + 1u : last;
            const uint8_t *a = run.rec.samples.data() + static_cast<size_t>(first) * kSample;
            const uint8_t *b = run.rec.samples.data() + static_cast<size_t>(second) * kSample;
            std::memcpy(run.merged, b, kSample);
            run.merged[2] = static_cast<uint8_t>(a[2] & b[2]); // buttons are active low: pressed in either stays pressed
            run.merged[3] = static_cast<uint8_t>(a[3] & b[3]);
            for (uint32_t i = 8; i < 20; ++i)                  // pressures: the larger
                run.merged[i] = std::max(a[i], b[i]);
            return run.merged;
        }

        void runTap(uint8_t *status)
        {
            Run &run = g_run;
            ++run.readsThisFrame;
            if (run.record)
            {
                if (run.frame < run.haveSample.size())
                {
                    std::memcpy(run.rec.samples.data() + static_cast<size_t>(run.frame) * kSample, status, kSample);
                    run.haveSample[run.frame] = 1u;
                }
                return;
            }
            std::memcpy(status + 2, sampleFor(run, run.frame) + 2, kSample - 2u);
        }

        void finishRun(PS2Runtime *runtime, bool ok)
        {
            Run &run = g_run;
            run.phase = Run::Phase::Done;
            if (ok && run.record)
            {
                for (size_t f = 1; f < run.haveSample.size(); ++f) // frames without a read keep the previous sample
                    if (!run.haveSample[f])
                        std::memcpy(run.rec.samples.data() + f * kSample, run.rec.samples.data() + (f - 1u) * kSample, kSample);
                if (writeFile(run.output, formatRecording(run.rec)))
                    KM_INFO(Game, "test: recorded " << run.rec.frames() << " frames (divider " << run.rec.divider << ") to " << run.output.string());
                else
                {
                    KM_ERROR(Game, "test: cannot write " << run.output.string());
                    ok = false;
                }
            }
            closeLogger(run.log);
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
                run.readsThisFrame = run.readsLastFrame = 0;
                run.sampleUsed = -1;
                if (run.record)
                {
                    run.rec.divider = run.divider;
                    run.rec.samples.assign(static_cast<size_t>(run.runFrames) * kSample, 0u);
                    run.haveSample.assign(run.runFrames, 0u);
                    for (uint32_t f = 0; f < run.runFrames; ++f) // neutral until a read happens
                    {
                        uint8_t *s = run.rec.samples.data() + static_cast<size_t>(f) * kSample;
                        s[2] = s[3] = 0xFFu;
                        s[4] = s[5] = s[6] = s[7] = 0x80u;
                    }
                }
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
                run.phase = Run::Phase::Running;
                KM_INFO(Game, "test: running " << run.runFrames << " frames at " << (run.divider == 1u ? 60 : 30) << " FPS"
                                               << (run.record ? " (recording the pad)" : " (playing the recording)"));
                [[fallthrough]];
            }
            case Run::Phase::Running:
            {
                run.readsLastFrame = run.readsThisFrame;
                run.readsThisFrame = 0;
                const uint8_t *sample = nullptr;
                if (run.record)
                {
                    if (run.frame > 0u && run.haveSample[run.frame - 1u])
                        sample = run.rec.samples.data() + static_cast<size_t>(run.frame - 1u) * kSample;
                }
                else if (run.sampleUsed >= 0)
                    sample = run.divider > run.rec.divider ? run.merged : run.rec.samples.data() + static_cast<size_t>(run.sampleUsed) * kSample;
                logFrame(run.log, rdram, run.frame, run.divider, run.readsLastFrame, run.record ? -1 : run.sampleUsed, sample);
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
            run.vblanks = run.rec.frames() * run.rec.divider;
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
            Recording rec;
            Logger log;
            uint8_t lastRead[kSample] = {};
            bool haveRead = false;
            uint32_t readsThisFrame = 0, readsLastFrame = 0;
            uint16_t prevPressed = 0;   // the pad's pressed bits at the last read (for the hotkey's edge)
            bool toggle = false;        // a press of the hotkey, taken at the next frame hook
            uint32_t casesWritten = 0;
        };
        Recorder g_rec;

        void recorderTap(uint8_t *status)
        {
            Recorder &rec = g_rec;
            const uint16_t pressed = static_cast<uint16_t>(~(status[2] | (status[3] << 8)) & 0xFFFFu);
            const uint16_t padBit = ps2x::hotkeys::binding(ps2x::hotkeys::Action::RecordCase).pad;
            if (padBit != 0u && (pressed & padBit) != 0u && (rec.prevPressed & padBit) == 0u)
                rec.toggle = true;
            rec.prevPressed = pressed;
            ++rec.readsThisFrame;
            std::memcpy(rec.lastRead, status, kSample);
            rec.haveRead = true;
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
            c.meta = "created=" + rec.stamp + "\ndivider=" + std::to_string(rec.divider) + "\nframes=" + std::to_string(rec.rec.frames()) +
                     "\nseconds=" + std::to_string(static_cast<double>(rec.rec.frames() * rec.divider) / 60.0) + "\nroom=0x";
            char hex[8];
            std::snprintf(hex, sizeof(hex), "%03X", rec.room);
            c.meta += hex;
            c.meta += "\nenemies=" + std::to_string(kDefaultEnemies) + "\n";
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
            ++rec.casesWritten;
            ps2x::menu::notify("Case saved: " + rec.stamp + " (" + std::to_string(rec.rec.frames()) + " frames)", 4.0f);
            KM_INFO(Game, "case: wrote " << caseFile.string() << ": " << rec.rec.frames() << " frames at " << (rec.divider == 1u ? 60 : 30)
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
                // Saved: this is frame 0 of the case.
                rec.divider = fps60() ? 1u : 2u;
                rec.rec.divider = rec.divider;
                rec.rec.samples.clear();
                rec.frame = 0;
                rec.haveRead = false;
                rec.readsThisFrame = rec.readsLastFrame = 0;
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
                // The sample of the previous frame: what the game saw (no read: the frame before's, or neutral).
                if (rec.frame > 0u)
                {
                    if (!rec.haveRead)
                    {
                        if (rec.rec.samples.empty())
                        {
                            std::memset(rec.lastRead, 0, kSample);
                            rec.lastRead[2] = rec.lastRead[3] = 0xFFu;
                            rec.lastRead[4] = rec.lastRead[5] = rec.lastRead[6] = rec.lastRead[7] = 0x80u;
                        }
                        else
                            std::memcpy(rec.lastRead, rec.rec.samples.data() + rec.rec.samples.size() - kSample, kSample);
                    }
                    rec.rec.samples.insert(rec.rec.samples.end(), rec.lastRead, rec.lastRead + kSample);
                    rec.haveRead = false;
                }
                logFrame(rec.log, rdram, rec.frame, rec.divider, rec.readsLastFrame, -1, rec.frame > 0u ? rec.lastRead : nullptr);
                ++rec.frame;
                if (rec.frame % 60u == 0u)
                {
                    char text[64];
                    std::snprintf(text, sizeof(text), "Recording case... %.0f s (press again to stop)", static_cast<double>(rec.frame * rec.divider) / 60.0);
                    ps2x::menu::notify(text, 1.6f);
                }
                if (toggle || rec.frame * rec.divider >= kMaxCaseVblanks)
                {
                    if (toggle && rec.rec.frames() > 1u) // the stopping press is the last sample: keep it out
                        rec.rec.samples.resize(rec.rec.samples.size() - kSample);
                    recorderStop();
                }
                return;
            }
            }
        }

        // ---- the one pad tap ---------------------------------------------------------------------------------------
        void padTap(int port, int, uint8_t *status)
        {
            if (port != 0)
                return;
            if (g_run.phase == Run::Phase::Running)
                runTap(status);
            else if (g_run.phase == Run::Phase::Off)
                recorderTap(status);
        }
    }

    void install(PS2Runtime &, const fs::path &exeDir)
    {
        g_exeDir = exeDir;
        ps2x::pad::setReadTap(&padTap);
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
            KM_INFO(Game, "test: replaying " << path.string() << ": " << g_run.rec.frames() << " frames recorded at "
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
        return g_rec.phase != Recorder::Phase::Idle;
    }
}
