#include <cpp/fmt.hpp>

const char digits[] = "0123456789abcdef";

namespace piru {
    void fmt::writeString(out_stream out, string_view value) {
        out(value.Data(), value.Size());
    }

    void fmt::writeChar(out_stream out, char value) {
        if ((value < 32 || value > 126) && (value != '\t' && value != '\n' && value != '\r')) {
            out("?", 1);
            return;
        }

        out(&value, 1);
    }

    void fmt::writeBool(out_stream out, bool value) {
        if (value) {
            out("true", 4);
        } else {
            out("false", 5);
        }
    }

    void fmt::writeInt(out_stream out, uint64_t value, uint64_t base) {
        if (value == 0) {
            writeChar(out, '0');
            return;
        }
        char tmp[64]; // must fit 64 digits for base 2
        size_t n = 0;
        while (value > 0) {
            tmp[n++] = digits[value % base];
            value /= base;
        }
        while (n > 0)
            writeChar(out, tmp[--n]);
    }

    void fmt::writePointer(out_stream out, const void *value) {
        writeString(out, "0x");
        writeInt(out, reinterpret_cast<uint64_t>(value), 16);
    }

    union double_bits {
        double d;
        uint64_t bits;
    };

    static const uint64_t doubleExpMask = 0x7FF0000000000000ULL;
    static const uint64_t doubleMantMask = 0x000FFFFFFFFFFFFFULL;
    static const uint64_t doubleSignMask = 0x8000000000000000ULL;

    bool IsNan(double value) {
        double_bits u{value};
        return (u.bits & doubleExpMask) == doubleExpMask && (u.bits & doubleMantMask) != 0;
    }

    bool IsInf(double value) {
        double_bits u{value};
        return (u.bits & doubleExpMask) == doubleExpMask && (u.bits & doubleMantMask) == 0;
    }

    bool SignBit(double value) {
        double_bits u{value};
        return (u.bits & doubleSignMask) != 0;
    }

    void fmt::writeDouble(out_stream out, double value) {
        if (IsNan(value)) { out("nan", 3); return; }

        if (IsInf(value)) {
            if (SignBit(value)) out("-", 1);
            out("inf", 3);
            return;
        }

        if (SignBit(value)) {
            out("-", 1);
            value = -value;
        }

        auto intPart = static_cast<uint64_t>(value);
        writeInt(out, intPart, 10);
        out(".", 1);
        double frac = value - (double)intPart;
        for (int i = 0; i < 6; ++i) {
            frac *= 10.0;
            int digit = (int)frac;
            writeChar(out, digits[digit]);
            frac -= digit;
        }
    }
}
