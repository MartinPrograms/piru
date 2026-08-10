#ifndef PIRU_THREAD_SCHEDULER_HPP
#define PIRU_THREAD_SCHEDULER_HPP

#include "heap_allocator.hpp"
#include "kernel.hpp"
#include "scheduler.hpp"
#include "subsystem.hpp"
#include "interrupts/idt.hpp"
#include "memory/paging.hpp"

namespace piru::subsystems {
    struct thread_scheduler {
        enum class state {
            RUNNING,
            READY,
            BLOCKED, // while sleeping
            DEAD
        };

        using entryPoint = void(*)(void* arg);

        struct thread_control_block {
            uint64_t id;
            state tcbState;
            bool isUser;
            uint64_t remainingSleepTime;

            uint64_t kernelStackBase;
            uint64_t kernelStackTop;
            uint64_t userStackBase;
            uint64_t userStackTop;

            memory::paging::state *pageState;

            entryPoint entry;
            void* arg;

            size_t fpuStateSize;
            alignas(64) uint8_t* fpuState;
        };

        constexpr static const char* SUBSYSTEM_NAME = "thread_scheduler";
        constexpr static bool IS_REQUIRED = true;

        constexpr static uint64_t THREAD_TIME = 10; // 10 ms per thread
        constexpr static size_t KERNEL_STACK_SIZE = 16 * 1024; // 16KiB
        constexpr static size_t USER_STACK_SIZE = 32 * 1024; // 32KiB

        [[=subsystem_constructor_tag]] thread_scheduler(heap_allocator& allocator);

        Result Initialize();

        uint64_t CreateThread(entryPoint entry, void* arg, bool isUser, memory::paging::state* pageState);
        void ExitCurrentThread();

        uint64_t Tick(interrupts::idt::registers *interruptContext);

        void Yield();
        void Sleep(uint64_t ms);

        thread_control_block* GetCurrentThread();
        thread_control_block* GetNextThread();

        void StartCore();
        bool IsCoreInitialized();

    private:
        heap_allocator& _allocator;
        threading::spinlock _lock;
        heap_allocator::state* _threadState{};
        core::cpu& _cpu;
        uint8_t *_cleanFpuState{};
        std::atomic<bool> _initializedCores[system::MaxCpuCount]{};

        thread_control_block* _currentThread[system::MaxCpuCount]{}; // unique per core, indexed using interrupts::gdt::GetCpuId()
        thread_control_block* _idleThread[system::MaxCpuCount]{};
        list<thread_control_block*> _threads; // shared between all cores, locked using _lock
        list<thread_control_block*> _sleepingThreads;
        list<thread_control_block*> _readyThreads;
        list<thread_control_block*> _deadThreads;

        thread_control_block *CreateIdleThread(); // returns a thread not part of the _threads list.
        void ResetTimer(); // resets the APIC timer
        uint64_t GetNewId();
        thread_control_block CreateTcb(entryPoint entry, void *arg, bool isUser, memory::paging::state *pageState, bool createUStack);
    };
}

#endif //PIRU_THREAD_SCHEDULER_HPP
