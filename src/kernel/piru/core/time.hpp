#ifndef PIRU_TIME_HPP
#define PIRU_TIME_HPP

#include <definitions.hpp>
#include <core/cpu.hpp>

#include "interrupts/apic.hpp"
#include "interrupts/idt.hpp"

namespace piru::core {
    class time {
    public:
        time(cpu &cpu);

        // Uses either invariant tsc (if found) or ACPI PM, to calibrate local apic timer.
        // Then uses the Limine Date At Boot request to set the unix timestamp.
        void Calibrate(interrupts::apic &apic, acpi &acpi);

        [[nodiscard]] uint64_t GetUnixTimestamp() const { return _unixTimeStamp; }
        [[nodiscard]] uint64_t GetApicFrequency() const { return _apicFrequency; }

        void Tick();

    private:
        cpu &_cpu;
        uint64_t _unixTimeStamp;
        uint64_t _apicFrequency;

        static constexpr uint32_t PM_TIMER_FREQ = 3579545;
        static constexpr uint32_t CALIBRATION_MS = 100;

        uint64_t CalibrateApicTsc(const acpi::pm_timer& pm);
        uint64_t CalibrateApicDirect(const acpi::pm_timer &pm, interrupts::apic &apic);
    };
} // core

#endif //PIRU_TIME_HPP
