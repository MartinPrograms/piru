#ifndef PIRU_RINGBUFFER_HPP
#define PIRU_RINGBUFFER_HPP

#include <cstddef>
#include <cstdint>
#include "math.hpp"

namespace piru {
    // Fixed-capacity FIFO queue. Does NOT use the heap, so it is safe to use before the heap allocator exists.
    // NOT thread-safe: the caller must provide locking.
    template<typename T, size_t N>
    class ringbuffer {
        static_assert(N > 0, "ringbuffer capacity must be greater than 0");

    private:
        T _buff[N]{};
        size_t _begin{0}; // index of the oldest element (next to pop)
        size_t _end{0};   // index where the next element will be pushed
        size_t _count{0}; // needed to tell "full" and "empty" apart, both have _begin == _end

    public:
        constexpr ringbuffer() = default;

        ringbuffer(const ringbuffer&) = delete;
        ringbuffer(ringbuffer&&) = delete;
        ringbuffer& operator=(const ringbuffer&) = delete;
        ringbuffer& operator=(ringbuffer&&) = delete;

        // Returns false (and does nothing) if the buffer is full.
        [[nodiscard]] bool TryPush(const T& element) {
            if (IsFull())
                return false;

            _buff[_end] = element;
            _end = (_end + 1) % N;
            _count++;
            return true;
        }

        // Removes the oldest element into `out`. Returns false (and leaves `out` untouched) if the buffer is empty.
        [[nodiscard]] bool TryPop(T& out) {
            if (IsEmpty())
                return false;

            out = _buff[_begin];
            _begin = (_begin + 1) % N;
            _count--;
            return true;
        }

        // Oldest element, without removing it. Returns nullptr if empty.
        [[nodiscard]] T* Peek() {
            return IsEmpty() ? nullptr : &_buff[_begin];
        }

        [[nodiscard]] const T* Peek() const {
            return IsEmpty() ? nullptr : &_buff[_begin];
        }

        void Clear() {
            _begin = 0;
            _end = 0;
            _count = 0;
        }

        [[nodiscard]] size_t Count() const { return _count; }
        [[nodiscard]] bool IsEmpty() const { return _count == 0; }
        [[nodiscard]] bool IsFull() const { return _count == N; }
        [[nodiscard]] static constexpr size_t Capacity() { return N; }
    };
}

#endif //PIRU_RINGBUFFER_HPP
