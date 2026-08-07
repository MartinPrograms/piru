#include "apic.hpp"

#include "kernel.hpp"
#include "logging.hpp"
#include "core/cpu.hpp"

namespace piru::interrupts {
    void apic::Initialize(piru::memory::paging& paging, core::acpi &acpi) {
        if (!_lapicMmio) {
            uint64_t lapic = core::cpu::ReadMSR(core::cpu::IA32_APIC_BASE) & 0xFFFFF000;
            uintptr_t mmioAddr = paging.FindAvailableVirtualAddressRangeKernel(1).start;
            paging.MapPage(
                kernel::GetInstance().GetCpu().pageState,
                mmioAddr,
                lapic,
                memory::paging::Flags::CacheDisable | memory::paging::Flags::WriteThrough | memory::paging::Flags::ReadWrite
            );

            _lapicMmio = reinterpret_cast<uint32_t *>(mmioAddr);
            _lapicBase = lapic;

            _madt = (core::acpi::madt*)acpi.GetTable("APIC", 0);
            _localAPICNMIcount = acpi.FindMADTEntryCount(_madt, core::acpi::madt_entry_type::LocalAPICNMI);
        }

        core::cpu::WriteMSR(core::cpu::IA32_APIC_BASE, (_lapicBase) | (1 << 11)); // Enable xAPIC

        WriteRegister(0xF0, ReadRegister(0xF0) | 0x1FF); // Vector 0xFF, enable
        WriteRegister(0x80, 0); // Accept all priorities

        auto cpuId = gdt::GetCpuId();

        bool matchFound = false;
        for (size_t i = 0; i < _localAPICNMIcount; i++) {
            auto entry = (core::acpi::madt_local_apic_nmi*)acpi.FindMADTEntry(_madt, core::acpi::madt_entry_type::LocalAPICNMI, i);
            if (entry->acpiProcessorId == cpuId || entry->acpiProcessorId == 0xFF) {
                auto flags = entry->flags;

                uint32_t lvt = (0b100 << 8);
                uint8_t polarity = flags & 0x3;
                if (polarity == 0b11) lvt |= (1 << 13);
                uint8_t trigger = (flags >> 2) & 0x3;
                if (trigger == 0b11) lvt |= (1 << 15);

                uint32_t reg = (entry->lint == 0) ? 0x350 : 0x360;
                WriteRegister(reg, lvt);

                matchFound = true;
            }
        }

        if (!matchFound) {
            WriteRegister(0x360, (0b100 << 8)); // Fallback
        }

        WriteRegister(0xB0, 0);
        WriteRegister(0xB0, 0);
    }

    void apic::EOI() {
        WriteRegister(0xB0, 0);
    }

    void apic::SetLvtTimer(uint8_t vector, TimerDivide divide, bool periodic, uint32_t count) {
        constexpr uint32_t LvtTimerReg = 0x320;
        constexpr uint32_t DivideConfigReg = 0x3E0;
        constexpr uint32_t InitialCountReg = 0x380;
        constexpr uint32_t LvtTimerPeriodic = (1 << 17);

        WriteRegister(DivideConfigReg, static_cast<uint32_t>(divide));
        WriteRegister(LvtTimerReg, vector | (periodic ? LvtTimerPeriodic : 0));
        WriteRegister(InitialCountReg, count);
    }

    uint32_t apic::GetLvtCount() {
        return ReadRegister(0x390);
    }

    void apic::SendIPI(uint8_t vector, uint32_t destination, DeliveryMode deliveryMode,
                       TriggerMode triggerMode, DestinationShorthand destinationShorthand) {
        constexpr uint32_t IcrLow = 0x300;
        constexpr uint32_t IcrHigh = 0x310;

        while (ReadRegister(IcrLow) & (1 << 12)) {
            // wait for any prior IPI to finish sending
        }

        if (destinationShorthand == DestinationShorthand::NoShorthand) {
            WriteRegister(IcrHigh, destination << 24);
        }

        uint32_t low = vector
            | (static_cast<uint32_t>(deliveryMode) << 8)
            | (1 << 14)
            | (static_cast<uint32_t>(triggerMode) << 15)
            | (static_cast<uint32_t>(destinationShorthand) << 18);

        WriteRegister(IcrLow, low);

        while (ReadRegister(IcrLow) & (1 << 12)) {
            // wait for send to complete before returning
        }
    }

    void apic::SendIPIAll(uint8_t vector) {
        SendIPI(vector, 0, DeliveryMode::Fixed, TriggerMode::Edge, DestinationShorthand::AllIncludingSelf);
    }

    void apic::SendIPIAllExcludingSelf(uint8_t vector) {
        SendIPI(vector, 0, DeliveryMode::Fixed, TriggerMode::Edge, DestinationShorthand::AllExcludingSelf);
    }

    void apic::SendIPISelf(uint8_t vector) {
        SendIPI(vector, 0, DeliveryMode::Fixed, TriggerMode::Edge, DestinationShorthand::Self);
    }

    uint32_t apic::ReceiveIPI() {
        return ReadRegister(0x310) >> 24;
    }

    void apic::Sleep(uint64_t durationInMs) {
        _sleeping = true;
        auto freq = kernel::GetInstance().data.time.GetApicFrequency();
        auto steps = (durationInMs * freq) / 1000;
        SetLvtTimer(idt::APIC_SLEEP_VECTOR, TimerDivide::By1, false, steps);

        while (_sleeping) {
            asm ("hlt");
        }
    }

    void apic::EndSleep() {
        _sleeping = false;
    }

    void apic::WriteRegister(uint32_t offset, uint32_t value) {
        _lapicMmio[offset / 4] = value;
    }

    uint32_t apic::ReadRegister(uint32_t offset) {
        return _lapicMmio[offset / 4];
    }

    uint32_t* apic::_lapicMmio = 0;
    uint64_t apic::_lapicBase = 0;
    size_t apic::_localAPICNMIcount = 0;
    core::acpi::madt* apic::_madt = nullptr;
}
