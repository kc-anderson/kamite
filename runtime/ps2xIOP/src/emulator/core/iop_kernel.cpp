#include <cstdint>
#include "iop_kernel.h"

#include <cstdio>
#include <cstdlib>

#include "iop_memory.h"
#include "../iop_emulator_const.h"

#include <algorithm>
#include <sstream>

namespace ps2x::iop::detail
{
    namespace
    {
        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            if (alignment <= 1u)
                return value;
            const uint32_t mask = alignment - 1u;
            return (value + mask) & ~mask;
        }
    }

    IopKernel::IopKernel(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    void IopKernel::reset()
    {
        m_threads.clear();
        m_semaphores.clear();
        m_eventFlags.clear();
        m_nextThreadId = 1;
        m_nextSemaphoreId = 1;
        m_nextEventFlagId = 1;
        m_nextCallThreadId = kFirstCallThreadId;
        m_currentThread = nullptr;
        m_continueId = 0;
        m_heldId = 0;
        m_heldSince = 0u;
    }

    bool IopKernel::dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };

        switch (ordinal)
        {
        case 4: // CreateThread
        {
            const uint32_t descriptor = cpu.gpr[4];
            IopThread thread;
            thread.id = static_cast<int>(m_nextThreadId++);
            thread.attr = m_memory.read32(descriptor + 0u);
            thread.option = m_memory.read32(descriptor + 4u);
            thread.entry = m_memory.read32(descriptor + 8u);
            thread.stackSize = std::max<uint32_t>(m_memory.read32(descriptor + 12u), 0x100u);
            thread.priority = std::clamp<uint32_t>(m_memory.read32(descriptor + 16u), 1u, 126u);
            thread.initialPriority = thread.priority;
            thread.stackBase = m_memory.allocate(thread.stackSize + kStackGuardBytes, 16u);
            if (thread.stackBase == 0u)
            {
                setV0(-400);
                return true;
            }
            const int id = thread.id;
            m_threads.emplace(id, std::move(thread));
            setV0(id);
            return true;
        }
        case 5: // DeleteThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.stackBase != 0u)
                (void)m_memory.freeAllocation(it->second.stackBase);
            m_threads.erase(it);
            setV0(0);
            return true;
        }
        case 6: // StartThread
        case 7: // StartThreadArgs
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            IopThread &thread = it->second;
            thread.cpu = {};
            thread.cpu.pc = thread.entry;
            thread.cpu.gpr[4] = cpu.gpr[5];
            thread.cpu.gpr[5] = ordinal == 7 ? cpu.gpr[6] : 0u;
            thread.cpu.gpr[28] = cpu.gpr[28];
            thread.cpu.gpr[29] = alignUp(thread.stackBase + thread.stackSize, 16u) - 16u;
            thread.cpu.gpr[31] = kThreadReturnSentinel;
            thread.state = IopThreadState::Ready;
            yieldIfHigherPriorityReady(cpu);
            setV0(0);
            return true;
        }
        case 8: // ExitThread
        case 9: // ExitDeleteThread
            if (m_currentThread != nullptr)
            {
                m_currentThread->state = ordinal == 9 ? IopThreadState::Dead : IopThreadState::Dormant;
                cpu.stopped = true;
                cpu.yielded = true;
            }
            setV0(0);
            return true;
        case 10:
        case 11: // TerminateThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            it->second.state = IopThreadState::Dormant;
            setV0(0);
            return true;
        }
        case 12:
        case 13:
            setV0(0);
            return true;
        case 14:
        case 15:
        {
            int id = static_cast<int>(cpu.gpr[4]);
            if (id == 0 && m_currentThread != nullptr)
                id = m_currentThread->id;
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            it->second.priority = std::clamp<uint32_t>(cpu.gpr[5], 1u, 126u);
            setV0(0);
            return true;
        }
        case 16:
        case 17:
            setV0(0);
            cpu.yielded = true;
            return true;
        case 18:
        case 19:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.state == IopThreadState::Sleep ||
                it->second.state == IopThreadState::Delay ||
                it->second.state == IopThreadState::Semaphore ||
                it->second.state == IopThreadState::EventFlag)
            {
                it->second.state = IopThreadState::Ready;
            }
            setV0(0);
            return true;
        }
        case 20:
            setV0(m_currentThread != nullptr ? m_currentThread->id : 0);
            return true;
        case 21:
            setV0(0x1000);
            return true;
        case 22:
        case 23:
            setV0(referThreadStatus(static_cast<int>(cpu.gpr[4]), cpu.gpr[5]) ? 0 : -1);
            return true;
        case 24: // SleepThread
            if (m_currentThread != nullptr)
            {
                if (m_currentThread->wakeupCount > 0)
                    --m_currentThread->wakeupCount;
                else
                    sleepCurrent(cpu);
            }
            setV0(0);
            return true;
        case 25:
        case 26:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.state == IopThreadState::Sleep)
                it->second.state = IopThreadState::Ready;
            else
                ++it->second.wakeupCount;
            if (ordinal == 25)
                yieldIfHigherPriorityReady(cpu);
            setV0(0);
            return true;
        }
        case 27:
        case 28:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            const int old = it->second.wakeupCount;
            it->second.wakeupCount = 0;
            setV0(old);
            return true;
        }
        case 29:
        case 30: // SuspendThread / iSuspendThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            it->second.state = IopThreadState::Suspended;
            if (m_currentThread == &it->second)
                cpu.yielded = true;
            setV0(0);
            return true;
        }
        case 31:
        case 32: // ResumeThread / iResumeThread
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_threads.find(id);
            if (it == m_threads.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.state == IopThreadState::Suspended)
                it->second.state = IopThreadState::Ready;
            if (ordinal == 31)
                yieldIfHigherPriorityReady(cpu);
            setV0(0);
            return true;
        }
        case 33: // DelayThread
            if (m_currentThread != nullptr)
            {
                const uint64_t delayCycles = (static_cast<uint64_t>(cpu.gpr[4]) * kIopClockHz + 999'999ull) / 1'000'000ull;
                delayCurrentUntil(currentCycle + std::max<uint64_t>(delayCycles, 1u), cpu);
            }
            setV0(0);
            return true;
        case 34: // GetSystemTime
            m_memory.write32(cpu.gpr[4], static_cast<uint32_t>(currentCycle));
            m_memory.write32(cpu.gpr[4] + 4u, static_cast<uint32_t>(currentCycle >> 32u));
            setV0(0);
            return true;
        case 35:
        case 36:
        case 37:
        case 38:
            setV0(0);
            return true;
        case 39: // USec2SysClock
        {
            const uint64_t cycles = (static_cast<uint64_t>(cpu.gpr[4]) * kIopClockHz) / 1'000'000ull;
            m_memory.write32(cpu.gpr[5], static_cast<uint32_t>(cycles));
            m_memory.write32(cpu.gpr[5] + 4u, static_cast<uint32_t>(cycles >> 32u));
            setV0(0);
            return true;
        }
        case 40:
        {
            const uint64_t cycles = static_cast<uint64_t>(m_memory.read32(cpu.gpr[4])) | (static_cast<uint64_t>(m_memory.read32(cpu.gpr[4] + 4u)) << 32u);
            const uint64_t usec = (cycles * 1'000'000ull) / kIopClockHz;
            if (cpu.gpr[5] != 0u)
                m_memory.write32(cpu.gpr[5], static_cast<uint32_t>(usec / 1'000'000ull));
            if (cpu.gpr[6] != 0u)
                m_memory.write32(cpu.gpr[6], static_cast<uint32_t>(usec % 1'000'000ull));
            setV0(0);
            return true;
        }
        case 41:
            setV0(0);
            return true;
        default:
            return false;
        }
    }

    bool IopKernel::referThreadStatus(int id, uint32_t outputAddress)
    {
        if (id == 0 && m_currentThread != nullptr)
            id = m_currentThread->id;
        const auto it = m_threads.find(id);
        if (it == m_threads.end() || outputAddress == 0u)
            return false;

        const IopThread &thread = it->second;
        uint32_t status = 0x10u;
        switch (thread.state)
        {
        case IopThreadState::Running:
            status = 0x01u;
            break;
        case IopThreadState::Ready:
            status = 0x02u;
            break;
        case IopThreadState::Sleep:
        case IopThreadState::Delay:
        case IopThreadState::Semaphore:
        case IopThreadState::EventFlag:
            status = 0x04u;
            break;
        case IopThreadState::Suspended:
            status = 0x08u;
            break;
        default:
            status = 0x10u;
            break;
        }

        m_memory.write32(outputAddress + 0u, thread.attr);
        m_memory.write32(outputAddress + 4u, thread.option);
        m_memory.write32(outputAddress + 8u, status);
        m_memory.write32(outputAddress + 12u, thread.entry);
        m_memory.write32(outputAddress + 16u, thread.stackBase);
        m_memory.write32(outputAddress + 20u, thread.stackSize);
        m_memory.write32(outputAddress + 24u, thread.cpu.gpr[28]);
        m_memory.write32(outputAddress + 28u, thread.initialPriority);
        m_memory.write32(outputAddress + 32u, thread.priority);
        m_memory.write32(outputAddress + 36u, thread.state == IopThreadState::Sleep ? 1u : thread.state == IopThreadState::Delay   ? 2u
                                                                                       : thread.state == IopThreadState::Semaphore ? 3u
                                                                                       : thread.state == IopThreadState::EventFlag ? 4u
                                                                                                                                   : 0u);
        m_memory.write32(outputAddress + 40u, static_cast<uint32_t>(thread.waitId));
        m_memory.write32(outputAddress + 44u, static_cast<uint32_t>(thread.wakeupCount));
        return true;
    }

    bool IopKernel::dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };

        // PS2X_IOP_SEMA_LOG=<id>: log every signal/wait on that IOP semaphore (thread, pc, count before).
        static const int logSema = []
        {
            const char *value = std::getenv("PS2X_IOP_SEMA_LOG");
            return value ? std::atoi(value) : 0;
        }();
        if (logSema != 0 && ordinal >= 6u && ordinal <= 9u && static_cast<int>(cpu.gpr[4]) == logSema)
        {
            const auto it = m_semaphores.find(logSema);
            static const char *const kNames[] = {"SignalSema", "iSignalSema", "WaitSema", "PollSema"};
            std::fprintf(stderr, "[iop-sema] %s(%d) thread=%d pc=0x%06x ra=0x%06x count=%d\n", kNames[ordinal - 6u], logSema,
                         m_currentThread ? m_currentThread->id : -1, cpu.pc, cpu.gpr[31],
                         it != m_semaphores.end() ? it->second.current : -999);
        }

        switch (ordinal)
        {
        case 4:
        {
            const uint32_t descriptor = cpu.gpr[4];
            Semaphore semaphore;
            semaphore.id = static_cast<int>(m_nextSemaphoreId++);
            semaphore.attr = m_memory.read32(descriptor + 0u);
            semaphore.option = m_memory.read32(descriptor + 4u);
            semaphore.current = static_cast<int>(m_memory.read32(descriptor + 8u));
            semaphore.maximum = std::max(1, static_cast<int>(m_memory.read32(descriptor + 12u)));
            m_semaphores.emplace(semaphore.id, semaphore);
            setV0(semaphore.id);
            return true;
        }
        case 5:
            setV0(m_semaphores.erase(static_cast<int>(cpu.gpr[4])) != 0u ? 0 : -1);
            return true;
        case 6:
        case 7:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.current < it->second.maximum)
                ++it->second.current;
            wakeOneSemaphore(id);
            if (ordinal == 6)
                yieldIfHigherPriorityReady(cpu);
            setV0(0);
            return true;
        }
        case 8:
        case 9:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(-1);
                return true;
            }
            if (it->second.current > 0)
            {
                --it->second.current;
                setV0(0);
            }
            else if (ordinal == 9)
                setV0(-419);
            else if (m_currentThread != nullptr)
            {
                m_currentThread->state = IopThreadState::Semaphore;
                m_currentThread->waitId = id;
                cpu.yielded = true;
                setV0(0);
            }
            return true;
        }
        case 11:
        case 12:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            const auto it = m_semaphores.find(id);
            if (it == m_semaphores.end())
            {
                setV0(-1);
                return true;
            }
            const uint32_t outputAddress = cpu.gpr[5];
            if (outputAddress != 0u)
            {
                m_memory.write32(outputAddress + 0u, it->second.attr);
                m_memory.write32(outputAddress + 4u, it->second.option);
                m_memory.write32(outputAddress + 8u, 0u);
                m_memory.write32(outputAddress + 12u, static_cast<uint32_t>(it->second.maximum));
                m_memory.write32(outputAddress + 16u, static_cast<uint32_t>(it->second.current));
                uint32_t waiters = 0u;
                for (const auto &[threadId, thread] : m_threads)
                {
                    if (thread.state == IopThreadState::Semaphore && thread.waitId == id)
                        ++waiters;
                }
                m_memory.write32(outputAddress + 20u, waiters);
            }
            setV0(0);
            return true;
        }
        default:
            return false;
        }
    }

    void IopKernel::wakeOneSemaphore(int id)
    {
        IopThread *best = nullptr;
        for (auto &[threadId, thread] : m_threads)
        {
            if (thread.state != IopThreadState::Semaphore || thread.waitId != id)
                continue;
            if (best == nullptr || thread.priority < best->priority)
                best = &thread;
        }

        const auto semaphore = m_semaphores.find(id);
        if (best != nullptr && semaphore != m_semaphores.end() && semaphore->second.current > 0)
        {
            --semaphore->second.current;
            best->state = IopThreadState::Ready;
            best->waitId = 0;
        }
    }

    bool IopKernel::eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode)
    {
        if (bits == 0u)
            return false;
        return (mode & 1u) != 0u ? (event.bits & bits) != 0u : (event.bits & bits) == bits;
    }

    void IopKernel::wakeEventWaiters(EventFlag &event)
    {
        for (auto &[threadId, thread] : m_threads)
        {
            if (thread.state != IopThreadState::EventFlag || thread.waitId != event.id)
                continue;
            if (!eventSatisfied(event, thread.waitBits, thread.waitMode))
                continue;

            if (thread.waitResultAddress != 0u)
                m_memory.write32(thread.waitResultAddress, event.bits);
            thread.cpu.gpr[2] = 0u;
            if ((thread.waitMode & 0x10u) != 0u)
                event.bits = 0u;
            thread.state = IopThreadState::Ready;
            thread.waitId = 0;
            thread.waitBits = 0;
            thread.waitMode = 0;
            thread.waitResultAddress = 0;
            if (event.bits == 0u)
                break;
        }
    }

    int IopKernel::createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits)
    {
        EventFlag event;
        event.id = static_cast<int>(m_nextEventFlagId++);
        event.attr = attr;
        event.option = option;
        event.bits = bits;
        const int id = event.id;
        m_eventFlags.emplace(id, event);
        return id;
    }

    bool IopKernel::setInternalEventFlag(int id, uint32_t bits)
    {
        const auto event = m_eventFlags.find(id);
        if (event == m_eventFlags.end())
            return false;
        event->second.bits |= bits;
        wakeEventWaiters(event->second);
        return true;
    }

    bool IopKernel::dispatchEventImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const auto setV0 = [&](int32_t value)
        {
            cpu.gpr[2] = static_cast<uint32_t>(value);
        };

        switch (ordinal)
        {
        case 4:
        {
            const uint32_t descriptor = cpu.gpr[4];
            setV0(createInternalEventFlag(m_memory.read32(descriptor + 0u),
                                          m_memory.read32(descriptor + 4u),
                                          m_memory.read32(descriptor + 8u)));
            return true;
        }
        case 5:
        {
            const int id = static_cast<int>(cpu.gpr[4]);
            if (m_eventFlags.erase(id) == 0u)
            {
                setV0(-1);
                return true;
            }
            for (auto &[threadId, thread] : m_threads)
            {
                if (thread.state == IopThreadState::EventFlag && thread.waitId == id)
                {
                    thread.state = IopThreadState::Ready;
                    thread.waitId = 0;
                    thread.cpu.gpr[2] = static_cast<uint32_t>(-1);
                }
            }
            setV0(0);
            return true;
        }
        case 6:
        case 7:
        {
            if (!setInternalEventFlag(static_cast<int>(cpu.gpr[4]), cpu.gpr[5]))
            {
                setV0(-1);
                return true;
            }
            if (ordinal == 6)
                yieldIfHigherPriorityReady(cpu);
            setV0(0);
            return true;
        }
        case 8:
        case 9:
        {
            const auto event = m_eventFlags.find(static_cast<int>(cpu.gpr[4]));
            if (event == m_eventFlags.end())
            {
                setV0(-1);
                return true;
            }
            // IOP ClearEventFlag applies a mask: callers pass ~bitsToClear.
            event->second.bits &= cpu.gpr[5];
            setV0(0);
            return true;
        }
        case 10: // WaitEventFlag
        case 11: // PollEventFlag
        {
            const auto event = m_eventFlags.find(static_cast<int>(cpu.gpr[4]));
            if (event == m_eventFlags.end())
            {
                setV0(-1);
                return true;
            }
            const uint32_t bits = cpu.gpr[5];
            const uint32_t mode = cpu.gpr[6];
            if (eventSatisfied(event->second, bits, mode))
            {
                if (cpu.gpr[7] != 0u)
                    m_memory.write32(cpu.gpr[7], event->second.bits);
                if ((mode & 0x10u) != 0u)
                    event->second.bits = 0u;
                setV0(0);
            }
            else if (ordinal == 11)
                setV0(-418);
            else if (m_currentThread != nullptr)
            {
                m_currentThread->state = IopThreadState::EventFlag;
                m_currentThread->waitId = event->second.id;
                m_currentThread->waitBits = bits;
                m_currentThread->waitMode = mode;
                m_currentThread->waitResultAddress = cpu.gpr[7];
                setV0(0);
                cpu.yielded = true;
            }
            else
                setV0(-418);
            return true;
        }
        case 13:
        case 14:
        {
            const auto event = m_eventFlags.find(static_cast<int>(cpu.gpr[4]));
            if (event == m_eventFlags.end())
            {
                setV0(-1);
                return true;
            }
            if (cpu.gpr[5] != 0u)
            {
                uint32_t waiters = 0u;
                for (const auto &[threadId, thread] : m_threads)
                {
                    if (thread.state == IopThreadState::EventFlag && thread.waitId == event->second.id)
                        ++waiters;
                }
                m_memory.write32(cpu.gpr[5] + 0u, event->second.attr);
                m_memory.write32(cpu.gpr[5] + 4u, event->second.option);
                m_memory.write32(cpu.gpr[5] + 8u, event->second.bits);
                m_memory.write32(cpu.gpr[5] + 12u, event->second.bits);
                m_memory.write32(cpu.gpr[5] + 16u, waiters);
            }
            setV0(0);
            return true;
        }
        default:
            return false;
        }
    }

    int IopKernel::createModuleStartThread(uint32_t entry, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t priority, uint32_t stackSize)
    {
        IopThread thread;
        thread.id = static_cast<int>(m_nextThreadId++);
        thread.entry = entry;
        thread.stackSize = stackSize;
        thread.priority = std::clamp<uint32_t>(priority, 1u, 126u);
        thread.initialPriority = thread.priority;
        thread.stackBase = m_memory.allocate(thread.stackSize + kStackGuardBytes, 16u);
        if (thread.stackBase == 0u)
            return 0;
        thread.cpu.pc = entry;
        thread.cpu.gpr[4] = a0;
        thread.cpu.gpr[5] = a1;
        thread.cpu.gpr[28] = gp;
        thread.cpu.gpr[29] = alignUp(thread.stackBase + thread.stackSize, 16u) - 16u;
        thread.cpu.gpr[31] = kThreadReturnSentinel;
        thread.state = IopThreadState::Ready;
        const int id = thread.id;
        m_threads.emplace(id, std::move(thread));
        return id;
    }

    bool IopKernel::threadFinished(int id) const
    {
        const auto it = m_threads.find(id);
        return it == m_threads.end() ||
               it->second.state == IopThreadState::Dormant ||
               it->second.state == IopThreadState::Dead;
    }

    uint32_t IopKernel::releaseModuleStartThread(int id)
    {
        const auto it = m_threads.find(id);
        if (it == m_threads.end())
            return 0u;
        const uint32_t result = it->second.cpu.gpr[2];
        if (it->second.state == IopThreadState::Dormant || it->second.state == IopThreadState::Dead)
        {
            if (it->second.stackBase != 0u)
                (void)m_memory.freeAllocation(it->second.stackBase);
            m_threads.erase(it);
        }
        return result;
    }

    int IopKernel::createCallThread(const IopCpuState &cpu, uint32_t priority)
    {
        IopThread thread;
        thread.id = static_cast<int>(m_nextCallThreadId++);
        if (m_nextCallThreadId >= 0x7FFFFFFFu)
            m_nextCallThreadId = kFirstCallThreadId;
        thread.entry = cpu.pc;
        thread.priority = std::clamp<uint32_t>(priority, 1u, 126u);
        thread.initialPriority = thread.priority;
        thread.cpu = cpu;
        thread.state = IopThreadState::Ready;
        const int id = thread.id;
        m_threads.insert_or_assign(id, std::move(thread));
        return id;
    }

    IopThread *IopKernel::beginThread(int id)
    {
        IopThread *thread = findThread(id);
        if (thread == nullptr)
            return nullptr;
        m_currentThread = thread;
        thread->state = IopThreadState::Running;
        thread->cpu.stopped = false;
        thread->cpu.yielded = false;
        return thread;
    }

    IopThread *IopKernel::findThread(int id)
    {
        const auto it = m_threads.find(id);
        return it != m_threads.end() ? &it->second : nullptr;
    }

    uint32_t IopKernel::threadPriority(int id, uint32_t fallback) const
    {
        const auto it = m_threads.find(id);
        return it != m_threads.end() ? it->second.priority : fallback;
    }

    void IopKernel::releaseThread(int id)
    {
        const auto it = m_threads.find(id);
        if (it == m_threads.end())
            return;
        if (m_currentThread == &it->second)
            m_currentThread = nullptr;
        if (it->second.stackBase != 0u)
            (void)m_memory.freeAllocation(it->second.stackBase);
        m_threads.erase(it);
    }

    void IopKernel::yieldIfHigherPriorityReady(IopCpuState &cpu)
    {
        if (m_currentThread == nullptr || &cpu != &m_currentThread->cpu)
            return;
        for (const auto &[id, thread] : m_threads)
        {
            if (thread.state == IopThreadState::Ready && thread.priority < m_currentThread->priority)
            {
                cpu.yielded = true;
                return;
            }
        }
    }

    void IopKernel::sleepCurrent(IopCpuState &cpu)
    {
        if (m_currentThread == nullptr)
            return;
        m_currentThread->state = IopThreadState::Sleep;
        cpu.yielded = true;
    }

    int IopKernel::hostWaitCurrent(IopCpuState &cpu)
    {
        if (m_currentThread == nullptr)
            return 0;
        m_currentThread->state = IopThreadState::Suspended;
        cpu.yielded = true;
        return m_currentThread->id;
    }

    void IopKernel::hostResume(int id)
    {
        const auto it = m_threads.find(id);
        if (it != m_threads.end() && it->second.state == IopThreadState::Suspended)
            it->second.state = IopThreadState::Ready;
    }

    void IopKernel::delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu)
    {
        if (m_currentThread == nullptr)
            return;
        m_currentThread->wakeCycle = wakeCycle;
        m_currentThread->state = IopThreadState::Delay;
        cpu.yielded = true;
    }

    int32_t IopKernel::bestReadyPriority(uint64_t currentCycle) const
    {
        int32_t best = INT32_MAX;
        for (const auto &[id, thread] : m_threads)
        {
            const bool ready = thread.state == IopThreadState::Ready ||
                               (thread.state == IopThreadState::Delay && thread.wakeCycle <= currentCycle);
            if (ready && static_cast<int32_t>(thread.priority) < best)
                best = static_cast<int32_t>(thread.priority);
        }
        return best;
    }

    IopThread *IopKernel::beginNextReady(uint64_t currentCycle)
    {
        // One pass: wake expired delays and pick the best ready thread (called for every timeslice).
        IopThread *next = nullptr;
        for (auto &[id, thread] : m_threads)
        {
            if (thread.state == IopThreadState::Delay && thread.wakeCycle <= currentCycle)
                thread.state = IopThreadState::Ready;
            if (thread.state != IopThreadState::Ready)
                continue;
            if (next == nullptr || thread.priority < next->priority ||
                (thread.priority == next->priority && thread.id < next->id))
                next = &thread;
        }
        if (next == nullptr)
            return nullptr;
        // The IOP kernel never timeslices: a running thread keeps the CPU until it blocks, yields, or a more urgent
        // thread becomes ready; a ready thread of the same priority waits. The emulator runs threads in slices, and
        // picking the lowest id at every slice let an equal-priority thread cut in: CRI's main thread (prio 39, its
        // once-a-vblank SJX tick) ran in the middle of the PSM thread's mix pass (prio 39, left channel freed, right
        // not yet), handed the left's free space to the EE a tick before the right's, and CRI's EE decoder (which
        // sizes both channels by the left's free chunk, adxsjd_get_wr) then dropped right-channel samples until the
        // right ran a whole 16 KB ring ahead: the music doubling (claude/audio-notes.md). PS2X_IOP_EQUAL_PRIORITY_SLICE=1:
        // the old behaviour.
        static const bool sliceEqual = [] { const char *v = std::getenv("PS2X_IOP_EQUAL_PRIORITY_SLICE"); return v && *v == '1'; }();
        // Safety net: a thread that keeps the CPU for a whole vblank of IOP time without blocking while an equal-
        // priority thread waits is spinning (a real IOP would hang there); let the other one in and say so.
        if (!sliceEqual && m_continueId != 0)
        {
            if (const auto it = m_threads.find(m_continueId);
                it != m_threads.end() && it->second.state == IopThreadState::Ready && it->second.priority <= next->priority)
            {
                if (m_continueId != m_heldId)
                {
                    m_heldId = m_continueId;
                    m_heldSince = currentCycle;
                }
                if (&it->second == next || currentCycle - m_heldSince < kVblankPeriodCycles)
                    next = &it->second;
                else
                {
                    static uint32_t logs = 0u;
                    if (logs < 4u && ++logs)
                        std::fprintf(stderr, "[iop-sched] thread %d (prio %u, pc 0x%06x) ran a vblank without blocking; letting "
                                             "equal-priority thread %d in\n",
                                     it->second.id, it->second.priority, it->second.cpu.pc, next->id);
                    m_heldId = 0;
                }
            }
        }
        if (next->id != m_continueId)
            m_heldId = 0;
        m_continueId = 0;

        m_currentThread = next;
        next->state = IopThreadState::Running;
        next->cpu.stopped = false;
        next->cpu.yielded = false;
        return next;
    }

    uint64_t IopKernel::nextWakeCycle(uint64_t fallback) const
    {
        uint64_t nextWake = fallback;
        for (const auto &[id, thread] : m_threads)
        {
            if (thread.state == IopThreadState::Delay)
                nextWake = std::min(nextWake, thread.wakeCycle);
        }
        return nextWake;
    }

    void IopKernel::endTimeslice(IopThread &thread, uint32_t returnSentinel)
    {
        m_continueId = 0;
        if (thread.cpu.pc == returnSentinel || thread.cpu.stopped)
            thread.state = IopThreadState::Dormant;
        else if (thread.state == IopThreadState::Running)
        {
            thread.state = IopThreadState::Ready;
            if (!thread.cpu.yielded) // the slice ran out: it was not giving the CPU away
                m_continueId = thread.id;
        }
        m_currentThread = nullptr;
        cleanupDeadThreads();
    }

    void IopKernel::cleanupDeadThreads()
    {
        for (auto thread = m_threads.begin(); thread != m_threads.end();)
        {
            if (thread->second.state != IopThreadState::Dead)
            {
                ++thread;
                continue;
            }
            if (thread->second.stackBase != 0u)
                (void)m_memory.freeAllocation(thread->second.stackBase);
            thread = m_threads.erase(thread);
        }
    }

    void IopKernel::terminateThreadsInRange(uint32_t base, uint32_t size)
    {
        for (auto &[id, thread] : m_threads)
        {
            const uint32_t pc = IopMemory::physicalAddress(thread.cpu.pc);
            if (pc >= base && pc < base + size)
                thread.state = IopThreadState::Dead;
        }
    }

    std::vector<std::string> IopKernel::describeThreads() const
    {
        std::vector<std::string> lines;
        for (const auto &[id, thread] : m_threads)
        {
            std::ostringstream line;
            line << "iop thread " << id << " state=" << static_cast<int>(thread.state) << " entry=0x" << std::hex << thread.entry
                 << " pc=0x" << thread.cpu.pc << " ra=0x" << thread.cpu.gpr[31] << std::dec << " prio=" << thread.priority
                 << " wait=" << thread.waitId << " bits=0x" << std::hex << thread.waitBits;
            lines.push_back(line.str());
        }
        for (const auto &[id, flag] : m_eventFlags)
        {
            std::ostringstream line;
            line << "iop eventflag " << id << " bits=0x" << std::hex << flag.bits;
            lines.push_back(line.str());
        }
        for (const auto &[id, sema] : m_semaphores)
        {
            std::ostringstream line;
            line << "iop sema " << id << " count=" << sema.current;
            lines.push_back(line.str());
        }
        return lines;
    }
}
