#ifndef PIRU_APIC_HPP
#define PIRU_APIC_HPP

#include <definitions.hpp>

#include "ioapic.hpp"
#include "core/acpi.hpp"
#include "memory/paging.hpp"

namespace piru::interrupts {
    enum class TimerDivide : uint32_t {
        By1 = 0b1011,
        By2 = 0b0000,
        By4 = 0b0001,
        By8 = 0b0010,
        By16 = 0b0011,
        By32 = 0b1000,
        By64 = 0b1001,
        By128 = 0b1010,
    };

    enum class DestinationShorthand : uint8_t {
        NoShorthand = 0,
        Self = 1,
        AllIncludingSelf = 2,
        AllExcludingSelf = 3,
    };

    class apic {
    public:
        void Initialize(piru::memory::paging& paging, core::acpi &acpi);
        void EOI();

        void SetLvtTimer(uint8_t vector, TimerDivide divide, bool periodic, uint32_t count);
        uint32_t GetLvtCount();

        void SendIPI(uint8_t vector, uint32_t destination, DeliveryMode deliveryMode, TriggerMode triggerMode, DestinationShorthand destinationShorthand);
        void SendIPIAll(uint8_t vector);
        void SendIPIAllExcludingSelf(uint8_t vector);
        void SendIPISelf(uint8_t vector);
        uint32_t ReceiveIPI();

        void Sleep(uint64_t durationInMs);
        void EndSleep(); // called by IDT.

    private:
        static void WriteRegister(uint32_t offset, uint32_t value);
        static uint32_t ReadRegister(uint32_t offset);
        static uint32_t *_lapicMmio;
        static uint64_t _lapicBase;
        static size_t _localAPICNMIcount;
        static core::acpi::madt *_madt;

        bool _sleeping = false;
    };
}

#endif //PIRU_APIC_HPP
