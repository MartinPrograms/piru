#ifndef PIRU_SERIAL_HPP
#define PIRU_SERIAL_HPP

#include <definitions.hpp>

#include "cpp/spinlock.hpp"
#include "cpp/string_view.hpp"

namespace piru::io {
    class serial {
    public:
        static constexpr uint16_t COM1 = 0x3F8;
        static constexpr uint16_t COM2 = 0x2F8;
        static constexpr uint16_t COM3 = 0x3E8;
        static constexpr uint16_t COM4 = 0x2E8;
        static constexpr uint16_t COM5 = 0x5F8;
        static constexpr uint16_t COM6 = 0x4F8;
        static constexpr uint16_t COM7 = 0x5E8;
        static constexpr uint16_t COM8 = 0x4E8;

        explicit serial(uint16_t port);
        void Initialize();
        void Write(const uint8_t* data, size_t count);
        void Write(string_view data);

    private:
        threading::spinlock _lock;
        uint16_t _port;
        bool _initialized;

        [[nodiscard]] uint32_t TransmitEmpty() const;
        [[nodiscard]] uint32_t Available() const;
    };
}

#endif //PIRU_SERIAL_HPP
