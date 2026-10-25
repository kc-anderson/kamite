#include "kamite/thread_policy.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace kamite::threadpolicy
{
#if defined(_WIN32)
    namespace
    {
        enum class Mode
        {
            Off,
            Auto,
            Spread,
            Ccx,
        };

        struct Core
        {
            uint64_t mask = 0u; // logical processors of this core inside the process mask
            std::vector<uint32_t> lps;
            uint32_t efficiency = 0u; // higher = faster core (hybrid CPUs); 0 everywhere on the others
            uint64_t l3 = 0u;         // mask of the L3 group (CCX) this core belongs to
            size_t index = 0u;
        };

        struct Placement
        {
            uint32_t lp = 0u;   // ideal processor
            uint64_t mask = 0u; // hard mask (ccx)
            int priority = THREAD_PRIORITY_NORMAL;
        };

        Mode g_mode = Mode::Auto;
        bool g_priority = true;
        // Threads with a hard mask in auto/spread (PS2X_THREAD_HARD=<name>,<name>|none). Default: the two OpenGL threads
        // (measured in the Urban Reign port: the NVIDIA driver's threaded-optimization worker goes away).
        std::string g_hardNames = "MainThread,GSThread";
        // Fewer than 4 cores in the process mask: no hard masks at all (ideal processors only).
        bool g_smallMachine = false;

        // The heavy threads, by name; helpers in the order they get cores (heaviest first).
        const char *const kGame = "GameThread";
        const char *const kHelpers[] = {"VUThread", "GIFThread", "GSThread", "MainThread", "IOPThread"};

        std::vector<Core> readCores(uint64_t processMask, size_t &totalCores, size_t &l3Groups)
        {
            std::vector<Core> cores;
            std::vector<uint64_t> l3Masks;
            DWORD length = 0u;
            GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
            std::vector<uint8_t> buffer(length);
            auto *info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data());
            if (length == 0u || !GetLogicalProcessorInformationEx(RelationAll, info, &length))
                return cores;
            for (DWORD offset = 0u; offset < length;)
            {
                const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data() + offset);
                if (entry->Relationship == RelationProcessorCore && entry->Processor.GroupMask[0].Group == 0u)
                {
                    Core core;
                    core.mask = static_cast<uint64_t>(entry->Processor.GroupMask[0].Mask);
                    core.efficiency = entry->Processor.EfficiencyClass;
                    cores.push_back(core);
                }
                else if (entry->Relationship == RelationCache && entry->Cache.Level == 3u && entry->Cache.GroupMask.Group == 0u)
                {
                    l3Masks.push_back(static_cast<uint64_t>(entry->Cache.GroupMask.Mask));
                }
                offset += entry->Size;
            }
            totalCores = cores.size();
            l3Groups = l3Masks.size();
            std::vector<Core> usable;
            for (Core &core : cores)
            {
                for (uint64_t l3 : l3Masks)
                    if ((l3 & core.mask) != 0u)
                        core.l3 = l3;
                core.mask &= processMask;
                for (uint32_t lp = 0u; lp < 64u; ++lp)
                    if (core.mask & (1ull << lp))
                        core.lps.push_back(lp);
                if (!core.lps.empty())
                {
                    core.index = usable.size();
                    usable.push_back(core);
                }
            }
            return usable;
        }

        std::unordered_map<std::string, Placement> makePlan(uint64_t processMask)
        {
            std::unordered_map<std::string, Placement> plan;
            size_t totalCores = 0u, l3Groups = 0u;
            std::vector<Core> cores = readCores(processMask, totalCores, l3Groups);
            if (cores.empty())
                return plan;
            uint32_t best = 0u;
            for (const Core &core : cores)
                best = std::max(best, core.efficiency);
            // Game core: the first of the fastest cores that does not hold LP 0 (interrupts/DPCs land there most).
            const Core *game = nullptr;
            for (const Core &core : cores)
                if (core.efficiency == best && !(core.mask & 1u))
                {
                    game = &core;
                    break;
                }
            if (!game)
                game = &cores.front();
            std::vector<const Core *> helpers;
            for (const Core &core : cores)
                if (&core != game)
                    helpers.push_back(&core);
            std::stable_sort(helpers.begin(), helpers.end(), [&](const Core *a, const Core *b) {
                if (a->efficiency != b->efficiency)
                    return a->efficiency > b->efficiency;
                const bool aNear = a->l3 == game->l3, bNear = b->l3 == game->l3;
                if (aNear != bNear)
                    return g_mode == Mode::Spread ? !aNear : aNear;
                const bool aZero = (a->mask & 1u) != 0u, bZero = (b->mask & 1u) != 0u;
                if (aZero != bZero)
                    return !aZero;
                return a->index < b->index;
            });
            // Slots: one LP per helper core, then their SMT siblings; the game core's sibling only on a small machine.
            std::vector<std::pair<uint32_t, uint64_t>> slots;
            for (size_t smt = 0u; smt < 2u; ++smt)
                for (const Core *core : helpers)
                    if (smt < core->lps.size())
                        slots.emplace_back(core->lps[smt], core->mask);
            g_smallMachine = cores.size() < 4u;
            if (g_smallMachine && game->lps.size() > 1u)
                slots.emplace_back(game->lps[1], game->mask);
            if (slots.empty())
                slots.emplace_back(game->lps.size() > 1u ? game->lps[1] : game->lps[0], game->mask);

            plan[kGame] = Placement{game->lps[0], game->mask, g_priority ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL};
            std::string text;
            char line[96];
            std::snprintf(line, sizeof(line), "GameThread->LP%u", game->lps[0]);
            text += line;
            for (size_t i = 0u; i < std::size(kHelpers); ++i)
            {
                const auto &slot = slots[i % slots.size()];
                plan[kHelpers[i]] = Placement{slot.first, slot.second, THREAD_PRIORITY_NORMAL};
                std::snprintf(line, sizeof(line), " %s->LP%u", kHelpers[i], slot.first);
                text += line;
            }
            size_t lps = 0u;
            for (const Core &core : cores)
                lps += core.lps.size();
            static const char *const modeNames[] = {"off", "auto", "spread", "ccx"};
            std::fprintf(stderr, "[thread-policy] %s: %zu of %zu cores (%zu logical) in the process mask 0x%llx, %zu L3 groups, "
                                 "hybrid %s; %s%s; hard masks: %s\n",
                         modeNames[static_cast<int>(g_mode)], cores.size(), totalCores, lps,
                         static_cast<unsigned long long>(processMask), l3Groups, best != 0u ? "yes" : "no", text.c_str(),
                         g_priority ? " (GameThread above normal)" : "",
                         g_smallMachine ? "none (fewer than 4 cores)" : g_mode == Mode::Ccx ? "all" : (g_hardNames.empty() ? "none" : g_hardNames.c_str()));
            return plan;
        }

        std::string threadName(HANDLE thread)
        {
            PWSTR wide = nullptr;
            std::string name;
            if (SUCCEEDED(GetThreadDescription(thread, &wide)) && wide)
            {
                for (const wchar_t *c = wide; *c; ++c)
                    name.push_back(static_cast<char>(*c < 128 ? *c : '?'));
                LocalFree(wide);
            }
            return name;
        }

        void apply(HANDLE thread, const std::string &name, const Placement &placement)
        {
            if (!g_smallMachine && (g_mode == Mode::Ccx || ("," + g_hardNames + ",").find("," + name + ",") != std::string::npos))
                SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(placement.mask));
            PROCESSOR_NUMBER ideal{};
            ideal.Group = 0u;
            ideal.Number = static_cast<BYTE>(placement.lp);
            const BOOL idealOk = SetThreadIdealProcessorEx(thread, &ideal, nullptr);
            const BOOL priorityOk = SetThreadPriority(thread, placement.priority);
            if (!idealOk || !priorityOk)
                std::fprintf(stderr, "[thread-policy] %s: ideal %s, priority %s (error %lu)\n", name.c_str(),
                             idealOk ? "ok" : "failed", priorityOk ? "ok" : "failed", GetLastError());
        }

        void governor()
        {
            SetThreadDescription(GetCurrentThread(), L"ThreadPolicy");
            const DWORD pid = GetCurrentProcessId();
            uint64_t planMask = 0u;
            std::unordered_map<std::string, Placement> plan;
            std::unordered_map<DWORD, uint64_t> applied;        // thread id -> process mask its placement was made for
            std::unordered_map<std::string, DWORD> placedNames; // target name -> thread id
            auto lastScan = std::chrono::steady_clock::now() - std::chrono::hours(1);
            for (;;)
            {
                DWORD_PTR processMask = 0u, systemMask = 0u;
                GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask);
                // The affinity can change after launch (Task Manager, a bench): re-plan.
                bool replanned = false;
                if (processMask != planMask)
                {
                    planMask = processMask;
                    plan = makePlan(planMask);
                    replanned = true;
                }
                // The thread snapshot covers every thread of the system (~tens of ms): every second until all the
                // target threads are placed, then every 15 s (threads may be recreated) or when the mask changes.
                const auto now = std::chrono::steady_clock::now();
                const bool allPlaced = placedNames.size() >= plan.size();
                if (replanned || now - lastScan >= std::chrono::seconds(allPlaced ? 15 : 1))
                {
                    lastScan = now;
                    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0u);
                    if (snapshot != INVALID_HANDLE_VALUE)
                    {
                        THREADENTRY32 entry{};
                        entry.dwSize = sizeof(entry);
                        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                        {
                            if (entry.th32OwnerProcessID != pid)
                                continue;
                            auto known = applied.find(entry.th32ThreadID);
                            if (known != applied.end() && known->second == planMask)
                                continue;
                            HANDLE thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                                       entry.th32ThreadID);
                            if (!thread)
                                continue;
                            const std::string name = threadName(thread);
                            auto placement = plan.find(name);
                            if (placement != plan.end())
                            {
                                apply(thread, name, placement->second);
                                applied[entry.th32ThreadID] = planMask;
                                placedNames[name] = entry.th32ThreadID;
                            }
                            else if (!name.empty())
                            {
                                applied[entry.th32ThreadID] = planMask; // not ours to place
                            }
                            CloseHandle(thread);
                        }
                        CloseHandle(snapshot);
                    }
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void startFromEnvironment()
    {
        // PS2X_PROCESS_AFFINITY=0-7 (or 0,2,4-5): restrict the process to these logical processors from boot, before any
        // thread or GL context exists, to stand in for a smaller machine.
        if (const char *cpus = std::getenv("PS2X_PROCESS_AFFINITY"); cpus && *cpus)
        {
            uint64_t mask = 0u;
            for (const char *c = cpus; *c;)
            {
                char *end = nullptr;
                const unsigned long lo = std::strtoul(c, &end, 10);
                unsigned long hi = lo;
                if (*end == '-')
                    hi = std::strtoul(end + 1, &end, 10);
                for (unsigned long lp = lo; lp <= hi && lp < 64u; ++lp)
                    mask |= 1ull << lp;
                c = *end == ',' ? end + 1 : end;
                if (end == c && *c)
                    break;
            }
            const BOOL ok = mask != 0u && SetProcessAffinityMask(GetCurrentProcess(), static_cast<DWORD_PTR>(mask));
            std::fprintf(stderr, "[thread-policy] process affinity %s -> 0x%llx %s\n", cpus, static_cast<unsigned long long>(mask),
                         ok ? "set" : "FAILED");
        }
        const char *mode = std::getenv("PS2X_THREAD_POLICY");
        if (mode && *mode)
        {
            if (!std::strcmp(mode, "off") || !std::strcmp(mode, "0"))
                g_mode = Mode::Off;
            else if (!std::strcmp(mode, "spread"))
                g_mode = Mode::Spread;
            else if (!std::strcmp(mode, "ccx"))
                g_mode = Mode::Ccx;
        }
        const char *priority = std::getenv("PS2X_THREAD_PRIORITY");
        g_priority = !(priority && *priority == '0');
        if (const char *hard = std::getenv("PS2X_THREAD_HARD"); hard && *hard)
            g_hardNames = std::strcmp(hard, "none") ? hard : "";
        if (g_mode == Mode::Off)
        {
            std::fprintf(stderr, "[thread-policy] off (PS2X_THREAD_POLICY=off)\n");
            return;
        }
        SetThreadDescription(GetCurrentThread(), L"MainThread");
        std::thread(governor).detach();
    }
#else
    void startFromEnvironment() {}
#endif
}
