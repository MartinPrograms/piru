#ifndef PIRU_MATH_HPP
#define PIRU_MATH_HPP

namespace piru::math {
    template<typename T>
[[nodiscard]] constexpr T Min(T a, T b) {
        return a < b ? a : b;
    }

    template<typename T>
    [[nodiscard]] constexpr T Max(T a, T b) {
        return a > b ? a : b;
    }

    template<typename T>
    [[nodiscard]] constexpr T Clamp(T value, T min, T max) {
        return Max(min, Min(max, value));
    }

    template<typename T>
    [[nodiscard]] constexpr T Abs(T value) {
        return value < 0 ? -value : value;
    }

    // todo: use a more efficient algorithm for this
    template<typename T>
    [[nodiscard]] constexpr T Pow(T base, T exponent) {
        if (exponent == 0) return 1;
        if (exponent < 0) return 1 / Pow(base, -exponent);
        T result = 1;
        for (T i = 0; i < exponent; i++) {
            result *= base;
        }
        return result;
    }

    template<typename T>
    // only works with integers
    [[nodiscard]] constexpr T AlignUp(T value, T alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    template<typename T>
    // only works with integers
    [[nodiscard]] constexpr T AlignDown(T value, T alignment) {
        return value & ~(alignment - 1);
    }

    template<typename T>
    [[nodiscard]] constexpr T IsPowerOf2(T value) {
        return (value & (value - 1)) == 0;
    }

    template<typename T>
    [[nodiscard]] constexpr T PowerOf2(T value) {
        if (value == 0) return 1;
        if (IsPowerOf2(value)) return value;
        T result = 1;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }
}

#endif //PIRU_MATH_HPP
