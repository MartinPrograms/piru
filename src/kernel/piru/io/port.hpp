#ifndef PIRU_PORT_HPP
#define PIRU_PORT_HPP

#include <definitions.hpp>

namespace piru::io::port {
    inline void outb(uint16_t port, uint8_t value) {
        asm volatile ("outb %0, %1" : : "a"(value), "d"(port));
    }

    inline void outw(uint16_t port, uint16_t value) {
        asm volatile ("outw %0, %1" : : "a"(value), "d"(port));
    }

    inline void outl(uint16_t port, uint32_t value) {
        asm volatile ("outl %0, %1" : : "a"(value), "d"(port));
    }

    inline uint8_t inb(uint16_t port) {
        uint8_t value;
        asm volatile ("inb %1, %0" : "=a"(value) : "d"(port));
        return value;
    }

    inline uint16_t inw(uint16_t port) {
        uint16_t value;
        asm volatile ("inw %1, %0" : "=a"(value) : "d"(port));
        return value;
    }

    inline uint32_t inl(uint16_t port) {
        uint32_t value;
        asm volatile ("inl %1, %0" : "=a"(value) : "d"(port));
        return value;
    }

    inline void iowait() {
        asm volatile ("outb %%al, $0x80" : : "a"(0));
    }
}

#endif //PIRU_PORT_HPP
