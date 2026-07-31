#ifndef PIRU_STRING_VIEW_HPP
#define PIRU_STRING_VIEW_HPP

#include <cstddef>
#include <cstdint>

#include "math.hpp"

namespace piru {
    class string_view {
    private:
        const char *_data{};
        size_t _size{};

        static constexpr size_t StringLength(const char* str) {
            size_t len = 0;
            while (str[len] != '\0') {
                len++;
            }
            return len;
        }

        static constexpr int StringCompare(const char *a, const char *b, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                if (a[i] < b[i]) {
                    return -1;
                }
                if (a[i] > b[i]) {
                    return 1;
                }
            }

            return 0;
        }

    public:
        static constexpr size_t npos = static_cast<size_t>(-1);

        constexpr string_view() noexcept : _data(""), _size(0) {}
        constexpr string_view(const char* str) : _data(str), _size(str ? StringLength(str) : 0) {}
        constexpr string_view(const char* str, size_t size) : _data(str), _size(size) {}

        [[nodiscard]] const char *Data() const noexcept {
            return _data;
        }

        [[nodiscard]] size_t Size() const noexcept {
            return _size;
        }

        constexpr bool operator==(const string_view &other) const noexcept {
            if (_size != other._size) {
                return false;
            }

            return StringCompare(_data, other._data, _size) == 0;
        }

        constexpr bool operator!=(const string_view &other) const noexcept {
            return !(*this == other);
        }

        [[nodiscard]] constexpr int Compare(const string_view& other) const noexcept {
            return StringCompare(_data, other._data, piru::math::Min(_size, other._size));
        }

        [[nodiscard]] constexpr string_view Substring(size_t start, size_t length) const noexcept {
            if (start >= _size)
                return {_data + _size, 0};

            return {_data + start, piru::math::Min(length, _size - start)};
        }

        [[nodiscard]] constexpr string_view Substring(size_t start) const noexcept {
            return Substring(start, _size - start);
        }

        [[nodiscard]] constexpr size_t CountOf(const string_view& needle) const noexcept {
            size_t count = 0;
            for (size_t i = 0; i <= _size - needle._size; ++i) {
                if (Substring(i, needle._size) == needle) {
                    count++;
                }
            }
            return count;
        }

        [[nodiscard]] constexpr size_t IndexOf(const string_view& needle) const noexcept {
            for (size_t i = 0; i <= _size - needle._size; ++i) {
                if (Substring(i, needle._size) == needle) {
                    return i;
                }
            }

            return npos;
        }
    };
}
#endif //PIRU_STRING_VIEW_HPP
