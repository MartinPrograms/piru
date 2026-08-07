#ifndef PIRU_GDT_HPP
#define PIRU_GDT_HPP

#include <definitions.hpp>

#include "tss.hpp"

namespace piru::interrupts {
    class gdt {
    public:
        void Initialize(uint32_t cpuId, tss *tss);
        static uint32_t GetCpuId();

        static constexpr uint64_t USER_CODE_SELECTOR = (7 * 8) | 3;
        static constexpr uint64_t USER_DATA_SELECTOR = (6 * 8) | 3;
        static constexpr uint64_t KERNEL_CODE_SELECTOR = 1 * 8;
        static constexpr uint64_t KERNEL_DATA_SELECTOR = 2 * 8;

    private:
        struct gdt_entry {
            uint16_t limitLow;
            uint16_t baseLow;
            uint8_t baseMiddle;
            uint8_t access;
            uint8_t granularity;
            uint8_t baseHigh;
        } PACKED;

        struct gdt_pointer {
            uint16_t limit;
            uint64_t base;
        } PACKED;

        uint32_t _cpuId{};
        tss* _tss{};

        static uint64_t _entries[system::MaxCpuCount][8]; // Null, Code, Data, TSS, UserCode, UserData
        static gdt_pointer _gdtp[system::MaxCpuCount];

        static void SetEntry(uint32_t cpuId, uint32_t index, uint32_t base, uint32_t limit, uint8_t access, uint8_t granularity);
        static void SetTssDescriptor(uint32_t cpuId, uint32_t index, uint64_t base, uint32_t limit);
    };
} // interrupts

#endif //PIRU_GDT_HPP
