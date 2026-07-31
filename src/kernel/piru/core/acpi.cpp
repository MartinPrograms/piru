#include "acpi.hpp"

#include "logging.hpp"
#include "boot/limine_definitions.hpp"
#include "io/port.hpp"


namespace piru::core {
    uint32_t acpi::pm_timer::Read() const {
        return io::port::inl(port);
    }

    uint32_t acpi::pm_timer::Elapsed(uint32_t start, uint32_t end) const {
        if (is32bit) {
            return end - start;
        }

        return (end >= start) ? (end - start) : (0x1000000 + end - start);
    }

    acpi::acpi(memory::paging &paging) : _paging(paging) {
    }

    void acpi::Initialize() {
        auto rsdp_request = boot::limine::rsdp_request.response;
        if (!rsdp_request)
            PANIC("Limine RSDP response is null, cannot continue!");

        auto higherHalf = boot::limine::hhdm_request.response->offset;

        _rsdp = static_cast<rsdp *>(rsdp_request->address);
        LOG_DEBUG("RSDP found at {} with revision {} ({})", (void*)_rsdp, _rsdp->revision, SDPRevisionStrings[_rsdp->revision]);

        if (_rsdp->revision > 0) {
            _xsdp = static_cast<xsdp *>(_rsdp);

            if (!ValidateXSDP(_xsdp))
                PANIC("Invalid XSDP checksum, cannot continue!");

            _xsdt = static_cast<xsdt *>((void*)(_xsdp->XSDTAddress + higherHalf));
        } else {
            _rsdt = static_cast<rsdt *>((void*)((uintptr_t)_rsdp->RSDTAddress + higherHalf));

            if (!ValidateSDT(_rsdt))
                PANIC("Invalid RSDT checksum, cannot continue!");
        }

        uint64_t sdtAddrPhys = (_rsdp->revision > 0) ? _xsdp->XSDTAddress : _rsdp->RSDTAddress;
        auto sdt = static_cast<sdt_header *>((void*)(sdtAddrPhys + higherHalf));

        if (!ValidateSDT(sdt))
            PANIC("Failed to verify SDT");


        _fadt = FindFADT(sdt);
        if (!_fadt)
            PANIC("Failed to find FADT");

        const uint64_t xdsdtOffset = 140; // offsetof doesnt work
        uint64_t dsdtPhys = 0;
        if (_fadt->length >= xdsdtOffset + sizeof(uint64_t) && _fadt->xDsdt != 0) dsdtPhys = _fadt->xDsdt;
        else dsdtPhys = _fadt->dsdt;

        if (!dsdtPhys)
            PANIC("Failed to find DSDT");

        _dsdt = (void*)(dsdtPhys + higherHalf);
        WriteByteCommand(_fadt->acpiEnable);

        if (_fadt->preferredPowerManagementProfile > 7)
            LOG_ERROR("Invalid power profile in FADT");
        else
            LOG_DEBUG("Device has power profile of {}", powerProfileStrings[_fadt->preferredPowerManagementProfile]);

        if (_fadt->PMTimerLength != 4)
            LOG_ERROR("Invalid PM timer length in FADT");
        else {
            _pmTimer.port = _fadt->PMTimerBlock;
            _pmTimer.is32bit = (_fadt->flags & (1 << 8)) != 0;
        }

        LOG_INFO("Initialized");
    }

    void * acpi::GetTable(string_view signature, size_t index) {
        if (signature == "DSDT") return _dsdt;
        if (signature == "FACP") return _fadt;
        if (signature == "RSDT") return _rsdt;
        if (signature == "XSDT") return _xsdt;

        const auto higherHalf = boot::limine::hhdm_request.response->offset;

        if (_rsdp->revision > 0) {
            size_t entries = (_xsdt->length - sizeof(sdt_header)) / sizeof(uint64_t);
            for (size_t i = 0; i < entries; i++) {
                uint64_t targetAddr = _xsdt->pointers[i];
                auto table = static_cast<sdt_header *>((void*)(targetAddr + higherHalf));
                if (string_view(reinterpret_cast<const char *>(&table->signature[0]), 4) == signature) {
                    if (index == 0)
                        return table;
                    index--;
                }
            }
        } else {
            size_t entries = (_rsdt->length - sizeof(sdt_header)) / sizeof(uint32_t);
            for (size_t i = 0; i < entries; i++) {
                uint64_t targetAddr = _rsdt->pointers[i];
                auto table = static_cast<sdt_header *>((void*)(targetAddr + higherHalf));
                if (string_view(reinterpret_cast<const char *>(&table->signature[0]), 4) == signature) {
                    if (index == 0)
                        return table;
                    index--;
                }
            }
        }

        return nullptr;
    }

    acpi::pm_timer & acpi::GetPM() {
        return _pmTimer;
    }

    size_t acpi::FindMADTEntryCount(const madt *madt, madt_entry_type type) {
        auto *current = (madt_entry_header*)((uint8_t*)madt + sizeof(acpi::madt));
        size_t count = 0;

        while ((uint8_t*)current < (uint8_t*)madt + madt->length) {
            if (current->type == (uint8_t)type) {
                count++;
            }

            current = (madt_entry_header*)((uint8_t*)current + current->length);
        }

        return count;
    }

    acpi::madt_entry_header *acpi::FindMADTEntry(const madt *madt, madt_entry_type type, size_t index) {
        auto *current = (madt_entry_header*)((uint8_t*)madt + sizeof(acpi::madt));

        while ((uint8_t*)current < (uint8_t*)madt + madt->length) {
            if (current->type == (uint8_t)type) {
                if (index == 0)
                    return current;
                index--;
            }

            current = (madt_entry_header*)((uint8_t*)current + current->length);
        }

        return nullptr;
    }

    void acpi::WriteByteCommand(uint8_t command) {
        threading::spinlock::scoped_lock lock(_lock, true);
        if (!(io::port::inw(_fadt->PM1aControlBlock) & 1)) {
            io::port::outb(_fadt->SMICommandPort, command);
            while (!(io::port::inw(_fadt->PM1aControlBlock) & 1));
        }
    }

    bool acpi::ValidateXSDP(xsdp *xsdp) {
        if (!xsdp) return false;
        if (!ValidateRSDP(xsdp)) return false;
        if (xsdp->length < sizeof(acpi::xsdp)) return false;

        uint8_t sum = 0;
        auto bytes = reinterpret_cast<uint8_t *>(xsdp);

        for (uint32_t i = 0; i < xsdp->length; i++) {
            sum += bytes[i];
        }

        if (sum != 0) return false;
        return true;
    }

    bool acpi::ValidateRSDP(rsdp *rsdp) {
        if (!rsdp) return false;

        uint8_t sum = 0;
        auto bytes = reinterpret_cast<uint8_t *>(rsdp);

        for (uint32_t i = 0; i < sizeof(acpi::rsdp); i++) {
            sum += bytes[i];
        }

        if (sum != 0) return false;
        return true;
    }

    bool acpi::ValidateSDT(sdt_header *sdt) {
        if (!sdt) return false;
        if (sdt->length < sizeof(sdt_header)) return false;

        uint8_t sum = 0;
        for (uint32_t i = 0; i < sdt->length; i++) {
            sum += reinterpret_cast<uint8_t *>(sdt)[i];
        }

        if (sum != 0) return false;
        return true;
    }

    acpi::fadt *acpi::FindFADT(sdt_header *sdt) {
        const auto higherHalf = boot::limine::hhdm_request.response->offset;

        size_t entries = (sdt->length - sizeof(sdt_header)) / ((_rsdp->revision > 0) ? sizeof(uint64_t) : sizeof(uint32_t));
        for (size_t entry = 0; entry < entries; entry++) {
            uint64_t table = 0;

            if (_rsdp->revision > 0) {
                table = (reinterpret_cast<xsdt *>(sdt))->pointers[entry];
            } else {
                table = (reinterpret_cast<rsdt *>(sdt))->pointers[entry];
            }

            if (string_view(reinterpret_cast<const char *>(&reinterpret_cast<sdt_header *>(table + higherHalf)->signature[0]), 4) == "FACP") {
                return reinterpret_cast<fadt *>(table + higherHalf);
            }
        }

        return nullptr;
    }
}
