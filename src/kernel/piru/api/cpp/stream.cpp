#include <cpp/stream.hpp>

#include <kernel.hpp>

namespace piru::stream {
    const ostream<char> debug = [](const char* data, size_t count) {
        auto &kernel = kernel::GetInstance();
        kernel.data.serial.Write((const uint8_t*)data, count);
        kernel.data.framebuffer.Write((const uint8_t*)data, count);
    };
}