#ifndef PIRU_IDT_HPP
#define PIRU_IDT_HPP

#include <definitions.hpp>

#include "cpp/spinlock.hpp"

namespace piru::interrupts {
    class idt {
    public:
        struct idt_entry {
            uint16_t offsetLow;
            uint16_t selector;
            uint8_t ist;
            uint8_t typeAttribute;
            uint16_t offsetMiddle;
            uint32_t offsetHigh;
            uint32_t zero;
        } PACKED;

        struct idt_pointer {
            uint16_t limit;
            uint64_t base;
        } PACKED;

        struct registers {
            uint64_t _pad0, rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
            uint64_t errorCode;
            uint64_t rip;
            uint64_t cs;
            uint64_t rflags;
            uint64_t user_rsp; /* The RSP of the process that was interrupted */
            uint64_t ss;
        } PACKED;

        using exception_handler = void(*)(registers *);
        using irq_handler = void(*)(registers *);

        constexpr static uint8_t APIC_SLEEP_VECTOR = 0xFF - 1;
        constexpr static uint8_t PANIC_VECTOR = 0xFF - 2;
        constexpr static uint8_t THREAD_SCHEDULER_VECTOR = 0xFF - 3;
        constexpr static uint8_t PAGING_TLB_SHOOTDOWN = 0xFF - 4;

        void Initialize();
        void InitializeCpu();
        uint8_t AllocateVector();
        void FreeVector(uint8_t uint8);

        void HandleIRQ(uint8_t irq, registers * regs);

        void RegisterIRQHandler(uint8_t irq, irq_handler handler) {
            threading::spinlock::scoped_lock lock(_lock, true);
            _irqHandlers[irq] = handler;
        }

        void RegisterExceptionHandler(uint8_t exception, exception_handler handler) {
            threading::spinlock::scoped_lock lock(_lock, true);
            _exceptionHandlers[exception] = handler;
        }

        void RemoveIRQHandler(uint8_t irq) {
            threading::spinlock::scoped_lock lock(_lock, true);
            _irqHandlers[irq] = nullptr;
        }

        void RemoveExceptionHandler(uint8_t exception) {
            threading::spinlock::scoped_lock lock(_lock, true);
            _exceptionHandlers[exception] = nullptr;
        }


    private:
        threading::spinlock _lock{};
        idt_pointer _idtPointer{};
        exception_handler _exceptionHandlers[32]{};
        irq_handler _irqHandlers[256]{};
        uint8_t _allocatedVectors[32]{}; // 8 irqs per uint8

        void SetIDTEntry(uint16_t vec, uint64_t p, uint8_t i);
    };
}

#endif //PIRU_IDT_HPP
