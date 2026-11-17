#ifndef PIRU_THREAD_SCHEDULER_HPP
#define PIRU_THREAD_SCHEDULER_HPP

#include "subsystem.hpp"

#include "kernel.hpp"
#include "heap_allocator.hpp"
#include "interrupts/idt.hpp"
#include "cpp/list.hpp"
#include "memory/paging.hpp"

extern "C" [[noreturn]] void ThreadStart(void(*entry)(void* arg), void* arg);
extern "C" void TheReaper(void* arg);
extern "C" void SwitchContext(uint64_t* oldRsp, uint64_t newRsp);
extern "C" void ThreadTrampoline();

namespace piru::subsystems {
    struct thread_scheduler {
        enum class state {
            RUNNING,
            READY,
            BLOCKED,
            DEAD
        };

        enum class block_reason {
            NONE,
            SLEEP,
            WAIT,
            MUTEX
        };

        using entry_point = void(*)(void* arg);

        struct thread_control_block {
            uint64_t tid;
            state tcbState;
            block_reason blockReason;
            bool isUser;
            uint64_t wakeDeadline; // MS
            uint64_t timeRan;

            uint64_t kernelStackBase;
            uint64_t kernelStackTop;
            uint64_t userStackBase;
            uint64_t userStackTop;
            uint64_t savedRsp;

            memory::paging::state *pageState;

            entry_point entry;
            void* arg;

            uint8_t* fpuState;
        };

        constexpr static const char* SUBSYSTEM_NAME = "thread_scheduler";
        constexpr static bool IS_REQUIRED = true;
        constexpr static bool TEST_SUBSYSTEM = true;

        constexpr static uint64_t THREAD_TIME = 10; // 10 ms per thread
        constexpr static size_t KERNEL_STACK_SIZE = 16 * 1024; // 16KiB
        constexpr static size_t USER_STACK_SIZE = 32 * 1024; // 32KiB

        [[=subsystem_constructor_tag]] thread_scheduler(heap_allocator& allocator);

        Result Initialize();
        bool RunTests();
        void PreemptIfNeeded();

        uint64_t CreateKernelThread(entry_point entry, void* arg, memory::paging::state* pageState);

        void Wake(uint64_t tid); // Rise and shine.
        void Yield(); // Immediately give this core to someone else
        void Sleep(uint64_t ms); // Sleep for a set amount of time
        void BlockCurrent(threading::spinlock& heldLock, block_reason reason); // Block until awoken. First locks _lock, then releases heldLock using unlock(false).
        [[noreturn]] void ExitCurrentThread();

        [[noreturn]] void StartCore();

        uint64_t GetCurrentTid();

        [[noreturn]] void Start();

    private:
        friend void ::ThreadStart(entry_point entry, void* arg);
        friend void ::TheReaper(void* arg);
        friend interrupts::idt;

        heap_allocator& _allocator;
        core::cpu& _cpu;
        kernel::cpu& _bsp;

        threading::spinlock _lock;
        threading::spinlock _deadThreadsLock;
        heap_allocator::state* _heap{};
        uint8_t *_cleanFpuState{};
        std::atomic<uint64_t> _timeCounter{}; // in ms
        std::atomic<bool> _initializedCores[system::MaxCpuCount]{};
        std::atomic<bool> _idleCores[system::MaxCpuCount]{};
        bool _needsRescheduling[system::MaxCpuCount]{};
        thread_control_block* _currentThread[system::MaxCpuCount]{};
        thread_control_block* _prevOnCpu[system::MaxCpuCount]{}; // what the cpu previously ran
        thread_control_block* _idleThread[system::MaxCpuCount]{};
        list<thread_control_block*> _threads{};
        list<thread_control_block*> _sleepingThreads{};
        list<thread_control_block*> _readyThreads{};
        list<thread_control_block*> _deadThreads{};

        thread_control_block* GetCurrent();

        void EnqueueReady(thread_control_block *tcb);

        void WakeLocked(thread_control_block* tcb); // Rise and shine.

        void FinishSwitch();
        void ScheduleLocked();
        void Schedule();
        void Reaper();
        void OnTick();
        uint64_t GetNextTid();
    };
}

#endif //PIRU_THREAD_SCHEDULER_HPP
