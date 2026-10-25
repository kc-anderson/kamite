// DRAFT for Kamite: adapted from the Urban Reign port's src/urban/hle_bindings.cpp
// (forks/urban-reign-recomp @2fa53c4, GPL-3.0). Changes: namespace/macros/env vars urban -> kamite, the address table
// is the game's (kamite_sdk_functions.inc, SLUS_215.03) and Urban-specific comments were replaced. The draft's ELF
// probe was removed (game-shell's disc check already pins the boot ELF sha256). The binding policy (kHleNames, kHleAliases, MPEG/IPU gate) is unchanged:
// keep the HLE list minimal and run the game's own SDK code everywhere else.

#include "kamite/hle_bindings.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"

#include "ps2_runtime_calls.h"
#include "ps2_stubs.h"
#include "ps2_syscalls.h"

#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

namespace kamite
{
    namespace
    {
        constexpr SdkFunction kSdkFunctions[] = {
#define X(address, name, method, score) {address, #name, method, score},
#include "kamite/kamite_sdk_functions.inc"
#undef X
        };

        // Same list as the Urban port (= the set the SotC port binds by name, plus the extra libcdvd entry points).
        // Everything else in the SDK - GS/DMA/VIF packet libraries, libvu0, libpad, libsdr, libmc, newlib - stays
        // original recompiled code. The game (SDK 3.1) was linked with dead-code stripping, so about 48 of these
        // names are not in its ELF at all (no sceDeci2*/sceTty*, no sceSifGetOtherData, no sceCdSeek/Stop/Pause...);
        // they are reported once at start-up. Per-name evidence: port_prep/hle-bindings/tools/kamite_binding_table.tsv.
        constexpr std::string_view kHleNames[] = {
            "sceSifStopDma", "sceSifDmaStat", "sceSifSetDma", "isceSifSetDma", "sceSifSetDChain", "isceSifSetDChain",
            "sceSifSetReg", "sceSifGetReg", "sceSifGetSreg", "sceSifSetSreg", "sceSifGetDataTable",
            "sceSifInitCmd", "sceSifExitCmd", "sceSifSetCmdBuffer", "sceSifSetSysCmdBuffer",
            "sceSifAddCmdHandler", "sceSifRemoveCmdHandler", "sceSifSendCmd", "sceSifWriteBackDCache",
            "sceSifInitRpc", "sceSifExitRpc", "sceSifGetOtherData", "sceSifBindRpc", "sceSifCallRpc", "sceSifCheckStatRpc",
            "sceSifSetRpcQueue", "sceSifRegisterRpc", "sceSifRemoveRpc", "sceSifRemoveRpcQueue",
            "sceSifGetNextRequest", "sceSifExecRequest", "sceSifRpcLoop",
            "sceSifInitIopHeap", "sceSifAllocIopHeap", "sceSifAllocSysMemory", "sceSifFreeSysMemory", "sceSifFreeIopHeap",
            "sceSifLoadIopHeap", "sceSifLoadFileReset", "sceSifLoadModuleBuffer", "sceSifLoadModule", "sceSifLoadElfPart",
            "sceSifLoadElf", "sceSifGetIopAddr", "sceSifSetIopAddr", "sceSifResetIop", "sceSifIsAliveIop", "sceSifSyncIop",
            "sceSifRebootIop",
            "sceFsInit", "sceFsReset", "sceOpen", "sceClose", "sceLseek", "sceRead", "sceWrite", "sceIoctl",
            "sceDeci2Open", "sceDeci2Close", "sceDeci2ReqSend", "sceDeci2Poll", "sceDeci2ExRecv", "sceDeci2ExSend",
            "sceDeci2ExReqSend", "sceDeci2ExLock", "sceDeci2ExUnLock",
            "sceTtyWrite", "sceTtyRead", "sceTtyInit", "sceResetttyinit", "scePrintf",
            "sceCdCallback", "sceCdInitEeCB", "sceCdSearchFile", "sceCdNcmdDiskReady", "sceCdSync", "sceCdSyncS", "sceCdInit",
            "sceCdDiskReady", "sceCdMmode", "sceCdRead", "sceCdReadIOPm", "sceCdGetError", "sceCdStatus", "sceCdReadClock",
            // Not in the SotC list, but they talk to the same missing cdvdfsv server (the game links only sceCdGetDiskType
            // and sceCdBreak of these).
            "sceCdGetDiskType", "sceCdSeek", "sceCdBreak", "sceCdStandby", "sceCdStop", "sceCdPause", "sceCdTrayReq",
            "sceMpegCreate", "sceMpegInit", "sceMpegDelete", "sceMpegAddBs", "sceMpegGetPicture", "sceMpegGetPictureRAW8",
            "sceMpegReset", "sceMpegIsEnd", "sceMpegIsRefBuffEmpty", "sceMpegSetDecodeMode", "sceMpegGetDecodeMode",
            "sceMpegAddCallback", "sceMpegClearRefBuff", "sceMpegGetPictureRAW8xy", "sceMpegSetDefaultPtsGap",
            "sceMpegResetDefaultPtsGap", "sceMpegSetImageBuff", "sceMpegDispWidth", "sceMpegDispHeight",
            "sceMpegDispCenterOffX", "sceMpegDispCenterOffY", "sceMpegDemuxPssRing", "sceMpegDemuxPss", "sceMpegAddStrCallback",
            "sceIpuStopDMA", "sceIpuRestartDMA", "sceIpuSync", "sceIpuInit",
            "_sceSDC", "_sceIDC",
        };
        // Linked and called by the game but deliberately NOT in this list, because the runtime has no handler for them
        // (ps2_call_list.h): sceDopen 0x3B52F0, sceDread 0x3B5520, sceDclose 0x3B53B8 (func_00386700, a "host0:"
        // directory scan reached from 0x385E98 <- 0x37E8D8) and sceLseek64 0x3B56A0 (func_00331DB8, only when the word
        // at 0x3D8480 is 1). They run as the game's own recompiled file-I/O code, which expects the state sceFsInit sets
        // up; sceFsInit itself is bound to the runtime handler, so these paths may fail (sceLseek64 returns -9).

        void traceCall(const R5900Context *ctx)
        {
            const SdkFunction *function = sdkFunctionAt(ctx->pc);
            KM_TRACE(Hook, "HLE " << (function ? function->name : std::string_view("?")) << std::hex << " a0=0x" << getRegU32(ctx, 4)
                                       << " a1=0x" << getRegU32(ctx, 5) << " a2=0x" << getRegU32(ctx, 6) << " a3=0x" << getRegU32(ctx, 7)
                                       << " ra=0x" << getRegU32(ctx, 31));
        }

        // SDK functions the runtime has no handler of their own for, bound to a compatible handler.
        struct HleAlias
        {
            std::string_view name;
            std::string_view handler;
            std::string_view why;
        };
        constexpr HleAlias kHleAliases[] = {
            // sceCdLayerSearchFile(fp, name, layer): same first two arguments. The game's disc is single-layer
            // (765,472 sectors). The game calls it directly from 0x332864 and through sceCdSearchFile (0x398B90).
            {"sceCdLayerSearchFile", "sceCdSearchFile", "layer argument ignored (single-layer disc)"},
        };

        // Equivalent of the recompiler's generated stub body: return to $ra unless the handler redirects.
        // The dispatcher has set ctx->pc to the called address, which names the SDK function for tracing.
        template <PS2Runtime::RecompiledFunction Handler>
        void hleTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            if (log::traceEnabled(log::Category::Hook))
                traceCall(ctx);
            ctx->pc = getRegU32(ctx, 31);
            Handler(rdram, ctx, runtime);
            if (log::traceEnabled(log::Category::Hook))
                KM_TRACE(Hook, "    -> v0=0x" << std::hex << getRegU32(ctx, 2));
        }

        const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> &syscallHandlers()
        {
            static const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> handlers = {
#define KAMITE_SYSCALL_ENTRY(name) {#name, &hleTrampoline<&ps2_syscalls::name>},
                PS2_SYSCALL_LIST(KAMITE_SYSCALL_ENTRY)
#undef KAMITE_SYSCALL_ENTRY
            };
            return handlers;
        }

        const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> &stubHandlers()
        {
            static const std::unordered_map<std::string_view, PS2Runtime::RecompiledFunction> handlers = {
#define KAMITE_STUB_ENTRY(name) {#name, &hleTrampoline<&ps2_stubs::name>},
                PS2_STUB_LIST(KAMITE_STUB_ENTRY)
#undef KAMITE_STUB_ENTRY
            };
            return handlers;
        }

        // Same resolution order as the recompiler: syscall list first, then stubs, with the leading-underscore alias.
        PS2Runtime::RecompiledFunction resolveHandler(std::string_view name, std::string &resolvedName)
        {
            if (const std::string_view syscall = ps2_runtime_calls::resolveSyscallName(name); !syscall.empty())
            {
                resolvedName = "ps2_syscalls::" + std::string(syscall);
                return syscallHandlers().at(syscall);
            }
            if (const std::string_view stub = ps2_runtime_calls::resolveStubName(name); !stub.empty())
            {
                resolvedName = "ps2_stubs::" + std::string(stub);
                return stubHandlers().at(stub);
            }
            return nullptr;
        }

        bool mpegHleWanted()
        {
            const char *ipu = std::getenv("PS2X_IPU");
            const char *hle = std::getenv("KAMITE_MPEG_HLE");
            return (hle && *hle == '1') || (ipu && *ipu == '0');
        }

        std::set<std::string, std::less<>> skippedFromEnvironment()
        {
            std::set<std::string, std::less<>> skipped;
            const char *value = std::getenv("KAMITE_HLE_SKIP");
            std::string_view list = value ? value : "";
            while (!list.empty())
            {
                const size_t comma = list.find(',');
                if (const std::string_view token = list.substr(0, comma); !token.empty())
                    skipped.emplace(token);
                list = comma == std::string_view::npos ? std::string_view() : list.substr(comma + 1);
            }
            return skipped;
        }
    }

    const SdkFunction *findSdkFunction(std::string_view name)
    {
        for (const SdkFunction &function : kSdkFunctions)
        {
            if (function.name == name)
                return &function;
        }
        return nullptr;
    }

    const SdkFunction *sdkFunctionAt(uint32_t address)
    {
        for (const SdkFunction &function : kSdkFunctions)
        {
            if (function.address == address)
                return &function;
        }
        return nullptr;
    }

    void installHleBindings(PS2Runtime &runtime)
    {
        // No ELF probe: game_disc.cpp already refuses any boot ELF whose sha256 is not SLUS_215.03's (game_disc.h).
        (void)runtime;
        const auto skipped = skippedFromEnvironment();
        size_t bound = 0;
        std::ostringstream missing;
        for (const std::string_view name : kHleNames)
        {
            const SdkFunction *function = findSdkFunction(name);
            if (!function)
            {
                missing << ' ' << name;
                continue;
            }
            if (skipped.count(name))
            {
                KM_INFO(Hook, "HLE binding skipped by KAMITE_HLE_SKIP: " << name);
                continue;
            }
            // By default the game's own libmpeg/libipu drive the runtime's emulated IPU (on unless PS2X_IPU=0); in the
            // Urban port the libmpeg HLE left the FMVs black (Urban docs/PROGRESS.md §89). KAMITE_MPEG_HLE=1 binds it.
            if ((name.rfind("sceMpeg", 0) == 0 || name.rfind("sceIpu", 0) == 0) && !mpegHleWanted())
                continue;
            std::string resolvedName;
            const PS2Runtime::RecompiledFunction handler = resolveHandler(name, resolvedName);
            if (!handler)
            {
                KM_ERROR(Hook, "no runtime handler for " << name);
                continue;
            }
            std::ostringstream reason;
            reason << resolvedName << " (located: " << function->method;
            if (function->method == "near")
                reason << ' ' << std::fixed << std::setprecision(2) << function->score;
            reason << ')';
            if (FunctionHooks::instance().replace(function->address, std::string(name), handler, ReplacementStatus::Hle, reason.str()))
                ++bound;
        }
        for (const HleAlias &alias : kHleAliases)
        {
            const SdkFunction *function = findSdkFunction(alias.name);
            if (!function || skipped.count(alias.name))
                continue;
            std::string resolvedName;
            const PS2Runtime::RecompiledFunction handler = resolveHandler(alias.handler, resolvedName);
            if (handler && FunctionHooks::instance().replace(function->address, std::string(alias.name), handler, ReplacementStatus::Hle,
                                                             resolvedName + ": " + std::string(alias.why)))
                ++bound;
        }
        KM_INFO(Hook, "HLE: bound " << bound << " SDK functions; not located in this ELF:" << missing.str());
    }
}
