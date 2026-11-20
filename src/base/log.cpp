#include "base/log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <vector>

namespace gm {

namespace {

std::atomic<u8> g_log_level{static_cast<u8>(LogLevel::Info)};
TraceFlags g_trace_flags{};
std::mutex g_log_mutex;

constexpr const char* level_tag(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

} // namespace

void Logger::set_level(LogLevel level) noexcept {
    g_log_level.store(static_cast<u8>(level), std::memory_order_relaxed);
}

LogLevel Logger::level() noexcept {
    return static_cast<LogLevel>(g_log_level.load(std::memory_order_relaxed));
}

void Logger::set_trace_flags(const TraceFlags& flags) noexcept {
    std::scoped_lock lock(g_log_mutex);
    g_trace_flags = flags;
}

const TraceFlags& Logger::trace_flags() noexcept {
    return g_trace_flags;
}

void Logger::write(LogLevel level, std::string_view category, std::string_view message) {
    if (static_cast<u8>(level) < g_log_level.load(std::memory_order_relaxed)) {
        return;
    }

    std::scoped_lock lock(g_log_mutex);
    FILE* out = (level >= LogLevel::Warn) ? stderr : stdout;
    std::fprintf(
        out,
        "[%s][%.*s] %.*s\n",
        level_tag(level),
        static_cast<int>(category.size()),
        category.data(),
        static_cast<int>(message.size()),
        message.data());
    std::fflush(out);
}

void Logger::writef(LogLevel level, std::string_view category, const char* fmt, ...) {
    if (static_cast<u8>(level) < g_log_level.load(std::memory_order_relaxed)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    va_list args_copy;
    va_copy(args_copy, args);

    char stack_buf[512];
    const int needed = std::vsnprintf(stack_buf, sizeof(stack_buf), fmt, args);
    va_end(args);

    if (needed < 0) {
        va_end(args_copy);
        return;
    }

    if (static_cast<usize>(needed) < sizeof(stack_buf)) {
        va_end(args_copy);
        write(level, category, std::string_view(stack_buf, static_cast<usize>(needed)));
        return;
    }

    std::vector<char> heap_buf(static_cast<usize>(needed) + 1);
    std::vsnprintf(heap_buf.data(), heap_buf.size(), fmt, args_copy);
    va_end(args_copy);
    write(level, category, std::string_view(heap_buf.data(), static_cast<usize>(needed)));
}

} // namespace gm
