#include "thread_scheduler.hpp"

#include "logging.hpp"
#include "core/smp.hpp"

extern "C" {
    void ThreadStart(void(*entry)(void *arg), void *arg) {
        auto sched = piru::kernel::GetInstance().data.subsystem_manager.GetSubsystem<piru::subsystems::thread_scheduler>();
        sched->FinishSwitch();
        sched->_lock.unlock(false);
        asm volatile ("sti");

        entry(arg);
        sched->ExitCurrentThread();
    }

    void TheReaper(void* t) {
        ((piru::subsystems::thread_scheduler*)t)->Reaper();
    }
}

namespace piru::subsystems {
    thread_scheduler::thread_scheduler(heap_allocator &allocator) : _allocator(allocator), _cpu(kernel::GetInstance().data.cpu), _bsp(kernel::GetInstance().GetCpu()) { }

    Result thread_scheduler::Initialize() {
        _heap = _allocator.CreateState("thread_scheduler", kernel::GetInstance().GetCpu().pageState, true);
        if (!_heap)
            return Result::FAILURE;

        _cleanFpuState = (uint8_t*)_heap->Allocate(_cpu.FpuStateSize(), 64);
        if (!_cleanFpuState)
            return Result::FAILURE;

        memset(_cleanFpuState, 0, _cpu.FpuStateSize());
        _cpu.SaveFpuState(_cleanFpuState);

        CreateKernelThread(TheReaper, this, kernel::GetInstance().GetCpu().pageState);

        return Result::SUCCESS;
    }

    // No tests to execute, a test requires taking over all cores in a return case.
    bool thread_scheduler::RunTests() {
        return true;
    }

    void thread_scheduler::PreemptIfNeeded() {
        if (_needsRescheduling[interrupts::gdt::GetCpuId()] && _cpu.PreemptEnabled()) {
            _needsRescheduling[interrupts::gdt::GetCpuId()] = false;
            Schedule();
        }

        // do nothing
    }

    uint64_t thread_scheduler::CreateKernelThread(entry_point entry, void *arg, memory::paging::state *pageState) {
        auto &pmm = kernel::GetInstance().data.pmm;
        auto &paging = kernel::GetInstance().data.paging;

        auto tcb = (thread_control_block*)_heap->Allocate(sizeof(thread_control_block));
        if (!tcb)
            PANIC("Failed to allocate TCB on heap");

        auto fpu = (uint8_t*)_heap->Allocate(_cpu.FpuStateSize(), 64);
        if (!fpu)
            PANIC("Failed to allocate FPU state on heap");

        memcpy(fpu, _cleanFpuState, _cpu.FpuStateSize());

        // Stack
        auto count = KERNEL_STACK_SIZE / system::PageSize;
        auto kStackPhys = (uintptr_t)pmm.Allocate(count);
        if (!kStackPhys)
            PANIC("Failed to allocate pages for stack");

        auto kStack = paging.MapAnywhereGuardedKernel(kStackPhys, count, memory::paging::Flags::NoExecute | memory::paging::Flags::ReadWrite, memory::paging::GuardRegion::Start);

        :: new(tcb) thread_control_block {
            .tid = GetNextTid(),
            .tcbState = state::READY,
            .blockReason = block_reason::NONE,
            .isUser = false,
            .wakeDeadline = 0,
            .timeRan = 0,
            .kernelStackBase = kStack.start,
            .kernelStackTop = kStack.end,
            .userStackBase = 0,
            .userStackTop = 0,
            .savedRsp = 0,
            .pageState = pageState,
            .entry = entry,
            .arg = arg,
            .fpuState = fpu
        };

        auto pushFn = [](uint64_t*& ptr, uint64_t value) {
            *(--ptr) = value;
        };

        auto stack = (uint64_t*)tcb->kernelStackTop;

        pushFn(stack, (uint64_t)ThreadTrampoline);
        pushFn(stack, 0); // rbp
        pushFn(stack, 0); // rbx
        pushFn(stack, (uint64_t)tcb->entry); // r12
        pushFn(stack, (uint64_t)tcb->arg); // r13
        pushFn(stack, 0); // r14
        pushFn(stack, 0); // r15

        tcb->savedRsp = (uint64_t)stack;

        threading::spinlock::scoped_lock lock(_lock, true);

        _threads.Add(tcb);
        EnqueueReady(tcb);

        return tcb->tid;
    }

    void thread_scheduler::Wake(uint64_t tid) {
        threading::spinlock::scoped_lock lock(_lock, true);
        for (const auto& tcb : _threads) {
            if (tcb->tid == tid) {
                if (tcb->blockReason != block_reason::SLEEP && tcb->blockReason != block_reason::WAIT)
                    LOG_WARNING("Tried awakening thread {} which is not allowed to be awoken by Wake", tid);
                else
                    WakeLocked(tcb);
                return;
            }
        }

        // No thread found
        LOG_WARNING("Thread {} not found", tid);
    }

    void thread_scheduler::Yield() {
        Schedule();
    }

    void thread_scheduler::Sleep(uint64_t ms) {
        threading::spinlock::scoped_lock lock(_lock, true);

        auto current = GetCurrent();
        if (current == _idleThread[interrupts::gdt::GetCpuId()])
            PANIC("Sleep called from idle thread");

        current->tcbState = state::BLOCKED;
        current->blockReason = block_reason::SLEEP;
        current->wakeDeadline = _timeCounter.load() + ms;
        _sleepingThreads.Add(current);

        ScheduleLocked();
    }

    void thread_scheduler::BlockCurrent(threading::spinlock &heldLock, block_reason reason) {
        threading::spinlock::scoped_lock lock(_lock, true);

        auto current = GetCurrent();
        if (current == _idleThread[interrupts::gdt::GetCpuId()])
            PANIC("Block called from the idle thread");

        current->tcbState = state::BLOCKED;
        current->blockReason = reason;

        heldLock.unlock(false);
        ScheduleLocked();
    }

    void thread_scheduler::ExitCurrentThread() {
        threading::spinlock::scoped_lock lock(_lock, true);

        auto current = GetCurrent();
        current->tcbState = state::DEAD;
        _threads.Remove(current);

        ScheduleLocked();

        PANIC("Should not happen");
    }

    void thread_scheduler::StartCore() {
        auto &kernel = kernel::GetInstance();

        auto tcb = (thread_control_block*)_heap->Allocate(sizeof(thread_control_block));
        if (!tcb)
            PANIC("Failed to allocate TCB on heap");

        auto fpu = (uint8_t*)_heap->Allocate(_cpu.FpuStateSize(), 64);
        if (!fpu)
            PANIC("Failed to allocate FPU state on heap");

        memcpy(fpu, _cleanFpuState, _cpu.FpuStateSize());

        :: new(tcb) thread_control_block {
            .tid = GetNextTid(),
            .tcbState = state::RUNNING,
            .blockReason = block_reason::NONE,
            .isUser = false,
            .wakeDeadline = 0,
            .timeRan = 0,
            .kernelStackBase = kernel.GetCpu().stackBase,
            .kernelStackTop = kernel.GetCpu().stackBase + (uintptr_t)system::StackTop - (uintptr_t)system::StackBottom,
            .userStackBase = 0,
            .userStackTop = 0,
            .savedRsp = 0,
            .pageState = kernel.GetCpu().pageState,
            .entry = (entry_point)0,
            .arg = 0,
            .fpuState = fpu
        };

        _idleThread[interrupts::gdt::GetCpuId()] = tcb;
        _currentThread[interrupts::gdt::GetCpuId()] = _idleThread[interrupts::gdt::GetCpuId()];

        _initializedCores[interrupts::gdt::GetCpuId()] = true;

        // set up apic timer
        auto second = kernel.data.time.GetApicFrequency();
        auto milliseconds = second / 1000;
        kernel.GetCpu().apic.SetLvtTimer(interrupts::idt::THREAD_SCHEDULER_VECTOR, interrupts::TimerDivide::By1, true, milliseconds * THREAD_TIME);

        while (true) {
            Schedule();
            asm volatile ("cli");

            _lock.lock(false);
            if (_readyThreads.Count() == 0) {
                _idleCores[interrupts::gdt::GetCpuId()].store(true);
                _lock.unlock(false);
                asm volatile ("sti; hlt");
            } else {
                _lock.unlock(false);
                asm volatile ("sti;");
            }
        }
    }

    uint64_t thread_scheduler::GetCurrentTid() {
        return GetCurrent() ? GetCurrent()->tid : 0;
    }

    void thread_scheduler::Start() {
        StartCore();
    }

    thread_scheduler::thread_control_block * thread_scheduler::GetCurrent() {
        return _currentThread[interrupts::gdt::GetCpuId()];
    }

    void thread_scheduler::EnqueueReady(thread_control_block *tcb) {
        tcb->tcbState = state::READY;
        tcb->blockReason = block_reason::NONE;
        _readyThreads.Add(tcb);

        auto &k = kernel::GetInstance();
        for (size_t i = 0; i < k.GetCpuCount(); i++) {
            auto cpu = k.GetCpuId(i);
            if (cpu == interrupts::gdt::GetCpuId())
                continue;
            if (_idleCores[cpu].exchange(false)) {
                k.GetCpu().apic.SendIPI(interrupts::idt::THREAD_RESCHEDULE_VECTOR, cpu, interrupts::DeliveryMode::Fixed, interrupts::TriggerMode::Edge, interrupts::DestinationShorthand::NoShorthand);
                break;
            }
        }
    }

    void thread_scheduler::WakeLocked(thread_control_block *tcb) {
        if (tcb->tcbState != state::BLOCKED)
            return;

        _sleepingThreads.Remove(tcb);

        EnqueueReady(tcb);
    }

    void thread_scheduler::FinishSwitch() {
        auto prev = _prevOnCpu[interrupts::gdt::GetCpuId()];
        if (prev->tcbState == state::RUNNING) {
            prev->tcbState = state::READY;
            if (prev != _idleThread[interrupts::gdt::GetCpuId()])
                EnqueueReady(prev);
        }

        if (prev->tcbState == state::BLOCKED) {
            // nothing
        }

        if (prev->tcbState == state::DEAD) {
            threading::spinlock::scoped_lock lock (_deadThreadsLock, true);
            _deadThreads.Add(prev);
        }
    }

    void thread_scheduler::ScheduleLocked() {
        auto prev = GetCurrent();
        thread_control_block *next = GetCurrent();

        // no threads to switch to
        if (_readyThreads.Count() == 0) {
            next = _idleThread[interrupts::gdt::GetCpuId()];

            if (prev->tcbState == state::RUNNING && prev != next)
                return;

            if (prev == next)
                return;
        } else {
            next = _readyThreads[0];
            if (next->tcbState != state::READY)
                PANIC("Thread in ready queue, but not ready!");

            _readyThreads.Remove(next);
        }

        if (prev == next)
            PANIC("What? Thread is ready but also active? Should not happen.");

        _cpu.SaveFpuState(prev->fpuState);
        _cpu.RestoreFpuState(next->fpuState);

        kernel::GetInstance().data.paging.SwitchToPageTable(next->pageState);

        _prevOnCpu[interrupts::gdt::GetCpuId()] = GetCurrent();
        _currentThread[interrupts::gdt::GetCpuId()] = next;

        next->tcbState = state::RUNNING;

        if (next != _idleThread[interrupts::gdt::GetCpuId()])
            _idleCores[interrupts::gdt::GetCpuId()].store(false);

        SwitchContext(&prev->savedRsp, next->savedRsp);
        FinishSwitch();
    }

    void thread_scheduler::Schedule() {
        auto flags = _lock.lock(true);
        ScheduleLocked();
        _lock.unlock(flags);
    }

    void thread_scheduler::Reaper() {
        while (true) {
            ringbuffer<thread_control_block*, 16> toFree;

            {
                threading::spinlock::scoped_lock lock(_deadThreadsLock, true);
                while (_deadThreads.Count() > 0) {
                    auto last = _deadThreads.Count() - 1;
                    if (!toFree.TryPush(_deadThreads[last]))
                        break;
                    _deadThreads.Remove(last);
                }
            }

            if (toFree.Count() == 0)
                Sleep(1000);
            else {
                while (!toFree.IsEmpty()){
                    thread_control_block* tcb;
                    if (!toFree.TryPop(tcb))
                        PANIC("Failed to pop TCB");

                    auto &k = kernel::GetInstance();

                    auto kStack = tcb->kernelStackBase;
                    auto uStack = tcb->userStackBase;

                    if (kStack) {
                        auto phys = k.data.paging.GetPhysicalAddress(k.data.paging.KernelPageTable(), kStack);
                        auto count = (size_t)(tcb->kernelStackTop - tcb->kernelStackBase) / system::PageSize;

                        k.data.paging.FreeGuardedRangeKernel({kStack, tcb->kernelStackTop}, memory::paging::GuardRegion::Start);
                        k.data.pmm.Free((void*)phys, count);
                    }

                    if (uStack) {
                        auto phys = k.data.paging.GetPhysicalAddress(tcb->pageState, uStack);
                        auto count = (size_t)(tcb->userStackTop - tcb->userStackBase) / system::PageSize;

                        k.data.paging.FreeGuardedRange(tcb->pageState, {uStack, tcb->userStackTop}, memory::paging::GuardRegion::Start);
                        k.data.pmm.Free((void*)phys, count);
                    }

                    _heap->Free(tcb->fpuState, _cpu.FpuStateSize(), 64);
                    _heap->Free(tcb, sizeof(thread_control_block));
                }
            }
        }
    }

    void thread_scheduler::OnTick() {
        if (_bsp.id == kernel::GetInstance().GetCpu().id)
            _timeCounter.fetch_add(THREAD_TIME);

        GetCurrent()->timeRan += THREAD_TIME;
        _needsRescheduling[interrupts::gdt::GetCpuId()] = true;

        {
            threading::spinlock::scoped_lock lock(_lock, true);
            for (size_t i = _sleepingThreads.Count(); i > 0; i--) {
                auto tcb = _sleepingThreads[i-1];

                if (tcb->wakeDeadline <= _timeCounter.load()) {
                    WakeLocked(tcb);
                }
            }
        }
    }

    uint64_t thread_scheduler::GetNextTid() {
        static std::atomic<uint64_t> tidCounter{1};
        return tidCounter.fetch_add(1);
    }
}
