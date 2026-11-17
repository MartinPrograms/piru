#include "smp.hpp"

#include "kernel.hpp"
#include "logging.hpp"
#include "interrupts/gdt.hpp"
#include "subsystems/thread_scheduler.hpp"

namespace piru::core {
    smp::CpuQueue smp::_queues[piru::system::MaxCpuCount];
    std::atomic<bool> smp::_draining[system::MaxCpuCount];

    void smp::DrainQueue(CpuQueue &queue, SmpQueue &copiedQueue) {
        auto cpuCore = interrupts::gdt::GetCpuId();

        {
            threading::spinlock::scoped_lock lock(queue.lock, false);
            while (!queue.queue.IsEmpty()) {
                QueuedOp out{};
                if (!queue.queue.TryPop(out))
                    PANIC("Failed to pop queued function for cpu {}", cpuCore);

                if (!copiedQueue.TryPush(out))
                    PANIC("Failed to push queued function for cpu {}", cpuCore);
            }
        }
    }

    void smp::CallOnVector() {
        auto cpuCore = interrupts::gdt::GetCpuId();
        if (_draining[cpuCore].load())
            return; // the outer CallOnVector on this cpu keeps draining until the queue is empty

        auto &cpu = kernel::GetInstance().data.cpu;

        _draining[cpuCore].store(true);

        cpu.PreemptDisable();

        auto& queue = _queues[cpuCore];

        while (true) {
            asm volatile ("cli");

            SmpQueue copiedQueue;
            DrainQueue(queue, copiedQueue);

            if (copiedQueue.IsEmpty())
                break;

            asm volatile ("sti");

            while (!copiedQueue.IsEmpty()) {
                QueuedOp out{};
                if (!copiedQueue.TryPop(out))
                    PANIC("Failed to pop queued function for cpu {}", cpuCore);

                out.func(out.data);
                if (out.wait)
                    out.wait->fetch_sub(1);
            }
        }

        cpu.PreemptEnable();
        _draining[cpuCore].store(false);
    }

    void smp::CallOn(uint32_t cpu, CallFunction func, void *data, WaitType wait) {
        auto& k = kernel::GetInstance();

        if (cpu == interrupts::gdt::GetCpuId())
            PANIC("Can not run call on self");

        if (k.GetCpuIndex(cpu) == (size_t)-1)
            PANIC("CPU {} not found, can not run on non-existent cpu", cpu);

        auto& queue = _queues[cpu];
        {
            threading::spinlock::scoped_lock lock(queue.lock, true);
            if (!queue.queue.TryPush({func, data, wait})) {
                PANIC("Failed to push CallOn function on cpu {}", cpu);
            }
        }
        k.GetCpu().apic.SendIPI(interrupts::idt::SMP_CALL_ON_VECTOR, cpu, interrupts::DeliveryMode::Fixed, interrupts::TriggerMode::Edge, interrupts::DestinationShorthand::NoShorthand);
    }

    void smp::CallOnAll(CallFunction func, void *data, bool wait, bool self) {
        if (!threading::spinlock::AreInterruptsEnabled() && wait)
            PANIC("Can only wait on other cpus, if interrupts are enabled");

        auto& k = kernel::GetInstance();
        std::atomic<int> waitAtomic = static_cast<int>(k.GetCpuCount()) - 1;
        auto waitPtr = wait ? &waitAtomic : nullptr;

        for (size_t cpuIdx = 0; cpuIdx < k.GetCpuCount(); cpuIdx++) {
            if ((k.GetCpuId(cpuIdx) == interrupts::gdt::GetCpuId()))
                continue;

            CallOn(k.GetCpuId(cpuIdx), func, data, waitPtr);
        }

        if (self)
            func(data);

        if (wait) {
            while (waitAtomic.load() != 0)
                asm volatile ("pause");
        }
    }
}
