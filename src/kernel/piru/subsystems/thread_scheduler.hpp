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
            BLOCKED,
            DEAD
        };

        struct thread_control_block {
            uint64_t id;
            state tcbState;
            void* rsp;
            void* stackBase;
            void* rip;
            void* arg;
            memory::paging::state* pageState;
            interrupts::idt::registers registers;
            uint8_t* fpuState;
            bool isUser;
            bool isFirstRun;
        };

        using entryPoint = void(*)(void* arg);

        constexpr static const char* SUBSYSTEM_NAME = "thread_scheduler";
        constexpr static bool IS_REQUIRED = true;

        constexpr static uint64_t THREAD_TIME = 10; // 10 ms per thread
        constexpr static size_t KERNEL_STACK_SIZE = 16 * 1024; // 16KB

        [[=subsystem_constructor_tag]] thread_scheduler(heap_allocator& allocator);

        Result Initialize();

        uint64_t CreateThread(entryPoint entry, void* arg, bool isUser, memory::paging::state* pageState);
        void ExitCurrentThread();

        void Tick(interrupts::idt::registers *interruptContext);

        void Yield();
        void Sleep(uint64_t ms);

        thread_control_block* GetCurrentThread();
        thread_control_block* GetNextThread();

        void StartCore();
        bool IsCoreInitialized();

    private:
        heap_allocator& allocator;
        threading::spinlock _lock;
        heap_allocator::state* _threadState{};
        core::cpu& _cpu;
        uint8_t *_cleanFpuState{};

        thread_control_block* _currentThread[system::MaxCpuCount]{};
        thread_control_block* _idleThread[system::MaxCpuCount]{};
        list<thread_control_block*> _threads;
        size_t _nextThreadIndex{};
        bool _initializedCores[system::MaxCpuCount]{};

        thread_control_block * CreateIdleThread();
        void ResetTimer();
        uint64_t GetNewId();
    };
}

#endif //PIRU_THREAD_SCHEDULER_HPP
