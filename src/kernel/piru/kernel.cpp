#include "kernel.hpp"

#include <cpp/stream.hpp>
#include <cpp/fmt.hpp>
#include <logging.hpp>

#include "boot/limine_definitions.hpp"

void Trampoline(limine_mp_info* info) {
    piru::kernel::GetInstance().CpuInit(info->lapic_id);
}

namespace piru {
    kernel & kernel::GetInstance() {
        static kernel kernel;
        return kernel;
    }

    void kernel::Bootstrap() {
        asm volatile ("cli");

        data.serial.Initialize();
        data.framebuffer.Initialize();
        data.pmm.Initialize();

        auto mp_response = boot::limine::mp_request.response;
        _bsp = mp_response->bsp_lapic_id;
        cpus[_bsp].tss.Initialize(_bsp, (void*)system::StackTop, (void*)system::DefaultFaultHandlerTop);
        cpus[_bsp].gdt.Initialize(_bsp, &cpus[_bsp].tss);

        data.idt.Initialize();
        data.idt.InitializeCpu();
        data.cpu.Initialize();
        data.cpu.InitializeCpu();
        data.paging.Initialize();
        data.paging.SwitchToKernelPageTable(); // Switch to shared higher half
        data.acpi.Initialize();

        cpus[_bsp].apic.Initialize(data.paging, data.acpi);
        data.ioapic_manager.Initialize();

        data.time.Calibrate(cpus[_bsp].apic, data.acpi);

        _cpuCount = mp_response->cpu_count;
        for (size_t i = 0; i < _cpuCount; i++) {
            _cpuIds[i] = mp_response->cpus[i]->lapic_id;
        }

        // This core is now fully initialized, and we can call CpuInit with each other core.
        for (size_t i = 0; i < mp_response->cpu_count; i++) {
            auto core = mp_response->cpus[i];
            if (core->lapic_id == _bsp) continue; // Skip BSP
            auto trampoline = Trampoline;
            __atomic_store(&core->goto_address, &trampoline, __ATOMIC_RELEASE);
        }

        CpuInit(_bsp);

        while (_initializedCount < _cpuCount) {
            asm ("pause"); // park
        }

        LOG_INFO("All {} CPUs initialized successfully", _cpuCount);
    }

    // Runs per core
    void kernel::CpuInit(uint32_t cpuId) {
        asm volatile ("cli");

        LOG_DEBUG("Initializing CPU with ID: {}", cpuId);

        if (cpuId != _bsp) {
            cpus[cpuId].tss.Initialize(cpuId, (void*)system::StackTop, (void*)system::DefaultFaultHandlerTop);
            cpus[cpuId].gdt.Initialize(cpuId, &cpus[cpuId].tss);
            data.idt.InitializeCpu();
            data.cpu.InitializeCpu();
            data.paging.SwitchToKernelPageTable();
            cpus[cpuId].apic.Initialize(data.paging, data.acpi);
        }

        LOG_DEBUG("CPU with ID: {} initialized successfully", cpuId);

        auto flags = _initLock.lock(false);
        _initializedCount++;
        _initLock.unlock(flags);

        if (cpuId != _bsp) {
            asm volatile ("sti");

            while (true) {
                asm ("hlt"); // park
            }
        }
    }

    void kernel::InitializeSubsystems() {
        data.subsystem_manager.Initialize();
    }

    void kernel::Start() {

    }

    struct kernel::cpu &kernel::GetCpu() {
        auto id = interrupts::gdt::GetCpuId();
        return cpus[id];
    }

    namespace core {
        [[noreturn]] void Panic(const char* message) {
            asm volatile ("cli");
            auto &kernel = kernel::GetInstance();

            fmt::format(stream::debug, "\n\n{}", kernel);

            // Halt the CPU
            while (true) {
                asm volatile("hlt");
            }
        }

        threading::spinlock & GetLogLock() {
            static threading::spinlock logLock;
            return logLock;
        }
    }
} // piru