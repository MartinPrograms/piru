#include "cpu.hpp"

#include "kernel.hpp"
#include "logging.hpp"

namespace {
    bool IsCharWhitespace(char c) {
        return (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f');
    }
}

namespace piru::core {
    void cpu::Initialize() {
        auto* currentChunk = reinterpret_cast<uint32_t*>(_cpuName);

        for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; ++leaf) {
            asm volatile(
                "pushq %%rbx\n\t"
                "cpuid\n\t"
                "movl %%ebx, %1\n\t"
                "popq %%rbx"
                : "=a"(currentChunk[0]), "=r"(currentChunk[1]), "=c"(currentChunk[2]), "=d"(currentChunk[3]) // Using '=r' lets this assembly manually move data out of EBX and into an arbitrary register
                : "a"(leaf)
                : "cc" // We need to tell the compiler that the CPU status flags have been altered
            );

            currentChunk += 4;
        }

        _cpuName[48] = '\0';
        for (int charIndex = 47; charIndex >= 0; charIndex--) {
            if (_cpuName[charIndex] == '\0' || IsCharWhitespace(_cpuName[charIndex])) {
                _cpuName[charIndex] = '\0';
            }
            else { break; }
        }

        uint8_t startIndex = 0;
        while (_cpuName[startIndex] != '\0' && IsCharWhitespace(_cpuName[startIndex])) { startIndex++; } // Find the first non-whitespace character
        if (startIndex > 0) {
            uint8_t shiftIndex = 0;
            while (_cpuName[startIndex + 1] != '\0') {
                _cpuName[shiftIndex] = _cpuName[startIndex + 1];
                shiftIndex++;
            }

            // Place a new null terminator at the end of the brand string since the contents have moved
            _cpuName[shiftIndex] = '\0';
        }

        _cpuNameLength = __builtin_strlen(_cpuName);
    }

    void cpu::InitializeCpu() {
        uint32_t mxcsr = 0x1F80;
        uint16_t controlWord = 0x00, statusWord = 0x5A5A;
        size_t cr0, cr4;

        const auto cpuId = interrupts::gdt::GetCpuId();

        // Query cpuId to check the supported SIMD extensions
        uint32_t cpuidEAX = 0, cpuidEBX = 0, cpuidECX = 0, cpuidEDX = 0;
        asm volatile(
            "cpuid"
            : "=a"(cpuidEAX), "=b"(cpuidEBX), "=c"(cpuidECX), "=d"(cpuidEDX)
            : "a"(1)
        );

        // The FPU and SSE are required, but AVX is optional
        bool hasFPU = (cpuidEDX & (1 << 0)) != 0;
        bool hasSSE = (cpuidEDX & (1 << 25)) != 0;
        bool hasAVX = (cpuidECX & (1 << 28)) != 0;

        if (!hasFPU || !hasSSE) {
            LOG_ERROR("Core #{} does not support one or more critical SIMD extensions (FPU: {}, SSE: {})!", cpuId, hasFPU, hasSSE);
            PANIC("One or more CPU cores does not support critical SIMD extensions!");
        }

        if (!hasAVX) {
            LOG_WARNING("Core #{} does not support AVX", cpuId);
        }

        // Reset and verify the FPU hardware
        asm volatile("fninit");
        asm volatile("fnstsw %0" : "=m"(statusWord));
        if (statusWord != 0) {
            LOG_ERROR("Core #{}: FPU status word mismatch ({} != 0x0)!", cpuId, (void*)(uintptr_t)statusWord);
            PANIC("SIMD initialization failed: FPU status word mismatch!");
        }

        asm volatile("fnstcw %0" : "=m"(controlWord));
        if ((controlWord & 0x103F) != 0x003F) {
            LOG_ERROR("Core #{}: FPU control word mismatch ({} != 0x003F)!", cpuId, (void*)(uintptr_t)controlWord);
            PANIC("SIMD initialization failed: FPU control word mismatch!");
        }

        // Configure EM/MP/TS in the CR0 register (disable emulation, set MP, clear TS)
        asm volatile("mov %%cr0, %0" : "=r"(cr0));
        cr0 &= ~(1 << 2);
        cr0 |= (1 << 1);
        cr0 &= ~(1 << 3);
        asm volatile("mov %0, %%cr0" :: "r"(cr0));

        // Configure CR4 for SSE (OSFXSR) and AVX (OSXSAVE, if supported)
        asm volatile("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1 << 9); // OSFXSR for SSE
        if (hasAVX) { cr4 |= (1 << 18); } // OSXSAVE for AVX
        asm volatile("mov %0, %%cr4" :: "r"(cr4));

        // Enable X87, SSE, and AVX states in the XCR0 register
        if (hasAVX) {
            uint32_t eax, edx;
            asm volatile("xgetbv" : "=a" (eax), "=d" (edx) : "c" (0));
            eax |= 7; // Bits 0 (x87), 1 (SSE), and 2 (AVX)
            asm volatile("xsetbv" : : "a" (eax), "d" (edx), "c" (0));
        }

        // Finalize the FPU control word, 0x37F is the default for masked exceptions and standard rounding
        asm volatile("fninit");
        controlWord = 0x37F;
        asm volatile("fldcw %0" :: "m"(controlWord));
        asm volatile("ldmxcsr %0" :: "m"(mxcsr));
    }

    void cpu::WriteMSR(uint32_t msr, uint64_t value) {
        asm volatile ("wrmsr" : : "c"(msr), "a"((uint32_t)(value & 0xFFFFFFFF)), "d"((uint32_t)(value >> 32)));
    }

    uint64_t cpu::ReadMSR(uint32_t msr) {
        uint64_t out;
        asm volatile ("rdmsr" : "=a"(out) : "c"(msr));
        return out;
    }

    uint64_t cpu::ReadTSC() {
        uint64_t out;
        asm volatile ("rdtsc" : "=A"(out));
        return out;
    }

    bool cpu::CpuId(uint64_t leaf, uint64_t *eax, uint64_t *ebx, uint64_t *ecx, uint64_t *edx) {
        uint64_t a = leaf, b = 0, c = 0, d = 0;

        asm volatile(
            "cpuid"
            : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
            : "a"(a), "c"(c)
        );

        if (eax) *eax = a;
        if (ebx) *ebx = b;
        if (ecx) *ecx = c;
        if (edx) *edx = d;

        return true;
    }


    bool cpu::HasInvariantTSC() {
        uint64_t max_extended_leaf = 0;

        if (!CpuId(0x80000000, &max_extended_leaf, nullptr, nullptr, nullptr)) {
            return false;
        }

        if (max_extended_leaf < 0x80000007) {
            return false;
        }

        uint64_t edx = 0;
        CpuId(0x80000007, nullptr, nullptr, nullptr, &edx);
        return (edx & (1ULL << 8)) != 0;
    }
}
