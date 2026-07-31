#include <cpp/time.hpp>
#include "core/time.hpp"

#include "kernel.hpp"

constexpr uint32_t EPOCH_YEAR = 1970;
constexpr uint32_t SECONDS_PER_DAY = 60 * 60 * 24;
constexpr uint32_t SECONDS_PER_HOUR = 60 * 60;
constexpr uint32_t SECONDS_PER_MINUTE = 60;

constexpr bool IsLeapYear(uint32_t year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

constexpr piru::string_view MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

constexpr uint32_t DAYS_PER_MONTH[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

constexpr piru::string_view WEEKDAYS[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

uint64_t piru::time::GetUnixTimestamp() {
    return kernel::GetInstance().data.time.GetUnixTimestamp();
}

piru::time::date piru::time::GetDateForTimestamp(uint64_t timestamp) {
    piru::time::date result{};

    result.second = timestamp % 60;
    timestamp /= 60;

    result.minute = timestamp % 60;
    timestamp /= 60;

    result.hour = timestamp % 24 + 1;
    uint64_t days = timestamp / 24;

    // Calculate weekday (Unix epoch 1970-01-01 was a Thursday, which is index 4)
    result.weekday = static_cast<uint8_t>((days + 4) % 7);

    uint32_t year = EPOCH_YEAR;
    while (true) {
        uint32_t daysInYear = IsLeapYear(year) ? 366 : 365;
        if (days < daysInYear) {
            break;
        }
        days -= daysInYear;
        year++;
    }
    result.year = static_cast<uint16_t>(year);

    uint8_t month = 0;
    while (month < 12) {
        uint32_t daysInMonth = DAYS_PER_MONTH[month];
        if (month == 1 && IsLeapYear(result.year)) {
            daysInMonth = 29;
        }

        if (days < daysInMonth) {
            break;
        }
        days -= daysInMonth;
        month++;
    }

    result.month = month + 1;
    result.day = days + 1;

    return result;
}

uint64_t piru::time::GetTimestampForDate(const date &date) {
    uint32_t ts = 0;

    uint8_t years = 0;
    uint8_t leapYears = 0;
    for (uint16_t year = EPOCH_YEAR; year < date.year; ++year) {
        years++;
        if (IsLeapYear(year)) {
            leapYears++;
        }
    }

    ts += (years * 365) * SECONDS_PER_DAY;
    ts += (leapYears * SECONDS_PER_DAY);

    for (uint8_t month = 0; month < date.month - 1; month++) {
        if (month == 1 && IsLeapYear(date.year)) {
            ts += SECONDS_PER_DAY; // Add an extra day for February in leap years
        }
        ts += DAYS_PER_MONTH[month] * SECONDS_PER_DAY;
    }

    ts += (date.day - 1) * SECONDS_PER_DAY;
    ts += date.hour * SECONDS_PER_HOUR;
    ts += date.minute * SECONDS_PER_MINUTE;
    ts += date.second;

    return ts;
}

piru::string_view piru::time::GetMonthName(uint8_t month) {
    if (month < 1 || month > 12) {
        return "Invalid Month";
    }

    return MONTHS[month - 1];
}

piru::string_view piru::time::GetDayName(uint8_t day) {
    if (day < 1 || day > 7) {
        return "Invalid Day";
    }

    return WEEKDAYS[day - 1];
}
