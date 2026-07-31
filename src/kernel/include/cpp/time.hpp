#ifndef PIRU_API_TIME_HPP
#define PIRU_API_TIME_HPP

#include <cstddef>
#include <cstdint>
#include "string_view.hpp"

namespace piru {
    class time {
    public:
        struct date {
            uint16_t year;
            uint8_t month; // 1-12
            uint8_t day; // 1-366
            uint8_t hour; // 1-24
            uint8_t minute; // 0-59
            uint8_t second; // 0-59
            uint8_t weekday; // 1-7
        };

        [[nodiscard]] static uint64_t GetUnixTimestamp();
        [[nodiscard]] static date GetDateForTimestamp(uint64_t timestamp);
        [[nodiscard]] static uint64_t GetTimestampForDate(const date& date);

        [[nodiscard]] static string_view GetMonthName(uint8_t month);
        [[nodiscard]] static string_view GetDayName(uint8_t day);
    };
}

#endif //PIRU_API_TIME_HPP
