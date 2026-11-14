#include "ioapic.hpp"

#include "kernel.hpp"
#include "logging.hpp"
#include "io/port.hpp"

namespace piru::interrupts {
    void ioapic::Initialize(memory::paging *paging, volatile uint32_t *ioApicAddr, uint32_t gsiBase, uint8_t ioApicId) {
        _paging = paging;
        _mmioBase = ioApicAddr;
        _gsiBase = gsiBase;
        _ioApicId = ioApicId;

        if (!_mmioBase)
            PANIC("MMIO Base address is null");

        uint32_t versionReg = ReadRegister(0x01);
        _version = versionReg & 0xFF;
        _maxRedirectionEntries = ((versionReg >> 16) & 0xFF) + 1;

        LOG_INFO("Initialized IOAPIC {} at GSI base {} with version {} and {} redirection entries", _ioApicId, _gsiBase, _version, _maxRedirectionEntries);

        for (uint32_t i = 0; i < _maxRedirectionEntries; i++) {
            SetRedirectionEntryInternal(i, 1ULL << 16); // Mask the interrupt
        }
    }

    bool ioapic::HasGSI(uint32_t gsi) {
        return (gsi >= _gsiBase) && (gsi < (_gsiBase + _maxRedirectionEntries));
    }

    void ioapic::SetRedirectionEntry(uint32_t gsi, uint64_t entry) {
        SetRedirectionEntryInternal(gsi - _gsiBase, entry);
    }

    uint32_t ioapic::ReadRegister(uint32_t reg) {
        threading::spinlock::scoped_lock lock(_lock, true);
        _mmioBase[0] = (reg & 0xFF);
        return _mmioBase[4];

    }

    void ioapic::WriteRegister(uint32_t reg, uint32_t value) {
        threading::spinlock::scoped_lock lock(_lock, true);
        _mmioBase[0] = (reg & 0xFF);
        _mmioBase[4] = value;
    }

    void ioapic::SetRedirectionEntryInternal(uint32_t index, uint64_t entry) {
        if (index >= _maxRedirectionEntries) {
            LOG_ERROR("Attempted to set redirection entry {} which is out of bounds for IOAPIC {} with {} entries", index, _ioApicId, _maxRedirectionEntries);
            return;
        }

        uint8_t regLow = 0x10 + (index * 2);
        uint8_t regHigh = regLow + 1;

        WriteRegister(regHigh, (entry >> 32) & 0xFFFFFFFF);
        WriteRegister(regLow, entry & 0xFFFFFFFF);
    }
}

namespace piru::interrupts {
    ioapic_manager::ioapic_manager(memory::paging &paging, core::acpi &acpi) : _paging(paging), _acpi(acpi) {
    }

    void ioapic_manager::Initialize() {
        DisablePIC();

        auto madt = (core::acpi::madt*)_acpi.GetTable("APIC", 0);

        for (uint32_t i = 0; i < 16; i++)
            _irqToGsi[i] = {i, 0};

        size_t isoCount = core::acpi::FindMADTEntryCount(madt, core::acpi::madt_entry_type::IRQSrcOverride);
        for (size_t i = 0; i < isoCount; i++) {
            auto entry = (core::acpi::madt_irqsrc_override*)core::acpi::FindMADTEntry(madt, core::acpi::madt_entry_type::IRQSrcOverride, i);
            if (!entry) continue;
            if (entry->IRQSource < 16)
                _irqToGsi[entry->IRQSource] = {entry->globalSysInterrupt, entry->flags};
        }

        _ioApicCount = core::acpi::FindMADTEntryCount(madt, core::acpi::madt_entry_type::IOAPIC);
        if (_ioApicCount > system::MaxIOAPICs) {
            LOG_ERROR("Found {} IOAPICs, but the maximum supported is {}", _ioApicCount, system::MaxIOAPICs);
            _ioApicCount = system::MaxIOAPICs;
        }

        LOG_DEBUG("Found {} IOAPIC(s)", _ioApicCount);

        for (size_t i = 0; i < _ioApicCount; i++) {
            auto entry = (core::acpi::madt_ioapic*)core::acpi::FindMADTEntry(madt, core::acpi::madt_entry_type::IOAPIC, i);
            if (!entry) {
                LOG_ERROR("Failed to find IOAPIC entry {}", i);
                continue;
            }
            LOG_DEBUG("Found IOAPIC entry {}", i);

            uintptr_t ioApicMmio = _paging.MapAnywhereKernel(
                entry->ioApicAddress, 1,
                memory::paging::Flags::ReadWrite | memory::paging::Flags::CacheDisable | memory::paging::Flags::WriteThrough
            ).start;

            _ioApic[i].Initialize(&_paging, (volatile uint32_t*)ioApicMmio, entry->globalSystemInterruptBase, entry->ioApicId);
        }
    }

    uint32_t ioapic_manager::GetGSI(uint32_t irq) {
        if (irq >= 16) {
            LOG_ERROR("IRQ {} is out of bounds, must be between 0 and 15", irq);
            return 0xFFFFFFFF;
        }
        return _irqToGsi[irq].gsi;
    }

    uint16_t ioapic_manager::GetIsoFlags(uint32_t irq) {
        if (irq >= 16) {
            LOG_ERROR("IRQ {} is out of bounds, must be between 0 and 15", irq);
            return 0;
        }
        return _irqToGsi[irq].isoFlags;
    }

    ioapic_manager::irq_mapping ioapic_manager::GetIRQ(uint32_t irq) {
        if (irq >= 16) {
            LOG_ERROR("IRQ {} is out of bounds, must be between 0 and 15", irq);
            return {0xFFFFFFFF, 0};
        }
        return _irqToGsi[irq];
    }

    ioapic * ioapic_manager::GetOwner(uint32_t gsi) {
        for (size_t i = 0; i < _ioApicCount; i++) {
            if (_ioApic[i].HasGSI(gsi)) {
                return &_ioApic[i];
            }
        }

        return nullptr;
    }
    void ioapic_manager::DisablePIC() {
        io::port::outb(0x21, 0xFF);
        io::port::iowait();
        io::port::outb(0xA1, 0xFF);
        io::port::iowait();

        io::port::outb(0x22, 0x70); // select IMCR
        io::port::iowait();
        io::port::outb(0x23, 0x01); // 0x01 = route via IOAPIC, 0x00 = route via PIC
        io::port::iowait();
    }
}
