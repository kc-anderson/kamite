#pragma once

#include "iop_cpu.h"
#include "ps2x/state_io.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    class IopMemory;

    enum class IopThreadState : uint8_t
    {
        Dormant,
        Ready,
        Running,
        Sleep,
        Delay,
        Semaphore,
        EventFlag,
        Suspended,
        Dead,
    };

    struct IopThread
    {
        int id = 0;
        IopThreadState state = IopThreadState::Dormant;
        IopCpuState cpu;
        uint32_t entry = 0;
        uint32_t stackBase = 0;
        uint32_t stackSize = 0;
        uint32_t priority = 0x40;
        uint32_t initialPriority = 0x40;
        uint32_t option = 0;
        uint32_t attr = 0;
        uint64_t wakeCycle = 0;
        int waitId = 0;
        uint32_t waitBits = 0;
        uint32_t waitMode = 0;
        uint32_t waitResultAddress = 0;
        int wakeupCount = 0;
    };

    class IopKernel
    {
    public:
        explicit IopKernel(IopMemory &memory) noexcept;

        void reset();

        [[nodiscard]] bool dispatchThreadImport(uint16_t ordinal, IopCpuState &cpu, uint64_t currentCycle);
        [[nodiscard]] bool dispatchSemaphoreImport(uint16_t ordinal, IopCpuState &cpu);
        [[nodiscard]] bool dispatchEventImport(uint16_t ordinal, IopCpuState &cpu);

        [[nodiscard]] int createInternalEventFlag(uint32_t attr, uint32_t option, uint32_t bits);
        [[nodiscard]] bool setInternalEventFlag(int id, uint32_t bits);

        void sleepCurrent(IopCpuState &cpu);
        // The current thread blocks on the host (IOP sceSifCallRpc to an EE server, mode 0): state Suspended until
        // hostResume(id). Returns the thread id, 0 when no thread is current.
        [[nodiscard]] int hostWaitCurrent(IopCpuState &cpu);
        void hostResume(int id);
        void delayCurrentUntil(uint64_t wakeCycle, IopCpuState &cpu);

        [[nodiscard]] IopThread *beginNextReady(uint64_t currentCycle);
        // Lowest priority value (= most urgent) among threads ready to run at currentCycle, or INT32_MAX if none.
        [[nodiscard]] int32_t bestReadyPriority(uint64_t currentCycle) const;
        [[nodiscard]] uint64_t nextWakeCycle(uint64_t fallback) const;
        void endTimeslice(IopThread &thread, uint32_t returnSentinel);
        void cleanupDeadThreads();
        void terminateThreadsInRange(uint32_t base, uint32_t size);

        [[nodiscard]] size_t threadCount() const noexcept { return m_threads.size(); }
        // PS2X_IOP_THREAD_LOG: one line per IOP thread (id, state, priority, pc, entry, wait object, wake cycle).
        template <typename Print>
        void forEachThread(Print &&print) const
        {
            for (const auto &[id, thread] : m_threads)
                print(thread);
        }
        [[nodiscard]] std::vector<std::string> describeThreads() const;

        [[nodiscard]] int createModuleStartThread(uint32_t entry, uint32_t gp, uint32_t a0, uint32_t a1, uint32_t priority, uint32_t stackSize);
        [[nodiscard]] bool threadFinished(int id) const;
        uint32_t releaseModuleStartThread(int id);

        [[nodiscard]] int createCallThread(const IopCpuState &cpu, uint32_t priority);
        [[nodiscard]] IopThread *beginThread(int id);
        [[nodiscard]] IopThread *findThread(int id);
        [[nodiscard]] uint32_t threadPriority(int id, uint32_t fallback) const;
        void releaseThread(int id);
        [[nodiscard]] IopThread *currentThread() const noexcept { return m_currentThread; }
        void setCurrentThread(IopThread *thread) noexcept { m_currentThread = thread; }

        template <typename Ar>
        void serialize(Ar &ar)
        {
            int current = m_currentThread ? m_currentThread->id : 0;
            ps2x::state::fields(ar, m_threads, m_semaphores, m_eventFlags, m_nextThreadId, m_nextSemaphoreId, m_nextEventFlagId, current);
            if constexpr (Ar::kLoading)
            {
                const auto it = m_threads.find(current);
                m_currentThread = it != m_threads.end() ? &it->second : nullptr;
            }
        }

    private:
        struct Semaphore
        {
            int id = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
            int current = 0;
            int maximum = 1;
        };

        struct EventFlag
        {
            int id = 0;
            uint32_t bits = 0;
            uint32_t attr = 0;
            uint32_t option = 0;
        };

        [[nodiscard]] bool referThreadStatus(int id, uint32_t outputAddress);
        void wakeOneSemaphore(int id);
        [[nodiscard]] static bool eventSatisfied(const EventFlag &event, uint32_t bits, uint32_t mode);
        void wakeEventWaiters(EventFlag &event);
        void yieldIfHigherPriorityReady(IopCpuState &cpu);

        IopMemory &m_memory;
        std::map<int, IopThread> m_threads;
        std::map<int, Semaphore> m_semaphores;
        std::map<int, EventFlag> m_eventFlags;
        uint32_t m_nextThreadId = 1;
        uint32_t m_nextSemaphoreId = 1;
        uint32_t m_nextEventFlagId = 1;
        uint32_t m_nextCallThreadId = kFirstCallThreadId;
        IopThread *m_currentThread = nullptr;
        // Threads that were running and can still run (their slice ran out, or a more urgent thread preempted them),
        // most recent first: they resume before other ready threads of their priority (beginNextReady).
        std::vector<int> m_resume;
        // Lowest priority value among ready threads other than `id` (INT32_MAX if none).
        [[nodiscard]] int32_t bestReadyPriorityExcept(int id) const;
        int m_heldId = 0;          // the thread kept on the CPU that way, and since when (the vblank safety net)
        uint64_t m_heldSince = 0u;

        static constexpr uint32_t kFirstCallThreadId = 0x40000000u;
    };
}
