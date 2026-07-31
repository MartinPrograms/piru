#ifndef PIRU_KERNEL_HPP
#define PIRU_KERNEL_HPP

#include <definitions.hpp>

#include "core/acpi.hpp"
#include "interrupts/apic.hpp"
#include "interrupts/gdt.hpp"
#include "interrupts/idt.hpp"
#include "interrupts/ioapic.hpp"
#include "interrupts/tss.hpp"
#include "io/framebuffer_console.hpp"
#include "io/serial.hpp"
#include "memory/paging.hpp"
#include "memory/physical_memory_manager.hpp"
#include "core/time.hpp"

namespace piru {
    class kernel {
    public:
        static kernel& GetInstance();

        void Bootstrap();
        void CpuInit(uint32_t cpuId);
        void InitializeSubsystems();
        void Start();

        // global shared data
        struct data {
            io::serial serial{io::serial::COM1};
            io::framebuffer_console framebuffer{};
            memory::physical_memory_manager pmm{};
            interrupts::idt idt{};
            core::cpu cpu{};
            memory::paging paging{pmm};
            core::acpi acpi{paging};
            interrupts::ioapic_manager ioapic_manager{paging, acpi};
            core::time time{cpu};
        };

        // per cpu core
        struct cpu {
            interrupts::tss tss{};
            interrupts::gdt gdt{};
            memory::paging::state *pageState{};
            interrupts::apic apic{};
        };

        data data;
        cpu cpus[system::MaxCpuCount];
        cpu& GetCpu();

    private:
        uint32_t _bsp{};
        uint32_t _cpuIds[system::MaxCpuCount]{};
        size_t _cpuCount{1};
    };
} // piru

#endif //PIRU_KERNEL_HPP
