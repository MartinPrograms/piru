#ifndef PIRU_IOAPIC_HPP
#define PIRU_IOAPIC_HPP

#include <definitions.hpp>

#include "core/acpi.hpp"
#include "memory/paging.hpp"
#include "memory/physical_memory_manager.hpp"

namespace piru::interrupts {
    enum class DeliveryMode : uint8_t {
        Fixed = 0b000,
        LowestPriority = 0b001,
        SMI = 0b010,
        NMI = 0b100,
        INIT = 0b101,
        ExtINT = 0b111,
    };

    enum class DestinationMode : uint8_t {
        Physical = 0,
        Logical = 1,
    };

    enum class Polarity : uint8_t {
        ActiveHigh = 0,
        ActiveLow = 1,
    };

    enum class TriggerMode : uint8_t {
        Edge = 0,
        Level = 1,
    };

    struct redirection_entry {
        uint8_t vector;
        DeliveryMode deliveryMode = DeliveryMode::Fixed;
        DestinationMode destinationMode = DestinationMode::Physical;
        Polarity polarity = Polarity::ActiveHigh;
        TriggerMode triggerMode = TriggerMode::Edge;
        bool masked = false;
        uint8_t destination;

        uint64_t Pack() const {
            uint64_t entry = vector;
            entry |= (static_cast<uint64_t>(deliveryMode) << 8);
            entry |= (static_cast<uint64_t>(destinationMode) << 11);
            entry |= (static_cast<uint64_t>(polarity) << 13);
            entry |= (static_cast<uint64_t>(triggerMode) << 15);
            entry |= (masked ? (1ULL << 16) : 0);
            entry |= (static_cast<uint64_t>(destination) << 56);
            return entry;
        }
    };

    inline void ApplyMpsIntiFlags(redirection_entry &entry, uint16_t flags) {
        uint8_t pol = flags & 0x3;
        entry.polarity = (pol == 0b11) ? Polarity::ActiveLow : Polarity::ActiveHigh;
        uint8_t trig = (flags >> 2) & 0x3;
        entry.triggerMode = (trig == 0b11) ? TriggerMode::Level : TriggerMode::Edge;
    }

    class ioapic {
    public:
        ioapic() = default;

        void Initialize(memory::paging *paging, volatile uint32_t *ioApicAddr, uint32_t gsiBase, uint8_t ioApicId);
        bool HasGSI(uint32_t gsi);
        void SetRedirectionEntry(uint32_t gsi, uint64_t entry);
        void SetRedirectionEntry(uint32_t gsi, const redirection_entry& entry) {
            SetRedirectionEntry(gsi, entry.Pack());
        }

    private:
        threading::spinlock _lock{};
        memory::paging* _paging{};
        volatile uint32_t *_mmioBase{};
        uint32_t _gsiBase;
        uint8_t _ioApicId{};
        uint16_t _version{};
        uint32_t _maxRedirectionEntries{};

        uint32_t ReadRegister(uint32_t reg);
        void WriteRegister(uint32_t reg, uint32_t value);
        void SetRedirectionEntryInternal(uint32_t index, uint64_t entry);
    };

    class ioapic_manager {
    public:
        struct irq_mapping {
            uint32_t gsi;
            uint16_t isoFlags;
        };

        ioapic_manager(memory::paging& paging, core::acpi& acpi);
        void Initialize();
        uint32_t GetGSI(uint32_t irq);
        uint16_t GetIsoFlags(uint32_t irq);
        irq_mapping GetIRQ(uint32_t irq);

        ioapic *GetOwner(uint32_t gsi);

    private:
        memory::paging& _paging;
        core::acpi& _acpi;

        ioapic _ioApic[system::MaxIOAPICs];
        size_t _ioApicCount{};
        irq_mapping _irqToGsi[16];

        static void DisablePIC();
    };
}

#endif //PIRU_IOAPIC_HPP
