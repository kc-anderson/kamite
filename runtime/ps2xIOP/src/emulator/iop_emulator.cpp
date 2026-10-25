#include <functional>
#include "iop_emulator.h"
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include "imports/iop_cdvd.h"
#include "devices/iop_sio2.h"
#include "devices/iop_spu2.h"
#include "core/iop_cpu.h"
#include "imports/iop_heaplib.h"
#include "imports/iop_imports.h"
#include "imports/iop_intrman.h"
#include "imports/iop_ioman.h"
#include "core/iop_kernel.h"
#include "imports/iop_loadcore.h"
#include "core/iop_memory.h"
#include "services/iop_module_loader.h"
#include "services/iop_rpc.h"
#include "imports/iop_stdio.h"
#include "imports/iop_sysclib.h"
#include "imports/iop_sysmem.h"
#include "imports/iop_timrman.h"
#include "imports/iop_vblank.h"
#include "iop_emulator_const.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <utility>

namespace ps2x::iop::detail
{
    // The CPU state being executed, for diagnostics in device code (e.g. the IOP pc of an SPU2 register write).
    const IopCpuState *g_iopLogCpu = nullptr;

    namespace
    {
        constexpr uint32_t kRamSize = IopMemory::RamSize;
        constexpr uint32_t kKernelHeapBase = IopMemory::HeapBase;
        constexpr uint32_t kKernelHeapLimit = IopMemory::HeapLimit;
        constexpr uint32_t kCallStackBase = kKernelHeapLimit;
        constexpr uint32_t kCallStackLimit = 0x001FFF00u;
        constexpr uint32_t kCallStackSize = 0x2000u;
        constexpr uint32_t kCallStackCapacity = (kCallStackLimit - kCallStackBase) / kCallStackSize;
        constexpr uint64_t kCdvdCompletionCycles = 128u;
        constexpr uint64_t kSifDmaBaseCycles = 64u;
        constexpr uint64_t kSifDmaBytesPerCycle = 4u;
        constexpr uint64_t kIopBatchCycles = 1024u;
        constexpr uint64_t kServerCallMaxCycles = kIopClockHz * 2u;

        uint32_t physicalAddress(uint32_t address)
        {
            return IopMemory::physicalAddress(address);
        }

        int32_t sign16(uint32_t value)
        {
            return static_cast<int16_t>(value & 0xFFFFu);
        }

        bool iequals(std::string_view lhs, std::string_view rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (size_t i = 0; i < lhs.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(lhs[i])) !=
                    std::tolower(static_cast<unsigned char>(rhs[i])))
                    return false;
            }
            return true;
        }

    }

    class IopEmulator::Impl final : public IopGuestExecutor
    {
    public:
        using CpuState = IopCpuState;

        struct Module
        {
            int id = 0;
            std::string path;
            std::string name;
            uint32_t base = 0;
            uint32_t size = 0;
            uint32_t entry = 0;
            uint32_t gp = 0;
            bool resident = false;

            template <typename Ar>
            void serialize(Ar &ar)
            {
                ps2x::state::fields(ar, id, path, name, base, size, entry, gp, resident);
            }
        };

        struct GuestCallback
        {
            uint32_t function = 0;
            uint32_t gp = 0;
        };

        struct ScheduledGuestCallback
        {
            uint32_t function = 0u;
            uint32_t gp = 0u;
            uint32_t argument = 0u;
        };

        explicit Impl(IopHost &hostRef)
            : host(hostRef),
              sysmem(host, memory),
              kernel(memory),
              cdvd(host, memory, kernel),
              vblank(kernel),
              rpc(host, memory, kernel),
              sysclib(memory),
              stdio(host, memory),
              heaplib(memory),
              intrman(memory),
              timrman(),
              ioman(memory),
              sio2(host, memory),
              cpuCore(memory),
              imports(memory),
              loadcore(memory, imports)
        {
            reset();
        }

        void reset()
        {
            memory.reset();
            // The synthetic RPC/server call-stack area (0x1F0000-0x1FFF00) is real IOP RAM that guest code uses as
            // stack. Mark it owned like allocate() does for thread stacks, so a server's sceSifSetDma from a stack
            // buffer (e.g. MCSERV GetInfo's 0xC0-byte info block at sp+16) copies it instead of being refused.
            (void)memory.zeroRam(kCallStackBase, kCallStackLimit - kCallStackBase);
            kernel.reset();
            modules.clear();
            imports.reset();
            rpc.reset();
            cdvd.reset();
            intrman.reset();
            timrman.reset();
            vblank.reset();
            ioman.reset();
            sio2.reset();
            memory.attachSio2(&sio2);
            spu2.reset();
            spu2.setAudioSink([this](std::span<const int16_t> samples) { host.submitAudio(samples); });
            host.resetAudio();
            memory.attachSpu2(&spu2, &totalCycles);
            pendingDmaInterrupts.clear();
            nextDmaDue = 0u;
            pendingGuestCallbacks.clear();
            nextModuleId = 1;
            moduleCursor = kModuleLoadBase;
            totalCycles = 0;
            totalInstructions = 0;
            eeCycleCarry = 0;
            eeSyncCycle = 0;
            activeCpu = nullptr;
            lastError.clear();
            servicingDmaInterrupts = false;
            servicingGuestCallbacks = false;
            criticalThreadId = 0;
            callDepth = 0u;
            secrMcCommandHandler = {};
            secrMcDevIdHandler = {};
            checkKelfPathCallback = {};
        }

        uint8_t read8(uint32_t address) const
        {
            return memory.read8(address);
        }

        uint16_t read16(uint32_t address) const
        {
            return memory.read16(address);
        }

        uint32_t read32(uint32_t address) const
        {
            return memory.read32(address);
        }

        void write8(uint32_t address, uint8_t value)
        {
            memory.write8(address, value);
            schedulePendingDma();
        }

        void write16(uint32_t address, uint16_t value)
        {
            memory.write16(address, value);
            schedulePendingDma();
        }

        void write32(uint32_t address, uint32_t value)
        {
            memory.write32(address, value);
            schedulePendingDma();
        }

        void schedulePendingDma()
        {
            if (!memory.hasDmaStart())
                return;
            if (const auto dma = memory.takeDmaStart())
            {
                if (dma->delayCycles == IopMemory::kDmaCancelDelay)
                    pendingDmaInterrupts.erase(dma->irq);
                else
                    pendingDmaInterrupts[dma->irq] = totalCycles + dma->delayCycles;
                nextDmaDue = 0u; // rescan on the next service

            }
        }

        bool readRam(uint32_t address, void *destination, size_t size) const
        {
            return memory.readRam(address, destination, size);
        }

        bool writeRam(uint32_t address, const void *source, size_t size)
        {
            return memory.writeRam(address, source, size);
        }

        bool zeroRam(uint32_t address, size_t size)
        {
            return memory.zeroRam(address, size);
        }

        bool isHardwareAddress(uint32_t phys) const
        {
            return memory.isHardwareAddress(phys);
        }

        uint32_t allocate(uint32_t size, uint32_t alignment = 16u, std::optional<uint32_t> fixed = std::nullopt)
        {
            return memory.allocate(size, alignment, fixed);
        }

        bool freeAllocation(uint32_t address)
        {
            return memory.freeAllocation(address);
        }

        void log(LogLevel level, std::string_view text)
        {
            host.log(level, text);
        }

        bool checkInterrupt(CpuState &cpu)
        {
            const uint32_t status = cpu.cop0[12];
            if ((status & 1u) == 0u)
                return false;
            if ((status & 0x2u) != 0u)
                return false;
            const bool pending = memory.interruptControl() != 0u && (memory.interruptStatus() & memory.interruptMask()) != 0u;
            if (!pending)
                return false;
            cpu.cop0[13] |= 0x400u;
            cpuCore.raiseException(cpu, 0u, cpu.pc, false);
            return true;
        }

        enum class ImportDisposition
        {
            Handled,
            JumpToGuest,
            Missing,
        };

        ImportDisposition dispatchImport(const IopImportCall &call, CpuState &cpu)
        {
            const uint32_t a0 = cpu.gpr[4];
            auto setV0 = [&](uint32_t value)
            {
                cpu.gpr[2] = value;
            };

            if (iequals(call.library, "sysmem") && sysmem.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "cdvdman") && cdvd.dispatchImport(call.ordinal, cpu))
            {
                if (const auto callback = cdvd.takeCompletionCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kCdvdCompletionCycles,
                        ScheduledGuestCallback{
                            callback->address,
                            callback->gp,
                            callback->reason,
                        });
                }
                return ImportDisposition::Handled;
            }

            if (iequals(call.library, "loadcore") && loadcore.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            if (iequals(call.library, "thbase") || iequals(call.library, "threadman"))
            {
                return kernel.dispatchThreadImport(call.ordinal, cpu, totalCycles)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thsemap"))
            {
                return kernel.dispatchSemaphoreImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "thevent"))
            {
                return kernel.dispatchEventImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "sifcmd"))
            {
                return rpc.dispatchSifCmdImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "intrman") && intrman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "secrman"))
            {
                switch (call.ordinal)
                {
                case 4: // SecrSetMcCommandHandler
                    secrMcCommandHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 5: // SecrSetMcDevIDHandler
                    secrMcDevIdHandler = {a0, cpu.gpr[28]};
                    setV0(0);
                    return ImportDisposition::Handled;
                case 6:
                    setV0(1);
                    return ImportDisposition::Handled;
                default:
                    break;
                }
            }
            if (iequals(call.library, "modload") && call.ordinal == 13u)
            {
                checkKelfPathCallback = {a0, cpu.gpr[28]};
                setV0(0);
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "ioman") && ioman.dispatchImport(call.ordinal, cpu, *this))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sifman"))
            {
                if (!rpc.dispatchSifManImport(call.ordinal, cpu))
                    return ImportDisposition::Missing;
                if (const auto callback = rpc.takeDmaCallback())
                {
                    pendingGuestCallbacks.emplace(
                        totalCycles + kSifDmaBaseCycles + callback->bytes / kSifDmaBytesPerCycle,
                        ScheduledGuestCallback{callback->function, callback->gp, callback->argument});
                }
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "vblank") && vblank.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "timrman") && timrman.dispatchImport(call.ordinal, cpu, totalCycles))
                return ImportDisposition::Handled;
            if (iequals(call.library, "dmacman"))
            {
                switch (call.ordinal)
                {
                case 28:
                    setV0(sio2.dmaRequest(a0, cpu.gpr[5], cpu.gpr[6], cpu.gpr[7], memory.read32(cpu.gpr[29] + 16u)) ? 1u : 0u);
                    break;
                case 32:
                    (void)sio2.dmaTransfer(a0);
                    setV0(0);
                    break;
                default:
                    setV0(0);
                    break;
                }
                return ImportDisposition::Handled;
            }
            if (iequals(call.library, "stdio") && stdio.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;
            if (iequals(call.library, "sysclib"))
            {
                return sysclib.dispatchImport(call.ordinal, cpu)
                           ? ImportDisposition::Handled
                           : ImportDisposition::Missing;
            }
            if (iequals(call.library, "heaplib") && heaplib.dispatchImport(call.ordinal, cpu))
                return ImportDisposition::Handled;

            const uint32_t target = imports.resolve(call.library, call.ordinal, call.version);
            if (target != 0u)
            {
                cpu.pc = target;
                cpu.branchPending = false;
                return ImportDisposition::JumpToGuest;
            }

            std::ostringstream out;
            out << "[IOP] unhandled import " << call.library << ':' << call.ordinal
                << " version=0x" << std::hex << call.version << " pc=0x" << cpu.pc;
            log(LogLevel::Warning, out.str());
            setV0(0);
            return ImportDisposition::Missing;
        }

        bool step(CpuState &cpu)
        {
            if (cpu.stopped)
                return false;
            if (cpu.pc == kThreadReturnSentinel || cpu.pc == kCallReturnSentinel)
            {
                cpu.stopped = true;
                return false;
            }
            if (physicalAddress(cpu.pc) >= kRamSize)
            {
                std::ostringstream out;
                out << "[IOP] execution outside RAM pc=0x" << std::hex << cpu.pc;
                log(LogLevel::Error, out.str());
                cpu.stopped = true;
                return false;
            }
            if (checkInterrupt(cpu))
                return true;

            // PS2X_IOP_TRACE_PC=addr[,addr...] (hex): log a0-a3/ra/sp/cycle each time the IOP reaches one of these pcs.
            static const std::vector<uint32_t> tracePcs = []
            {
                std::vector<uint32_t> pcs;
                if (const char *list = std::getenv("PS2X_IOP_TRACE_PC"))
                {
                    std::string text(list);
                    size_t start = 0;
                    while (start < text.size())
                    {
                        const size_t comma = text.find(',', start);
                        pcs.push_back(static_cast<uint32_t>(std::stoul(text.substr(start, comma - start), nullptr, 16)));
                        start = comma == std::string::npos ? text.size() : comma + 1;
                    }
                }
                return pcs;
            }();
            // PS2X_IOP_PC_HIST=<seconds>: sample the IOP pc every 64 instructions and print the 25 hottest pcs (with the
            // current thread) every <seconds> of IOP time, to find busy loops.
            static const uint64_t histPeriod = []
            {
                const char *value = std::getenv("PS2X_IOP_PC_HIST");
                return value ? static_cast<uint64_t>(std::strtod(value, nullptr) * 36864000.0) : 0ull;
            }();
            if (histPeriod != 0u && (totalInstructions & 63u) == 0u)
            {
                static std::unordered_map<uint64_t, uint32_t> hist;
                static uint64_t nextReport = histPeriod;
                const IopThread *current = kernel.currentThread();
                ++hist[(static_cast<uint64_t>(current ? current->id : 0) << 32) | cpu.pc];
                if (totalCycles >= nextReport)
                {
                    nextReport = totalCycles + histPeriod;
                    std::vector<std::pair<uint32_t, uint64_t>> top;
                    uint64_t total = 0;
                    for (const auto &[key, count] : hist)
                    {
                        top.emplace_back(count, key);
                        total += count;
                    }
                    std::sort(top.rbegin(), top.rend());
                    std::fprintf(stderr, "[iop-pc-hist] %llu samples:", static_cast<unsigned long long>(total));
                    for (size_t i = 0; i < top.size() && i < 25u; ++i)
                        std::fprintf(stderr, " t%u:%06x=%.1f%%", static_cast<uint32_t>(top[i].second >> 32),
                                     static_cast<uint32_t>(top[i].second), 100.0 * top[i].first / total);
                    std::fputc('\n', stderr);
                    // The same samples by module (pc inside a loaded IRX) and by IOP thread.
                    std::map<std::string, uint64_t> byModule;
                    std::map<uint32_t, uint64_t> byThread;
                    for (const auto &[count, key] : top)
                    {
                        const uint32_t pc = static_cast<uint32_t>(key);
                        std::string name = "other";
                        for (const auto &[id, module] : modules)
                            if (pc >= module.base && pc < module.base + module.size)
                                name = module.name;
                        byModule[name] += count;
                        byThread[static_cast<uint32_t>(key >> 32)] += count;
                    }
                    std::fprintf(stderr, "[iop-pc-hist]   by module:");
                    for (const auto &[name, count] : byModule)
                        std::fprintf(stderr, " %s=%.1f%%", name.c_str(), 100.0 * count / total);
                    std::fprintf(stderr, " | by thread:");
                    for (const auto &[thread, count] : byThread)
                        std::fprintf(stderr, " t%u=%.1f%%", thread, 100.0 * count / total);
                    std::fputc('\n', stderr);
                    hist.clear();
                }
            }

            if (!tracePcs.empty() && std::find(tracePcs.begin(), tracePcs.end(), cpu.pc) != tracePcs.end())
            {
                std::fprintf(stderr, "[iop-pc] %06x a0=%08x a1=%08x a2=%08x a3=%08x ra=%06x sp=%06x cycle=%llu\n", cpu.pc, cpu.gpr[4],
                             cpu.gpr[5], cpu.gpr[6], cpu.gpr[7], cpu.gpr[31], cpu.gpr[29], static_cast<unsigned long long>(totalCycles));
                // PS2X_IOP_TRACE_REGS=1: also v0 (a value just returned) and s0/s1 at each hit.
                static const bool traceRegs = [] { const char *v = std::getenv("PS2X_IOP_TRACE_REGS"); return v && *v == '1'; }();
                if (traceRegs)
                    std::fprintf(stderr, "[iop-regs] %06x v0=%08x s0=%08x s1=%08x\n", cpu.pc, cpu.gpr[2], cpu.gpr[16], cpu.gpr[17]);
                // PS2X_IOP_TRACE_MEM=<base>:<stride>:<count>:<off>[/<off>...] (hex): with each trace-pc hit, print the
                // words at base+i*stride+off for the count entries of a table, skipping entries whose first word is 0
                // (e.g. CRI's SJRBF pool 3ef50:40:80:4/c/10/20 = used, data bytes, free bytes, size).
                static const std::vector<uint32_t> traceMem = []
                {
                    std::vector<uint32_t> fields;
                    if (const char *spec = std::getenv("PS2X_IOP_TRACE_MEM"))
                    {
                        std::string text(spec);
                        for (char &c : text)
                            if (c == ':' || c == '/')
                                c = ' ';
                        std::istringstream in(text);
                        std::string token;
                        while (in >> token)
                            fields.push_back(static_cast<uint32_t>(std::stoul(token, nullptr, 16)));
                    }
                    return fields.size() >= 4u ? fields : std::vector<uint32_t>{};
                }();
                // PS2X_IOP_TRACE_ARGMEM=<gpr>:<words> (decimal): also print that many words at the address in that register.
                static const std::pair<uint32_t, uint32_t> argMem = []
                {
                    unsigned reg = 0u, words = 0u;
                    if (const char *spec = std::getenv("PS2X_IOP_TRACE_ARGMEM"))
                        if (std::sscanf(spec, "%u:%u", &reg, &words) != 2 || reg >= 32u)
                            words = 0u;
                    return std::pair<uint32_t, uint32_t>{reg, std::min(words, 16u)};
                }();
                if (argMem.second != 0u)
                {
                    std::fprintf(stderr, "[iop-argmem] %06x", cpu.pc);
                    for (uint32_t i = 0; i < argMem.second; ++i)
                        std::fprintf(stderr, " %08x", memory.read32(cpu.gpr[argMem.first] + i * 4u));
                    std::fputc('\n', stderr);
                }
                if (!traceMem.empty())
                {
                    std::fprintf(stderr, "[iop-mem] %06x", cpu.pc);
                    for (uint32_t i = 0; i < traceMem[2]; ++i)
                    {
                        const uint32_t entry = traceMem[0] + i * traceMem[1];
                        if (memory.read32(entry + traceMem[3]) == 0u)
                            continue;
                        std::fprintf(stderr, " %x:", i);
                        for (size_t f = 3u; f < traceMem.size(); ++f)
                            std::fprintf(stderr, "%s%x", f == 3u ? "" : ",", memory.read32(entry + traceMem[f]));
                    }
                    std::fputc('\n', stderr);
                }
            }

            // Import stubs start with "jr ra"; decode (a search back for the import table) only runs on those.
            const auto import = memory.read32(cpu.pc) == 0x03E00008u ? imports.decode(cpu.pc) : std::nullopt;
            if (import)
            {
                const ImportDisposition disposition = dispatchImport(*import, cpu);
                ++totalInstructions;
                ++totalCycles;
                if (disposition == ImportDisposition::JumpToGuest)
                    return true;
                cpu.pc = cpu.gpr[31];
                cpu.branchPending = false;
                return !cpu.stopped;
            }

            const bool running = cpuCore.executeInstruction(cpu);
            schedulePendingDma();
            ++totalInstructions;
            ++totalCycles;
            return running;
        }

        bool interruptsEnabled() const noexcept
        {
            static const bool honour = [] { const char *v = std::getenv("PS2X_IOP_INTR_MASK"); return !v || *v != '0'; }();
            return !honour || memory.interruptControl() != 0u;
        }

        uint32_t runCpu(CpuState &cpu, uint32_t instructionBudget)
        {
            CpuState *previous = activeCpu;
            activeCpu = &cpu;
            g_iopLogCpu = &cpu;
            const uint64_t start = totalInstructions;
            while (!cpu.stopped && !cpu.yielded && totalInstructions - start < instructionBudget)
            {
                if (!step(cpu))
                    break;
                // Interrupt-context work waits while the thread holds CpuSuspendIntr (I_CTRL = 0), as on the real IOP:
                // the CRI ADX driver's DMA callback edits the same chunk lists as its server thread inside such a
                // section, and running it there lost a chunk and hung the story "Now Loading" (PROGRESS §34).
                if (!interruptsEnabled())
                    continue;
                if (!servicingDmaInterrupts && !pendingDmaInterrupts.empty())
                    servicePendingDmaInterrupts();
                if (!servicingGuestCallbacks && !pendingGuestCallbacks.empty())
                    servicePendingGuestCallbacks();
            }
            activeCpu = previous;
            g_iopLogCpu = previous;
            return static_cast<uint32_t>(totalInstructions - start);
        }

        uint32_t callFunction(uint32_t address,
                              uint32_t a0,
                              uint32_t a1,
                              uint32_t a2,
                              uint32_t a3,
                              uint32_t gp,
                              uint32_t budget = kMaxCallInstructions)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            cpu.gpr[29] = callStackPointer(depth);
            cpu.gpr[31] = kCallReturnSentinel;
            runCpu(cpu, budget);
            return cpu.gpr[2];
        }

        uint32_t callStackPointer(uint32_t depth) const
        {
            if (depth < kCallStackCapacity)
                return kCallStackLimit - depth * kCallStackSize - 32u;
            if (activeCpu && activeCpu->gpr[29] > kCallStackBase + kStackGuardBytes)
                return (activeCpu->gpr[29] - kStackGuardBytes) & ~15u;
            return kCallStackBase - 32u;
        }

        uint32_t executeGuestFunction(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp) override
        {
            return callFunction(address, a0, a1, a2, a3, gp);
        }

        uint32_t executeServerFunction(uint32_t address,
                                       uint32_t a0,
                                       uint32_t a1,
                                       uint32_t a2,
                                       uint32_t a3,
                                       uint32_t gp,
                                       uint32_t priority) override
        {
            uint32_t result = 0u;
            (void)runServerFunction(address, a0, a1, a2, a3, gp, priority, nullptr, result);
            return result;
        }

        bool executeServerFunctionAsync(uint32_t address,
                                        uint32_t a0,
                                        uint32_t a1,
                                        uint32_t a2,
                                        uint32_t a3,
                                        uint32_t gp,
                                        uint32_t priority,
                                        std::function<void(uint32_t)> later,
                                        uint32_t &result) override
        {
            return runServerFunction(address, a0, a1, a2, a3, gp, priority, &later, result);
        }

        // PS2X_IOP_NOWAIT_DEFER (IopRpcBridge::handleRpc): the server call becomes a ready IOP thread with a pending
        // stack slot of its own, run by the IOP scheduler like a NOWAIT call that blocked (servicePendingServerCalls
        // finishes it). Nothing of it runs on the caller's (EE) thread.
        bool startServerFunctionAsync(uint32_t address,
                                      uint32_t a0,
                                      uint32_t a1,
                                      uint32_t a2,
                                      uint32_t a3,
                                      uint32_t gp,
                                      uint32_t priority,
                                      std::function<void(uint32_t)> later) override
        {
            // Its own rule, not runServerFunction's: the call stack area has 7 slots, and that rule (4 pending slots
            // plus 4 for nesting) never finds one free. A deferred call takes one of the 2 lowest slots and leaves 5
            // for synchronous calls, which nest 1-2 deep in practice.
            constexpr uint32_t kDeferredSlots = 2u;
            static_assert(kDeferredSlots <= kPendingServerSlots);
            if (callDepth != 0u || activeCpu != nullptr || kCallStackCapacity < kDeferredSlots + 5u)
                return false;
            uint32_t pendingSlot = kDeferredSlots;
            for (uint32_t slot = 0u; slot < kDeferredSlots && pendingSlot == kDeferredSlots; ++slot)
            {
                const bool used = std::any_of(pendingServerCalls.begin(), pendingServerCalls.end(),
                                              [slot](const PendingServerCall &call) { return call.slot == slot; });
                if (!used)
                    pendingSlot = slot;
            }
            if (pendingSlot == kDeferredSlots)
                return false; // both busy: the caller runs it synchronously as before
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            cpu.gpr[29] = callStackPointer(kCallStackCapacity - 1u - pendingSlot);
            cpu.gpr[31] = kCallReturnSentinel;
            const int id = kernel.createCallThread(cpu, priority);
            pendingServerCalls.push_back(PendingServerCall{id, pendingSlot, std::move(later)});
            return true;
        }

        // Server calls left running (executeServerFunctionAsync): each has a stack slot of its own at the bottom of
        // the call stack area, since depth-0 calls keep reusing the top one.
        struct PendingServerCall
        {
            int threadId = 0;
            uint32_t slot = 0u;
            std::function<void(uint32_t)> done;
        };
        static constexpr uint32_t kPendingServerSlots = 4u;
        std::vector<PendingServerCall> pendingServerCalls;

        void servicePendingServerCalls()
        {
            for (size_t i = 0; i < pendingServerCalls.size();)
            {
                if (!kernel.threadFinished(pendingServerCalls[i].threadId))
                {
                    ++i;
                    continue;
                }
                PendingServerCall call = std::move(pendingServerCalls[i]);
                pendingServerCalls.erase(pendingServerCalls.begin() + static_cast<std::ptrdiff_t>(i));
                IopThread *thread = kernel.findThread(call.threadId);
                const uint32_t result = thread ? thread->cpu.gpr[2] : 0u;
                if (thread)
                    kernel.releaseThread(call.threadId);
                if (call.done)
                    call.done(result);
            }
        }

        // An EE request reaches the IOP through the SIF interrupt, which the IOP takes only with interrupts enabled. A
        // batch can end while a thread is inside CpuSuspendIntr (criticalThreadId: runCycles resumes it first), and a
        // request served right then ran inside that thread's critical section. CRI ADX locks everything with
        // CpuSuspendIntr: an RNA destroy (DTX urpc 9 -> PSM_Destroy) freed the SJRBF that the PSM output thread's rate
        // converter (RCV_ExecHndl, under the PSM lock) was using; the thread resumed, SJRBF_GetChunk reported
        // "E2004090212 : Specified handle is invalid." and CRI's error handler spun forever with interrupts off: IOP
        // and game frozen after a boss "CHECK" (STATUS 2026-10-17 freeze). Let that thread leave the section (or block)
        // before the server runs. PS2X_IOP_RPC_WAIT_CRITICAL=0: the old behaviour. PS2X_IOP_CRITICAL_LOG=1: log every
        // wait (default: the first 4).
        //
        // Priority (sweep 2026-10-17, IOPR-1): with interrupts on, the SIF interrupt only makes the server thread ready;
        // it runs once it is the most urgent ready thread. A thread of higher priority that a batch end left part-way
        // through unlocked work (CRI's PSM mixer, PSMOUT prio 39, uses the channel SJRBFs with no lock) runs first, so
        // a DTX urpc 9 destroy (server prio 48) can no longer free a buffer under a running mix. Equal priority does
        // not wait (strict <). The IOP time run here is work those threads would run in the next batch anyway, so it
        // stays charged to the EE clock (no eeSyncCycle credit as in the blocked path below: that would let the IOP,
        // and the audio it produces, run ahead of the EE by every wait).
        void finishPreemptedCriticalSection(uint32_t address, uint32_t priority)
        {
            static const bool wait = [] { const char *v = std::getenv("PS2X_IOP_RPC_WAIT_CRITICAL"); return !(v && *v == '0'); }();
            static const bool logAll = [] { const char *v = std::getenv("PS2X_IOP_CRITICAL_LOG"); return v && *v == '1'; }();
            if (!wait || activeCpu != nullptr || callDepth != 0u)
                return;
            const auto heldInside = [this] { return criticalThreadId != 0 && !interruptsEnabled(); };
            const auto urgentReady = [this, priority]
            { return kernel.bestReadyPriority(totalCycles) < static_cast<int32_t>(priority); };
            const bool critical = heldInside();
            if (!critical && !urgentReady())
                return;
            const int heldId = critical ? criticalThreadId : 0;
            const IopThread *held = heldId != 0 ? kernel.findThread(heldId) : nullptr;
            const uint32_t heldPc = held ? held->cpu.pc : 0u;
            const uint64_t start = totalCycles;
            const uint64_t deadline = totalCycles + kCriticalSectionMaxCycles;
            while ((heldInside() || urgentReady()) && totalCycles < deadline)
                runCycles(kDefaultSlice);
            const bool stuckCritical = heldInside();
            const bool stuck = stuckCritical || urgentReady();
            ++criticalWaits;
            if (critical)
                ++criticalSectionWaits;
            if (stuck ? ++criticalStuckLogs > 8u : (!logAll && criticalWaits > 4u))
                return; // a thread that never leaves (e.g. CRI's error halt) would otherwise log at every request
            std::ostringstream out;
            out << "[IOP] EE request to server 0x" << std::hex << address << std::dec << " (prio " << priority << ") waited ";
            if (critical)
                out << "for thread " << heldId << " (pc 0x" << std::hex << heldPc << std::dec << ") to leave CpuSuspendIntr";
            else
                out << "for higher-priority ready threads";
            out << ": " << (totalCycles - start) << " cycles";
            if (stuckCritical)
            {
                const IopThread *now = kernel.findThread(criticalThreadId);
                out << ", thread " << criticalThreadId << " still inside after the cap (pc 0x" << std::hex
                    << (now ? now->cpu.pc : 0u) << std::dec << describeCriHalt(now ? now->cpu.pc : 0u)
                    << "); the request runs anyway";
            }
            else if (stuck)
                out << ", a thread of priority " << kernel.bestReadyPriority(totalCycles)
                    << " still ready after the cap; the request runs anyway";
            out << " (wait " << criticalWaits << ", in a critical section " << criticalSectionWaits << ")";
            log(stuck ? LogLevel::Warning : LogLevel::Info, out.str());
        }

        // Where a held thread is: IRX id + offset, and whether it spins on a jump to itself. CRI ADX 9.67's SJ error
        // handler ends in such a loop at IRX offset 0xb57c (IRX id of "CRI ADX Driver", base 0x2c800) after
        // printing e.g. "E2004090212 : Specified handle is invalid." with interrupts off (sweep IOPR-6).
        std::string describeCriHalt(uint32_t pc) const
        {
            std::ostringstream out;
            const uint32_t physical = IopMemory::physicalAddress(pc);
            for (const auto &[id, module] : modules)
            {
                if (physical < module.base || physical >= module.base + module.size)
                    continue;
                out << " = IRX id " << id << " base 0x" << std::hex << module.base << " +0x" << (physical - module.base)
                    << std::dec;
                break;
            }
            for (const uint32_t at : {physical, physical - 4u})
            {
                if (at + 4u > IopMemory::RamSize)
                    continue;
                const uint32_t op = memory.read32(at);
                if ((op >> 26) == 2u && ((((at + 4u) & 0xF0000000u) | ((op & 0x03FFFFFFu) << 2)) == at))
                {
                    out << ", spinning on a jump to itself (halt loop)";
                    break;
                }
            }
            return out.str();
        }
        static constexpr uint64_t kCriticalSectionMaxCycles = kIopClockHz / 500u; // 2 ms of IOP time
        uint32_t criticalWaits = 0u;
        uint32_t criticalSectionWaits = 0u;
        uint32_t criticalStuckLogs = 0u;

        bool runServerFunction(uint32_t address,
                               uint32_t a0,
                               uint32_t a1,
                               uint32_t a2,
                               uint32_t a3,
                               uint32_t gp,
                               uint32_t priority,
                               std::function<void(uint32_t)> *later,
                               uint32_t &callResult)
        {
            struct CallDepthGuard
            {
                uint32_t &depth;
                ~CallDepthGuard() { --depth; }
            };

            finishPreemptedCriticalSection(address, priority); // EE requests only (not nested IOP calls)
            const uint32_t depth = callDepth++;
            const CallDepthGuard depthGuard{callDepth};
            // A call that may be left running needs a stack nobody else uses: only an EE request (not nested), with a
            // free pending slot.
            uint32_t pendingSlot = kPendingServerSlots;
            if (later && depth == 0u && activeCpu == nullptr && kCallStackCapacity > kPendingServerSlots + 4u)
            {
                for (uint32_t slot = 0u; slot < kPendingServerSlots && pendingSlot == kPendingServerSlots; ++slot)
                {
                    const bool used = std::any_of(pendingServerCalls.begin(), pendingServerCalls.end(),
                                                  [slot](const PendingServerCall &call) { return call.slot == slot; });
                    if (!used)
                        pendingSlot = slot;
                }
            }
            CpuState cpu{};
            cpu.pc = address;
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            cpu.gpr[7] = a3;
            cpu.gpr[28] = gp;
            cpu.gpr[29] = pendingSlot < kPendingServerSlots ? callStackPointer(kCallStackCapacity - 1u - pendingSlot)
                                                            : callStackPointer(depth);
            cpu.gpr[31] = kCallReturnSentinel;

            IopThread *const previous = kernel.currentThread();
            const int id = kernel.createCallThread(cpu, priority);
            IopThread *thread = kernel.beginThread(id);
            uint32_t executed = 0u;
            bool blocked = false;
            while (executed < kMaxCallInstructions)
            {
                executed += runCpu(thread->cpu, kMaxCallInstructions - executed);
                if (thread->cpu.stopped || !thread->cpu.yielded)
                    break;
                if (thread->state != IopThreadState::Running)
                {
                    blocked = true;
                    break;
                }
                thread->cpu.yielded = false;
            }
            kernel.setCurrentThread(previous);

            if (blocked && pendingSlot < kPendingServerSlots)
            {
                pendingServerCalls.push_back(PendingServerCall{id, pendingSlot, std::move(*later)});
                callResult = 0u;
                return false;
            }

            if (blocked)
            {
                // A server that keeps the SIO2 busy is working, not deadlocked: formatting a memory card reads two
                // pages of each of the 1024 blocks (~6 s of IOP time) before it writes anything. With the fixed 2 s
                // cap the format RPC returned early ("Formatting complete" with nothing written), the abandoned call
                // kept MCMAN locked and every later card access hung. Each window with SIO2 transfers extends the
                // deadline, up to 120 s.
                const uint64_t blockedStart = totalCycles;
                uint64_t deadline = totalCycles + kServerCallMaxCycles;
                const uint64_t hardDeadline = totalCycles + kServerCallMaxCycles * 60u;
                uint64_t transfers = sio2.stats().transfers;
                for (;;)
                {
                    while (!kernel.threadFinished(id) && totalCycles < deadline)
                        runCycles(kDefaultSlice);
                    if (kernel.threadFinished(id) || sio2.stats().transfers == transfers || totalCycles >= hardDeadline)
                        break;
                    transfers = sio2.stats().transfers;
                    deadline = std::min(hardDeadline, totalCycles + kServerCallMaxCycles);
                }
                // The EE waits for this call without its clock moving, so the IOP time run here is not owed to the EE.
                // Counting it made the IOP stop afterwards until the EE caught up: a memory card save runs ~10 s of
                // IOP time in ~3 s, then PADMAN sat idle for ~10 s (no input after "Save complete").
                if (eeSyncCycle <= blockedStart)
                {
                    eeSyncCycle += totalCycles - blockedStart;
                    creditedCycles += totalCycles - blockedStart;
                }
                kernel.setCurrentThread(previous);
                if (!kernel.threadFinished(id))
                {
                    std::ostringstream out;
                    out << "[IOP] RPC server 0x" << std::hex << address << " still blocked after "
                        << std::dec << kServerCallMaxCycles << " cycles";
                    log(LogLevel::Warning, out.str());
                }
            }

            thread = kernel.findThread(id);
            const uint32_t result = thread ? thread->cpu.gpr[2] : 0u;
            if (thread && (thread->cpu.stopped || kernel.threadFinished(id)))
                kernel.releaseThread(id);

            // On the IOP a server that wakes a more urgent thread (SignalSema, WakeupThread...) is preempted before it
            // replies, so that thread consumes the request buffer before the EE can send the next request into it.
            // Let such threads run now (as in the God of War port); without it CRI's DTX/RNA traffic could pick up the
            // next request's data and Urban Reign's story mission load stalled now and then.
            // Off by default (did not fix that stall, 3 of 6 story runs still hung): PS2X_IOP_RPC_SETTLE=1 enables it.
            static const bool settle = [] { const char *v = std::getenv("PS2X_IOP_RPC_SETTLE"); return v && *v == '1'; }();
            if (settle && activeCpu == nullptr) // only for RPCs from the EE, never re-entrantly
            {
                constexpr uint64_t kSettleCycles = 4000000u; // ~0.1 s of IOP time at most
                const uint64_t settleEnd = totalCycles + kSettleCycles;
                while (kernel.bestReadyPriority(totalCycles) < static_cast<int32_t>(priority) && totalCycles < settleEnd)
                    runCycles(kDefaultSlice);
                kernel.setCurrentThread(previous);
            }
            callResult = result;
            return true;
        }

        uint32_t executeGuestFunctionWithBudget(uint32_t address,
                                                uint32_t a0,
                                                uint32_t a1,
                                                uint32_t a2,
                                                uint32_t a3,
                                                uint32_t gp,
                                                uint32_t instructionBudget) override
        {
            return callFunction(address, a0, a1, a2, a3, gp, instructionBudget);
        }

        // Not that good to use exception handling for control flow but will do for now
        void servicePendingDmaInterrupts()
        {
            // Called after every instruction while a DMA is pending (always, with AutoDMA streaming): skip the map
            // scan until the earliest completion is due.
            if (servicingDmaInterrupts || pendingDmaInterrupts.empty() || totalCycles < nextDmaDue)
                return;

            servicingDmaInterrupts = true;

            std::vector<int> completed;
            for (auto it = pendingDmaInterrupts.begin(); it != pendingDmaInterrupts.end();)
            {
                if (it->second > totalCycles)
                {
                    ++it;
                    continue;
                }
                completed.push_back(it->first);
                it = pendingDmaInterrupts.erase(it);
            }
            // Earliest remaining completion; a DMA started by the handlers below resets it to 0 (rescan).
            nextDmaDue = ~0ull;
            for (const auto &[irq, cycle] : pendingDmaInterrupts)
                nextDmaDue = std::min(nextDmaDue, cycle);
            try
            {
                for (const int irq : completed)
                    (void)intrman.dispatchInterrupt(irq, *this);
            }
            catch (...)
            {
                servicingDmaInterrupts = false;
                throw;
            }
            servicingDmaInterrupts = false;
        }

        void servicePendingGuestCallbacks()
        {
            if (servicingGuestCallbacks || pendingGuestCallbacks.empty())
                return;

            std::vector<ScheduledGuestCallback> callbacks;
            for (auto it = pendingGuestCallbacks.begin(); it != pendingGuestCallbacks.end();)
            {
                if (it->first > totalCycles)
                    break;
                callbacks.push_back(it->second);
                it = pendingGuestCallbacks.erase(it);
            }
            if (callbacks.empty())
                return;

            servicingGuestCallbacks = true;
            try
            {
                for (const ScheduledGuestCallback &callback : callbacks)
                {
                    if (callback.function != 0u)
                    {
                        (void)callFunction(callback.function,
                                           callback.argument,
                                           0u,
                                           0u,
                                           0u,
                                           callback.gp,
                                           100000u);
                    }
                }
            }
            catch (...)
            {
                servicingGuestCallbacks = false;
                throw;
            }
            servicingGuestCallbacks = false;
        }

        // PS2X_IOP_THREAD_LOG=<seconds>: periodic dump of the IOP threads (diagnosing a starved or blocked driver).
        void logIopThreads()
        {
            static const uint64_t period = []
            {
                const char *value = std::getenv("PS2X_IOP_THREAD_LOG");
                return value ? static_cast<uint64_t>(std::strtoull(value, nullptr, 10)) * 36864000ull : 0ull;
            }();
            if (period == 0u || totalCycles < nextThreadLog)
                return;
            nextThreadLog = totalCycles + period;
            static const char *const kStates[] = {"dormant", "ready", "run", "sleep", "delay", "sema", "evflag", "susp", "dead"};
            std::fprintf(stderr, "[iop-threads] cycle %llu\n", static_cast<unsigned long long>(totalCycles));
            kernel.forEachThread([&](const IopThread &t)
            {
                const unsigned state = static_cast<unsigned>(t.state);
                std::fprintf(stderr, "[iop-threads]   id=%d %-7s prio=%u pc=0x%06x entry=0x%06x wait=%d wake=%llu wakeups=%d\n", t.id,
                             state < 9u ? kStates[state] : "?", t.priority, t.cpu.pc, t.entry, t.waitId,
                             static_cast<unsigned long long>(t.wakeCycle), t.wakeupCount);
            });
        }
        uint64_t nextThreadLog = 0u;

        // Log only (sweep L1): a thread that keeps interrupts off for 2 s of IOP time has stopped the whole IOP (no
        // interrupt, no other thread runs; e.g. CRI's SJ error halt after "E2004090212"). Before this nothing in the
        // log said which thread it was, and PS2X_IOP_THREAD_LOG goes quiet in exactly this state.
        void noteLongCriticalSection(const IopThread &thread, uint64_t sliceStart)
        {
            if (criticalHeldId != thread.id)
            {
                criticalHeldId = thread.id;
                criticalHeldSince = sliceStart;
                criticalHeldLogged = false;
                return;
            }
            if (criticalHeldLogged || totalCycles - criticalHeldSince < kServerCallMaxCycles || criticalHeldLogs >= 4u)
                return;
            criticalHeldLogged = true;
            ++criticalHeldLogs;
            std::ostringstream out;
            out << "[IOP] thread " << thread.id << " (prio " << thread.priority << ", entry 0x" << std::hex << thread.entry
                << ") has kept interrupts off (CpuSuspendIntr) for " << std::dec << (totalCycles - criticalHeldSince)
                << " cycles: no IOP interrupt or other IOP thread can run; pc 0x" << std::hex << thread.cpu.pc << " ra 0x"
                << thread.cpu.gpr[31] << std::dec << describeCriHalt(thread.cpu.pc);
            log(LogLevel::Error, out.str());
        }
        int criticalHeldId = 0;
        uint64_t criticalHeldSince = 0u;
        bool criticalHeldLogged = false;
        uint32_t criticalHeldLogs = 0u;

        void runCycles(uint64_t cycles) noexcept
        {
            try
            {
                const uint64_t target = totalCycles + cycles;
                while (totalCycles < target)
                {
                    // A thread whose timeslice ended inside CpuSuspendIntr keeps the CPU: no interrupt work and no
                    // thread switch until it re-enables interrupts or blocks.
                    IopThread *next = nullptr;
                    if (criticalThreadId != 0)
                    {
                        const IopThread *held = kernel.findThread(criticalThreadId);
                        if (!interruptsEnabled() && held && held->state == IopThreadState::Ready)
                            next = kernel.beginThread(criticalThreadId);
                        criticalThreadId = 0;
                    }
                    if (next == nullptr)
                    {
                        servicePendingDmaInterrupts();
                        servicePendingGuestCallbacks();
                        if (!pendingServerCalls.empty())
                            servicePendingServerCalls();
                        if (rpc.hasEeRpcCompletions()) // IOP -> EE sceSifCallRpc replies: wake the waiting threads
                            rpc.serviceEeRpcCompletions();
                        timrman.serviceDue(totalCycles, *this);
                        vblank.serviceDue(totalCycles, *this);
                        logIopThreads();
                        next = kernel.beginNextReady(totalCycles);
                    }
                    if (!next)
                    {
                        uint64_t nextWake = kernel.nextWakeCycle(target);
                        for (const auto &[irq, completionCycle] : pendingDmaInterrupts)
                            nextWake = std::min(nextWake, completionCycle);
                        if (!pendingGuestCallbacks.empty())
                            nextWake = std::min(nextWake, pendingGuestCallbacks.begin()->first);
                        nextWake = timrman.nextEventCycle(nextWake);
                        nextWake = vblank.nextEventCycle(nextWake);
                        totalCycles = std::max(totalCycles + 1u, std::min(target, nextWake));
                        continue;
                    }
                    const uint64_t before = totalCycles;
                    runCpu(next->cpu, static_cast<uint32_t>(std::min<uint64_t>(kDefaultSlice, target - totalCycles)));
                    const int ranId = next->id;
                    kernel.endTimeslice(*next, kThreadReturnSentinel);
                    if (!interruptsEnabled())
                    {
                        if (const IopThread *ran = kernel.findThread(ranId); ran && ran->state == IopThreadState::Ready)
                        {
                            criticalThreadId = ranId;
                            noteLongCriticalSection(*ran, before);
                        }
                        else
                            criticalHeldId = 0;
                    }
                    else
                        criticalHeldId = 0;
                    if (totalCycles == before)
                        ++totalCycles;
                }
                spu2.advanceTo(totalCycles);
            }
            catch (...)
            {
                // Runtime scheduling must never throw through EeScheduler::accountCycles().
            }
        }

        uint32_t runModuleStart(uint32_t entry, uint32_t argc, uint32_t argv, uint32_t gp)
        {
            const int id = kernel.createModuleStartThread(entry, gp, argc, argv, kModuleStartPriority, kModuleStartStackSize);
            if (id == 0)
                return callFunction(entry, argc, argv, 0u, 0u, gp);
            const uint64_t deadline = totalCycles + kModuleStartMaxCycles;
            while (!kernel.threadFinished(id) && totalCycles < deadline)
                runCycles(kDefaultSlice);
            if (!kernel.threadFinished(id))
            {
                std::ostringstream out;
                out << "[IOP] module_start at 0x" << std::hex << entry << " did not return; its thread keeps running";
                log(LogLevel::Warning, out.str());
                return 0u;
            }
            return kernel.releaseModuleStartThread(id);
        }

        ModuleLoadResult loadImage(std::string path, std::span<const uint8_t> image, const void *arguments, uint32_t argumentSize)
        {
            ModuleLoadResult result{true, -1, -1};
            const IopImageLoadResult loaded = IopModuleLoader::load(image, memory, moduleCursor);
            moduleCursor = loaded.nextModuleCursor;
            if (!loaded)
            {
                if (loaded.error == IopImageLoadError::InvalidElf)
                    log(LogLevel::Error, "[IOP] rejected invalid/non-MIPS IRX ELF");
                else if (loaded.error == IopImageLoadError::ArenaExhausted)
                    log(LogLevel::Error, "[IOP] module arena exhausted");
                return result;
            }
            if (!loaded.relocationsComplete)
                log(LogLevel::Warning, "[IOP] one or more IRX relocations were unsupported");

            Module module;
            module.id = nextModuleId++;
            module.path = std::move(path);
            const size_t slash = module.path.find_last_of("/\\:");
            module.name = slash == std::string::npos ? module.path : module.path.substr(slash + 1u);
            module.base = loaded.base;
            module.size = loaded.size;
            module.entry = loaded.entry;
            module.gp = loaded.gp;

            // The IOP loader (modload StartModule) calls module_start(argc, argv): argv[0] is the module path, argv[1..]
            // are the NUL-separated strings of the argument block (sceSifLoadModule's arglen bytes; an unterminated
            // last string is closed), argv[argc] = 0. The game's CRI_ADXI ("sdinit=0\0spucore=1") and SNDDRV ("dmach=0")
            // parse argv; passing (byte count, raw text) left CRI at its defaults (second cold sceSdInit, ADMA on core 0).
            std::vector<uint32_t> argOffsets;
            const auto *argBytes = static_cast<const uint8_t *>(arguments);
            if (argBytes)
            {
                for (uint32_t i = 0u; i < argumentSize;)
                {
                    argOffsets.push_back(i);
                    while (i < argumentSize && argBytes[i] != 0u)
                        ++i;
                    ++i;
                }
            }
            const uint32_t argc = 1u + static_cast<uint32_t>(argOffsets.size());
            const uint32_t vectorBytes = (argc + 1u) * 4u;
            const uint32_t nameBytes = static_cast<uint32_t>(module.path.size()) + 1u;
            const uint32_t args = allocate(vectorBytes + nameBytes + argumentSize + 1u, 16u);
            uint32_t argv = 0u;
            if (args)
            {
                argv = args;
                const uint32_t nameAddress = args + vectorBytes;
                writeRam(nameAddress, module.path.data(), nameBytes - 1u);
                write8(nameAddress + nameBytes - 1u, 0u);
                const uint32_t textAddress = nameAddress + nameBytes;
                if (argBytes && argumentSize)
                    writeRam(textAddress, argBytes, argumentSize);
                write8(textAddress + argumentSize, 0u);
                write32(argv, nameAddress);
                for (size_t n = 0; n < argOffsets.size(); ++n)
                    write32(argv + 4u * static_cast<uint32_t>(n + 1u), textAddress + argOffsets[n]);
                write32(argv + 4u * argc, 0u);
            }
            const uint32_t startResult = runModuleStart(module.entry, argv ? argc : 0u, argv, module.gp);
            if (args)
                freeAllocation(args);
            module.resident = startResult == 0u || startResult == 2u;
            result.moduleId = module.id;
            result.startResult = static_cast<int32_t>(startResult);
            modules[module.id] = std::move(module);

            std::ostringstream out;
            out << "[IOP] loaded IRX id=" << result.moduleId
                << " entry=0x" << std::hex << modules[result.moduleId].entry
                << " base=0x" << modules[result.moduleId].base
                << " start=" << std::dec << result.startResult;
            log(LogLevel::Info, out.str());
            return result;
        }

        ModuleLoadResult loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            if (!IopModuleLoader::readWholeHostFile(host, path, image))
            {
                log(LogLevel::Warning, std::string("[IOP] failed to open IRX '") + std::string(path) + "'");
                return {true, -1, -1};
            }
            return loadImage(std::string(path), image, arguments, argumentSize);
        }

        ModuleLoadResult loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
        {
            std::vector<uint8_t> image;
            const bool fromIopRam = IopModuleLoader::readElf([this](uint32_t address, void *destination, size_t size)
                                                             { return memory.readRam(address, destination, size); },
                                                             guestAddress, image);
            if (!fromIopRam && !IopModuleLoader::readElfFromGuest(host, guestAddress, image))
                return {true, -1, -1};
            std::ostringstream tag;
            tag << "buffer@0x" << std::hex << guestAddress;
            return loadImage(tag.str(), image, arguments, argumentSize);
        }

        bool stopModule(int32_t moduleId, int32_t *result)
        {
            auto it = modules.find(moduleId);
            if (it == modules.end())
                return false;
            // A removable IRX normally exposes a stop entry through module metadata. We do not guess it; terminate owned execution and release the image cleanly.
            kernel.terminateThreadsInRange(it->second.base, it->second.size);
            rpc.removeServersInRange(it->second.base, it->second.size);
            imports.eraseRange(it->second.base, it->second.size);
            modules.erase(it);
            kernel.cleanupDeadThreads();
            if (result)
                *result = 0;
            return true;
        }

        void accountEeCycles()
        {
            eeCycleCarry -= static_cast<int64_t>((totalCycles - eeSyncCycle) * 8u);
            eeSyncCycle = totalCycles;
        }

        template <typename Ar>
        void serialize(Ar &ar)
        {
            using namespace ps2x::state;
            if constexpr (!Ar::kLoading)
                accountEeCycles();
            ar.section(fourcc("IOPE"));
            fields(ar, modules, pendingDmaInterrupts, pendingGuestCallbacks, nextModuleId, moduleCursor, totalCycles, totalInstructions,
                   eeCycleCarry, secrMcCommandHandler, secrMcDevIdHandler, checkKelfPathCallback);
            nextDmaDue = 0u;
            ar.section(fourcc("IMEM"));
            memory.serialize(ar);
            ar.section(fourcc("IKRN"));
            kernel.serialize(ar);
            ar.section(fourcc("ICDV"));
            if constexpr (Ar::kLoading)
                cdvd.loadState(ar);
            else
                cdvd.saveState(ar);
            ar.section(fourcc("IRPC"));
            rpc.serialize(ar);
            ar.section(fourcc("IINT"));
            intrman.serialize(ar);
            timrman.serialize(ar);
            ioman.serialize(ar);
            ar.section(fourcc("ISIO"));
            sio2.serialize(ar);
            // Multitap slots and pads B-D (optional: states saved before the multitap have no IMTP section).
            if constexpr (Ar::kLoading)
            {
                if (ar.nextSectionIs(fourcc("IMTP")))
                {
                    ar.section(fourcc("IMTP"));
                    sio2.serializeMultitap(ar);
                }
            }
            else
            {
                ar.section(fourcc("IMTP"));
                sio2.serializeMultitap(ar);
            }
            ar.section(fourcc("ISPU"));
            spu2.serialize(ar);
            ar.section(fourcc("IIMP"));
            imports.serialize(ar);
            // The vblank edges are counted from the IOP time of the last one serviced. It was not saved: after a load
            // the restored totalCycles was behind it and no vblank handler ran until the IOP caught up with the time of
            // the load, so PADMAN (it polls the pads from such a handler) stopped for as long as the game had run since
            // the state's point (input back after ~20 s, or never). States saved before this have no IVBL section.
            if constexpr (Ar::kLoading)
            {
                if (ar.nextSectionIs(fourcc("IVBL")))
                {
                    ar.section(fourcc("IVBL"));
                    vblank.serialize(ar);
                }
                else
                    vblank.resyncAfterLoad(totalCycles);
            }
            else
            {
                ar.section(fourcc("IVBL"));
                vblank.serialize(ar);
            }
            // The thread a batch end left inside CpuSuspendIntr (sweep IOPR-2). Threads are saved Ready, so without it a
            // load let interrupt work, other threads and EE requests run inside that section (the CRI corruption class
            // of the boss-CHECK freeze), and the old session's id stayed in place. States without ICRT: none held.
            if constexpr (Ar::kLoading)
            {
                criticalThreadId = 0;
                if (ar.nextSectionIs(fourcc("ICRT")))
                {
                    ar.section(fourcc("ICRT"));
                    field(ar, criticalThreadId);
                }
                criticalHeldId = 0;
            }
            else
            {
                ar.section(fourcc("ICRT"));
                field(ar, criticalThreadId);
            }
            if constexpr (Ar::kLoading)
            {
                eeSyncCycle = totalCycles;
                pendingServerCalls.clear(); // a state is never saved with one (save blocker); threads come from the file
                host.resetAudio();
                activeCpu = nullptr;
                servicingDmaInterrupts = false;
                servicingGuestCallbacks = false;
                callDepth = 0u;
            }
        }

        IopHost &host;
        IopMemory memory;
        IopSysmem sysmem;
        IopKernel kernel;
        IopCdvd cdvd;
        IopVblank vblank;
        IopRpcBridge rpc;
        IopSysclib sysclib;
        IopStdio stdio;
        IopHeaplib heaplib;
        IopIntrman intrman;
        IopTimrman timrman;
        IopIoman ioman;
        IopSio2 sio2;
        IopSpu2 spu2;
        IopCpuCore cpuCore;
        IopImportRegistry imports;
        IopLoadcore loadcore;
        std::map<int, Module> modules;
        std::map<int, uint64_t> pendingDmaInterrupts;
        uint64_t nextDmaDue = 0u; // earliest completion cycle in pendingDmaInterrupts (0 = rescan)
        std::multimap<uint64_t, ScheduledGuestCallback> pendingGuestCallbacks;
        uint32_t nextModuleId = 1;
        uint32_t moduleCursor = kModuleLoadBase;
        uint64_t totalCycles = 0;
        uint64_t totalInstructions = 0;
        int64_t eeCycleCarry = 0;
        uint64_t eeSyncCycle = 0;
        uint64_t creditedCycles = 0; // PS2X_IOP_CLOCK_LOG: IOP time run while the EE waited on a blocked RPC server
        uint64_t eeCyclesFed = 0;    // PS2X_IOP_CLOCK_LOG: EE cycles handed to runEeCycles
        CpuState *activeCpu = nullptr;
        std::string lastError;
        bool servicingDmaInterrupts = false;
        bool servicingGuestCallbacks = false;
        int criticalThreadId = 0; // thread preempted by a slice end inside CpuSuspendIntr; it runs next (runCycles)
        uint32_t callDepth = 0u;
        GuestCallback secrMcCommandHandler;
        GuestCallback secrMcDevIdHandler;
        GuestCallback checkKelfPathCallback;
    };

    IopEmulator::IopEmulator(IopHost &host)
        : m_impl(std::make_unique<Impl>(host))
    {
    }

    IopEmulator::~IopEmulator() = default;

    void IopEmulator::reset()
    {
        m_impl->reset();
    }

    ModuleLoadResult IopEmulator::loadModule(std::string_view path, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModule(path, arguments, argumentSize);
    }

    ModuleLoadResult IopEmulator::loadModuleBuffer(uint32_t guestAddress, const void *arguments, uint32_t argumentSize)
    {
        return m_impl->loadModuleBuffer(guestAddress, arguments, argumentSize);
    }

    bool IopEmulator::stopModule(int32_t moduleId, int32_t *result)
    {
        return m_impl->stopModule(moduleId, result);
    }

    void IopEmulator::runEeCycles(uint64_t eeCycles) noexcept
    {
        m_impl->accountEeCycles();
        m_impl->eeCycleCarry += static_cast<int64_t>(eeCycles);
        // PS2X_IOP_CLOCK_LOG=1: every ~5 s of IOP time, IOP time vs the EE time fed to it and the IOP time credited
        // while the EE waited on a blocked RPC server (IOP ahead of the EE by that much).
        static const bool clockLog = [] { const char *v = std::getenv("PS2X_IOP_CLOCK_LOG"); return v && *v == '1'; }();
        if (clockLog)
        {
            m_impl->eeCyclesFed += eeCycles;
            static uint64_t nextLog = 0u;
            if (m_impl->totalCycles >= nextLog)
            {
                nextLog = m_impl->totalCycles + kIopClockHz * 5u;
                std::fprintf(stderr, "[iop-clock] iop %.4f s ee %.4f s credited %.4f s carry %lld\n",
                             static_cast<double>(m_impl->totalCycles) / kIopClockHz,
                             static_cast<double>(m_impl->eeCyclesFed) / (kIopClockHz * 8.0),
                             static_cast<double>(m_impl->creditedCycles) / kIopClockHz,
                             static_cast<long long>(m_impl->eeCycleCarry));
            }
        }
        if (m_impl->eeCycleCarry < static_cast<int64_t>(kIopBatchCycles * 8u))
            return;
        m_impl->runCycles(static_cast<uint64_t>(m_impl->eeCycleCarry) / 8u);
        m_impl->accountEeCycles();
    }

    RpcResult IopEmulator::handleRpc(const RpcRequest &request)
    {
        return m_impl->rpc.handleRpc(request, *m_impl);
    }

    bool IopEmulator::hasPendingRpc() const noexcept
    {
        return !m_impl->pendingServerCalls.empty() || m_impl->rpc.hasEeRpcInFlight();
    }

    bool IopEmulator::hasRpcServer(uint32_t sid) const noexcept
    {
        return m_impl->rpc.hasServer(sid);
    }

    void IopEmulator::onSifTransfer(const SifTransfer &transfer)
    {
        m_impl->rpc.onSifTransfer(transfer);
    }

    uint32_t IopEmulator::allocateMemory(uint32_t size, uint32_t alignment)
    {
        return m_impl->memory.allocate(size, alignment);
    }

    bool IopEmulator::freeMemory(uint32_t address)
    {
        return m_impl->memory.freeAllocation(address);
    }

    bool IopEmulator::readMemory(uint32_t address, void *destination, size_t size) const
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.readRam(address, destination, size);
    }

    bool IopEmulator::writeMemory(uint32_t address, const void *source, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.writeRam(address, source, size);
    }

    bool IopEmulator::zeroMemory(uint32_t address, size_t size)
    {
        return isMemoryRange(address, size) &&
               m_impl->memory.zeroRam(address, size);
    }

    bool IopEmulator::isMemoryRange(uint32_t address, size_t size) const
    {
        const bool physicalSegment = address < IopMemory::RamSize;
        const bool cachedSegment = address >= 0x80000000u && address < 0x80200000u;
        const bool uncachedSegment = address >= 0xA0000000u && address < 0xA0200000u;
        if (!physicalSegment && !cachedSegment && !uncachedSegment)
            return false;
        const uint32_t physical = IopMemory::physicalAddress(address);
        return physical <= IopMemory::RamSize && size <= IopMemory::RamSize - physical;
    }

    uint64_t IopEmulator::eeCyclesUntilBatch() const noexcept
    {
        const int64_t threshold = static_cast<int64_t>(kIopBatchCycles * 8u);
        return m_impl->eeCycleCarry < threshold ? static_cast<uint64_t>(threshold - m_impl->eeCycleCarry) : 0u;
    }

    uint64_t IopEmulator::cycles() const noexcept
    {
        return m_impl->totalCycles;
    }

    uint64_t IopEmulator::instructions() const noexcept
    {
        return m_impl->totalInstructions;
    }

    uint32_t IopEmulator::loadedModuleCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->modules.size());
    }

    uint32_t IopEmulator::threadCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->kernel.threadCount());
    }

    std::vector<std::string> IopEmulator::describeThreads() const
    {
        return m_impl->kernel.describeThreads();
    }

    uint32_t IopEmulator::rpcServerCount() const noexcept
    {
        return static_cast<uint32_t>(m_impl->rpc.serverCount());
    }

    void IopEmulator::saveState(ps2x::state::Writer &out)
    {
        m_impl->serialize(out);
    }

    void IopEmulator::loadState(ps2x::state::Reader &in)
    {
        m_impl->serialize(in);
    }
}
