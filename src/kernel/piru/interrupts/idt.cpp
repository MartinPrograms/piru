#include "idt.hpp"

#include "kernel.hpp"
#include "logging.hpp"

extern "C" {

void IRQHandler(uint8_t irq, uint64_t errorCode, piru::interrupts::idt::registers* regs) {
    if (irq == 0xFF) {
        LOG_DEBUG("Spurious IRQ received, ignoring.");
        return;
    }

    piru::kernel::GetInstance().data.idt.HandleIRQ(irq, regs);
}

void ExceptionHandler(uint64_t exceptionVector, uint64_t errorCode, piru::interrupts::idt::registers* regs) {
    LOG_ERROR("Exception occurred: vector={}, errorCode={}", exceptionVector, errorCode);
    LOG_ERROR("Registers: {}", *regs);
    PANIC("Exception occurred, halting the system.");
}

extern void* ISRStubTable[256];

}

namespace piru::interrupts {
    static idt::idt_entry _idtEntries[256] ALIGNED(16) = {};

    void idt::Initialize() {
        _idtPointer.base = (uint64_t)&_idtEntries[0];
        _idtPointer.limit = sizeof(idt_entry) * 256 - 1;

        for (uint16_t vec = 0; vec < 256; vec++) {
            SetIDTEntry(vec, (uint64_t)(((void**)&ISRStubTable)[vec]), 0x8E);
        }

        _idtEntries[8].ist = 1;
        _idtEntries[13].ist = 1;
        _idtEntries[14].ist = 1;

        // Reserve 0-32
        _allocatedVectors[0] = 0xFF;
        _allocatedVectors[1] = 0xFF;
        _allocatedVectors[2] = 0xFF;
        _allocatedVectors[3] = 0xFF;

        // Reserve 253, 254 and 255
        _allocatedVectors[31] = 0xE0; // 11100000

        RegisterIRQHandler(APIC_SLEEP_VECTOR, [](registers* regs) {
            kernel::GetInstance().GetCpu().apic.EndSleep();
        });
    }

    void idt::InitializeCpu() {
        asm volatile ("lidt %0" : : "m" (_idtPointer));
    }

    uint8_t idt::AllocateVector() {
        threading::spinlock::scoped_lock lock(_lock, true);
        for (uint8_t i = 0; i < 32; i++) {
            if (_allocatedVectors[i] != 0xFF) {
                for (uint8_t bit = 0; bit < 8; bit++) {
                    if (!(_allocatedVectors[i] & (1 << bit))) {
                        _allocatedVectors[i] |= (1 << bit);
                        return i * 8 + bit;
                    }
                }
                return i;
            }
        }
        return 0;
    }

    void idt::FreeVector(uint8_t vec) {
        threading::spinlock::scoped_lock lock(_lock, true);
        uint8_t index = vec / 8;
        if (index < 32 || vec > 252)
            PANIC("Attempted to free reserved vector");

        uint8_t bit = vec % 8;
        _allocatedVectors[index] &= ~(1 << bit);
    }

    void idt::HandleIRQ(uint8_t irq, registers *regs) {
        if (_irqHandlers[irq]) {
            _irqHandlers[irq](regs);
        } else {
            LOG_WARNING("Unhandled IRQ: {}", irq);
        }

        kernel::GetInstance().GetCpu().apic.EOI();
    }

    void idt::SetIDTEntry(uint16_t vec, uint64_t isr, uint8_t i) {
        idt_entry &entry = _idtEntries[vec];
        entry.offsetLow = isr & 0xFFFF;
        entry.selector = 0x08;
        entry.ist = 0;
        entry.typeAttribute = i;
        entry.offsetMiddle = (isr >> 16) & 0xFFFF;
        entry.offsetHigh = (isr >> 32) & 0xFFFFFFFF;
        entry.zero = 0;
    }
}
