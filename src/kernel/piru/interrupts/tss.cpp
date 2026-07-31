#include "tss.hpp"

namespace piru::interrupts {
    tss::tss_struct tss::_tss[system::MaxCpuCount] ALIGNED(16) = {};

    void tss::Initialize(uint32_t cpuId, void *rsp0, void *ist1) {
        _cpuid = cpuId;
        _tss[_cpuid] = {};
        _tss[_cpuid].rsp0 = (uint64_t)rsp0;
        _tss[_cpuid].ist1 = (uint64_t)ist1;
        _tss[_cpuid].ioMapBase = sizeof(tss_struct);
    }

    void* tss::TssAddress() {
        return &_tss[_cpuid];
    }
}
