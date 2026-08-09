#include "thread_scheduler.hpp"

#include "kernel.hpp"
#include "logging.hpp"
#include <atomic>

extern "C" {
    void ThreadEntryWrapper();
    void ThreadIrqExit(uint64_t rsp);
    void ThreadIrqFirstRunExit(uint64_t rsp);

    void ThreadExited() {
        auto &kernel = piru::kernel::GetInstance();
        auto threader = kernel.data.subsystem_manager.GetSubsystem<piru::subsystems::thread_scheduler>();
        threader->DestroyThread(threader->GetCurrentThread()->id);
        threader->Tick(nullptr);
    }
}

namespace piru::subsystems {
    void HandleThreadInterrupt(interrupts::idt::registers * registers) {
        auto &kernel = kernel::GetInstance();
        auto threader = kernel.data.subsystem_manager.GetSubsystem<thread_scheduler>();
        threader->Tick(registers);
        auto thread = threader->GetCurrentThread();
        kernel.GetCpu().apic.EOI();

        if (thread->isFirstRun){
            thread->isFirstRun = false;
            ThreadIrqFirstRunExit(reinterpret_cast<uint64_t>(thread->rsp));
        } else {
            ThreadIrqExit(reinterpret_cast<uint64_t>(thread->rsp));
        }
    }

    void Idle(void* arg) {
        while (true) {
            asm volatile("hlt");
        }
    }

    thread_scheduler::thread_scheduler(heap_allocator &allocator) : allocator(allocator) {

    }


    Result thread_scheduler::Initialize() {
        _threadState = allocator.CreateState("thread_scheduler", kernel::GetInstance().GetCpu().pageState, true);
        if (!_threadState) {
            LOG_ERROR("Failed to create thread scheduler state!");
            return Result::FAILURE;
        }

        kernel::GetInstance().data.idt.RegisterIRQHandler(interrupts::idt::THREAD_SCHEDULER_VECTOR, HandleThreadInterrupt);

        for (auto & i : _currentThread) {
            i = nullptr;
        }

        for (size_t i = 0; i < kernel::GetInstance().GetCpuCount(); i++) {
            _idleThread[kernel::GetInstance().GetCpuId(i)] = CreateIdleThread();
        }

        return Result::SUCCESS;
    }

    uint64_t thread_scheduler::CreateThread(entryPoint entry, void *arg, bool isUser, memory::paging::state *pageState) {
        thread_control_block* tcb = nullptr;
        {
            threading::spinlock::scoped_lock lock(_lock, true);

            void* stackBase = _threadState->Allocate(KERNEL_STACK_SIZE, 16);
            uint8_t* fpuState = (uint8_t*)_threadState->Allocate(FPU_STATE_SIZE, 64);
            memset(fpuState, 0, FPU_STATE_SIZE);

            // asm volatile("fninit; xsave %0" : : "m"(*fpuState) : "memory");
            tcb = (thread_control_block*)_threadState->Allocate(sizeof(thread_control_block), alignof(thread_control_block));

            *tcb = {
                .id = GetNewId(),
                .tcbState = state::READY,
                .rsp = stackBase,
                .stackBase = stackBase,
                .rip = nullptr,
                .arg = arg,
                .pageState = pageState,
                .registers = {},
                .fpuState = fpuState,
                .isUser = isUser,
                .isFirstRun = true
            };

            auto* stackPtr = (uint64_t*)((uint8_t*)stackBase + KERNEL_STACK_SIZE);

            uint64_t codeSegment = isUser ? interrupts::gdt::USER_CODE_SELECTOR : interrupts::gdt::KERNEL_CODE_SELECTOR;
            uint64_t dataSegment = isUser ? interrupts::gdt::USER_DATA_SELECTOR : interrupts::gdt::KERNEL_DATA_SELECTOR;
            uint64_t initialRflags = 0x202;

            auto push = [](uint64_t*& sp, uint64_t value) {
                *(--sp) = value;
            };

            push(stackPtr, dataSegment); // SS
            push(stackPtr, (uint64_t)stackBase + KERNEL_STACK_SIZE); // RSP
            push(stackPtr, initialRflags); // RFLAGS
            push(stackPtr, codeSegment); // CS
            push(stackPtr, (uint64_t)ThreadEntryWrapper); // RIP

            push(stackPtr, 0); // R15
            push(stackPtr, 0); // R14
            push(stackPtr, (uint64_t)arg); // R13
            push(stackPtr, (uint64_t)entry); // R12
            push(stackPtr, 0); // RBP
            push(stackPtr, 0); // RBX

            tcb->rsp = stackPtr;
            _threads.Add(tcb);
        }

        kernel::GetInstance().GetCpu().apic.SendIPIAll(interrupts::idt::THREAD_SCHEDULER_VECTOR);
        return tcb->id;
    }

    void thread_scheduler::DestroyThread(uint64_t threadId) {
        threading::spinlock::scoped_lock lock(_lock, true);
        auto index = _threads.IndexOf([](thread_control_block* const &t, void* data) {
            return t->id == *(uint64_t*)data;
        }, &threadId);

        if (index == _threads.npos) {
            LOG_ERROR("Failed to destroy thread with ID {}: not found in ready threads!", threadId);
            return;
        }

        auto tcb = _threads[index];
        tcb->tcbState = state::DEAD;
    }

    thread_scheduler::thread_control_block* thread_scheduler::GetNextThread() {
        size_t count = _threads.Count();
        if (count == 0) return nullptr;

        for (size_t i = 0; i < count; i++) {
            size_t index = (i + _nextThreadIndex) % count;
            if (_threads[index]->tcbState == state::READY) {
                _nextThreadIndex = (index + 1) % count;
                return _threads[index];
            }
        }
        return nullptr;
    }

    void thread_scheduler::Tick(interrupts::idt::registers *interruptContext) {
        threading::spinlock::scoped_lock lock(_lock, true);
        auto current = GetCurrentThread();
        auto next = GetNextThread();

        if (!next && !current) {
            _currentThread[interrupts::gdt::GetCpuId()] = _idleThread[interrupts::gdt::GetCpuId()];
            return;
        }

        if (!next)
            next = current;


        if (current && interruptContext) {
            current->registers = *interruptContext;
            current->rsp = (void*)interruptContext;
            // asm volatile ("fxsave %0" : "=m"(*current->fpuState));
        }

        if (current == next) // continue where we left off.
            return;

        if (current)
            current->tcbState = state::READY;

        next->tcbState = state::RUNNING;
        // asm volatile ("fxrstor %0" : : "m"(*next->fpuState));

        _currentThread[interrupts::gdt::GetCpuId()] = next;
    }

    void thread_scheduler::Yield() {
        asm volatile ("int $0xFD");
    }

    void thread_scheduler::Sleep(uint64_t ms) {

    }

    thread_scheduler::thread_control_block * thread_scheduler::GetCurrentThread() {
        return _currentThread[interrupts::gdt::GetCpuId()];
    }

    void thread_scheduler::StartCore() {
        auto apic = kernel::GetInstance().GetCpu().apic;
        apic.SetLvtTimer(interrupts::idt::THREAD_SCHEDULER_VECTOR, interrupts::TimerDivide::By1, true, kernel::GetInstance().data.time.GetApicFrequency() / 1000 * THREAD_TIME);

        asm volatile ("sti");

        while (true) {
            asm volatile ("hlt");
        }
    }

    thread_scheduler::thread_control_block * thread_scheduler::CreateIdleThread() {
        threading::spinlock::scoped_lock lock(_lock, true);

        void* stackBase = _threadState->Allocate(KERNEL_STACK_SIZE, 16);
        uint8_t* fpuState = (uint8_t*)_threadState->Allocate(FPU_STATE_SIZE, 64);
        memset(fpuState, 0, FPU_STATE_SIZE);

        // asm volatile ("fninit; fxsave %0" : "=m"(*fpuState));
        auto tcb = (thread_control_block*)_threadState->Allocate(sizeof(thread_control_block), alignof(thread_control_block));

        *tcb = {
            .id = GetNewId(),
            .tcbState = state::READY,
            .rsp = stackBase,
            .stackBase = stackBase,
            .rip = nullptr,
            .arg = nullptr,
            .pageState = kernel::GetInstance().GetCpu().pageState,
            .registers = {},
            .fpuState = fpuState,
            .isUser = false,
            .isFirstRun = true
        };

        auto* stackPtr = (uint64_t*)((uint8_t*)stackBase + KERNEL_STACK_SIZE);

        uint64_t codeSegment = interrupts::gdt::KERNEL_CODE_SELECTOR;
        uint64_t dataSegment = interrupts::gdt::KERNEL_DATA_SELECTOR;
        uint64_t initialRflags = 0x202;

        auto push = [](uint64_t*& sp, uint64_t value) {
            *(--sp) = value;
        };

        push(stackPtr, dataSegment); // SS
        push(stackPtr, (uint64_t)stackBase + KERNEL_STACK_SIZE); // RSP
        push(stackPtr, initialRflags); // RFLAGS
        push(stackPtr, codeSegment); // CS
        push(stackPtr, (uint64_t)ThreadEntryWrapper); // RIP

        push(stackPtr, 0); // R15
        push(stackPtr, 0); // R14
        push(stackPtr, (uint64_t)nullptr); // R13
        push(stackPtr, (uint64_t)Idle); // R12
        push(stackPtr, 0); // RBP
        push(stackPtr, 0); // RBX

        tcb->rsp = stackPtr;
        return tcb;
    }

    uint64_t thread_scheduler::GetNewId() {
        static std::atomic<uint64_t> id{0};
        return ++id;
    }
}
