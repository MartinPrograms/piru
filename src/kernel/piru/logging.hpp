#ifndef PIRU_LOGGING_HPP
#define PIRU_LOGGING_HPP

#include <definitions.hpp>
#include <cpp/fmt.hpp>
#include <cpp/stream.hpp>
#include <cpp/ANSI.hpp>
#include <cpp/spinlock.hpp>

#include "config.hpp"

namespace piru::core {
    threading::spinlock &GetLogLock();

    template<LogLevel level, typename... Args>
    void LogFmt(string_view location, string_view fmt, Args&&... args) {
        if constexpr ((int)level < (int)piru::config::ConsoleLogLevel)
            return;

        threading::spinlock::scoped_lock lock(GetLogLock(), true);

        string_view color = ANSI::Colors::Foreground::White;
        string_view defaultColor = ANSI::Colors::Foreground::White;

        string_view levelStr = "DEBUG";
        switch (level) {
            case LogLevel::DEBUG:
                color = ANSI::Colors::Foreground::Cyan;
                levelStr = "DEBUG";
                break;
            case LogLevel::INFO:
                color = ANSI::Colors::Foreground::Green;
                levelStr = "INFO";
                break;
            case LogLevel::WARNING:
                color = ANSI::Colors::Foreground::Yellow;
                levelStr = "WARNING";
                break;
            case LogLevel::ERROR:
                color = ANSI::Colors::Foreground::Red;
                levelStr = "ERROR";
                break;
            case LogLevel::FATAL:
                color = ANSI::Colors::Foreground::Magenta;
                levelStr = "FATAL";
                break;
        }

        stream::debug("[", 1);
        stream::debug(color.Data(), color.Size());
        stream::debug(levelStr.Data(), levelStr.Size());
        stream::debug(defaultColor.Data(), defaultColor.Size());
        stream::debug("] ", 3);

        stream::debug("[", 1);
        stream::debug(location.Data(), location.Size());
        stream::debug("]: ", 4);

        fmt::format(stream::debug, fmt, std::forward<Args>(args)...);
        stream::debug("\n\r", 2);
    }
}

#define LOG_MESSAGE(level, location, message, ...) \
    piru::core::LogFmt<level>(location, message __VA_OPT__(,) __VA_ARGS__)

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#define LOG_DEBUG(message, ...) LOG_MESSAGE(piru::core::LogLevel::DEBUG, __FILE_NAME__ ":" STRINGIFY(__LINE__), message, __VA_ARGS__)
#define LOG_INFO(message, ...) LOG_MESSAGE(piru::core::LogLevel::INFO, __FILE_NAME__ ":" STRINGIFY(__LINE__), message, __VA_ARGS__)
#define LOG_WARNING(message, ...) LOG_MESSAGE(piru::core::LogLevel::WARNING, __FILE_NAME__ ":" STRINGIFY(__LINE__), message, __VA_ARGS__)
#define LOG_ERROR(message, ...) LOG_MESSAGE(piru::core::LogLevel::ERROR, __FILE_NAME__ ":" STRINGIFY(__LINE__), message, __VA_ARGS__)
#define LOG_FATAL(message, ...) LOG_MESSAGE(piru::core::LogLevel::FATAL, __FILE_NAME__ ":" STRINGIFY(__LINE__), message, __VA_ARGS__)

#define PANIC(message, ...) \
    do { \
        LOG_FATAL(message __VA_OPT__(,) __VA_ARGS__); \
        piru::core::Panic(message); \
    } while (0)

#endif //PIRU_LOGGING_HPP
