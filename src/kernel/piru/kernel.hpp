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
#include "core/subsystem_manager.hpp"
#include <atomic>

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
            core::subsystem_manager subsystem_manager{};
        };

        // per cpu core
        struct cpu {
            uint32_t id;
            uint64_t stackBase;
            uint64_t faultHandlerStackBase;
            interrupts::tss tss{};
            interrupts::gdt gdt{};
            memory::paging::state *pageState{};
            uint64_t lastShootdownGen;
            interrupts::apic apic{};
        };

        data data;
        cpu cpus[system::MaxCpuCount];

        cpu& GetCpu();

        [[nodiscard]] size_t GetCpuCount() {
            return _cpuCount;
        }

        [[nodiscard]] size_t GetCpuId(uint32_t index) {
            return _cpuIds[index];
        }

        [[nodiscard]] size_t GetCpuIndex(uint32_t id) {
            for (size_t i = 0; i < _cpuCount; i++) {
                if (_cpuIds[i] == id) return i;
            }
            return -1;
        }

        [[nodiscard]] uint32_t GetBsp() {
            return _bsp;
        }

    private:
        uint32_t _bsp{};
        uint32_t _cpuIds[system::MaxCpuCount]{};
        size_t _cpuCount{1};
        size_t _initializedCount{0};
        threading::spinlock _initLock{};
        std::atomic<bool> _schedulerReady{false};
    };
} // piru

#endif //PIRU_KERNEL_HPP
