#include "time.hpp"

#include "kernel.hpp"
#include "logging.hpp"
#include "boot/limine_definitions.hpp"
#include <cpp/time.hpp>

namespace piru::core {
    time::time(cpu &cpu) : _cpu(cpu), _unixTimeStamp(0), _apicFrequency(0) {

    }

    void time::Calibrate(interrupts::apic &apic, acpi &acpi) {
        uint64_t crystalFreq = 0;

        if (_cpu.HasInvariantTSC()) {
            uint64_t eax = 0, ebx = 0, ecx = 0, edx = 0;
            _cpu.CpuId(0x15, &eax, &ebx, &ecx, &edx);

            if (ebx != 0) {
                crystalFreq = ecx != 0
                    ? ecx
                    : (CalibrateApicTsc(acpi.GetPM()) * eax) / ebx;
                _apicFrequency = crystalFreq;
            } else {
                _apicFrequency = CalibrateApicDirect(acpi.GetPM(), apic);
            }
        } else {
            _apicFrequency = CalibrateApicDirect(acpi.GetPM(), apic);
        }

        LOG_DEBUG("Calibrated APIC timer frequency: {} Hz", _apicFrequency);

        auto dab = boot::limine::date_at_boot_request.response;
        if (dab) {
            _unixTimeStamp = dab->timestamp;
        } else {
            PANIC("Limine date at boot request response is null, cannot set unix timestamp!");
        }
    }

    void time::Tick() {
        _unixTimeStamp++;
    }

    uint64_t time::CalibrateApicTsc(const acpi::pm_timer &pm) {
        uint32_t targetTicks = (static_cast<uint64_t>(PM_TIMER_FREQ) * CALIBRATION_MS) / 1000;

        uint32_t start = pm.Read();
        uint64_t tscStart = cpu::ReadTSC();
        while (pm.Elapsed(start, pm.Read()) < targetTicks) {
        }
        uint64_t tscEnd = cpu::ReadTSC();

        return (tscEnd - tscStart) * 1000 / CALIBRATION_MS;
    }

    uint64_t time::CalibrateApicDirect(const acpi::pm_timer &pm, interrupts::apic &apic) {
        uint32_t targetTicks = (static_cast<uint64_t>(PM_TIMER_FREQ) * CALIBRATION_MS) / 1000;

        apic.SetLvtTimer(0, interrupts::TimerDivide::By16, false, UINT32_MAX);
        uint32_t start = pm.Read();

        while (pm.Elapsed(start, pm.Read()) < targetTicks) {}

        uint32_t apicElapsed = 0xFFFFFFFF - apic.GetLvtCount();
        apic.SetLvtTimer(32, interrupts::TimerDivide::By16, false, 0);

        return (static_cast<uint64_t>(apicElapsed) * 1000 / CALIBRATION_MS) * 16;
    }
} // core