#ifndef PIRU_MAIN_H
#define PIRU_MAIN_H

#include <cstddef>
#include <cstdint>

#define PACKED __attribute__((__packed__))
#define ALIGNED(x) __attribute__((__aligned__(x)))

extern "C" {
    void *memcpy(void * dest, const void * src, size_t n);
    void *memset(void *s, int c, size_t n);
    void *memmove(void *dest, const void *src, size_t n);
    int memcmp(const void *s1, const void *s2, size_t n);
}

namespace piru::constants {
    constexpr uint64_t KiB = 0x400;
    constexpr uint64_t MiB = KiB * 0x400;
    constexpr uint64_t GiB = MiB * 0x400;
    constexpr uint64_t TiB = GiB * 0x400;
    constexpr uint64_t PiB = TiB * 0x400;
}

namespace piru::core {
    [[noreturn]] void Panic(const char* message);
}

namespace piru::system {
    constexpr inline size_t MaxCpuCount = 512;
    constexpr inline uint64_t KernelOffset = 0xFFFFFFFF80000000;
    constexpr inline size_t PageSize = 0x1000;
    constexpr inline size_t MaxIOAPICs = 50;

    extern volatile uintptr_t *StackBottom;
    extern volatile uintptr_t *StackTop;
    extern volatile uintptr_t *KernelBase;
    extern volatile uintptr_t *KernelEnd;
    extern volatile uintptr_t *DefaultFaultHandlerTop;
    extern volatile uintptr_t *DefaultFaultHandlerBottom;
    extern volatile size_t StackSize;
    extern volatile size_t KernelSize;
}

#endif //PIRU_MAIN_H
