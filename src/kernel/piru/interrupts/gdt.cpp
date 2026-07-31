#include "gdt.hpp"

#include "core/cpu.hpp"

extern "C" {
    extern void LoadGDT(uint64_t gdt_pointer);
    extern void LoadTSS(uint16_t tss_selector);
}

namespace piru::interrupts {
    void gdt::Initialize(uint32_t cpuId, tss *tss) {
        _cpuId = cpuId;
        _tss = tss;

        auto tssAddress = tss->TssAddress();

        SetEntry(_cpuId, 0, 0, 0, 0, 0); // Null descriptor
        SetEntry(_cpuId, 1, 0, 0xFFFFF, 0x9A, 0xA0); // Code segment
        SetEntry(_cpuId, 2, 0, 0xFFFFF, 0x92, 0xA0); // Data segment
        SetTssDescriptor(_cpuId, 3, (uint64_t)tssAddress, sizeof(tss::tss_struct) - 1);
        SetEntry(_cpuId, 5, 0, 0xFFFFF, 0xF2, 0xC0); // Star anchor
        SetEntry(_cpuId, 6, 0, 0xFFFFF, 0xF2, 0xA0); // UserData 64 DPL3 SS on SYSRET
        SetEntry(_cpuId, 7, 0, 0xFFFFF, 0xFA, 0xA0); // UserCode 64 DPL3 CS on SYSRET

        _gdtp[_cpuId].limit = sizeof(_entries[_cpuId]) - 1;
        _gdtp[_cpuId].base = (uint64_t)&_entries[_cpuId];

        LoadGDT((uint64_t)(&_gdtp[_cpuId]));
        LoadTSS(0x18);

        core::cpu::WriteMSR(core::cpu::KernelGSBase, (uint64_t)cpuId);
    }

    uint32_t gdt::GetCpuId() {
        auto cpuId = core::cpu::ReadMSR(core::cpu::KernelGSBase);
        return cpuId;
    }

    uint64_t gdt::_entries[system::MaxCpuCount][8] ALIGNED(16) = {};
    gdt::gdt_pointer gdt::_gdtp[system::MaxCpuCount] = {};

    void gdt::SetEntry(uint32_t cpuId, uint32_t index, uint32_t base, uint32_t limit, uint8_t access,
        uint8_t granularity) {
        gdt_entry entry{};
        entry.limitLow = limit & 0xFFFF;
        entry.baseLow = base & 0xFFFF;
        entry.baseMiddle = (base >> 16) & 0xFF;
        entry.access = access;
        entry.granularity = (limit >> 16) & 0x0F;
        entry.granularity |= granularity & 0xF0;
        entry.baseHigh = (base >> 24) & 0xFF;

        memcpy(&_entries[cpuId][index], &entry, sizeof(gdt_entry));
    }

    void gdt::SetTssDescriptor(uint32_t cpuId, uint32_t index, uint64_t base, uint32_t limit) {
        uint64_t low = 0;
        uint64_t high = 0;

        low |= (limit & 0xFFFFULL);
        low |= (base & 0xFFFFFFULL) << 16;
        low |= (0x89ULL) << 40;
        low |= ((limit >> 16) & 0xFULL) << 48;
        low |= ((base >> 24) & 0xFFULL) << 56;

        high |= (base >> 32) & 0xFFFFFFFFULL;

        _entries[cpuId][index] = low;
        _entries[cpuId][index + 1] = high;
    }
} // interrupts