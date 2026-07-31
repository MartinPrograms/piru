#ifndef PIRU_ACPI_HPP
#define PIRU_ACPI_HPP

#include <definitions.hpp>

#include "cpp/string_view.hpp"
#include "memory/paging.hpp"

namespace piru::core {
    class acpi {
    private:
        static constexpr string_view SDPRevisionStrings[] = {
            "ACPI 1.0",
            "Unknown",
            "ACPI 2.0+"
        };

        static constexpr string_view powerProfileStrings[] = {
            "Unspecified",
            "Desktop",
            "Mobile",
            "Workstation",
            "Enterprise Server",
            "SOHO Server",
            "Appliance PC",
            "Performance Server"
        };
    public:
        struct rsdp {
            uint8_t signature[8];
            uint8_t checksum;
            uint8_t OEMId[6];
            uint8_t revision;
            uint32_t RSDTAddress;
        } PACKED;

        struct xsdp : rsdp {
            uint32_t length;
            uint64_t XSDTAddress;
            uint8_t extendedChecksum;
            uint8_t reserved[3];
        } PACKED;

        struct sdt_header {
            uint8_t signature[4];
            uint32_t length;
            uint8_t revision;
            uint8_t checksum;
            uint8_t OEMId[6];
            uint8_t OEMTableId[8];
            uint32_t OEMRevision;
            uint32_t creatorId;
            uint32_t creatorRevision;
        } PACKED;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
        struct rsdt : sdt_header {
            uint32_t pointers[];
        } PACKED;

        struct xsdt : sdt_header {
            uint64_t pointers[];
        } PACKED;
#pragma GCC diagnostic pop

        struct madt : sdt_header {
            uint32_t localAPICAddr;
            uint32_t flags;
        } PACKED;

        struct madt_entry_header {
            uint8_t type;
            uint8_t length;
        } PACKED;

        enum class madt_entry_type : uint8_t {
            LocalAPIC = 0,
            IOAPIC = 1,
            IRQSrcOverride = 2,
            NMI = 3,
            LocalAPICNMI = 4,
            LocalAPICOverride = 5,
        };

        struct madt_local_apic : madt_entry_header { // type 0
            uint8_t acpiProcessorId;
            uint8_t apicId;
            uint32_t flags;
        } PACKED;

        struct madt_ioapic : madt_entry_header { // type 1
            uint8_t ioApicId;
            uint8_t reserved;
            uint32_t ioApicAddress;
            uint32_t globalSystemInterruptBase;
        } PACKED;

        struct madt_irqsrc_override : madt_entry_header { // type 2
            uint8_t busSource;
            uint8_t IRQSource;
            uint32_t globalSysInterrupt;
            uint16_t flags;
        } PACKED;

        struct madt_local_apic_nmi : madt_entry_header { // type 4
            uint8_t acpiProcessorId;
            uint16_t flags;
            uint8_t lint;
        } PACKED;

        struct madt_local_apic_override : madt_entry_header { // type 5
            uint16_t reserved;
            uint64_t address;
        } PACKED;

        struct pm_timer {
            uint32_t port;
            bool is32bit;

            uint32_t Read() const;
            uint32_t Elapsed(uint32_t start, uint32_t end) const;
        };

        struct generic_addr {
            uint8_t addressSpace;
            uint8_t bitWidth;
            uint8_t bitOffset;
            uint8_t accessSize;
            uint64_t address;
        } PACKED;

        struct fadt : sdt_header {
            uint32_t firmwareControl;
            uint32_t dsdt;
            uint8_t reserved;
            uint8_t preferredPowerManagementProfile;
            uint16_t SCIInterrupt;
            uint32_t SMICommandPort;
            uint8_t acpiEnable;
            uint8_t acpiDisable;
            uint8_t S4BIOSRequired;
            uint8_t PStateControl;
            uint32_t PM1aEventBlock;
            uint32_t PM1bEventBlock;
            uint32_t PM1aControlBlock;
            uint32_t PM1bControlBlock;
            uint32_t PM2ControlBlock;
            uint32_t PMTimerBlock;
            uint32_t GPE0Block;
            uint32_t GPE1Block;
            uint8_t PM1EventLength;
            uint8_t PM1ControlLength;
            uint8_t PM2ControlLength;
            uint8_t PMTimerLength;
            uint8_t GPE0Length;
            uint8_t GPE1Length;
            uint8_t GPE1Base;
            uint8_t CStateControl;
            uint16_t worstC2Latency;
            uint16_t worstC3Latency;
            uint16_t flushSize;
            uint16_t flushStride;
            uint8_t dutyOffset;
            uint8_t dutyWidth;
            uint8_t dayAlarm;
            uint8_t monthAlarm;
            uint8_t century;
            uint16_t bootArchitectureFlags;
            uint8_t reserved2;
            uint32_t flags;
            generic_addr resetReg;
            uint8_t resetValue;
            uint8_t reserved3[3];
            uint64_t xFirmwareControl;
            uint64_t xDsdt;
            generic_addr xPM1aEventBlock;
            generic_addr xPM1bEventBlock;
            generic_addr xPM1aControlBlock;
            generic_addr xPM1bControlBlock;
            generic_addr xPM2ControlBlock;
            generic_addr xPMTimerBlock;
            generic_addr xGPE0Block;
            generic_addr xGPE1Block;
        } PACKED;

        explicit acpi(memory::paging & paging);

        void Initialize();
        void *GetTable(string_view signature, size_t index);

        acpi::pm_timer & GetPM();

        static size_t FindMADTEntryCount(const madt *madt, madt_entry_type type);
        static madt_entry_header *FindMADTEntry(const madt *madt, madt_entry_type type, size_t index);

    private:
        memory::paging &_paging;
        threading::spinlock _lock{};
        rsdp *_rsdp{};
        rsdt *_rsdt{};
        xsdp *_xsdp{};
        xsdt *_xsdt{};
        fadt *_fadt{};
        void *_dsdt{};
        pm_timer _pmTimer{};

        fadt *FindFADT(sdt_header *sdt);
        void WriteByteCommand(uint8_t command);
        static bool ValidateSDT(sdt_header *sdt);
        static bool ValidateRSDP(rsdp *rsdp);
        static bool ValidateXSDP(xsdp *xsdp);
    };
}

#endif //PIRU_ACPI_HPP
