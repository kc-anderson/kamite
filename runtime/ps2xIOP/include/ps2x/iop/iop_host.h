#pragma once

#include "ps2x/iop/iop_pad.h"
#include "ps2x/iop/iop_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ps2x::iop
{
    // The SPU2's voices (a game's sound effects; streams come in through the ADMA input instead) scaled before the
    // mix: 1 = as the game set them. Any thread.
    void setVoiceGain(float gain);

    enum class HostPathKind : uint32_t
    {
        ElfDirectory = 0,
        CdRoot = 1,
        CdImage = 2,
        HostRoot = 3,
        MemoryCardRoot = 4,
    };

    enum class LogLevel : uint32_t
    {
        Debug = 0,
        Info = 1,
        Warning = 2,
        Error = 3,
    };

    enum class MemoryCardOperation : uint32_t
    {
        Init,
        GetInfo,
        Open,
        Close,
        Seek,
        Read,
        Write,
        Flush,
        Chdir,
        GetDir,
        SetFileInfo,
        Delete,
        Format,
        Unformat,
        Mkdir,
    };

    struct MemoryCardRequest
    {
        MemoryCardOperation operation = MemoryCardOperation::Init;
        // The fifth argument is carried in $t0 by the EE n32 ABI
        std::array<uint32_t, 5> arguments{};
    };

    class IopHost
    {
    public:
        virtual ~IopHost() = default;

        virtual bool readGuest(uint32_t address, void *destination, size_t size) const = 0;
        virtual bool writeGuest(uint32_t address, const void *source, size_t size) = 0;
        virtual bool zeroGuest(uint32_t address, size_t size) = 0;
        virtual bool normalizeGuestAddress(uint32_t address, uint32_t &normalized) const = 0;

        // IOP RAM is a distinct address space from the EE guest. TODO remove this later
        virtual bool readIopMemory(uint32_t address, void *destination, size_t size) const
        {
            (void)address;
            (void)destination;
            (void)size;
            return false;
        }
        virtual bool writeIopMemory(uint32_t address, const void *source, size_t size)
        {
            (void)address;
            (void)source;
            (void)size;
            return false;
        }
        virtual bool zeroIopMemory(uint32_t address, size_t size)
        {
            (void)address;
            (void)size;
            return false;
        }
        virtual bool normalizeIopAddress(uint32_t address, uint32_t &normalized) const
        {
            (void)address;
            normalized = 0u;
            return false;
        }
        virtual uint32_t allocateIopHandle(IopHandleKind kind) = 0;
        virtual uint32_t allocateGuest(uint32_t size, uint32_t alignment) = 0;
        virtual void freeGuest(uint32_t address) = 0;

        virtual void audioCommand(uint32_t sid, uint32_t function, GuestBuffer send, GuestBuffer receive) = 0;

        virtual void submitAudio(std::span<const int16_t> samples) { (void)samples; }
        virtual void resetAudio() {}

        virtual std::string hostPath(HostPathKind kind) const = 0;
        virtual std::string translateGuestPath(std::string_view path) const = 0;
        virtual uint64_t openHostFile(std::string_view path) = 0;
        virtual bool hostFileSize(uint64_t handle, uint64_t &size) const = 0;
        virtual bool readHostFile(uint64_t handle,
                                  uint64_t offset,
                                  void *destination,
                                  size_t size,
                                  size_t &bytesRead) = 0;
        virtual void closeHostFile(uint64_t handle) = 0;

        virtual int32_t memoryCard(const MemoryCardRequest &request) = 0;

        virtual bool hasGuestFunction(uint32_t address) const = 0;
        virtual bool invokeGuestFunction(uint64_t callToken,
                                         uint32_t address,
                                         uint32_t a0,
                                         uint32_t a1,
                                         uint32_t a2,
                                         uint32_t a3,
                                         uint32_t *resultAddress) = 0;

        // Deliver an IOP -> EE SIF command packet. The default keeps hosts
        // which do not emulate the EE command dispatcher source-compatible.
        virtual bool sendSifCommand(uint32_t commandId,
                                    const void *packet,
                                    size_t packetSize)
        {
            (void)commandId;
            (void)packet;
            (void)packetSize;
            return false;
        }

        virtual bool readPad(uint32_t port, PadInputState &state)
        {
            (void)port;
            state = {};
            return false;
        }

        virtual void padVibration(uint32_t port, uint8_t smallMotor, uint8_t largeMotor)
        {
            (void)port;
            (void)smallMotor;
            (void)largeMotor;
        }

        // Multitap slots (0 = A, the pad plugged straight in; 1-3 = B-D). Hosts without multitap support answer slot A
        // through readPad/padVibration and leave the other slots empty.
        virtual bool readPadSlot(uint32_t port, uint32_t slot, PadInputState &state)
        {
            if (slot == 0u)
                return readPad(port, state);
            state = {};
            return false;
        }

        virtual void padVibrationSlot(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor)
        {
            if (slot == 0u)
                padVibration(port, smallMotor, largeMotor);
        }

        // IOP -> EE RPC (IOP sceSifBindRpc / sceSifCallRpc to a server the EE registered with sceSifRegisterRpc). Sony's
        // MCMAN binds the EE libmc server 0x80000400 (mcman export 62) and calls it from its fast read/write paths.
        // bind: the EE server descriptor and its receive/callback buffers (false: no EE server with this SID yet).
        virtual bool bindEeRpcServer(uint32_t sid, uint32_t &serverData, uint32_t &buffer, uint32_t &callbackBuffer)
        {
            (void)sid;
            serverData = buffer = callbackBuffer = 0u;
            return false;
        }
        // call: the send bytes go to the server's buffer and its function runs on the EE (like the EE RPC loop
        // thread); done(receive bytes, ok) runs on the EE thread when it returns. false: nothing was queued.
        virtual bool callEeRpcServer(uint32_t serverData,
                                     uint32_t function,
                                     std::vector<uint8_t> send,
                                     uint32_t receiveSize,
                                     std::function<void(std::vector<uint8_t> receive, bool ok)> done)
        {
            (void)serverData, (void)function, (void)send, (void)receiveSize, (void)done;
            return false;
        }

        virtual void log(LogLevel level, std::string_view message) = 0;
        // A pending NOWAIT RPC (RpcResult::pending) finished on the IOP; its receive data has been written.
        virtual void completeRpc(uint64_t callToken) { (void)callToken; }
    };
}
