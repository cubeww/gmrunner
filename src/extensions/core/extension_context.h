#pragma once

#include "base/types.h"
#include "extensions/core/extension_host.h"
#include "gml/value.h"

#include <cmath>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace gm {

// GM8 runtime external_define value types: ty_real = 0, ty_string = 1.
enum class ExternalValueType : i32 {
    Real = 0,
    String = 1,
};

// Shared argument extraction helpers usable both directly and through CallContext.
[[nodiscard]] inline f64 ext_real(std::span<const RValue> args, usize i, f64 def = 0.0) noexcept {
    if (i >= args.size()) return def;
    if (args[i].is_string()) {
        return std::strtod(args[i].as_string().c_str(), nullptr);
    }
    return args[i].as_real();
}

[[nodiscard]] inline i32 ext_i32(std::span<const RValue> args, usize i, i32 def = 0) noexcept {
    const f64 v = ext_real(args, i, static_cast<f64>(def));
    if (!std::isfinite(v)) return 0;
    return static_cast<i32>(v);
}

[[nodiscard]] inline u32 ext_u32(std::span<const RValue> args, usize i, u32 def = 0) noexcept {
    const f64 v = ext_real(args, i, static_cast<f64>(def));
    if (!std::isfinite(v)) return 0u;
    return static_cast<u32>(static_cast<i64>(v));
}

[[nodiscard]] inline bool ext_bool(std::span<const RValue> args, usize i, bool def = false) noexcept {
    return ext_real(args, i, def ? 1.0 : 0.0) >= 0.5;
}

[[nodiscard]] inline std::string ext_str(std::span<const RValue> args, usize i, std::string_view def = {}) {
    if (i >= args.size()) return std::string(def);
    if (args[i].is_string()) return args[i].as_string();
    return format_gm_real(args[i].as_real());
}

// Per-invocation call context passed to extension handlers.
struct CallContext {
    IExtensionHost& host;
    std::string_view func_name;
    std::span<const RValue> args;
    ExternalValueType expected_return = ExternalValueType::Real;

    [[nodiscard]] usize argc() const noexcept { return args.size(); }

    [[nodiscard]] f64 real(usize i, f64 def = 0.0) const noexcept {
        return ext_real(args, i, def);
    }

    [[nodiscard]] i32 i32_arg(usize i, i32 def = 0) const noexcept {
        return ext_i32(args, i, def);
    }

    [[nodiscard]] u32 u32_arg(usize i, u32 def = 0) const noexcept {
        return ext_u32(args, i, def);
    }

    [[nodiscard]] bool boolean(usize i, bool def = false) const noexcept {
        return ext_bool(args, i, def);
    }

    [[nodiscard]] std::string str(usize i, std::string_view def = {}) const {
        return ext_str(args, i, def);
    }

    [[nodiscard]] const RValue& raw(usize i) const noexcept {
        static const RValue kZero(0.0);
        return i < args.size() ? args[i] : kZero;
    }
};

namespace detail {

// Stack-resident argument holder for compile-time typed extension bindings.
// Ensures temporary strings (for const char* / std::string_view) outlive the call
// while borrowing existing RValue strings with zero copies.
template <typename T, typename Enable = void>
struct ArgSlot;

template <>
struct ArgSlot<bool> {
    bool value = false;
    explicit ArgSlot(const CallContext& ctx, usize i) noexcept : value(ctx.boolean(i, false)) {}
    [[nodiscard]] bool get() const noexcept { return value; }
};

template <typename T>
struct ArgSlot<T, std::enable_if_t<std::is_floating_point_v<T>>> {
    T value{};
    explicit ArgSlot(const CallContext& ctx, usize i) noexcept : value(static_cast<T>(ctx.real(i, 0.0))) {}
    [[nodiscard]] T get() const noexcept { return value; }
};

template <typename T>
struct ArgSlot<T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>>> {
    T value{};
    explicit ArgSlot(const CallContext& ctx, usize i) noexcept {
        const f64 v = ctx.real(i, 0.0);
        if (!std::isfinite(v)) {
            value = T{};
        } else if constexpr (std::is_unsigned_v<T>) {
            value = static_cast<T>(static_cast<i64>(v));
        } else {
            value = static_cast<T>(v);
        }
    }
    [[nodiscard]] T get() const noexcept { return value; }
};

template <>
struct ArgSlot<std::string> {
    std::string value;
    explicit ArgSlot(const CallContext& ctx, usize i) : value(ctx.str(i)) {}
    [[nodiscard]] const std::string& get() const noexcept { return value; }
};

template <>
struct ArgSlot<std::string_view> {
    std::string owned;
    std::string_view borrowed;
    bool use_owned = false;
    explicit ArgSlot(const CallContext& ctx, usize i) {
        if (i < ctx.args.size()) {
            if (ctx.args[i].is_string()) {
                borrowed = ctx.args[i].as_string();
            } else {
                owned = format_gm_real(ctx.args[i].as_real());
                use_owned = true;
            }
        }
    }
    [[nodiscard]] std::string_view get() const noexcept {
        return use_owned ? std::string_view(owned) : borrowed;
    }
};

template <>
struct ArgSlot<const char*> {
    std::string owned;
    const char* borrowed = "";
    bool use_owned = false;
    explicit ArgSlot(const CallContext& ctx, usize i) {
        if (i < ctx.args.size()) {
            if (ctx.args[i].is_string()) {
                borrowed = ctx.args[i].as_string().c_str();
            } else {
                owned = format_gm_real(ctx.args[i].as_real());
                use_owned = true;
            }
        }
    }
    [[nodiscard]] const char* get() const noexcept {
        return use_owned ? owned.c_str() : borrowed;
    }
};

template <>
struct ArgSlot<RValue> {
    const RValue* ptr = nullptr;
    explicit ArgSlot(const CallContext& ctx, usize i) noexcept : ptr(&ctx.raw(i)) {}
    [[nodiscard]] const RValue& get() const noexcept { return *ptr; }
};

template <typename Ret>
[[nodiscard]] inline RValue pack_return(Ret&& val) {
    using D = std::remove_cvref_t<Ret>;
    if constexpr (std::is_same_v<D, RValue>) {
        return std::forward<Ret>(val);
    } else if constexpr (std::is_same_v<D, bool>) {
        return RValue(val ? 1.0 : 0.0);
    } else if constexpr (std::is_arithmetic_v<D>) {
        return RValue(static_cast<f64>(val));
    } else if constexpr (std::is_same_v<D, std::string>) {
        return RValue(std::forward<Ret>(val));
    } else if constexpr (std::is_same_v<D, std::string_view>) {
        return RValue(val);
    } else if constexpr (std::is_same_v<D, const char*>) {
        return RValue(val ? std::string(val) : std::string{});
    } else {
        static_assert(sizeof(D) == 0, "Unsupported return type for extension function binding");
    }
}

} // namespace detail

} // namespace gm
