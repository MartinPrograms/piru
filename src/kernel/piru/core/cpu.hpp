#ifndef PIRU_CPU_HPP
#define PIRU_CPU_HPP

#include <definitions.hpp>

namespace piru::core {
    class cpu {
    public:
        static constexpr uint32_t KernelGSBase = 0xC0000102;
        static constexpr uint32_t GSBase = 0xC0000101;
        static constexpr uint32_t FSBase = 0xC0000100;
        static constexpr uint32_t IA32_APIC_BASE = 0x1B;

        void Initialize();
        void InitializeCpu();

        static void WriteMSR(uint32_t msr, uint64_t value);
        static uint64_t ReadMSR(uint32_t msr);
        static uint64_t ReadTSC();

        void SaveFpuState(void* fpuState);
        void RestoreFpuState(void* fpuState);
        uint64_t FpuStateSize();

        // Features:
        bool CpuId(uint64_t leaf, uint64_t *eax, uint64_t *ebx, uint64_t *ecx, uint64_t *edx);
        bool HasInvariantTSC();

    private:
        char _cpuName[49] = "\0";
        uint8_t _cpuNameLength = 0;
        bool _useXSave[system::MaxCpuCount]{};
        uint64_t _xsaveMask[system::MaxCpuCount]{};
        uint64_t _fpuStateSize[system::MaxCpuCount]{};
    };
}

#endif //PIRU_CPU_HPP
