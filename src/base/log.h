#pragma once

#include "base/types.h"

#include <cstdio>
#include <string>
#include <string_view>

namespace gm {

enum class LogLevel : u8 {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
    Fatal = 5,
};

struct TraceFlags {
    bool events = false;
    bool dll_calls = false;
    bool vfs = false;
};

class Logger {
public:
    static void set_level(LogLevel level) noexcept;
    [[nodiscard]] static LogLevel level() noexcept;

    static void set_trace_flags(const TraceFlags& flags) noexcept;
    [[nodiscard]] static const TraceFlags& trace_flags() noexcept;

    static void write(LogLevel level, std::string_view category, std::string_view message);
    static void writef(LogLevel level, std::string_view category, const char* fmt, ...);
};

} // namespace gm

#define GM_LOG_TRACE(cat, ...) ::gm::Logger::writef(::gm::LogLevel::Trace, (cat), __VA_ARGS__)
#define GM_LOG_DEBUG(cat, ...) ::gm::Logger::writef(::gm::LogLevel::Debug, (cat), __VA_ARGS__)
#define GM_LOG_INFO(cat, ...)  ::gm::Logger::writef(::gm::LogLevel::Info,  (cat), __VA_ARGS__)
#define GM_LOG_WARN(cat, ...)  ::gm::Logger::writef(::gm::LogLevel::Warn,  (cat), __VA_ARGS__)
#define GM_LOG_ERROR(cat, ...) ::gm::Logger::writef(::gm::LogLevel::Error, (cat), __VA_ARGS__)
#define GM_LOG_FATAL(cat, ...) ::gm::Logger::writef(::gm::LogLevel::Fatal, (cat), __VA_ARGS__)
