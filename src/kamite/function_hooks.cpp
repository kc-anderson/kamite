#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "kamite/module_registry.h"
#include "kamite/sha256.h"
#include "runtime/ps2_save_state.h"
// Guest RAM ranges the runtime reserves for itself (RPC/TLS/boot-mode pools, callback stacks, legacy pools); only
// constants, checked against the DLL slot hole below.
#include "../../runtime/ps2xRuntime/src/lib/Kernel/GuestKernelMemory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kamite
{
    namespace
    {
        struct Hook
        {
            FunctionBinding binding;
            GuestFunction original = nullptr;
            std::vector<EntryObserver> observers;
            std::shared_ptr<const std::vector<EntryObserver>> snapshot;
        };

        std::mutex g_mutex;
        std::unordered_map<uint32_t, Hook> g_hooks;

        const char *statusName(ReplacementStatus status)
        {
            switch (status)
            {
            case ReplacementStatus::Recompiled:
                return "recomp";
            case ReplacementStatus::Observed:
                return "recomp+observed";
            case ReplacementStatus::Native:
                return "native";
            case ReplacementStatus::Hle:
                return "hle";
            case ReplacementStatus::TemporaryStub:
                return "TEMPORARY-STUB";
            }
            return "?";
        }
    }

    FunctionHooks &FunctionHooks::instance()
    {
        static FunctionHooks hooks;
        return hooks;
    }

    void FunctionHooks::attach(PS2Runtime &runtime)
    {
        m_runtime = &runtime;
    }

    GuestFunction FunctionHooks::original(uint32_t address) const
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_hooks.find(address);
        return it == g_hooks.end() ? nullptr : it->second.original;
    }

    bool FunctionHooks::observeEntry(uint32_t address, std::string name, EntryObserver observer)
    {
        if (!m_runtime)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_hooks.find(address);
        if (it == g_hooks.end())
        {
            if (!m_runtime->hasFunction(address))
            {
                KM_ERROR(Hook, "cannot observe 0x" << std::hex << address << " (" << name << "): no recompiled function");
                return false;
            }
            GuestFunction original = m_runtime->lookupFunction(address);
            if (!original)
            {
                KM_ERROR(Hook, "cannot observe 0x" << std::hex << address << " (" << name << "): no recompiled function");
                return false;
            }
            Hook hook;
            hook.binding = FunctionBinding{address, name, ReplacementStatus::Observed, "entry observer"};
            hook.original = original;
            it = g_hooks.emplace(address, std::move(hook)).first;
            if (!m_runtime->replaceFunction(address, &FunctionHooks::observedTrampoline))
            {
                g_hooks.erase(it);
                return false;
            }
        }
        it->second.observers.push_back(std::move(observer));
        it->second.snapshot = std::make_shared<const std::vector<EntryObserver>>(it->second.observers);
        return true;
    }

    bool FunctionHooks::replace(uint32_t address, std::string name, GuestFunction implementation, ReplacementStatus status, std::string reason)
    {
        if (!m_runtime)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        // Addresses the recompiler did not see as function entries have an empty slot; lookupFunction would log an error.
        GuestFunction original = m_runtime->hasFunction(address) ? m_runtime->lookupFunction(address) : nullptr;
        auto &hook = g_hooks[address];
        if (!hook.original)
            hook.original = original;
        hook.binding = FunctionBinding{address, std::move(name), status, std::move(reason)};
        return m_runtime->replaceFunction(address, implementation);
    }

    void FunctionHooks::observedTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The dispatcher stores the called address in ctx->pc before invoking the slot.
        const uint32_t address = ctx->pc;
        GuestFunction original = nullptr;
        std::vector<EntryObserver> observers;
        std::shared_ptr<const std::vector<EntryObserver>> snapshot;
        static const bool fast = [] {
            const char *setting = std::getenv("KAMITE_FAST_OBSERVERS");
            return !setting || std::string_view(setting) != "0";
        }();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_hooks.find(address);
            if (it == g_hooks.end())
            {
                KM_ERROR(Hook, "trampoline entered with unknown pc 0x" << std::hex << address);
                runtime->requestStop();
                return;
            }
            original = it->second.original;
            if (fast)
                snapshot = it->second.snapshot;
            else
                observers = it->second.observers;
        }
        for (const auto &observer : snapshot ? *snapshot : observers)
            observer(rdram, ctx, runtime);
        original(rdram, ctx, runtime);
    }

    void FunctionHooks::logBindings() const
    {
        std::map<uint32_t, FunctionBinding> sorted;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &[address, hook] : g_hooks)
                sorted.emplace(address, hook.binding);
        }
        for (const auto &[address, binding] : sorted)
        {
            KM_INFO(Hook, "0x" << std::hex << std::setw(8) << std::setfill('0') << address << std::dec << " " << binding.name << " ["
                               << statusName(binding.status) << "] " << binding.reason);
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // SNR2 DLL load logger (observe only).
    // ---------------------------------------------------------------------------------------------------------------
    namespace
    {
        constexpr uint32_t kSnr2LinkEntry = 0x00394DD8u;     // func_00394DD8(image, out)
        constexpr uint32_t kSnr2RegisterEntry = 0x00394C18u; // (a0 = image name, a1 = image), after relocation
        // func_00394D00(a0 = image): removes the image from the 0x3F5520 registry. Only caller: the unlink step
        // func_00394F20 (jal at 0x394F90), right after the image's destructor runner (header+0x1C, jalr at 0x394F88).
        // Unlink callers: cMc_DllRelease (jal at 0x2AE614), func_002BE6C8 (jal at 0x2BE6D0). Not 0x394328: that is
        // a patch(a0, a1, 0) wrapper.
        constexpr uint32_t kSnr2UnregisterEntry = 0x00394D00u;
        constexpr uint32_t kSnr2Magic24 = 0x00524E53u;       // "SNR": func_00393F80 lui 0x52 / ori 0x4E53, masked 0xFFFFFF

        // SNR2 header (SN ProDG relocatable image), fields as in emoose's ProDG-SNR2.bt
        // (audit_scratch/route/overlay/emoose_ProDG-SNR2.bt); in a .rel file the addresses are offsets from the header.
        struct Snr2Header
        {
            uint32_t magic;       // 0x00 "SNR2"
            uint32_t relocTable;  // 0x04
            uint32_t relocCount;  // 0x08
            uint32_t funcTable;   // 0x0C
            uint32_t funcCount;   // 0x10
            uint32_t imageName;   // 0x14 e.g. "..\PS2_EE_Release\Event.elf"
            uint32_t ctors;       // 0x18 constructor runner; func_00394DD8 jalr's it after linking
            uint32_t dtors;       // 0x1C
            uint32_t exports;     // 0x20 copied to *out by func_00394DD8 when the export count is not 0
            uint32_t exportCount; // 0x24
            uint32_t align;       // 0x28 func_00393F80: ((align - 1) & image) must be 0
            uint32_t fileSize;    // 0x2C
        };
        static_assert(sizeof(Snr2Header) == 0x30u, "SNR2 header fields 0x00-0x2C");

        // Guest pointer -> offset in the 32 MB of RDRAM (main RAM through KUSEG/KSEG0/KSEG1 and the uncached mirrors).
        bool toPhysical(uint32_t address, uint32_t size, uint32_t &physical)
        {
            switch (address >> 28)
            {
            case 0x0:
            case 0x2:
            case 0x3:
            case 0x8:
            case 0x9:
            case 0xA:
            case 0xB:
                break;
            default:
                return false;
            }
            physical = address & 0x0FFFFFFFu;
            return physical < PS2_RAM_SIZE && size <= PS2_RAM_SIZE - physical;
        }

        bool readHeader(const uint8_t *rdram, uint32_t base, Snr2Header &header, uint8_t &version)
        {
            uint32_t physical = 0;
            if (!toPhysical(base, sizeof(Snr2Header), physical))
                return false;
            std::memcpy(&header, rdram + physical, sizeof(Snr2Header)); // guest and host are both little-endian
            version = rdram[physical + 3u];
            return true;
        }

        std::string readGuestString(const uint8_t *rdram, uint32_t address, size_t maxLength = 160u)
        {
            uint32_t physical = 0;
            if (address == 0u)
                return "";
            if (!toPhysical(address, 1u, physical))
                return "<bad pointer>";
            std::string text;
            for (size_t i = 0; i < maxLength && physical + i < PS2_RAM_SIZE; ++i)
            {
                const char c = static_cast<char>(rdram[physical + i]);
                if (c == '\0')
                    return text;
                text.push_back(c >= 0x20 && c < 0x7F ? c : '?');
            }
            return text + "...";
        }

        // Before relocation the header holds offsets from the image; afterwards (presumably) addresses.
        uint32_t resolve(uint32_t base, const Snr2Header &header, uint32_t value)
        {
            if (value == 0u)
                return 0u;
            return value < header.fileSize ? base + value : value;
        }

        // The result func_00393F80 will return for this header.
        uint32_t headerCheck(uint32_t base, const Snr2Header &header, uint8_t version)
        {
            if ((header.magic & 0x00FFFFFFu) != kSnr2Magic24)
                return 1u;
            if (static_cast<uint32_t>(version) - 0x31u >= 2u)
                return 2u;
            return ((header.align - 1u) & base) != 0u ? 4u : 0u;
        }

        const char *checkName(uint32_t check)
        {
            switch (check)
            {
            case 0u:
                return "ok";
            case 1u:
                return "bad magic";
            case 2u:
                return "bad version";
            case 4u:
                return "misaligned";
            }
            return "?";
        }

        struct LoadedImage
        {
            std::string name;
            uint32_t size = 0;
        };

        std::mutex g_dllMutex;
        std::map<uint32_t, LoadedImage> g_dllImages; // base -> last image linked there
        std::atomic<uint32_t> g_dllSequence{0};
        std::FILE *g_dllCsv = nullptr;
        bool g_dllCsvTried = false;

        std::string csvQuote(const std::string &text)
        {
            std::string out = "\"";
            for (const char c : text)
                out += c == '"' ? std::string("\"\"") : std::string(1, c);
            return out + "\"";
        }

        // KAMITE_DLL_LOG=<file>: one CSV row per event (appended; the header row when the file is new or empty).
        void writeCsv(const std::string &row)
        {
            std::lock_guard<std::mutex> lock(g_dllMutex);
            if (!g_dllCsvTried)
            {
                g_dllCsvTried = true;
                const char *path = std::getenv("KAMITE_DLL_LOG");
                if (path && *path)
                {
                    std::error_code error;
                    const bool fresh = !std::filesystem::exists(path, error) || std::filesystem::file_size(path, error) == 0u;
                    g_dllCsv = std::fopen(path, "ab");
                    if (!g_dllCsv)
                        KM_WARN(Dll, "cannot open KAMITE_DLL_LOG " << path);
                    else if (fresh)
                        std::fputs("seq,seconds,event,base,size,end,check,name,relocs,funcs,ctors,dtors,exports,align,ra\n", g_dllCsv);
                }
            }
            if (g_dllCsv)
            {
                std::fputs(row.c_str(), g_dllCsv);
                std::fflush(g_dllCsv);
            }
        }

        std::string hex32(uint32_t value)
        {
            std::ostringstream text;
            text << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
            return text.str();
        }

        std::string hexShort(uint32_t value)
        {
            std::ostringstream text;
            text << "0x" << std::hex << value;
            return text.str();
        }

        std::string secondsText()
        {
            std::ostringstream text;
            text << std::fixed << std::setprecision(4) << log::elapsedSeconds();
            return text.str();
        }

        // One CSV row with the 15 columns of the header written by writeCsv.
        std::string csvRow(std::initializer_list<std::string> fields)
        {
            std::string row;
            bool first = true;
            for (const std::string &field : fields)
            {
                if (!first)
                    row += ',';
                row += field;
                first = false;
            }
            return row + "\n";
        }

        // ---- recompiled module code (generated_modules/, module_registry.h) ----------------------------------------
        // Installed packs: physical base -> pack. Guarded by g_dllMutex. Module code is generated for one load base
        // (literal PCs and return addresses), so a pack is installed only when name, base, size and the code bytes
        // in guest RAM all match what it was generated from (SotC module_guard scheme).
        std::map<uint32_t, const ModulePack *> g_installedModules;

        const ModulePack *findModulePack(const std::string &imageName, uint32_t base, const ModulePack **sameName)
        {
            size_t count = 0;
            const ModulePack *const *packs = modulePacks(count);
            for (size_t i = 0; i < count; ++i)
            {
                if (imageName != packs[i]->imageName)
                    continue;
                if (sameName)
                    *sameName = packs[i];
                if (packs[i]->base == base)
                    return packs[i];
            }
            return nullptr;
        }

        std::string moduleTextSha256(const uint8_t *rdram, const ModulePack &pack)
        {
            std::vector<uint8_t> text(rdram + pack.textBegin, rdram + pack.textEnd);
            for (size_t i = 0; i < pack.liveSiteCount; ++i)
                std::memset(text.data() + (pack.liveSites[i] - pack.textBegin), 0, 4u);
            return Sha256::hex(text.data(), text.size());
        }

        // Removes every installed module that overlaps [begin, end) (room DLLs reuse heap addresses). Caller holds
        // g_dllMutex.
        void evictModulesLocked(uint32_t begin, uint32_t end, const char *why)
        {
            for (auto it = g_installedModules.begin(); it != g_installedModules.end();)
            {
                const uint32_t moduleBegin = it->first;
                const uint32_t moduleEnd = moduleBegin + it->second->size;
                if (moduleBegin < end && begin < moduleEnd)
                {
                    ps2x::clearModuleFunctions(moduleBegin, moduleEnd);
                    KM_INFO(Dll, "module code removed (" << why << "): " << it->second->imageName << " base " << hex32(moduleBegin));
                    it = g_installedModules.erase(it);
                }
                else
                    ++it;
            }
        }

        // A later link can re-patch import sites inside an already installed module (func_003943D0); its recompiled
        // code would then be stale. At most a few modules (~130 KB) are resident, so re-hash all of them per link.
        void recheckInstalledLocked(const uint8_t *rdram)
        {
            for (auto it = g_installedModules.begin(); it != g_installedModules.end();)
            {
                const ModulePack &pack = *it->second;
                if (moduleTextSha256(rdram, pack) != pack.maskedTextSha256)
                {
                    KM_ERROR(Dll, "installed module " << pack.imageName << " base " << hex32(it->first)
                                                      << " changed in guest RAM; its recompiled code is removed");
                    ps2x::clearModuleFunctions(it->first, it->first + pack.size);
                    it = g_installedModules.erase(it);
                }
                else
                    ++it;
            }
        }

        void writeRamDump(const uint8_t *rdram, const char *why)
        {
            const char *path = std::getenv("PS2X_DUMP_RAM_ON_MISSING");
            if (!path || !*path)
                return;
            if (FILE *file = std::fopen(path, "wb"))
            {
                std::fwrite(rdram, 1, PS2_RAM_SIZE, file);
                std::fclose(file);
                KM_INFO(Dll, "EE RAM written to " << path << " (" << why << ")");
            }
        }

        // At 0x394C18 entry: relocation is done, the constructor runner has not been called yet.
        void installModuleCode(const uint8_t *rdram, PS2Runtime *runtime, const std::string &name, uint32_t base, uint32_t size)
        {
            uint32_t physical = 0;
            if (!toPhysical(base, size ? size : 4u, physical))
                return;
            std::lock_guard<std::mutex> lock(g_dllMutex);
            // Whatever was installed over this range is stale now, whether or not a pack exists for the new image.
            evictModulesLocked(physical, physical + (size ? size : 4u), "overlapped by a new link");
            recheckInstalledLocked(rdram);
            const ModulePack *sameName = nullptr;
            const ModulePack *pack = findModulePack(name, physical, &sameName);
            if (!pack)
            {
                // No stop here: the first call into the module then stops on a missing function, which writes the
                // PS2X_DUMP_RAM_ON_MISSING dump (requestStop does not).
                if (sameName)
                    KM_WARN(Dll, "recompiled code for " << name << " is for base " << hex32(sameName->base) << ", not "
                                                         << hex32(physical) << "; not installed (its first call will stop)");
                else
                    KM_WARN(Dll, "no recompiled code for " << name << " at " << hex32(physical) << "; its first call will stop");
                return;
            }
            const std::string digest = moduleTextSha256(rdram, *pack);
            if (pack->size != size || digest != pack->maskedTextSha256)
            {
                KM_ERROR(Dll, name << " at " << hex32(physical) << " does not match its recompiled image (size 0x" << std::hex << size
                                   << " vs 0x" << pack->size << ", code sha256 " << digest << " vs " << pack->maskedTextSha256
                                   << "); stopping");
                writeRamDump(rdram, "module mismatch");
                runtime->requestStop();
                return;
            }
            size_t installed = 0;
            for (size_t i = 0; i < pack->entryCount; ++i)
                installed += ps2x::setModuleFunction(pack->entries[i].address, pack->entries[i].function) ? 1u : 0u;
            g_installedModules[physical] = pack;
            KM_INFO(Dll, "module code installed: " << name << " base " << hex32(physical) << " entries " << std::dec << installed
                                                   << "/" << pack->entryCount << " code " << hex32(pack->textBegin) << "-"
                                                   << hex32(pack->textEnd) << " sha256 ok");
        }

        // ---- fixed DLL slots (KAMITE_PORT_NOTES.md section 9) ------------------------------------------------------
        // The game reads each room DLL and Event.rel into heap D (RoomHeap 0x754220 = 0x11C0000..0x1FFB000) at an
        // address that depends on the room's .dat size and on gameplay (r040.rel alone: 8 bases; Event.rel: any), while
        // recompiled module code is valid at one base only. So the DLL allocation of each class is moved into a small
        // heap of its own, built in the game's own cHeap format, inside the RAM hole 0x7957C0..0x8A0000 that nothing in
        // the boot ELF or the 81 DLLs addresses (crt0 SetupHeap/newlib sbrk and the runtime's guestMalloc stop at
        // 0x7957C0; heap A starts at 0x8A0000; zero in every RAM dump so far). Only the cHeap* argument of those two
        // cDvd_ReadAlloc calls changes; allocation, trim, free and the SNR2 link are the game's own code. Each class
        // has at most one DLL resident (RelSys slot 0 = room, slot 2 = Event.rel), so every DLL of a class links at
        // the same base: room DLLs 0x799080, Event.rel 0x7A8080 (tools/snr2/gen_modules.py emits them there).
        //   cHeap descriptor {+0 first block, +4 size, +8 0}; block {+0 desc, +4 prev, +8 next, +0xC free, +0x10 used}
        //   (Arena_init 0x2A9498, reset func_002A94F0, cHeap_allocFirstFit 0x2A9538, link 0x2A9620, trim 0x2A9648,
        //   cHeap_free 0x2A9680, which unlinks through block->prev and ignores its heap argument).
        constexpr uint32_t kReadAllocEntry = 0x00200CB0u; // cDvd_ReadAlloc(a0 Dvd, a1 path, a2 void **out, a3 cHeap *, ...)
        constexpr uint32_t kHeapResetEntry = 0x002A94F0u; // func_002A94F0(a0 = cHeap *): empties the heap
        constexpr uint32_t kRoomHeap = 0x00754220u;       // heap D descriptor (MemDefalutHeapPtr 0x3C3CF0 points here)
        // Heap C descriptor: func_0030ECF8 0x30ED64-0x30ED74 = Arena_init(0x754230, 0x1110000, 0xB0000). cMc_DllLoad
        // reads McPort.rel into it (a3 at 0x2AE51C); IdDispObj resets empty it (func_00161590 0x1615A4).
        constexpr uint32_t kMcHeap = 0x00754230u;
        // SNR2 registry: u32 count, u32 count ^ 0x31415926, then 0x400 entries {name, base, name ^ base ^ 0x31415926}.
        constexpr uint32_t kSnr2Registry = 0x003F5520u;
        constexpr uint32_t kSnr2RegistryEntries = 0x400u;
        constexpr uint32_t kHoleBegin = 0x007957C0u; // runtime guestMalloc / EndOfHeap clamp (System.cpp:544-551)
        constexpr uint32_t kHoleEnd = 0x008A0000u;   // heap A start (func_0030ECF8)
        constexpr uint32_t kSlotDescBegin = 0x00798000u;
        constexpr uint32_t kSlotDescEnd = 0x00798030u; // room 0x798000, event 0x798010, mcport 0x798020
        static_assert(kHoleBegin >= ps2x::kernel_memory::kCallbackStackTop, "slot hole overlaps the runtime callback stacks");
        static_assert(kHoleEnd <= ps2x::kernel_memory::kLegacyRpcServerPoolBase, "slot hole overlaps the legacy runtime pools");

        struct DllSlot
        {
            const char *cls;
            uint32_t ra;        // return address of the cDvd_ReadAlloc call that loads this class
            uint32_t heap;      // the cHeap* the game passes there
            uint32_t desc;      // slot heap descriptor (16 bytes)
            uint32_t heapBase;  // slot heap first block
            uint32_t heapSize;
            uint32_t highWater; // bytes from heapBase any DLL ever used (guard: the rest of an empty slot stays 0)
            bool releasePending = false; // its DLL was unregistered; the next slot event reports whether the block went
        };
        DllSlot g_slots[] = {
            // cRelSys_loadSlot 0x2BE560: jal cDvd_ReadAlloc at 0x2BE598, a3 = 0x754220. Largest: r501 (0xB800 read).
            {"room", 0x002BE5A0u, kRoomHeap, 0x00798000u, 0x00799000u, 0x0000F000u, 0u},
            // LoadResourceEntry_297378: a3 = MemDefalutHeapPtr (heap D), jal at 0x297408. event.rel reads 0x14000.
            {"event", 0x00297410u, kRoomHeap, 0x00798010u, 0x007A8000u, 0x00018000u, 0u},
            // cMc_DllLoad 0x2AE4A8: jal cDvd_ReadAlloc at 0x2AE538 (ra 0x2AE540), a3 = heap C 0x754230. Every McPort
            // load (boot task cMc_taskBody 0x2AE8A8, cMc_loadRunRelease 0x2AE788 / B 0x2AE838) goes through it.
            // McPort.rel 0x1BEC7 bytes, read 0x1C000: needs 0x1C0A0 of the 0x20000 slot. One McPort at a time
            // (a busy slot stops the second load). The 0x7C0000 boundary: func_0030ECF8 stores 0x7C0000 to the globals
            // 0x3C3CF4/0x3C3D00 (0x30ED28/0x30ED30); no boot or DLL code reads them by absolute address (the
            // 0x3CF4/0x3D00 loads in func_0021FD50 0x22234C/0x222A9C are offsets into the object at *(s1+0x304)) and
            // no other code builds 0x7C0000 (lui 0x7C only in func_0030ECF8): a plain number, no heap ends there.
            {"mcport", 0x002AE540u, kMcHeap, 0x00798020u, 0x007C0000u, 0x00020000u, 0u},
        };
        static_assert(0x007C0000u + 0x00020000u <= kHoleEnd && 0x007A8000u + 0x00018000u <= 0x007C0000u &&
                          0x00799000u + 0x0000F000u <= 0x007A8000u,
                      "slot layout");
        std::mutex g_slotMutex;
        int g_slotCheck = 0; // 0 not yet checked, 1 ok, -1 redirect disabled
        std::set<uint32_t> g_guardReported;
        std::set<uint32_t> g_heapWarned;

        uint32_t ramU32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + (address & 0x01FFFFFFu), 4u);
            return value;
        }

        void setRamU32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & 0x01FFFFFFu), &value, 4u);
        }

        uint32_t slotDllBase(const DllSlot &slot)
        {
            return (slot.heapBase + 0x20u + 0x1Fu + 0x80u) & ~0x7Fu; // allocFirstFit on an empty heap, align 0x80
        }

        bool slotInitialized(const uint8_t *rdram, const DllSlot &slot)
        {
            return ramU32(rdram, slot.desc) == slot.heapBase && ramU32(rdram, slot.desc + 4u) == slot.heapSize &&
                   ramU32(rdram, slot.desc + 8u) == 0u;
        }

        bool slotBusy(const uint8_t *rdram, const DllSlot &slot)
        {
            return ramU32(rdram, slot.heapBase + 8u) != 0u; // first block's next
        }

        // What func_002A94F0 writes for this descriptor.
        void resetSlotHeap(uint8_t *rdram, const DllSlot &slot)
        {
            setRamU32(rdram, slot.heapBase + 0x8u, 0u);
            setRamU32(rdram, slot.heapBase + 0x4u, 0u);
            setRamU32(rdram, slot.heapBase + 0x10u, 0x20u);
            setRamU32(rdram, slot.heapBase + 0xCu, slot.heapSize - 0x20u);
            setRamU32(rdram, slot.heapBase + 0x0u, slot.desc);
        }

        uint32_t firstNonZero(const uint8_t *rdram, uint32_t begin, uint32_t end)
        {
            for (uint32_t a = begin; a < end; a += 4u)
                if (ramU32(rdram, a) != 0u)
                    return a;
            return 0u;
        }

        struct RegistryEntry
        {
            uint32_t name;
            uint32_t base;
        };

        std::vector<RegistryEntry> registryEntries(const uint8_t *rdram, uint32_t begin = 0u, uint32_t end = 0xFFFFFFFFu)
        {
            std::vector<RegistryEntry> out;
            for (uint32_t i = 0; i < kSnr2RegistryEntries; ++i)
            {
                const uint32_t entry = kSnr2Registry + 8u + 12u * i;
                const uint32_t base = ramU32(rdram, entry + 4u);
                if (base != 0u && (base & 0x01FFFFFFu) >= begin && (base & 0x01FFFFFFu) < end)
                    out.push_back(RegistryEntry{ramU32(rdram, entry), base});
            }
            return out;
        }

        // The hole must stay zero except the slot descriptors, the used part of each empty slot heap and the whole of a
        // busy one. A WARN here means something else writes there (then: base-relative codegen instead of slots).
        // Caller holds g_slotMutex.
        void checkHoleLocked(const uint8_t *rdram, const char *when)
        {
            std::vector<std::pair<uint32_t, uint32_t>> skip{{kSlotDescBegin, kSlotDescEnd}};
            for (const DllSlot &slot : g_slots)
            {
                if (!slotInitialized(rdram, slot))
                    continue;
                const uint32_t used = slotBusy(rdram, slot) ? slot.heapSize : std::max(slot.highWater, 0x20u);
                skip.emplace_back(slot.heapBase, slot.heapBase + std::min(used, slot.heapSize));
            }
            std::sort(skip.begin(), skip.end());
            uint32_t at = kHoleBegin;
            auto scan = [&](uint32_t begin, uint32_t end) {
                if (begin >= end)
                    return;
                const uint32_t hit = firstNonZero(rdram, begin, end);
                if (hit && g_guardReported.insert(hit).second)
                    KM_WARN(Dll, "slot hole guard (" << when << "): nonzero word " << hex32(ramU32(rdram, hit)) << " at "
                                                     << hex32(hit) << " outside the DLL slots; something else uses "
                                                     << hex32(kHoleBegin) << ".." << hex32(kHoleEnd));
            };
            for (const auto &[begin, end] : skip)
            {
                scan(at, begin);
                at = std::max(at, end);
            }
            scan(at, kHoleEnd);
        }

        void warnLeakedLocked(const uint8_t *rdram, const DllSlot &slot, const char *when)
        {
            for (const RegistryEntry &entry : registryEntries(rdram, slot.heapBase, slot.heapBase + slot.heapSize))
                KM_WARN(Dll, "slot " << slot.cls << " (" << when << "): SNR2 registry still lists "
                                     << readGuestString(rdram, entry.name) << " at " << hex32(entry.base)
                                     << " (linked DLL not unlinked)");
        }

        // Lifetime check after an unregister (onUnregister sets releasePending): by the next slot event the game's own
        // cHeap_free (McPort: cMc_DllRelease 0x2AE5E8) or the heap-D reset mirror (rooms, Event.rel) must have emptied
        // the slot, and the SNR2 registry must no longer list anything inside it. Caller holds g_slotMutex.
        void reportReleasedSlotsLocked(const uint8_t *rdram, const char *when)
        {
            for (DllSlot &slot : g_slots)
            {
                if (!slot.releasePending || !slotInitialized(rdram, slot))
                    continue;
                slot.releasePending = false;
                const size_t listed = registryEntries(rdram, slot.heapBase, slot.heapBase + slot.heapSize).size();
                if (slotBusy(rdram, slot) || listed)
                    KM_WARN(Dll, "slot " << slot.cls << " (" << when << "): its DLL was unregistered but the slot still holds block "
                                         << hex32(ramU32(rdram, slot.heapBase + 8u)) << ", registry entries " << listed);
                else
                    KM_INFO(Dll, "slot " << slot.cls << " (" << when << "): empty after release (no block, no registry entry)");
            }
        }

        // cDvd_ReadAlloc entry: for the room-DLL, Event.rel and McPort reads only, a3 (heap D / heap C) becomes the
        // class's slot heap.
        void onReadAlloc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = getRegU32(ctx, 31);
            DllSlot *slot = nullptr;
            for (DllSlot &candidate : g_slots)
                if (candidate.ra == ra)
                    slot = &candidate;
            if (!slot)
                return;
            const uint32_t heap = getRegU32(ctx, 7);
            const std::string path = readGuestString(rdram, getRegU32(ctx, 5));
            std::lock_guard<std::mutex> lock(g_slotMutex);
            if (g_slotCheck == 0)
            {
                // SetupHeap ran in crt0 long before the first DLL read, so the runtime's heap limit is final here.
                const uint32_t limit = runtime->guestHeapLimit();
                if (limit > kSlotDescBegin)
                {
                    KM_ERROR(Dll, "runtime guest heap limit " << hex32(limit) << " reaches into the DLL slot hole "
                                                              << hex32(kSlotDescBegin) << "; DLL slots disabled");
                    g_slotCheck = -1;
                }
                else
                {
                    KM_INFO(Dll, "DLL slots: hole " << hex32(kHoleBegin) << ".." << hex32(kHoleEnd) << ", runtime heap limit "
                                                    << hex32(limit) << "; room base " << hex32(slotDllBase(g_slots[0]))
                                                    << ", event base " << hex32(slotDllBase(g_slots[1])) << ", mcport base "
                                                    << hex32(slotDllBase(g_slots[2])));
                    g_slotCheck = 1;
                }
            }
            if (g_slotCheck < 0)
                return;
            if (heap != slot->heap)
            {
                if (g_heapWarned.insert(ra).second)
                    KM_WARN(Dll, "slot " << slot->cls << ": ReadAlloc at ra " << hex32(ra) << " passes heap " << hex32(heap)
                                         << ", not " << hex32(slot->heap) << "; not redirected (" << path << ")");
                return;
            }
            reportReleasedSlotsLocked(rdram, "before redirect");
            checkHoleLocked(rdram, "before redirect");
            if (!slotInitialized(rdram, *slot))
            {
                if (firstNonZero(rdram, slot->desc, slot->desc + 0x10u) || firstNonZero(rdram, slot->heapBase, slot->heapBase + slot->heapSize))
                {
                    KM_ERROR(Dll, "slot " << slot->cls << " heap " << hex32(slot->heapBase) << " is not empty RAM; not redirected ("
                                          << path << ")");
                    return;
                }
                setRamU32(rdram, slot->desc + 0x0u, slot->heapBase);
                setRamU32(rdram, slot->desc + 0x4u, slot->heapSize);
                setRamU32(rdram, slot->desc + 0x8u, 0u);
                setRamU32(rdram, slot->desc + 0xCu, 0u);
                resetSlotHeap(rdram, *slot);
                KM_INFO(Dll, "slot " << slot->cls << " heap created: descriptor " << hex32(slot->desc) << " heap " << hex32(slot->heapBase)
                                     << " size 0x" << std::hex << slot->heapSize);
            }
            warnLeakedLocked(rdram, *slot, "before redirect");
            if (slotBusy(rdram, *slot))
            {
                // Left in heap D: no pack exists there, so the DLL's first call stops (missing function + RAM dump).
                KM_ERROR(Dll, "slot " << slot->cls << " is busy (block " << hex32(ramU32(rdram, slot->heapBase + 8u))
                                      << "); " << path << " stays in heap " << hex32(heap));
                return;
            }
            ctx->r[7] = _mm_set_epi64x(_mm_extract_epi64(ctx->r[7], 1), static_cast<int64_t>(static_cast<int32_t>(slot->desc)));
            KM_INFO(Dll, "slot redirect " << slot->cls << ": ra " << hex32(ra) << " heap " << hex32(heap) << " -> " << hex32(slot->desc)
                                          << " (DLL base " << hex32(slotDllBase(*slot)) << ") " << path);
            writeCsv(csvRow({std::to_string(++g_dllSequence), secondsText(), std::string("slot-redirect-") + slot->cls, hex32(slotDllBase(*slot)),
                             "", "", "", csvQuote(path), "", "", "", "", "", "", hex32(ra)}));
        }

        // func_002A94F0 entry: a reset of heap D also empties the room and Event.rel slots (their DLLs would have been
        // in heap D on the PS2, so they share its lifetime). A reset of heap C (IdDispObj resets, func_00161590) would
        // have dropped a McPort block there too, so it empties the McPort slot, but never while McPort is still
        // registered (its code would keep running from a slot the next load reuses): WARN and keep it then.
        void onHeapReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t heap = getRegU32(ctx, 4);
            if (heap != kRoomHeap && heap != kMcHeap)
                return;
            const char *when = heap == kRoomHeap ? "heap D reset" : "heap C reset";
            std::lock_guard<std::mutex> lock(g_slotMutex);
            reportReleasedSlotsLocked(rdram, when);
            for (DllSlot &slot : g_slots)
            {
                if (slot.heap != heap || !slotInitialized(rdram, slot))
                    continue;
                const bool wasBusy = slotBusy(rdram, slot);
                if (heap == kMcHeap)
                {
                    if (!registryEntries(rdram, slot.heapBase, slot.heapBase + slot.heapSize).empty())
                    {
                        warnLeakedLocked(rdram, slot, when);
                        KM_WARN(Dll, "slot " << slot.cls << " kept at " << when << " (ra " << hex32(getRegU32(ctx, 31))
                                             << "): its DLL is still registered");
                        continue;
                    }
                    if (!wasBusy)
                        continue; // nothing to drop; keep the log quiet for the frequent menu resets
                }
                else
                    warnLeakedLocked(rdram, slot, when);
                resetSlotHeap(rdram, slot);
                KM_INFO(Dll, "slot " << slot.cls << " reset with " << (heap == kRoomHeap ? "heap D" : "heap C") << " (ra "
                                     << hex32(getRegU32(ctx, 31)) << ")" << (wasBusy ? "; it held a block" : ""));
            }
            if (g_slotCheck > 0 && heap == kRoomHeap) // heap C resets come with menu objects; the scan stays on heap D
                checkHoleLocked(rdram, when);
        }

        // After a registration: the slot bytes this DLL's read may have touched (whole 2 KB sectors).
        void noteSlotImage(uint32_t base, uint32_t size)
        {
            std::lock_guard<std::mutex> lock(g_slotMutex);
            for (DllSlot &slot : g_slots)
            {
                const uint32_t physical = base & 0x01FFFFFFu;
                if (physical >= slot.heapBase && physical < slot.heapBase + slot.heapSize)
                    slot.highWater = std::max(slot.highWater, std::min(slot.heapSize, physical - slot.heapBase + ((size + 0x7FFu) & ~0x7FFu)));
            }
        }

        // Savestate load (F9 / requestLoad): guest RAM, and with it the SNR2 registry and the slot heaps, came back
        // from the file, but the host module table did not. With shared slot bases a stale pack would run another
        // DLL's bytes, so every installed pack is dropped and the registry's entries are reinstalled by name + base +
        // sha256.
        void resyncAfterStateLoad(PS2Runtime &runtime)
        {
            const uint8_t *rdram = runtime.memory().getRDRAM();
            size_t reinstalled = 0, missing = 0;
            {
                std::lock_guard<std::mutex> lock(g_dllMutex);
                for (const auto &[base, pack] : g_installedModules)
                    ps2x::clearModuleFunctions(base, base + pack->size);
                g_installedModules.clear();
                g_dllImages.clear();
                for (const RegistryEntry &entry : registryEntries(rdram))
                {
                    uint32_t physical = 0;
                    Snr2Header header{};
                    uint8_t version = 0;
                    const std::string name = readGuestString(rdram, entry.name);
                    if (!toPhysical(entry.base, 4u, physical) || !readHeader(rdram, entry.base, header, version))
                        continue;
                    g_dllImages[entry.base] = LoadedImage{name, header.fileSize};
                    const ModulePack *pack = findModulePack(name, physical, nullptr);
                    if (!pack || pack->size != header.fileSize || moduleTextSha256(rdram, *pack) != pack->maskedTextSha256)
                    {
                        ++missing;
                        KM_WARN(Dll, "state load: " << name << " at " << hex32(physical) << " has no matching recompiled pack; its next call will stop");
                        continue;
                    }
                    evictModulesLocked(physical, physical + pack->size, "state load");
                    for (size_t i = 0; i < pack->entryCount; ++i)
                        ps2x::setModuleFunction(pack->entries[i].address, pack->entries[i].function);
                    g_installedModules[physical] = pack;
                    ++reinstalled;
                }
            }
            {
                std::lock_guard<std::mutex> lock(g_slotMutex);
                for (DllSlot &slot : g_slots)
                {
                    uint32_t last = 0;
                    for (uint32_t a = slot.heapBase; a < slot.heapBase + slot.heapSize; a += 4u)
                        if (ramU32(rdram, a) != 0u)
                            last = a + 4u - slot.heapBase;
                    slot.highWater = last;
                }
                g_guardReported.clear();
            }
            KM_INFO(Dll, "state load: module packs reinstalled " << reinstalled << ", without pack " << missing);
        }

        // 0x394D00 entry (func_00394D00(a0 = image), called only from the unlink func_00394F20 at 0x394F90, after the
        // image's destructor runner): the module's code is no longer callable.
        void onUnregister(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t image = getRegU32(ctx, 4);
            const uint32_t ra = getRegU32(ctx, 31);
            uint32_t physical = 0;
            if (!toPhysical(image, 4u, physical))
                return;
            std::string name;
            {
                std::lock_guard<std::mutex> lock(g_dllMutex);
                const auto it = g_dllImages.find(image);
                if (it != g_dllImages.end())
                    name = it->second.name;
                evictModulesLocked(physical, physical + 4u, "unregistered");
            }
            {
                std::lock_guard<std::mutex> lock(g_slotMutex);
                for (DllSlot &slot : g_slots)
                    if (physical >= slot.heapBase && physical < slot.heapBase + slot.heapSize)
                        slot.releasePending = true;
            }
            KM_INFO(Dll, "unregister " << (name.empty() ? std::string("<unknown>") : name) << " base " << hex32(image) << " ra " << hex32(ra));
            writeCsv(csvRow({std::to_string(++g_dllSequence), secondsText(), "unregistered", hex32(image), "", "", "",
                             csvQuote(name), "", "", "", "", "", "", hex32(ra)}));
        }

        // 0x394DD8 entry: the image is in RAM, not yet checked or relocated.
        void onLink(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t base = getRegU32(ctx, 4);
            const uint32_t ra = getRegU32(ctx, 31);
            const uint32_t seq = ++g_dllSequence;
            Snr2Header header{};
            uint8_t version = 0;
            if (!readHeader(rdram, base, header, version))
            {
                KM_WARN(Dll, "#" << seq << " link: image pointer " << hex32(base) << " is outside guest RAM (ra " << hex32(ra) << ")");
                writeCsv(csvRow({std::to_string(seq), secondsText(), "link-bad-pointer", hex32(base), "", "", "", "", "", "", "", "", "", "",
                                 hex32(ra)}));
                return;
            }
            const uint32_t check = headerCheck(base, header, version);
            const std::string name = readGuestString(rdram, resolve(base, header, header.imageName));
            {
                std::lock_guard<std::mutex> lock(g_dllMutex);
                g_dllImages[base] = LoadedImage{name, header.fileSize};
            }
            KM_INFO(Dll, "#" << seq << " link " << (name.empty() ? std::string("<no name>") : name) << " base " << hex32(base) << " size 0x"
                             << std::hex << header.fileSize << " (end " << hex32(base + header.fileSize) << ") check " << std::dec << check
                             << " (" << checkName(check) << ") relocs " << header.relocCount << " funcs " << header.funcCount
                             << " ctors +0x" << std::hex << header.ctors << " align 0x" << header.align << " ra " << hex32(ra));
            writeCsv(csvRow({std::to_string(seq), secondsText(), "link", hex32(base), hexShort(header.fileSize), hex32(base + header.fileSize),
                             std::to_string(check), csvQuote(name), std::to_string(header.relocCount), std::to_string(header.funcCount),
                             hexShort(header.ctors), hexShort(header.dtors), hexShort(header.exports), hexShort(header.align), hex32(ra)}));
        }

        // 0x394C18 entry: relocation is done; a0 = the image name, a1 = the image. The constructor runner at
        // header+0x18 is the first guest call into the DLL's own code (no recompiled function exists for it yet).
        void onRegister(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t nameAddress = getRegU32(ctx, 4);
            const uint32_t base = getRegU32(ctx, 5);
            const uint32_t ra = getRegU32(ctx, 31);
            const uint32_t seq = ++g_dllSequence;
            Snr2Header header{};
            uint8_t version = 0;
            const bool haveHeader = readHeader(rdram, base, header, version);
            std::string name = readGuestString(rdram, nameAddress);
            uint32_t size = haveHeader ? header.fileSize : 0u;
            {
                std::lock_guard<std::mutex> lock(g_dllMutex);
                const auto it = g_dllImages.find(base);
                if (it != g_dllImages.end())
                {
                    if (name.empty() || name == "<bad pointer>")
                        name = it->second.name;
                    if (!size)
                        size = it->second.size;
                }
            }
            const uint32_t ctors = haveHeader ? resolve(base, header, header.ctors) : 0u;
            KM_INFO(Dll, "#" << seq << " registered " << (name.empty() ? std::string("<no name>") : name) << " base " << hex32(base)
                             << " size 0x" << std::hex << size << " name@" << hex32(nameAddress) << " ctors " << hex32(ctors)
                             << " (header 0x" << (haveHeader ? header.ctors : 0u) << ") ra " << hex32(ra));
            writeCsv(csvRow({std::to_string(seq), secondsText(), "registered", hex32(base), hexShort(size), hex32(base + size), "",
                             csvQuote(name), "", "", hex32(ctors), "", "", "", hex32(ra)}));
            installModuleCode(rdram, runtime, name, base, size);
            noteSlotImage(base, size);
        }
    }

    void installDllLoadLogger(PS2Runtime &runtime)
    {
        auto &hooks = FunctionHooks::instance();
        if (!runtime.hasFunction(kSnr2LinkEntry) || !hooks.observeEntry(kSnr2LinkEntry, "snr2_link_log", &onLink))
        {
            KM_WARN(Dll, "SNR2 link step 0x394dd8 has no recompiled entry; DLL loads are not logged");
            return;
        }
        // 0x394C18 lies inside func_00394828's range in recomp/decomp_functions.csv (0x394828-0x394D58), so a
        // generation from that CSV may have no entry for it: then only the link line is logged.
        if (runtime.hasFunction(kSnr2RegisterEntry))
            hooks.observeEntry(kSnr2RegisterEntry, "snr2_register_log", &onRegister);
        else
            KM_WARN(Dll, "0x394c18 (SNR2 image registration) has no recompiled entry; only link lines are logged");
        if (!runtime.hasFunction(kSnr2UnregisterEntry) || !hooks.observeEntry(kSnr2UnregisterEntry, "snr2_unregister", &onUnregister))
            KM_WARN(Dll, "0x394d00 (SNR2 image unregistration) has no recompiled entry; unlinked module code stays installed");
        {
            size_t count = 0;
            modulePacks(count);
            KM_INFO(Dll, "recompiled SNR2 module packs: " << count);
        }
        // Fixed DLL slots (room DLLs, Event.rel): see the slot comment above onReadAlloc.
        if (!runtime.hasFunction(kReadAllocEntry) || !hooks.observeEntry(kReadAllocEntry, "dll_slot_redirect", &onReadAlloc))
            KM_ERROR(Dll, "cDvd_ReadAlloc 0x200cb0 has no recompiled entry; room DLLs and Event.rel stay in heap D (no packs there)");
        if (!runtime.hasFunction(kHeapResetEntry) || !hooks.observeEntry(kHeapResetEntry, "dll_slot_heap_reset", &onHeapReset))
            KM_ERROR(Dll, "heap reset 0x2a94f0 has no recompiled entry; DLL slots are not emptied with heap D");
        PS2Runtime *runtimePtr = &runtime;
        ps2x::savestate::registerExtension(
            "kamite.modules",
            [](ps2x::state::Writer &out) {
                uint32_t version = 1u; // the state itself is guest RAM (registry + slot heaps); this only marks the hook
                ps2x::state::field(out, version);
            },
            [runtimePtr](ps2x::state::Reader &in) {
                uint32_t version = 0u;
                ps2x::state::field(in, version);
                resyncAfterStateLoad(*runtimePtr);
            });
        KM_INFO(Dll, "SNR2 DLL load logger + module code installer on" << (std::getenv("KAMITE_DLL_LOG") ? "; CSV: " : "")
                                                            << (std::getenv("KAMITE_DLL_LOG") ? std::getenv("KAMITE_DLL_LOG") : ""));
    }
}

namespace kamite
{
    // The game's flip copy at internal resolution (see function_hooks.h). Each frame func_0030B870 builds the PATH3
    // chain at 0x752D80 that copies the drawn frame (FBP 0x70) to the displayed one (FBP 0): a sprite with RGBA k, then
    // (unless 0x7474A0+0x578 is 1) two sprites one line up and down with RGBA (0x80 - k) / 2, ALPHA Cd + Cs. k is the
    // word at 0x3C3CCC (0x40, written once by the init func_0030B298 at 0x30B37C, read at 0x30BBB4/0x30BD74/0x30C590):
    // 0.5 * I(y) + 0.25 * I(y-1) + 0.25 * I(y+1), the interlace flicker filter. At SCALE x the copies are offset by
    // SCALE high-resolution rows, so every edge shows three times (round 7 simulation of ev01 field 4400). With k = 0x80
    // The game's own builder makes a full-weight copy and two RGBA-0 additions (Cs = 0, no change): the frame as drawn.
    void installHiresFlipCopy(PS2Runtime &runtime)
    {
        constexpr uint32_t kFlipChainBuilder = 0x30B870u;
        constexpr uint32_t kFlickerWeight = 0x3C3CCCu;
        const char *scaleValue = std::getenv("PS2X_GS_SCALE");
        const int scale = scaleValue ? std::atoi(scaleValue) : 1;
        if (scale <= 1)
            return;
        if (!runtime.hasFunction(kFlipChainBuilder) ||
            !FunctionHooks::instance().observeEntry(kFlipChainBuilder, "hires_flip_copy", [](uint8_t *rdram, R5900Context *, PS2Runtime *) {
                const uint32_t full = 0x80u;
                std::memcpy(rdram + kFlickerWeight, &full, sizeof(full));
            }))
        {
            KM_WARN(Gs, "flip chain builder 0x30b870 has no recompiled entry; the flicker filter stays on at " << scale << "x");
            return;
        }
        KM_INFO(Gs, "internal resolution " << scale << "x: flicker filter of the flip copy off (0x3c3ccc = 0x80 before 0x30b870)");
    }
}
