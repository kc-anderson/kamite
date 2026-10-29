// The test mode: a scripted run the port measures itself (claude/test-mode.md).
//
// KAMITE_TEST=<script>. The script (key = value lines; # comments; paths relative to the exe folder):
//   state   = states\dodge.state   the save state loaded first (none: the run starts at boot)
//   mode    = record | play        record: the pad is written to `input` at the end; play: `input` drives the game
//   input   = tests\dodge.pad      the recording (one line of 32 pad status bytes per game frame, at the recording's rate)
//   frames  = 300                  the length, in 30 FPS game frames (real time: frames / 30 s; at 60 twice the frames)
//   csv     = tests\dodge_60.csv   the per-frame log; the collision pool goes to <csv>.shapes.csv
//   fps60   = 0 | 1                optional: the frame rate for this run (else kamite.ini's)
//   enemies = 4                    optional: how many enemies the row has columns for (list order)
//
// How it runs (all on the game thread, from the frame hook at the top of the main loop): a couple of frames into the
// boot the state is requested (ps2x::savestate), the first frame after it loaded is test frame 0, and after the last
// frame the files are closed and the runtime asked to stop. The pad goes through ps2x::pad's read tap on scePadRead,
// where the host has just filled the 32 status bytes the game will parse: record keeps the last read of each game
// frame; play writes bytes 2..31 of the recording's sample for the same real time over them (status and mode stay the
// game's). A recording made at 30 and played at 60 holds each sample for two game frames, so every input lasts as
// long as it did; the other way round every second sample is used.
#include "kamite/test_mode.h"
#include "kamite/frame_patches.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "runtime/ps2_pad_tap.h"
#include "runtime/ps2_save_state.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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

        struct Script
        {
            fs::path state, input, csv;
            bool record = false;
            uint32_t frames30 = 300u; // in 30 FPS game frames
            int fps60 = -1;
            uint32_t enemies = 4u;
        };

        enum class Phase
        {
            Off,
            Boot,    // waiting for the game loop to settle, then the state is requested
            Loading, // waiting for the state
            Running,
            Done
        };

        Script g_script;
        Phase g_phase = Phase::Off;
        bool g_failed = false;
        uint64_t g_hookFrames = 0;         // frame hooks since boot
        uint32_t g_frame = 0;              // test frame (0 = the first after the state loaded)
        uint32_t g_runFrames = 0;          // frames to run at this rate
        uint32_t g_divider = 2u;           // this run's vblanks per game frame
        uint32_t g_loadsBefore = 0;
        std::chrono::steady_clock::time_point g_loadRequested;
        std::FILE *g_csv = nullptr;
        std::FILE *g_shapes = nullptr;
        // The recording: one sample per game frame at g_recordDivider.
        std::vector<uint8_t> g_samples;
        std::vector<uint8_t> g_haveSample; // record: a read happened in that frame
        uint32_t g_recordDivider = 2u;
        uint32_t g_readsThisFrame = 0, g_readsLastFrame = 0;
        int64_t g_sampleUsed = -1;         // play: the sample index written at the last read

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

        // ---- the script --------------------------------------------------------------------------------------------
        std::string trim(std::string text)
        {
            const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
            text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
            text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
            return text;
        }

        bool readScript(const fs::path &path, const fs::path &exeDir, Script &script, std::string &error)
        {
            std::ifstream in(path);
            if (!in)
            {
                error = "cannot open " + path.string();
                return false;
            }
            const auto resolve = [&](const std::string &value) {
                const fs::path p(value);
                return p.is_absolute() ? p : exeDir / p;
            };
            std::string line;
            int number = 0;
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
                    script.state = resolve(value);
                else if (key == "input")
                    script.input = resolve(value);
                else if (key == "csv")
                    script.csv = resolve(value);
                else if (key == "mode")
                {
                    if (value == "record")
                        script.record = true;
                    else if (value == "play")
                        script.record = false;
                    else
                    {
                        error = "line " + std::to_string(number) + ": mode is record or play";
                        return false;
                    }
                }
                else if (key == "frames")
                    script.frames30 = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                else if (key == "fps60")
                    script.fps60 = value == "1" || value == "true" || value == "on" ? 1 : 0;
                else if (key == "enemies")
                    script.enemies = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
                else
                {
                    error = "line " + std::to_string(number) + ": unknown key " + key;
                    return false;
                }
            }
            if (script.input.empty())
            {
                error = "no input file";
                return false;
            }
            if (script.frames30 == 0u)
            {
                error = "frames must be above 0";
                return false;
            }
            return true;
        }

        // ---- the recording -----------------------------------------------------------------------------------------
        bool loadRecording(const fs::path &path, std::string &error)
        {
            std::ifstream in(path);
            if (!in)
            {
                error = "cannot open " + path.string();
                return false;
            }
            std::string line;
            uint32_t declared = 0;
            g_samples.clear();
            while (std::getline(in, line))
            {
                line = trim(line);
                if (line.empty() || line[0] == '#')
                    continue;
                if (line.rfind("kamite-pad", 0) == 0)
                    continue;
                if (line.rfind("divider", 0) == 0)
                {
                    g_recordDivider = static_cast<uint32_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
                    continue;
                }
                if (line.rfind("frames", 0) == 0)
                {
                    declared = static_cast<uint32_t>(std::strtoul(line.c_str() + 6, nullptr, 10));
                    continue;
                }
                if (line.size() != kSample * 2u)
                {
                    error = path.string() + ": a sample line has " + std::to_string(line.size()) + " characters, not 64";
                    return false;
                }
                for (uint32_t i = 0; i < kSample; ++i)
                {
                    const std::string byte = line.substr(i * 2u, 2u);
                    g_samples.push_back(static_cast<uint8_t>(std::strtoul(byte.c_str(), nullptr, 16)));
                }
            }
            if (g_recordDivider != 1u && g_recordDivider != 2u)
            {
                error = path.string() + ": divider must be 1 or 2";
                return false;
            }
            if (g_samples.empty())
            {
                error = path.string() + ": no samples";
                return false;
            }
            if (declared != 0u && declared != g_samples.size() / kSample)
                KM_WARN(Game, "test: " << path.string() << " declares " << declared << " frames, has " << g_samples.size() / kSample);
            return true;
        }

        bool saveRecording(const fs::path &path)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            std::FILE *out = std::fopen(path.string().c_str(), "wb");
            if (!out)
                return false;
            const uint32_t frames = static_cast<uint32_t>(g_samples.size() / kSample);
            std::fprintf(out, "kamite-pad 1\ndivider %u\nframes %u\n", g_recordDivider, frames);
            for (uint32_t f = 0; f < frames; ++f)
            {
                for (uint32_t i = 0; i < kSample; ++i)
                    std::fprintf(out, "%02X", g_samples[f * kSample + i]);
                std::fputc('\n', out);
            }
            std::fclose(out);
            return true;
        }

        // The sample a game frame of this run takes: the one covering the same real time in the recording.
        uint32_t sampleFor(uint32_t frame)
        {
            const uint64_t index = (static_cast<uint64_t>(frame) * g_divider) / g_recordDivider;
            const uint64_t last = g_samples.size() / kSample - 1u;
            return static_cast<uint32_t>(index < last ? index : last);
        }

        void padTap(int port, int, uint8_t *status)
        {
            if (port != 0 || g_phase != Phase::Running)
                return;
            ++g_readsThisFrame;
            if (g_script.record)
            {
                if (g_frame < g_haveSample.size())
                {
                    std::memcpy(g_samples.data() + static_cast<size_t>(g_frame) * kSample, status, kSample);
                    g_haveSample[g_frame] = 1u;
                }
                return;
            }
            const uint32_t index = sampleFor(g_frame);
            std::memcpy(status + 2, g_samples.data() + static_cast<size_t>(index) * kSample + 2u, kSample - 2u);
            g_sampleUsed = index;
        }

        // ---- the log -----------------------------------------------------------------------------------------------
        std::FILE *openOut(const fs::path &path)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            return std::fopen(path.string().c_str(), "wb");
        }

        void writeHeaders()
        {
            std::fputs("frame,div,reads,sample,buttons,lx,ly,rx,ry,held,room,stp,sta,hitstop,"
                       "pl_x,pl_y,pl_z,pl_heading,pl_mode,pl_phase,pl_step,pl_arg,pl_motion,pl_mframe,pl_mleft,pl_seq,pl_hp,pl_guard,pl_speed,"
                       "pl_w568,pl_w5F8,pl_w5FC,pl_b61D,pl_b621,pl_b622,pl_b623,pl_b624,pl_b625,pl_b626,pl_b627,pl_b628,pl_h648,"
                       "pl_f15D0,pl_h15F4,pl_b1600,pl_b1601,pl_b1602,pl_b1603,pl_h1616,pl_h1618,pl_hurt,"
                       "shapes,active,attack_on,hurt_on,enemies",
                       g_csv);
            for (uint32_t i = 0; i < g_script.enemies; ++i)
                std::fprintf(g_csv, ",em%u_addr,em%u_kind,em%u_hp,em%u_mode,em%u_phase,em%u_step,em%u_motion,em%u_mframe,"
                                    "em%u_x,em%u_y,em%u_z,em%u_heading,em%u_speed,em%u_guard,em%u_flags",
                             i, i, i, i, i, i, i, i, i, i, i, i, i, i, i);
            std::fputc('\n', g_csv);
            std::fputs("frame,rec,group,active,kind,x,y,z,a,b,c,owner\n", g_shapes);
        }

        // Gene's take unit: the pool records of its nodes, in list order (dodge probe, debug_tools.cpp).
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

        void logFrame(const uint8_t *rdram)
        {
            // The pad as the game saw it in the previous frame (record: the sample kept; play: the sample written).
            const uint8_t *sample = nullptr;
            if (g_script.record)
            {
                if (g_frame > 0u && g_haveSample[g_frame - 1u])
                    sample = g_samples.data() + static_cast<size_t>(g_frame - 1u) * kSample;
            }
            else if (g_sampleUsed >= 0)
                sample = g_samples.data() + static_cast<size_t>(g_sampleUsed) * kSample;
            const unsigned buttons = sample ? (~(sample[2] | (sample[3] << 8)) & 0xFFFFu) : 0u;
            std::fprintf(g_csv, "%u,%u,%u,%lld,%04X,%u,%u,%u,%u,%08X,%u,%08X,%08X,%u", g_frame, g_divider, g_readsLastFrame,
                         static_cast<long long>(g_sampleUsed), buttons, sample ? sample[6] : 0x80u, sample ? sample[7] : 0x80u,
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
                std::fprintf(g_csv, ",%.3f,%.3f,%.3f,%.4f,%u,%u,%u,%u,%X,%.3f,%.3f,%04X,%d,%.3f,%.3f", x, y, z, heading,
                             rd8(rdram, pl + GameObj::kMode), rd8(rdram, pl + GameObj::kMode + 1u), rd8(rdram, pl + GameObj::kMode + 2u),
                             rd8(rdram, pl + GameObj::kMode + 3u), rd32(rdram, pl + kObjMotion), rdf(rdram, pl + kObjMotionFrame),
                             rdf(rdram, pl + kObjMotionLeft), rd16(rdram, pl + kObjSeq), rds16(rdram, pl + kObjHp), rdf(rdram, pl + kObjGuard),
                             rdf(rdram, pl + kObjSpeedRate));
                std::fprintf(g_csv, ",%u,%u,%u,%u", rd32(rdram, pl + 0x568u), rd32(rdram, pl + 0x5F8u), rd32(rdram, pl + 0x5FCu),
                             rd8(rdram, pl + 0x61Du));
                for (uint32_t off = 0x621u; off <= 0x628u; ++off)
                    std::fprintf(g_csv, ",%u", rd8(rdram, pl + off));
                std::fprintf(g_csv, ",%u,%.3f,%04X,%u,%u,%u,%u,%u,%u,%s", rd16(rdram, pl + 0x648u), rdf(rdram, pl + 0x15D0u),
                             rd16(rdram, pl + 0x15F4u), rd8(rdram, pl + 0x1600u), rd8(rdram, pl + 0x1601u), rd8(rdram, pl + 0x1602u),
                             rd8(rdram, pl + 0x1603u), rd16(rdram, pl + 0x1616u), rd16(rdram, pl + 0x1618u), plHurt.c_str());
            }
            else
                for (int i = 0; i < 37; ++i) // the player columns
                    std::fputc(',', g_csv);

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
                    std::fprintf(g_shapes, "%u,%u,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%s\n", g_frame, i, group, on ? 1u : 0u, kind, x, y, z,
                                 a, kind == 0u ? 0.0f : b, kind == 4u ? c : 0.0f, owner);
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
            std::fprintf(g_csv, ",%u,%u,%u,%u,%u", used, active, attackOn, hurtOn, static_cast<unsigned>(enemies.size()));
            for (uint32_t i = 0; i < g_script.enemies; ++i)
            {
                if (i >= enemies.size())
                {
                    for (int k = 0; k < 15; ++k) // the enemy's columns
                        std::fputc(',', g_csv);
                    continue;
                }
                const uint32_t em = enemies[i];
                float x, y, z, heading;
                objectPosition(rdram, em, x, y, z, heading);
                std::fprintf(g_csv, ",%X,%03X,%d,%u,%u,%u,%X,%.3f,%.3f,%.3f,%.3f,%.4f,%.3f,%.3f,%08X", em, rd16(rdram, em + kObjKind),
                             rds16(rdram, em + kObjHp), rd8(rdram, em + GameObj::kMode), rd8(rdram, em + GameObj::kMode + 1u),
                             rd8(rdram, em + GameObj::kMode + 2u), rd32(rdram, em + kObjMotion), rdf(rdram, em + kObjMotionFrame), x, y, z,
                             heading, rdf(rdram, em + kObjSpeedRate), rdf(rdram, em + kObjGuard), rd32(rdram, em + kObjFlags));
            }
            std::fputc('\n', g_csv);
        }

        void finish(PS2Runtime *runtime, bool ok)
        {
            g_phase = Phase::Done;
            ps2x::pad::setReadTap(nullptr);
            if (ok && g_script.record)
            {
                // Frames without a read keep the previous frame's sample.
                for (size_t f = 1; f < g_haveSample.size(); ++f)
                    if (!g_haveSample[f])
                        std::memcpy(g_samples.data() + f * kSample, g_samples.data() + (f - 1u) * kSample, kSample);
                if (saveRecording(g_script.input))
                    KM_INFO(Game, "test: recorded " << g_haveSample.size() << " frames (divider " << g_recordDivider << ") to " << g_script.input.string());
                else
                {
                    KM_ERROR(Game, "test: cannot write " << g_script.input.string());
                    ok = false;
                }
            }
            if (g_csv)
                std::fclose(g_csv);
            if (g_shapes)
                std::fclose(g_shapes);
            g_csv = g_shapes = nullptr;
            if (!ok)
                g_failed = true;
            KM_INFO(Game, "test: " << (ok ? "done" : "failed") << " after " << g_frame << " frames; stopping the run");
            if (runtime)
                runtime->requestStop();
        }
    }

    void install(PS2Runtime &, const fs::path &exeDir)
    {
        const char *value = std::getenv("KAMITE_TEST");
        if (!value || !*value)
            return;
        const fs::path scriptPath = fs::path(value).is_absolute() ? fs::path(value) : exeDir / value;
        std::string error;
        if (!readScript(scriptPath, exeDir, g_script, error))
        {
            KM_ERROR(Game, "test: " << scriptPath.string() << ": " << error);
            g_failed = true;
            return;
        }
        if (g_script.csv.empty())
            g_script.csv = g_script.input.parent_path() / (g_script.input.stem().string() + (g_script.record ? "_record" : "_play") + ".csv");
        if (!g_script.record && !loadRecording(g_script.input, error))
        {
            KM_ERROR(Game, "test: " << error);
            g_failed = true;
            return;
        }
        if (g_script.fps60 >= 0)
            requestFps60(g_script.fps60 != 0);
        g_phase = Phase::Boot;
        KM_INFO(Game, "test: " << scriptPath.string() << ": " << (g_script.record ? "record to " : "play ") << g_script.input.string()
                               << ", " << g_script.frames30 << " frames at 30 FPS" << (g_script.state.empty() ? "" : ", state " + g_script.state.string())
                               << ", log " << g_script.csv.string());
    }

    void onFrame(uint8_t *rdram, R5900Context *, PS2Runtime *runtime)
    {
        if (g_phase == Phase::Off || g_phase == Phase::Done)
            return;
        ++g_hookFrames;
        switch (g_phase)
        {
        case Phase::Boot:
            if (g_hookFrames < 3u) // the loop is up; a frame rate switch from the script has been applied
                return;
            if (g_script.state.empty())
            {
                g_phase = Phase::Loading; // falls into the start below on this frame
                g_loadsBefore = ps2x::savestate::loadCount() - 1u;
            }
            else
            {
                std::error_code ec;
                if (!fs::exists(g_script.state, ec))
                {
                    KM_ERROR(Game, "test: no state " << g_script.state.string());
                    finish(runtime, false);
                    return;
                }
                g_loadsBefore = ps2x::savestate::loadCount();
                ps2x::savestate::requestLoad(g_script.state);
                g_loadRequested = std::chrono::steady_clock::now();
                g_phase = Phase::Loading;
                return;
            }
            [[fallthrough]];
        case Phase::Loading:
        {
            if (ps2x::savestate::loadCount() == g_loadsBefore)
            {
                if (!g_script.state.empty() &&
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - g_loadRequested).count() > kLoadTimeoutSeconds)
                {
                    KM_ERROR(Game, "test: the state did not load within " << kLoadTimeoutSeconds << " s");
                    finish(runtime, false);
                }
                return;
            }
            // Loaded: this is test frame 0.
            g_divider = fps60() ? 1u : 2u;
            g_runFrames = g_script.frames30 * 2u / g_divider;
            g_frame = 0;
            g_readsThisFrame = g_readsLastFrame = 0;
            g_sampleUsed = -1;
            if (g_script.record)
            {
                g_recordDivider = g_divider;
                g_samples.assign(static_cast<size_t>(g_runFrames) * kSample, 0u);
                g_haveSample.assign(g_runFrames, 0u);
                // A frame with no read keeps a neutral sample (buttons all off, sticks centred).
                for (uint32_t f = 0; f < g_runFrames; ++f)
                {
                    uint8_t *s = g_samples.data() + static_cast<size_t>(f) * kSample;
                    s[2] = s[3] = 0xFFu;
                    s[4] = s[5] = s[6] = s[7] = 0x80u;
                }
            }
            g_csv = openOut(g_script.csv);
            g_shapes = openOut(g_script.csv.string() + ".shapes.csv");
            if (!g_csv || !g_shapes)
            {
                KM_ERROR(Game, "test: cannot write " << g_script.csv.string());
                finish(runtime, false);
                return;
            }
            writeHeaders();
            ps2x::pad::setReadTap(&padTap);
            g_phase = Phase::Running;
            KM_INFO(Game, "test: running " << g_runFrames << " frames at " << (g_divider == 1u ? 60 : 30) << " FPS"
                                           << (g_script.record ? " (recording the pad)" : " (playing the recording)"));
            [[fallthrough]];
        }
        case Phase::Running:
            g_readsLastFrame = g_readsThisFrame;
            g_readsThisFrame = 0;
            logFrame(rdram);
            if (++g_frame >= g_runFrames)
                finish(runtime, true);
            return;
        default:
            return;
        }
    }

    bool failed()
    {
        return g_failed;
    }
}
