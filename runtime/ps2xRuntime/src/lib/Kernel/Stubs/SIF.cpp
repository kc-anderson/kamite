#include "Common.h"
#include "SIF.h"
#include "ps2x/state_io.h"
#include "../Syscalls/RPC.h"
#include "../../ps2_iop_transport.h"
#include "runtime/ps2_address.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2_stubs
{
    void sceSifCmdIntrHdlr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TODO_NAMED("sceSifCmdIntrHdlr", rdram, ctx, runtime);
    }

    void sceSifLoadModule(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifLoadModule(rdram, ctx, runtime);
    }

    void sceSifSendCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t srcAddr = getRegU32(ctx, 7); // $a3
        const uint32_t dstAddr = readStackU32(rdram, ctx, 16);
        const uint32_t size = readStackU32(rdram, ctx, 20);
        if (size != 0u && srcAddr != 0u && dstAddr != 0u)
        {
            std::vector<uint8_t> payload(size);
            bool valid = runtime != nullptr;
            for (uint32_t i = 0; i < size; ++i)
            {
                const uint8_t *src = getConstMemPtr(rdram, srcAddr + i);
                if (!src)
                {
                    valid = false;
                    break;
                }
                payload[i] = *src;
            }
            if (!valid || !runtime->writeIopMemory(dstAddr, payload.data(), payload.size()))
            {
                setReturnS32(ctx, 0);
                return;
            }
        }

        setReturnS32(ctx, 1);
    }

    namespace
    {
        struct Ps2SifDmaTransfer
        {
            uint32_t src = 0;
            uint32_t dest = 0;
            int32_t size = 0;
            int32_t attr = 0;
        };
        static_assert(sizeof(Ps2SifDmaTransfer) == 16u, "Unexpected SIF DMA descriptor size");

        std::mutex g_sifDmaTransferMutex;
        uint32_t g_nextSifDmaTransferId = 1u;
        std::mutex g_sifCmdStateMutex;
        std::unordered_map<uint32_t, uint32_t> g_sifRegs;
        std::unordered_map<uint32_t, uint32_t> g_sifSregs;
        struct SifCmdHandler
        {
            uint32_t function = 0u;
            uint32_t argument = 0u;
        };

        std::unordered_map<uint32_t, SifCmdHandler> g_sifCmdHandlers;
        uint32_t g_sifCmdBuffer = 0u;
        uint32_t g_sifSysCmdBuffer = 0u;
        bool g_sifCmdInitialized = false;
        uint32_t g_sifGetRegLogCount = 0u;
        uint32_t g_sifSetRegLogCount = 0u;

        constexpr uint32_t kSifRegBootStatus = 0x4u;
        constexpr uint32_t kSifRegMainAddr = 0x80000000u;
        constexpr uint32_t kSifRegSubAddr = 0x80000001u;
        constexpr uint32_t kSifRegMsCom = 0x80000002u;
        constexpr uint32_t kSifBootReadyMask = 0x00020000u;

        void seedDefaultSifRegsLocked()
        {
            g_sifRegs.clear();
            g_sifSregs.clear();
            g_sifCmdHandlers.clear();
            g_sifCmdBuffer = 0u;
            g_sifSysCmdBuffer = 0u;
            g_sifCmdInitialized = false;
            g_sifGetRegLogCount = 0u;
            g_sifSetRegLogCount = 0u;

            g_sifRegs[kSifRegBootStatus] = kSifBootReadyMask;
            g_sifRegs[kSifRegMainAddr] = 0u;
            g_sifRegs[kSifRegSubAddr] = 0u;
            g_sifRegs[kSifRegMsCom] = 0u;
        }

        bool shouldTraceSifReg(uint32_t reg)
        {
            switch (reg)
            {
            case 0x2u:
            case 0x4u:
            case 0x80000000u:
            case 0x80000001u:
            case 0x80000002u:
                return true;
            default:
                return false;
            }
        }

        struct SifStateInitializer
        {
            SifStateInitializer()
            {
                std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
                seedDefaultSifRegsLocked();
            }
        } g_sifStateInitializer;

        // Kamite: transfers complete inside sceSifSetDma, but on hardware a status poll right after
        // sceSifSetDma sees the transfer queued (>= 0). The game's IRX loader (func_002BE240, 0x2BE3D8-0x2BE3E8)
        // spins "while (sceSifDmaStat(id) < 0)" and only exits on that first non-negative status. So each id
        // reports "in progress" (0) to its first poll and "done" (-1) after that; unknown/old ids report done.
        constexpr uint32_t kSifDmaFreshIdSlots = 64u;
        struct FreshSifDma
        {
            uint32_t id = 0u;
            uint64_t issueCycle = 0u;
            uint64_t bytes = 0u;
        };
        std::array<FreshSifDma, kSifDmaFreshIdSlots> g_sifDmaFreshIds{};
        uint32_t g_sifDmaFreshIdNext = 0u;

        // PS2X_SIF_DMA_BUSY (default on): the "in progress" answer to the first poll is only given while the transfer
        // could still be running on hardware: 0.6M EE cycles (2 ms) + 32 per byte (~9 MB/s), generous next to
        // PCSX2, which finishes a SIF DMA within a few hundred cycles. A later poll sees it done, as on the console.
        // The IRX loader polls in the next instructions and still sees it queued once. CRI ADX's EE transfer server
        // (0x340130) polls each PCM transfer it started one vblank later and gives up for that vblank while one is
        // in progress: with "in progress" for every first poll, each transfer into the IOP stream buffer reached
        // CRI's IOP mixer one field later than on hardware (measured: committed 3 DTX exchanges after the write), the feed
        // ran 1/129 short of 48 kHz and the PSM mixer filled a 256/512-sample unit with exact zeros twice every 2.75 s
        // of title music (in play every ~0.4 s: "sound cuts out"). 2026-10-18, port_prep/adx_gaps/EXPERIMENTS.md:
        // 28 -> 0 zero bursts per 50 s boot. PS2X_SIF_DMA_BUSY=0: old behaviour.
        // 2026-10-19: the same exact-zero stream drops came back in stage 3 (7 in 12 s, core 1 ADMA only). The window
        // is now PCSX2-like, 20,000 cycles + 4 per byte (68 us + ~74 MB/s); the IRX loader's poll in the next
        // instructions still sees it queued. A first poll that the 2 ms rule would have answered "in progress" is
        // logged ([sif-dma], at most 40). PS2X_SIF_DMA_SLOW=1: the 2 ms rule.
        bool sifDmaSlowWindow()
        {
            static const bool slow = [] { const char *v = std::getenv("PS2X_SIF_DMA_SLOW"); return v && *v == '1'; }();
            return slow;
        }

        uint64_t sifDmaBusyCycles(uint64_t bytes, bool slow)
        {
            const uint64_t clamped = std::min<uint64_t>(bytes, 1u << 20);
            return slow ? 600000u + clamped * 32u : 20000u + clamped * 4u;
        }

        bool sifDmaBusyWindow()
        {
            static const bool enabled = [] { const char *v = std::getenv("PS2X_SIF_DMA_BUSY"); return !(v && *v == '0'); }();
            return enabled;
        }

        uint32_t allocateSifDmaTransferId(uint64_t cycle, uint64_t bytes)
        {
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            uint32_t id = g_nextSifDmaTransferId++;
            if (id == 0u)
            {
                id = g_nextSifDmaTransferId++;
            }
            g_sifDmaFreshIds[g_sifDmaFreshIdNext++ % kSifDmaFreshIdSlots] = FreshSifDma{id, cycle, bytes};
            return id;
        }

        bool consumeFreshSifDmaTransferId(uint32_t id, uint64_t cycle)
        {
            if (id == 0u)
            {
                return false;
            }
            std::lock_guard<std::mutex> lock(g_sifDmaTransferMutex);
            for (FreshSifDma &slot : g_sifDmaFreshIds)
            {
                if (slot.id == id)
                {
                    slot.id = 0u;
                    if (!sifDmaBusyWindow())
                        return true;
                    const uint64_t elapsed = cycle > slot.issueCycle ? cycle - slot.issueCycle : 0u;
                    const bool busy = elapsed < sifDmaBusyCycles(slot.bytes, sifDmaSlowWindow());
                    static uint32_t logsLeft = 40u;
                    if (!busy && logsLeft != 0u && elapsed < sifDmaBusyCycles(slot.bytes, true))
                    {
                        --logsLeft;
                        std::fprintf(stderr, "[sif-dma] %llu-byte transfer polled %.2f ms after issue: done (the 2 ms rule said in progress)\n",
                                     static_cast<unsigned long long>(slot.bytes), static_cast<double>(elapsed) / 294912.0);
                    }
                    return busy;
                }
            }
            return false;
        }

        bool isCopyableGuestAddress(uint32_t addr)
        {
            if (Ps2AddressInRange(addr, PS2_SCRATCHPAD_BASE, PS2_SCRATCHPAD_SIZE))
            {
                return true;
            }

            if (addr < PS2_EE_UNCACHED_RAM_MIRROR_BASE)
            {
                return true;
            }

            if (Ps2IsUncachedRamMirrorAddress(addr))
            {
                return true;
            }

            if (Ps2IsKseg01Address(addr))
            {
                return true;
            }

            return false;
        }

        bool canAccessEeRange(const uint8_t *rdram, uint32_t address, uint32_t sizeBytes)
        {
            if (!rdram)
            {
                return false;
            }
            if (sizeBytes == 0u)
            {
                return true;
            }
            if (sizeBytes - 1u > std::numeric_limits<uint32_t>::max() - address)
            {
                return false;
            }
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint32_t byteAddress = address + i;
                if (!isCopyableGuestAddress(byteAddress) || getConstMemPtr(rdram, byteAddress) == nullptr)
                {
                    return false;
                }
            }
            return true;
        }

        bool readEeRange(const uint8_t *rdram, uint32_t address, void *destination, uint32_t sizeBytes)
        {
            if ((!destination && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            auto *bytes = static_cast<uint8_t *>(destination);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                const uint8_t *source = getConstMemPtr(rdram, address + i);
                if (!source)
                    return false;
                bytes[i] = *source;
            }
            return true;
        }

        bool writeEeRange(uint8_t *rdram, uint32_t address, const void *source, uint32_t sizeBytes)
        {
            if ((!source && sizeBytes != 0u) || !canAccessEeRange(rdram, address, sizeBytes))
                return false;
            ps2TraceGuestRangeWrite(rdram, address, sizeBytes, "SIF IOP-to-EE DMA", nullptr);
            const auto *bytes = static_cast<const uint8_t *>(source);
            for (uint32_t i = 0u; i < sizeBytes; ++i)
            {
                uint8_t *destination = getMemPtr(rdram, address + i);
                if (!destination)
                    return false;
                *destination = bytes[i];
            }
            return true;
        }
    }

    void resetSifState()
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
    }

    bool dispatchSifCommand(uint8_t *rdram,
                            PS2Runtime *runtime,
                            uint32_t commandId,
                            const void *packet,
                            size_t packetSize) noexcept
    {
        if (!rdram || !runtime || !packet || packetSize < 16u || packetSize > 112u)
            return false;

        SifCmdHandler registered{};
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            const auto handler = g_sifCmdHandlers.find(commandId);
            if (handler == g_sifCmdHandlers.end() || handler->second.function == 0u)
                return false;
            registered = handler->second;
        }

        if (!runtime->hasFunction(registered.function))
            return false;

        const uint32_t packetAddress = runtime->guestMalloc(static_cast<uint32_t>(packetSize), 16u);
        if (packetAddress == 0u)
            return false;

        uint8_t *const first = getMemPtr(rdram, packetAddress);
        uint8_t *const last = getMemPtr(rdram, packetAddress + static_cast<uint32_t>(packetSize - 1u));
        if (!first || !last || last < first || static_cast<size_t>(last - first) != packetSize - 1u)
        {
            runtime->guestFree(packetAddress);
            return false;
        }

        ps2TraceGuestRangeWrite(rdram, packetAddress, static_cast<uint32_t>(packetSize), "SIF command packet", nullptr);
        std::memcpy(first, packet, packetSize);

        try
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::SifCommand;
            invocation.tag = commandId;
            invocation.context = runtime->cpu();
            invocation.context.pc = registered.function;
            SET_GPR_U32(&invocation.context, 4, packetAddress);
            SET_GPR_U32(&invocation.context, 5, registered.argument);
            SET_GPR_U32(&invocation.context, 6, 0u);
            SET_GPR_U32(&invocation.context, 7, 0u);
            SET_GPR_U32(&invocation.context, 29, 0u);
            SET_GPR_U32(&invocation.context, 31, 0u);
            invocation.onComplete = [runtime, packetAddress](const R5900Context &, R5900Context &)
            {
                runtime->guestFree(packetAddress);
            };
            runtime->eeScheduler().queueInvocation(std::move(invocation));
            return true;
        }
        catch (...)
        {
            runtime->guestFree(packetAddress);
            return false;
        }
    }

    void sceSifAddCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        const uint32_t handler = getRegU32(ctx, 5);
        const uint32_t argument = getRegU32(ctx, 6);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers[cid] = SifCmdHandler{handler, argument};
        setReturnS32(ctx, 0);
    }

    void sceSifAllocIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t reqSize = getRegU32(ctx, 4);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(reqSize, 64u) : 0u);
    }

    void sceSifAllocSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t size = getRegU32(ctx, 5);
        setReturnU32(ctx, runtime ? runtime->allocateIopMemory(size, 64u) : 0u);
    }

    void sceSifBindRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifBindRpc(rdram, ctx, runtime);
    }

    void sceSifCheckStatRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifCheckStatRpc(rdram, ctx, runtime);
    }

    void sceSifDmaStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t trid = getRegU32(ctx, 4);

        // Transfers are applied immediately by sceSifSetDma in this runtime; the first poll of a fresh id still
        // reports "in progress" while the transfer would still be running on hardware (see sifDmaBusyWindow).
        setReturnS32(ctx, consumeFreshSifDmaTransferId(trid, runtime ? runtime->eeScheduler().currentCycle() : 0u) ? 0 : -1);
    }

    void sceSifExecRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifExitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        seedDefaultSifRegsLocked();
        setReturnS32(ctx, 0);
    }

    void sceSifExitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifFreeIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifFreeSysMemory(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const uint32_t addr = getRegU32(ctx, 4);
        setReturnS32(ctx, runtime && runtime->freeIopMemory(addr) ? 0 : -1);
    }

    void sceSifGetDataTable(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        setReturnU32(ctx, g_sifCmdBuffer);
    }

    void sceSifGetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 4));
    }

    void sceSifGetNextRequest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifGetOtherData(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rdAddr = getRegU32(ctx, 4);
        const uint32_t srcAddr = getRegU32(ctx, 5);
        const uint32_t dstAddr = getRegU32(ctx, 6);
        const int32_t sizeSigned = static_cast<int32_t>(getRegU32(ctx, 7));

        if (sizeSigned <= 0)
        {
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t size = static_cast<uint32_t>(sizeSigned);
        if (size > PS2_RAM_SIZE)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                std::cerr << "sceSifGetOtherData rejected oversized transfer size=0x"
                          << std::hex << size << std::dec << std::endl;
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        std::vector<uint8_t> payload(size);
        if (!runtime || !runtime->isIopMemoryRange(srcAddr, size) ||
            !canAccessEeRange(rdram, dstAddr, size) ||
            !runtime->readIopMemory(srcAddr, payload.data(), payload.size()) ||
            !writeEeRange(rdram, dstAddr, payload.data(), size))
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifGetOtherData copy failed src=0x" << std::hex << srcAddr
                              << " dst=0x" << dstAddr
                              << " size=0x" << size
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, -1);
            return;
        }

        // SifRpcReceiveData_t keeps src/dest/size at offsets 0x10/0x14/0x18.
        if (uint8_t *rd = getMemPtr(rdram, rdAddr))
        {
            std::memcpy(rd + 0x10u, &srcAddr, sizeof(srcAddr));
            std::memcpy(rd + 0x14u, &dstAddr, sizeof(dstAddr));
            std::memcpy(rd + 0x18u, &size, sizeof(size));
        }

        if (runtime)
        {
            PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                ps2x::iop::SifTransferKind::GetOtherData,
                                                                ps2x::iop::SifTransferPhase::AfterCopy,
                                                                srcAddr,
                                                                dstAddr,
                                                                size,
                                                            });
        }

        setReturnS32(ctx, 0);
    }

    void sceSifGetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                value = it->second;
            }
            shouldLog = shouldTraceSifReg(reg) && g_sifGetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifGetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifGetReg] reg=0x" << std::hex << reg
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, value);
    }

    void sceSifGetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        uint32_t value = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                value = it->second;
            }
        }
        setReturnU32(ctx, value);
    }

    void sceSifInitCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdInitialized = true;
        setReturnS32(ctx, 0);
    }

    void sceSifInitIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The physical IOP allocator is initialized by IopSubsystem::reset().
        setReturnS32(ctx, 0);
    }

    void sceSifInitRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifInitRpc(rdram, ctx, runtime);
    }

    void sceSifIsAliveIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifLoadElf(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElf(rdram, ctx, runtime);
    }

    void sceSifLoadElfPart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadElfPart(rdram, ctx, runtime);
    }

    void sceSifLoadFileReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadIopHeap(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifLoadModuleBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::sceSifLoadModuleBuffer(rdram, ctx, runtime);
    }

    void sceSifRebootIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRegisterRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRegisterRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveCmdHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cid = getRegU32(ctx, 4);
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        g_sifCmdHandlers.erase(cid);
        setReturnS32(ctx, 0);
    }

    void sceSifRemoveRpc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpc(rdram, ctx, runtime);
    }

    void sceSifRemoveRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifRemoveRpcQueue(rdram, ctx, runtime);
    }

    void sceSifResetIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PS2IopTransport::reset(runtime);
        setReturnS32(ctx, 1);
    }

    void sceSifRpcLoop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSetCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifCmdBuffer;
            g_sifCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void isceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDChain(rdram, ctx, runtime);
    }

    void isceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceSifSetDma(rdram, ctx, runtime);
    }

    void sceSifSetDChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void sceSifSetDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t dmatAddr = getRegU32(ctx, 4);
        const uint32_t count = getRegU32(ctx, 5);

        const uint32_t listAddr = getRegU32(ctx, 4);
        PS2_IF_AGRESSIVE_LOGS({
            std::cerr << "[sceSifSetDma:CALL] pc=0x" << std::hex << ctx->pc
                      << " ra=0x" << getRegU32(ctx, 31)
                      << " list=0x" << listAddr
                      << " count=" << std::dec << count
                      << std::endl;

            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                const uint32_t src = READ32(desc + 0);
                const uint32_t dst = READ32(desc + 4);
                const uint32_t size = READ32(desc + 8);
                const uint32_t attr = READ32(desc + 12);

                std::cerr << "[sceSifSetDma:DESC] i=" << i
                          << " src=0x" << std::hex << src
                          << " dst=0x" << dst
                          << " size=0x" << size
                          << " attr=0x" << attr
                          << " pc=0x" << ctx->pc
                          << " ra=0x" << getRegU32(ctx, 31)
                          << std::dec << std::endl;
            }
        });

        // PS2X_EE_SIFDMA_LOG=1: one line per EE -> IOP sceSifSetDma descriptor (source, IOP destination, size, attr).
        static const bool logEeSifDma = [] { const char *v = std::getenv("PS2X_EE_SIFDMA_LOG"); return v && *v == '1'; }();
        if (logEeSifDma && dmatAddr && count != 0u && count <= 32u)
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t desc = listAddr + i * 16;
                std::fprintf(stderr, "[ee-sifdma] ee 0x%08x -> iop 0x%06x size=%u attr=0x%x ra=0x%x eecycle=%llu\n", READ32(desc + 0),
                             READ32(desc + 4), READ32(desc + 8), READ32(desc + 12), getRegU32(ctx, 31),
                             static_cast<unsigned long long>(runtime ? runtime->eeScheduler().currentCycle() : 0u));
            }
        }

        if (!dmatAddr || count == 0u || count > 32u)
        {
            setReturnS32(ctx, 0);
            return;
        }

        std::array<Ps2SifDmaTransfer, 32u> pending{};
        uint32_t pendingCount = 0u;
        bool ok = true;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entryAddr = dmatAddr + (i * static_cast<uint32_t>(sizeof(Ps2SifDmaTransfer)));
            const uint8_t *entry = getConstMemPtr(rdram, entryAddr);
            if (!entry)
            {
                ok = false;
                break;
            }

            Ps2SifDmaTransfer xfer{};
            std::memcpy(&xfer, entry, sizeof(xfer));
            if (xfer.size <= 0)
            {
                continue;
            }

            const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
            if (sizeBytes > PS2_RAM_SIZE)
            {
                ok = false;
                break;
            }
            if (!runtime || !canAccessEeRange(rdram, xfer.src, sizeBytes) || !runtime->isIopMemoryRange(xfer.dest, sizeBytes))
            {
                ok = false;
                break;
            }

            pending[pendingCount++] = xfer;
        }

        if (ok)
        {
            for (uint32_t i = 0; i < pendingCount; ++i)
            {
                const Ps2SifDmaTransfer &xfer = pending[i];
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::BeforeCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
                const uint32_t sizeBytes = static_cast<uint32_t>(xfer.size);
                std::vector<uint8_t> payload(sizeBytes);
                if (!readEeRange(rdram, xfer.src, payload.data(), sizeBytes) || !runtime->writeIopMemory(xfer.dest, payload.data(), payload.size()))
                {
                    ok = false;
                    break;
                }
                // PS2X_EE_SIFDMA_DUMP=<iop address hex>: the first 0x100 bytes of every EE -> IOP transfer to that address.
                static const uint32_t dumpDest = [] { const char *v = std::getenv("PS2X_EE_SIFDMA_DUMP"); return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 16)) : ~0u; }();
                if (xfer.dest == dumpDest)
                {
                    std::fprintf(stderr, "[ee-sifdma-dump] iop 0x%x size=%u:", xfer.dest, sizeBytes);
                    for (uint32_t i = 0; i + 4u <= sizeBytes && i < 0x100u; i += 4u)
                    {
                        uint32_t word;
                        std::memcpy(&word, payload.data() + i, 4u);
                        std::fprintf(stderr, " %08x", word);
                    }
                    std::fputc('\n', stderr);
                }
                if (runtime)
                {
                    PS2IopTransport::notifyTransfer(runtime, rdram, {
                                                                        ps2x::iop::SifTransferKind::SetDma,
                                                                        ps2x::iop::SifTransferPhase::AfterCopy,
                                                                        xfer.src,
                                                                        xfer.dest,
                                                                        static_cast<uint32_t>(xfer.size),
                                                                    });
                }
            }
        }

        if (!ok)
        {
            static uint32_t warnCount = 0;
            if (warnCount < 32u)
            {
                PS2_IF_AGRESSIVE_LOGS({
                    std::cerr << "sceSifSetDma failed dmat=0x" << std::hex << dmatAddr
                              << " count=0x" << count
                              << std::dec << std::endl;
                });
                ++warnCount;
            }
            setReturnS32(ctx, 0);
            return;
        }

        ps2_syscalls::dispatchDmacHandlersForCause(rdram, runtime, 5u);

        uint64_t transferBytes = 0u;
        for (uint32_t i = 0; i < count && i < 32u; ++i)
            transferBytes += READ32(listAddr + i * 16u + 8u);
        setReturnS32(ctx, static_cast<int32_t>(allocateSifDmaTransferId(runtime ? runtime->eeScheduler().currentCycle() : 0u, transferBytes)));
    }

    void sceSifSetIopAddr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnU32(ctx, getRegU32(ctx, 5));
    }

    void sceSifSetReg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        bool shouldLog = false;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifRegs.find(reg);
            if (it != g_sifRegs.end())
            {
                prev = it->second;
            }
            g_sifRegs[reg] = value;
            shouldLog = shouldTraceSifReg(reg) && g_sifSetRegLogCount < 128u;
            if (shouldLog)
            {
                ++g_sifSetRegLogCount;
            }
        }
        if (shouldLog)
        {
            PS2_IF_AGRESSIVE_LOGS({
                auto flags = std::cerr.flags();
                std::cerr << "[sceSifSetReg] reg=0x" << std::hex << reg
                          << " prev=0x" << prev
                          << " value=0x" << value
                          << " pc=0x" << (ctx ? ctx->pc : 0u)
                          << " ra=0x" << (ctx ? getRegU32(ctx, 31) : 0u)
                          << std::dec << std::endl;
                std::cerr.flags(flags);
            });
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetRpcQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ps2_syscalls::SifSetRpcQueue(rdram, ctx, runtime);
    }

    void sceSifSetSreg(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t reg = getRegU32(ctx, 4);
        const uint32_t value = getRegU32(ctx, 5);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            auto it = g_sifSregs.find(reg);
            if (it != g_sifSregs.end())
            {
                prev = it->second;
            }
            g_sifSregs[reg] = value;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifSetSysCmdBuffer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t newBuffer = getRegU32(ctx, 4);
        uint32_t prev = 0u;
        {
            std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
            prev = g_sifSysCmdBuffer;
            g_sifSysCmdBuffer = newBuffer;
        }
        setReturnU32(ctx, prev);
    }

    void sceSifStopDma(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void sceSifSyncIop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 1);
    }

    void sceSifWriteBackDCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    template <typename Ar>
    static void serializeSifStubState(Ar &ar)
    {
        std::lock_guard<std::mutex> lock(g_sifCmdStateMutex);
        ps2x::state::fields(ar, g_sifRegs, g_sifSregs, g_sifCmdHandlers, g_sifCmdBuffer, g_sifSysCmdBuffer, g_sifCmdInitialized);
        std::lock_guard<std::mutex> dmaLock(g_sifDmaTransferMutex);
        ps2x::state::field(ar, g_nextSifDmaTransferId);
    }

    void saveSifStubState(ps2x::state::Writer &out)
    {
        serializeSifStubState(out);
    }

    void loadSifStubState(ps2x::state::Reader &in)
    {
        serializeSifStubState(in);
    }
}
