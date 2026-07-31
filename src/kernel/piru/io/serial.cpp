#include "serial.hpp"

#include "port.hpp"

namespace piru::io {
    serial::serial(uint16_t port) : _port(port), _initialized(false) {

    }

    void serial::Initialize() {
        port::outb(_port + 1, 0x00); // Disable all interrupts

        port::outb(_port + 3, 0x80); // Enable DLAB (set baud rate divisor)
        port::outb(_port + 0, 0x03); // 38400 baud
        port::outb(_port + 1, 0x00);

        port::outb(_port + 3, 0x03); // 8 bit transmission
        port::outb(_port + 2, 0xC7); // FIFO with 14 byte threshold
        port::outb(_port + 4, 0x0B); // IRQs enabled, RTS/DSR set

        constexpr int maxRetries = 1000;
        for (int i = 0; i < maxRetries; i++) {
            if (TransmitEmpty()) {
                _initialized = true;
                Write("\n\r\n\r\n\r\n\r");
                return;
            }
        }
    }

    void serial::Write(const uint8_t *data, size_t count) {
        if (!_initialized)
            return;

        threading::spinlock::scoped_lock lock(_lock, true);
        for (size_t i = 0; i < count; i++) {
            while (!TransmitEmpty()) {
                // Wait for the transmit buffer to be empty
            }
            port::outb(_port, data[i]);
        }
    }

    void serial::Write(string_view data) {
        Write((const uint8_t*)data.Data(), data.Size());
    }

    uint32_t serial::TransmitEmpty() const {
        return port::inb(_port + 5) & 0x20;
    }

    uint32_t serial::Available() const {
        return port::inb(_port + 5) & 0x01;
    }
}
