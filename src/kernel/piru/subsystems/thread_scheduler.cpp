#include "thread_scheduler.hpp"

#include "logging.hpp"
#include "interrupts/idt.hpp"

extern "C" {
    extern void ThreadTrampoline();

    void ThreadExit() {
        auto threading = piru::kernel::GetInstance().data.subsystem_manager.GetSubsystem<piru::subsystems::thread_scheduler>();
        threading->ExitCurrentThread();
    }

    USED uint64_t HandleIrq(piru::interrupts::idt::registers* regs) {
        auto threading = piru::kernel::GetInstance().data.subsystem_manager.GetSubsystem<piru::subsystems::thread_scheduler>();
        auto& apic = piru::kernel::GetInstance().GetCpu().apic;

        if (!threading->IsCoreInitialized()) {
            apic.EOI();
            return (uint64_t)regs;
        }

        auto rsp = threading->Tick(regs);
        apic.EOI();
        return rsp;
    }
}

namespace piru::subsystems {
    thread_scheduler::thread_scheduler(heap_allocator &allocator) : _allocator(allocator), _cpu(kernel::GetInstance().data.cpu) {

    }

    Result thread_scheduler::Initialize() {
        _threadState = _allocator.CreateState("thread_scheduler_state", kernel::GetInstance().GetCpu().pageState, true);
        _cleanFpuState = (uint8_t*) _threadState->Allocate(_cpu.FpuStateSize(), 64);
        memset(_cleanFpuState, 0, _cpu.FpuStateSize());
        _cpu.SaveFpuState(_cleanFpuState);

        for (auto & core : _initializedCores)
            core = false;

        for (auto & currentThread : _currentThread)
            currentThread = nullptr;

        return Result::SUCCESS;
    }

    uint64_t thread_scheduler::CreateThread(entryPoint entry, void *arg, bool isUser, memory::paging::state *pageState) {
        threading::spinlock::scoped_lock lock(_lock, true);

        if (isUser)
            PANIC("im lazy, not done yet");

        thread_control_block tcb = CreateTcb(entry, arg, isUser, pageState, false);

        auto tcbHeap = (thread_control_block*)_threadState->Allocate(sizeof(thread_control_block), alignof(thread_control_block));
        *tcbHeap = tcb;
        _threads.Add(tcbHeap);
        _readyThreads.Add(tcbHeap);

        return tcbHeap->id;
    }

    void thread_scheduler::ExitCurrentThread() {
        auto flags = _lock.lock(true);
        auto current = GetCurrentThread();
        if (!current)
            PANIC("How? Current thread exited, but no current thread exists.");

        current->tcbState = state::DEAD;
        _deadThreads.Add(current);

        _lock.unlock(flags);
        Yield();
    }

    uint64_t thread_scheduler::Tick(interrupts::idt::registers *interruptContext) {
        threading::spinlock::scoped_lock lock(_lock, true);
        auto current = GetCurrentThread();
        auto next = GetNextThread();

        if (!next) {
            if (current && current->tcbState != state::DEAD) {
                next = current;
            } else {
                next = _idleThread[interrupts::gdt::GetCpuId()];
            }
        }

        if (current && current->tcbState != state::DEAD)
            current->userStackTop = (uint64_t)interruptContext;

        if (current && current != next) {
            _cpu.SaveFpuState(current->fpuState);
        }

        kernel::GetInstance().data.paging.SwitchToPageTable(next->pageState);

        _cpu.RestoreFpuState(next->fpuState);
        _currentThread[interrupts::gdt::GetCpuId()] = next;

        if (current && current->tcbState != state::DEAD) {
            current->tcbState = state::READY;
            _readyThreads.Add(current);
        }

        next->tcbState = state::RUNNING;

        return next->userStackTop;
    }

    void thread_scheduler::Yield() {
        asm volatile ("int $0xFC");
    }

    void thread_scheduler::Sleep(uint64_t ms) {
        // TODO.
    }

    thread_scheduler::thread_control_block *thread_scheduler::GetCurrentThread() {
        return _currentThread[interrupts::gdt::GetCpuId()];
    }

    thread_scheduler::thread_control_block *thread_scheduler::GetNextThread() {
        if (_readyThreads.Count() > 0) {
            auto thread = _readyThreads[0];
            _readyThreads.Remove(thread);
            return thread;
        }

        return nullptr;
    }

    void thread_scheduler::StartCore() {
        {
            threading::spinlock::scoped_lock lock(_lock, true);
            _idleThread[interrupts::gdt::GetCpuId()] = CreateIdleThread();
            _initializedCores[interrupts::gdt::GetCpuId()].store(true, std::memory_order_release);
        }

        ResetTimer();

        asm volatile ("sti");

        while (true)
            asm volatile ("pause"); // wait for timer interrupts
    }

    bool thread_scheduler::IsCoreInitialized() {
        return _initializedCores[interrupts::gdt::GetCpuId()].load(std::memory_order_acquire);
    }

    thread_scheduler::thread_control_block * thread_scheduler::CreateIdleThread() {
        auto tcb = CreateTcb([](void*) { while (true) asm volatile("pause"); }, nullptr, false, kernel::GetInstance().GetCpu().pageState, false);
        auto mem = _threadState->Allocate(sizeof(thread_control_block), alignof(thread_control_block));
        return :: new (mem) thread_control_block(tcb);
    }

    void thread_scheduler::ResetTimer() {
        kernel::GetInstance().GetCpu().apic.SetLvtTimer(interrupts::idt::THREAD_SCHEDULER_VECTOR, interrupts::TimerDivide::By1, true, (THREAD_TIME * kernel::GetInstance().data.time.GetApicFrequency()) / 1000);
    }

    uint64_t thread_scheduler::GetNewId() {
        static std::atomic<uint64_t> id{0};
        return ++id;
    }

    thread_scheduler::thread_control_block thread_scheduler::CreateTcb(entryPoint entry, void *arg, bool isUser, memory::paging::state *pageState, bool createUStack) {
        auto fpuState = _threadState->Allocate(_cpu.FpuStateSize(), 64);
        memcpy(fpuState, _cleanFpuState, _cpu.FpuStateSize());

        auto kStackBase = (uint64_t)_threadState->Allocate(KERNEL_STACK_SIZE, 16);
        auto kStackTop = kStackBase + KERNEL_STACK_SIZE;

        uint64_t uStackBase = kStackBase;
        if (createUStack)
            uStackBase = (uint64_t)_threadState->Allocate(USER_STACK_SIZE, 16);

        uint64_t uStackTop = kStackTop;
        if (createUStack)
            uStackTop = uStackBase + USER_STACK_SIZE;

        thread_control_block tcb = {
            .id = GetNewId(),
            .tcbState = state::READY,
            .isUser = isUser,
            .remainingSleepTime = 0,
            .kernelStackBase = kStackBase,
            .kernelStackTop = kStackTop,
            .userStackBase = uStackBase,
            .userStackTop = uStackTop,
            .pageState = pageState,
            .entry = entry,
            .arg = arg,
            .fpuStateSize = _cpu.FpuStateSize(),
            .fpuState = (uint8_t*)fpuState,
        };

        auto pushFn = [](uint64_t*& ptr, uint64_t value) {
            *(--ptr) = value;
        };

        auto stack = (uint64_t*)tcb.kernelStackTop;

        auto ss = isUser ? interrupts::gdt::USER_DATA_SELECTOR : interrupts::gdt::KERNEL_DATA_SELECTOR;
        auto cs = isUser ? interrupts::gdt::USER_CODE_SELECTOR : interrupts::gdt::KERNEL_CODE_SELECTOR;

        auto rflags = 0x202; // interrupts enabled.

        // SS, RSP, RFLAGS, CS, RIP
        // RIP will point to the trampoline, which loads R13 into RSI, then jumps too R12 which is EntryPoint.
        pushFn(stack, ss);
        pushFn(stack, tcb.userStackTop);
        pushFn(stack, rflags);
        pushFn(stack, cs);
        pushFn(stack, (uint64_t)ThreadTrampoline);

        pushFn(stack, 0); // r15
        pushFn(stack, 0); // r14
        pushFn(stack, (uint64_t)tcb.arg); // r13
        pushFn(stack, (uint64_t)tcb.entry); // r12
        pushFn(stack, 0); // r11
        pushFn(stack, 0); // r10
        pushFn(stack, 0); // r9
        pushFn(stack, 0); // r8
        pushFn(stack, 0); // rbp
        pushFn(stack, 0); // rdi
        pushFn(stack, 0); // rsi
        pushFn(stack, 0); // rdx
        pushFn(stack, 0); // rcx
        pushFn(stack, 0); // rbx
        pushFn(stack, 0); // rax
        pushFn(stack, 0); // rax (pad)

        tcb.userStackTop = (uint64_t)stack;

        return tcb;
    }
}
