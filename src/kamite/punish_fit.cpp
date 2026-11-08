// Buck Breaker's punish fits (K, Nov 8): a punish made for one body leaves Gene too far from (or into) another, e.g.
// Stinger, made for the fat men, leaves a gap on a thug. Per punish and per body: Forward / Side / Turn for Gene.
//
// Where the distance comes from. Each paired punish places Gene once at its start (Stinger again at two later stages)
// with func_001299F0(a0 = Gene, a1 = the enemy, a2 = &vec{x, 0, z, 1} on the caller's stack, a3 = mode, f12 = yaw):
// mode 0 turns Gene to face the enemy, the enemy's heading (+0x104) = Gene's + pi, then Gene's += f12; mode 1 only sets
// the enemy's heading = Gene's + pi. Then Gene's position = enemy.pos + RotY(enemy heading) . (x * enemy +0x110, 0,
// z * enemy +0x118) (the enemy's size). After that each of the two moves by its own motion's root travel
// (func_0014F928 / 0014F9F0: their own +0x330 / +0x360 into their own position), never by the other's position, so
// Gene moved in the middle of the punish stays moved: a slider changed while it plays moves him at once.
//
// The wrapper (FunctionHooks::replace on 0x1299F0, only for the calls of the four punishes, by their return address):
// Turn goes in f12 (mode 0), or for mode 1 is taken off Gene's heading for the call and put back after (the enemy's
// heading stays relative to Gene's facing without it); then Gene is moved by RotY(his facing without Turn) .
// (Side * enemy +0x110, 0, Forward * enemy +0x118). RotY as the game's (sceVu0RotMatrixY, func_0030A7B8):
// x' = x cos + z sin, z' = -x sin + z cos; +z = forward, +x = to the left (npc_poses' sliders), + yaw = turning left.
//
// The body: the enemy's skeleton (its cModel's parts, list at +0x144, count at +0x2B4; each part's offset +0xE0 and
// parent +0x148, as npc_poses reads them), hashed from the parts after the root (the root's offset is the motion's
// hip): recolors share it, and a bigger copy of the same skeleton too (the values are scaled by the enemy's size like
// the game's own). Hashed at each prompt and placement; the file keeps which enemies were seen with each body.
//
// "Doesn't fit": Buck Breaker's case hook asks doesntFit() and keeps the enemy's own punish for that pick and body.

#include "kamite/punish_fit.h"

#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_time_control.h"
#include "pcsx2gs_api.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace kamite::punish_fit
{
    namespace
    {
        namespace work = game::GameWork;

        constexpr uint32_t kAlign = 0x001299F0u;
        constexpr float kPi = 3.14159265358979f, kDegrees = kPi / 180.0f;

        // The four punishes' calls of func_001299F0, by return address (Gene's phases 57 / 59 / 61 / 62).
        struct Call
        {
            uint32_t ra;
            Punish punish;
        };
        constexpr Call kCalls[] = {{0x0011770Cu, Suplex},     {0x00117AC8u, Stinger}, {0x00117B90u, Stinger},
                                   {0x00117E40u, Stinger},    {0x0011825Cu, CobraTwist}, {0x00118790u, Spanking}};
        constexpr uint8_t kGenePhase[kPunishCount] = {57u, 59u, 61u, 62u};
        const char *const kNames[kPunishCount] = {"Suplex", "Stinger", "Cobra Twist", "Spanking"};
        const char *const kFileNames[kPunishCount] = {"Suplex", "Stinger", "CobraTwist", "Spanking"};

        // func_002DCD50's switch (buck_breaker.cpp): the enemy's own punish, to tell the game's own pairs.
        constexpr uint32_t kCaseTable = 0x0044D430u, kCases = 0x7Au, kSuplexCase = 0x002DD050u;

        constexpr uint32_t kNumber = 0x564u, kScaleX = 0x110u, kScaleZ = 0x118u, kCaught = 0x694u;
        constexpr uint32_t kParts = 0x144u, kPartCount = 0x2B4u, kPartOffset = 0xE0u, kPartParent = 0x148u;

        struct Fit
        {
            float forward = 0.0f; // metres (at size 1), + = Gene steps in
            float side = 0.0f;    // metres, + = to Gene's left
            float turn = 0.0f;    // degrees, + = Gene turns to his left
            bool off = false;     // doesn't fit: Buck Breaker keeps the enemy's own punish
            [[nodiscard]] bool empty() const { return forward == 0.0f && side == 0.0f && turn == 0.0f && !off; }
        };

        struct Pair
        {
            int punish = -1;
            uint32_t gene = 0, enemy = 0, body = 0;
            uint16_t number = 0;
            bool native = false; // the enemy's own punish
            bool live = false;   // still playing
            Fit applied;         // what Gene was placed with (live changes move him by the difference)
        };

        uint8_t *g_ram = nullptr;
        bool g_on = false;
        PS2Runtime::RecompiledFunction g_align = nullptr;
        fs::path g_file, g_shippedFile;

        std::mutex g_mutex; // the fits, the bodies seen and the pair (EE thread and the menu)
        std::map<uint64_t, Fit> g_fits;                   // punish << 32 | body
        std::map<uint32_t, std::set<uint16_t>> g_seen;    // body -> enemy numbers met with it
        Pair g_pair;

        // The menu's slider state (menu thread).
        bool g_dragging = false;
        std::optional<std::chrono::steady_clock::time_point> g_stepCommit;
        std::string g_status;

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
        // offset's bits. 0 when it cannot be read.
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

        // EE thread, g_mutex held. The enemy's body, noted as seen with its number. Not cached by number: the model an
        // enemy number loads is not known to be fixed (the em files do not follow the numbers: em1b.dat holds a man
        // while 0x21B is one of the women), and the hash is cheap (asked at a prompt and at a placement only).
        uint32_t bodyOf(const uint8_t *rdram, uint32_t enemy, uint16_t &number)
        {
            number = static_cast<uint16_t>(rd32(rdram, enemy + kNumber));
            uint32_t body = skeletonHash(rdram, enemy);
            if (!body)
                body = 0x80000000u | number; // no skeleton to read: the enemy on its own
            g_seen[body].insert(number);
            return body;
        }

        int ownPunish(const uint8_t *rdram, uint16_t number)
        {
            const uint32_t index = static_cast<uint32_t>(number) - 0x205u;
            const uint32_t target = index < kCases ? rd32(rdram, kCaseTable + 4u * index) : kSuplexCase;
            switch (target)
            {
            case kSuplexCase:
            case 0x002DD17Cu: // the coin flip: Suplex or Pummel
                return Suplex;
            case 0x002DD090u:
                return Stinger;
            case 0x002DD22Cu:
                return CobraTwist;
            case 0x002DD148u:
                return Spanking;
            default:
                return -1;
            }
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
            int punish = -1;
            for (const Call &call : kCalls)
                if (call.ra == ra)
                    punish = call.punish;
            const uint32_t gene = GPR_U32(ctx, 4) & 0x1FFFFFFu, enemy = GPR_U32(ctx, 5) & 0x1FFFFFFu;
            if (punish < 0 || !inRam(gene, 0x700u) || !inRam(enemy, 0x700u))
            {
                g_align(rdram, ctx, runtime);
                return;
            }
            const uint32_t mode = GPR_U32(ctx, 7);
            Fit fit;
            uint32_t body;
            uint16_t number;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                body = bodyOf(rdram, enemy, number);
                if (const auto it = g_fits.find(fitKey(punish, body)); it != g_fits.end())
                    fit = it->second;
            }
            const float turn = fit.turn * kDegrees;
            if (mode == 1u)
            {
                if (turn != 0.0f)
                    wrF(rdram, gene + 0x104u, wrap(rdF(rdram, gene + 0x104u) - turn));
            }
            else
                ctx->f[12] += turn;
            g_align(rdram, ctx, runtime);
            const float facing = wrap(rdF(rdram, gene + 0x104u) - (mode == 1u ? 0.0f : turn));
            moveGene(rdram, gene, enemy, fit.side, fit.forward, mode == 1u ? turn : 0.0f, facing);

            std::lock_guard<std::mutex> lock(g_mutex);
            const bool same = g_pair.live && g_pair.gene == gene && g_pair.enemy == enemy && g_pair.punish == punish;
            if (!same)
                KM_INFO(Game, "punish fit: " << kNames[punish] << " on body " << std::hex << body << std::dec << " ("
                                             << enemyName(number) << "): forward " << fit.forward << " side " << fit.side
                                             << " turn " << fit.turn);
            g_pair = Pair{punish, gene, enemy, body, number, ownPunish(rdram, number) == punish, true, fit};
        }

        // EE thread, each vblank: a fit changed while its punish plays moves Gene by the difference.
        void onVblank()
        {
            if (!g_ram)
                return;
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_pair.live)
                return;
            const uint32_t gene = rd32(g_ram, work::address(work::kPlayerObj)) & 0x1FFFFFFu;
            if (gene != g_pair.gene || !inRam(gene, 0x700u) || g_ram[gene + game::GameObj::kMode] != 0u ||
                g_ram[gene + game::GameObj::kMode + 1u] != kGenePhase[g_pair.punish] ||
                (rd32(g_ram, gene + kCaught) & 0x1FFFFFFu) != g_pair.enemy)
            {
                g_pair.live = false;
                return;
            }
            Fit now;
            if (const auto it = g_fits.find(fitKey(g_pair.punish, g_pair.body)); it != g_fits.end())
                now = it->second;
            const Fit &was = g_pair.applied;
            if (now.forward == was.forward && now.side == was.side && now.turn == was.turn)
                return;
            const float facing = wrap(rdF(g_ram, gene + 0x104u) - was.turn * kDegrees);
            moveGene(g_ram, gene, g_pair.enemy, now.side - was.side, now.forward - was.forward,
                     (now.turn - was.turn) * kDegrees, facing);
            g_pair.applied = now;
        }

        void load()
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_fits.clear();
            g_seen.clear();
            // The player's own file; without one, the fits the port ships. Save writes the player's file.
            std::error_code error;
            const fs::path &file = fs::exists(g_file, error) ? g_file : g_shippedFile;
            std::ifstream in(file);
            std::string line;
            while (std::getline(in, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream fields(line);
                std::string kind, body;
                if (!(fields >> kind >> body))
                    continue;
                const uint32_t key = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16));
                if (!key)
                    continue;
                if (kind == "body")
                {
                    std::string name;
                    auto &seen = g_seen[key];
                    while (fields >> name)
                        if (name.size() == 4 && name.compare(0, 2, "em") == 0)
                            seen.insert(static_cast<uint16_t>(0x200u + std::strtoul(name.c_str() + 2, nullptr, 16)));
                }
                else
                {
                    int punish = -1;
                    for (int i = 0; i < kPunishCount; ++i)
                        if (kind == kFileNames[i])
                            punish = i;
                    Fit fit;
                    int off = 0;
                    if (punish < 0 || !(fields >> fit.forward >> fit.side >> fit.turn >> off))
                        continue;
                    fit.off = off != 0;
                    g_fits[fitKey(punish, key)] = fit;
                }
            }
            if (!g_fits.empty())
                KM_INFO(Game, "punish fit: " << g_fits.size() << " fits from " << file.filename().string()
                                             << (file == g_shippedFile ? " (shipped)" : ""));
        }

        bool save()
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            std::ofstream out(g_file, std::ios::binary | std::ios::trunc);
            if (!out)
                return false;
            out << "# Kamite's punish fits for Buck Breaker (F2 > Debug > Punish fit writes this file).\r\n"
                   "# body <skeleton> <enemies met with it>\r\n"
                   "# <punish> <skeleton> forward (m, + = Gene steps in) side (m, + = his left) turn (degrees, + = to his\r\n"
                   "# left) doesn't fit (1 = Buck Breaker keeps the enemy's own punish). Metres at size 1 (scaled by the\r\n"
                   "# enemy's size like the game's own placement).\r\n";
            char text[160];
            for (const auto &[body, numbers] : g_seen)
            {
                std::snprintf(text, sizeof(text), "body %08x", body);
                out << text;
                for (uint16_t number : numbers)
                    out << ' ' << enemyName(number);
                out << "\r\n";
            }
            for (const auto &[key, fit] : g_fits)
            {
                if (fit.empty())
                    continue;
                std::snprintf(text, sizeof(text), "%s %08x %.3f %.3f %.1f %d\r\n", kFileNames[key >> 32],
                              static_cast<uint32_t>(key), fit.forward, fit.side, fit.turn, fit.off ? 1 : 0);
                out << text;
            }
            return static_cast<bool>(out);
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
        std::lock_guard<std::mutex> lock(g_mutex);
        uint16_t number;
        const uint32_t body = bodyOf(rdram, enemy & 0x1FFFFFFu, number);
        const auto it = g_fits.find(fitKey(punish, body));
        return it != g_fits.end() && it->second.off;
    }

    void drawPanel(const Pcsx2GsUi *ui)
    {
        ui->separator_text("Punish fit (Buck Breaker)");
        if (!g_on)
        {
            ui->text_disabled("Buck Breaker is off (launcher: Gameplay).");
            return;
        }
        Pair pair;
        Fit fit;
        std::string enemies;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            pair = g_pair;
            if (pair.punish >= 0)
            {
                if (const auto it = g_fits.find(fitKey(pair.punish, pair.body)); it != g_fits.end())
                    fit = it->second;
                for (uint16_t number : g_seen[pair.body])
                    enemies += (enemies.empty() ? "" : ", ") + enemyName(number);
            }
        }
        if (pair.punish < 0)
        {
            ui->text_disabled("Do a Suplex, Stinger, Cobra Twist or Spanking: its sliders show here.");
            return;
        }
        char line[256];
        std::snprintf(line, sizeof(line), "%s on body %08x (%s)%s", kNames[pair.punish], pair.body, enemies.c_str(),
                      pair.live ? ": playing" : "");
        ui->text(line);

        bool changed = false, dragging = false, stepped = false;
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
        slider("Forward (+ = Gene steps in)##fitfwd", fit.forward, -1.0f, 1.0f, "%.3f m");
        slider("Side (+ = to Gene's left)##fitside", fit.side, -1.0f, 1.0f, "%.3f m");
        slider("Turn (+ = Gene turns left)##fitturn", fit.turn, -45.0f, 45.0f, "%.1f deg");
        ui->begin_disabled(pair.native);
        int off = fit.off ? 1 : 0;
        if (ui->checkbox(pair.native ? "Doesn't fit (this is the enemy's own punish)##fitoff" : "Doesn't fit (keep this body's own punish when picked)##fitoff", &off))
        {
            fit.off = off != 0;
            changed = true;
        }
        ui->end_disabled();
        if (ui->button("Reset##fitreset"))
        {
            fit = Fit{};
            changed = true;
            stepped = true;
        }
        ui->same_line();
        if (ui->button("Save##fitsave"))
            g_status = save() ? "Saved." : "Could not write punish_fit.txt.";
        if (!g_status.empty())
        {
            ui->same_line();
            ui->text(g_status.c_str());
        }
        if (changed)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_fits[fitKey(pair.punish, pair.body)] = fit;
            g_status.clear();
        }
        // Paused, a finished change steps one frame so it shows (steps from the keys count as done after half a second).
        const auto now = std::chrono::steady_clock::now();
        if (stepped)
            g_stepCommit = now + std::chrono::milliseconds(500);
        const bool stepsDone = g_stepCommit && now >= *g_stepCommit;
        if (stepsDone)
            g_stepCommit.reset();
        const bool released = (g_dragging && !dragging) || stepsDone;
        g_dragging = dragging;
        if (released && pair.live && ps2x::timecontrol::paused())
            ps2x::timecontrol::step();
        ui->text_disabled("Changes move Gene at once while the punish plays; paused, letting go of a slider steps one "
                          "frame. F5 before pressing the button, F9 to play it again. Recolors share a body; values scale "
                          "with the enemy's size. Save keeps them in punish_fit.txt.");
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
        load();
        ps2x::pnach::addFrameHook(&onVblank);
    }
}
