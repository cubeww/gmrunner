#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace gm {

using i8 = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using f32 = float;
using f64 = double;
using usize = std::size_t;

enum class VersionMode : u8 {
    Auto = 0,
    GM80 = 80,
    GM81 = 81,
    GM82 = 82,
};

enum class EncodingMode : u8 {
    LegacyAnsi = 0, // GM 8.0 single-byte / Windows codepage strings
    Utf8 = 1,       // GM 8.1 / 8.2 UTF-8 internal strings
};

inline constexpr std::string_view version_mode_name(VersionMode mode) noexcept {
    switch (mode) {
        case VersionMode::Auto: return "auto";
        case VersionMode::GM80: return "8.0";
        case VersionMode::GM81: return "8.1";
        case VersionMode::GM82: return "8.2";
    }
    return "unknown";
}

struct Error {
    std::string message;

    Error() = default;
    explicit Error(std::string msg) : message(std::move(msg)) {}
    explicit Error(std::string_view msg) : message(msg) {}
    explicit Error(const char* msg) : message(msg ? msg : "") {}
};

template <typename T>
class Result {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Error err) : storage_(std::move(err)) {}

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] const Error& error() const& { return std::get<Error>(storage_); }
    [[nodiscard]] const std::string& error_message() const& { return std::get<Error>(storage_).message; }

private:
    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    Result() = default;
    Result(Error err) : error_(std::move(err)) {}

    static Result success() { return Result{}; }

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const& { return *error_; }
    [[nodiscard]] const std::string& error_message() const& { return error_->message; }

private:
    std::optional<Error> error_;
};

struct NonCopyable {
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&) = default;
    NonCopyable& operator=(NonCopyable&&) = default;
};

} // namespace gm
