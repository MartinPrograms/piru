#ifndef PIRU_STREAMS_HPP
#define PIRU_STREAMS_HPP

#include <cpp/string_view.hpp>

namespace piru::stream {
    template <typename T>
    /// Writes T[0..count] to a buffer
    using ostream = void(*)(const T* data, size_t count);

    template <typename T>
    /// Reads T[0..count] from a buffer, the return value is the size of the input (including the just read bytes)
    /// If the return value is greater or equal to count, the input is not empty, and you can read the rest of the input by rereading until the return value is smaller than count
    using istream = size_t(*)(T* data, size_t count);

    const extern ostream<char> debug;
}

#endif // PIRU_STREAMS_HPP