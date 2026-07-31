#ifndef PIRU_TSS_HPP
#define PIRU_TSS_HPP

#include <definitions.hpp>

namespace piru::interrupts {
    class gdt;

    class tss {
    public:
        void Initialize(uint32_t cpuId, void *rsp0, void *ist1);

    private:
        struct tss_struct {
            uint32_t reserved0;
            uint64_t rsp0;
            uint64_t rsp1;
            uint64_t rsp2;
            uint64_t reserved1;
            uint64_t ist1;
            uint64_t ist2;
            uint64_t ist3;
            uint64_t ist4;
            uint64_t ist5;
            uint64_t ist6;
            uint64_t ist7;
            uint64_t reserved2;
            uint16_t reserved3;
            uint16_t ioMapBase;
        } PACKED;

        uint32_t _cpuid{};
        static tss_struct _tss[system::MaxCpuCount];

        friend piru::interrupts::gdt;
        void* TssAddress();
    };
}

#endif //PIRU_TSS_HPP
