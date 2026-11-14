#include "kernel.hpp"

#include <cpp/stream.hpp>
#include <cpp/fmt.hpp>
#include <logging.hpp>

#include "config.hpp"
#include "boot/limine_definitions.hpp"
#include "subsystems.hpp"
#include "core/smp.hpp"
#include "tests/paging_test.hpp"
#include "tests/smp_test.hpp"

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

            auto stackPageCount = ((uintptr_t)system::StackTop - (uintptr_t)system::StackBottom) / system::PageSize;
            auto physRsp0 = data.pmm.Allocate(stackPageCount);
            auto virtRsp0 = data.paging.MapAnywhereKernel((uintptr_t)physRsp0, stackPageCount, memory::paging::Flags::ReadWrite);

            auto faultStackPageCount = ((uintptr_t)system::DefaultFaultHandlerTop - (uintptr_t)system::DefaultFaultHandlerBottom) / system::PageSize;
            auto physIst1 = data.pmm.Allocate(faultStackPageCount);
            auto virtIst1 = data.paging.MapAnywhereKernel((uintptr_t)physIst1, faultStackPageCount, memory::paging::Flags::ReadWrite);

            cpus[core->lapic_id].stackBase = virtRsp0.end;
            cpus[core->lapic_id].faultHandlerStackBase = virtIst1.end;

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
            cpus[cpuId].tss.Initialize(cpuId, (void*)cpus[cpuId].stackBase, (void*)cpus[cpuId].faultHandlerStackBase);
            cpus[cpuId].gdt.Initialize(cpuId, &cpus[cpuId].tss);
            data.idt.InitializeCpu();
            data.cpu.InitializeCpu();
            data.paging.SwitchToKernelPageTable();
            cpus[cpuId].apic.Initialize(data.paging, data.acpi);
        }

        LOG_DEBUG("CPU with ID: {} initialized successfully", cpuId);
        asm volatile ("sti"); // SHOULD be done by the scheduler, but for now we do this here.
        // already reloaded kernel page table above.

        auto flags = _initLock.lock(false);
        _initializedCount++;
        _initLock.unlock(flags);


        if (cpuId != _bsp) {
            core::smp::Idle();
        }
    }

    void kernel::InitializeSubsystems() {
        data.subsystem_manager.Initialize();
    }

    void kernel::Start() {
        if constexpr (config::RunTlbControlTest) tests::RunPagingControlTest();
        data.paging.EnableShootdown();
        if constexpr (config::RunSmpTests) tests::RunSmpTests();
        if constexpr (config::RunTlbControlTest) tests::RunPagingTests();

        if constexpr (config::RunSubsystemSelfTests) data.subsystem_manager.Test();
    }

    struct kernel::cpu &kernel::GetCpu() {
        auto id = interrupts::gdt::GetCpuId();
        return cpus[id];
    }

    namespace core {
        [[noreturn]] void Panic(const char* message) {
            asm volatile ("cli");
            auto &kernel = kernel::GetInstance();
            kernel.GetCpu().apic.SendIPIAllExcludingSelf(interrupts::idt::PANIC_VECTOR);

            // Halt the CPU
            while (true) {
                asm volatile("hlt");
            }
        }

        extern "C" [[noreturn]] void abort() {
            Panic("Kernel abort() called");
        }

        threading::spinlock & GetLogLock() {
            static threading::spinlock logLock;
            return logLock;
        }
    }
} // piru