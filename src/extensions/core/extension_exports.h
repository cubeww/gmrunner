#pragma once

#include "base/encoding.h"
#include "base/log.h"
#include "base/types.h"
#include "extensions/core/extension_context.h"

#include <deque>
#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gm {

// C++20 transparent string hash enabling zero-allocation std::string_view lookups.
struct TransparentStringHash {
    using is_transparent = void;

    [[nodiscard]] usize operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    [[nodiscard]] usize operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
    [[nodiscard]] usize operator()(const char* s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

template <typename V>
using StringHashMap = std::unordered_map<std::string, V, TransparentStringHash, std::equal_to<>>;

// Lightweight POD dispatch descriptor stored per registered extension export.
struct ExportedFunction {
    using Trampoline = RValue (*)(void* instance, const CallContext& ctx);

    Trampoline invoke = nullptr;
    void* instance = nullptr;
    u32 flags = 0;

    [[nodiscard]] explicit operator bool() const noexcept { return invoke != nullptr; }
};

namespace detail {

template <typename... Args>
struct TypeList {};

template <typename Sig>
struct FnTraits;

template <typename R, typename... Args>
struct FnTraits<R (*)(Args...)> {
    using Ret = R;
    using ArgList = TypeList<Args...>;
};

template <typename R, typename... Args>
struct FnTraits<R (*)(Args...) noexcept> {
    using Ret = R;
    using ArgList = TypeList<Args...>;
};

template <typename R, typename... Args>
struct FnTraits<R(Args...)> {
    using Ret = R;
    using ArgList = TypeList<Args...>;
};

template <typename R, typename... Args>
struct FnTraits<R(Args...) noexcept> {
    using Ret = R;
    using ArgList = TypeList<Args...>;
};

template <typename Sig>
struct MemFnTraits;

template <typename C, typename R, typename... Args>
struct MemFnTraits<R (C::*)(Args...)> {
    using Class = C;
    using Ret = R;
    using ArgList = TypeList<Args...>;
    static constexpr bool kIsConst = false;
};

template <typename C, typename R, typename... Args>
struct MemFnTraits<R (C::*)(Args...) const> {
    using Class = C;
    using Ret = R;
    using ArgList = TypeList<Args...>;
    static constexpr bool kIsConst = true;
};

template <typename C, typename R, typename... Args>
struct MemFnTraits<R (C::*)(Args...) noexcept> {
    using Class = C;
    using Ret = R;
    using ArgList = TypeList<Args...>;
    static constexpr bool kIsConst = false;
};

template <typename C, typename R, typename... Args>
struct MemFnTraits<R (C::*)(Args...) const noexcept> {
    using Class = C;
    using Ret = R;
    using ArgList = TypeList<Args...>;
    static constexpr bool kIsConst = true;
};

template <typename Ret, typename Fn, typename... Args, usize... Is>
[[nodiscard]] inline RValue invoke_slots(Fn&& fn, const CallContext& ctx, TypeList<Args...>, std::index_sequence<Is...>) {
    if constexpr (std::is_void_v<Ret>) {
        std::forward<Fn>(fn)(ArgSlot<std::remove_cvref_t<Args>>(ctx, Is).get()...);
        return RValue(0.0);
    } else {
        return pack_return(std::forward<Fn>(fn)(ArgSlot<std::remove_cvref_t<Args>>(ctx, Is).get()...));
    }
}

template <typename Ret, typename Fn, typename... Args, usize... Is>
[[nodiscard]] inline RValue invoke_slots_with_host(Fn&& fn, const CallContext& ctx, TypeList<Args...>, std::index_sequence<Is...>) {
    if constexpr (std::is_void_v<Ret>) {
        std::forward<Fn>(fn)(ctx.host, ArgSlot<std::remove_cvref_t<Args>>(ctx, Is).get()...);
        return RValue(0.0);
    } else {
        return pack_return(std::forward<Fn>(fn)(ctx.host, ArgSlot<std::remove_cvref_t<Args>>(ctx, Is).get()...));
    }
}

template <typename Ret, typename Fn>
[[nodiscard]] inline RValue invoke_marshaled(Fn&& fn, const CallContext& ctx, TypeList<>) {
    (void)ctx;
    if constexpr (std::is_void_v<Ret>) {
        std::forward<Fn>(fn)();
        return RValue(0.0);
    } else {
        return pack_return(std::forward<Fn>(fn)());
    }
}

template <typename Ret, typename Fn, typename First, typename... Rest>
[[nodiscard]] inline RValue invoke_marshaled(Fn&& fn, const CallContext& ctx, TypeList<First, Rest...>) {
    if constexpr (sizeof...(Rest) == 0 && std::is_same_v<std::remove_cvref_t<First>, CallContext>) {
        if constexpr (std::is_void_v<Ret>) {
            std::forward<Fn>(fn)(ctx);
            return RValue(0.0);
        } else {
            return pack_return(std::forward<Fn>(fn)(ctx));
        }
    } else if constexpr (std::is_same_v<std::remove_cvref_t<First>, IExtensionHost>) {
        return invoke_slots_with_host<Ret>(std::forward<Fn>(fn), ctx, TypeList<Rest...>{}, std::index_sequence_for<Rest...>{});
    } else {
        return invoke_slots<Ret>(std::forward<Fn>(fn), ctx, TypeList<First, Rest...>{}, std::index_sequence_for<First, Rest...>{});
    }
}

template <auto Fn>
RValue free_fn_trampoline(void*, const CallContext& ctx) {
    using Traits = FnTraits<decltype(Fn)>;
    return invoke_marshaled<typename Traits::Ret>(
        [](auto&&... args) -> decltype(auto) {
            return Fn(std::forward<decltype(args)>(args)...);
        },
        ctx,
        typename Traits::ArgList{});
}

template <auto Method>
RValue member_fn_trampoline(void* instance, const CallContext& ctx) {
    using Traits = MemFnTraits<decltype(Method)>;
    using ObjPtr = std::conditional_t<Traits::kIsConst, const typename Traits::Class*, typename Traits::Class*>;
    ObjPtr self = static_cast<ObjPtr>(instance);
    return invoke_marshaled<typename Traits::Ret>(
        [self](auto&&... args) -> decltype(auto) {
            return (self->*Method)(std::forward<decltype(args)>(args)...);
        },
        ctx,
        typename Traits::ArgList{});
}

template <typename F>
RValue stateless_callable_trampoline(void*, const CallContext& ctx) {
    F fn{};
    if constexpr (std::is_invocable_v<F&, const CallContext&>) {
        using Ret = std::invoke_result_t<F&, const CallContext&>;
        if constexpr (std::is_void_v<Ret>) {
            fn(ctx);
            return RValue(0.0);
        } else {
            return pack_return(fn(ctx));
        }
    } else {
        using Traits = MemFnTraits<decltype(&F::operator())>;
        return invoke_marshaled<typename Traits::Ret>(fn, ctx, typename Traits::ArgList{});
    }
}

} // namespace detail

// Per-extension function registry mapping export names -> O(1) ExportedFunction entries.
class ExportTable : public NonCopyable {
public:
    ExportTable() = default;
    ~ExportTable() = default;

    void set_case_insensitive(bool enabled) noexcept { case_insensitive_ = enabled; }
    [[nodiscard]] bool is_case_insensitive() const noexcept { return case_insensitive_; }

    void add_prefix_alias(std::string_view prefix) {
        if (!prefix.empty()) {
            prefix_aliases_.emplace_back(prefix);
        }
    }

    void add_prefix_aliases(std::initializer_list<std::string_view> prefixes) {
        for (std::string_view p : prefixes) {
            add_prefix_alias(p);
        }
    }

    // Scoped default flags applied to all registrations made inside body.
    template <typename Fn>
    void with_flags(u32 flags, Fn&& body) {
        const u32 prev = default_flags_;
        default_flags_ = flags;
        struct Guard {
            u32& dst;
            u32 saved;
            ~Guard() { dst = saved; }
        } guard{default_flags_, prev};
        if constexpr (std::is_invocable_v<Fn, ExportTable&>) {
            std::forward<Fn>(body)(*this);
        } else {
            std::forward<Fn>(body)();
        }
    }

    // Bind a raw ExportedFunction entry directly.
    void add(std::string_view name, ExportedFunction fn) {
        if (fn.flags == 0 && default_flags_ != 0) {
            fn.flags = default_flags_;
        }
        std::string key = normalize_key(name);
        if (entries_.contains(key)) {
            GM_LOG_WARN("ext", "Duplicate export '%s' registered in ExportTable (overwriting)", key.c_str());
        }
        entries_[std::move(key)] = fn;
    }

    void add(std::initializer_list<std::string_view> names, ExportedFunction fn) {
        for (std::string_view name : names) {
            add(name, fn);
        }
    }

    // Compile-time free/C function binding (zero heap allocation, automatic argument marshaling).
    template <auto Fn>
    void bind(std::string_view name) {
        add(name, ExportedFunction{&detail::free_fn_trampoline<Fn>, nullptr, default_flags_});
    }

    template <auto Fn>
    void bind(std::initializer_list<std::string_view> names) {
        add(names, ExportedFunction{&detail::free_fn_trampoline<Fn>, nullptr, default_flags_});
    }

    // Compile-time member function binding (zero heap allocation, automatic argument marshaling).
    template <auto Method, typename T>
    void bind(T* instance, std::string_view name) {
        using Owner = typename detail::MemFnTraits<decltype(Method)>::Class;
        static_assert(std::is_base_of_v<Owner, std::remove_const_t<T>>,
                      "Bound instance pointer must derive from the method's class");
        add(name, ExportedFunction{
            &detail::member_fn_trampoline<Method>,
            const_cast<void*>(static_cast<const void*>(static_cast<const Owner*>(instance))),
            default_flags_});
    }

    template <auto Method, typename T>
    void bind(T* instance, std::initializer_list<std::string_view> names) {
        using Owner = typename detail::MemFnTraits<decltype(Method)>::Class;
        static_assert(std::is_base_of_v<Owner, std::remove_const_t<T>>,
                      "Bound instance pointer must derive from the method's class");
        add(names, ExportedFunction{
            &detail::member_fn_trampoline<Method>,
            const_cast<void*>(static_cast<const void*>(static_cast<const Owner*>(instance))),
            default_flags_});
    }

    // Lambda / functor binding (supports both raw CallContext and typed argument signatures).
    template <typename Callable>
    void bind(std::string_view name, Callable&& callable) {
        add(name, make_callable_entry(std::forward<Callable>(callable)));
    }

    template <typename Callable>
    void bind(std::initializer_list<std::string_view> names, Callable&& callable) {
        add(names, make_callable_entry(std::forward<Callable>(callable)));
    }

    // Constant & stub helpers.
    void bind_const(std::string_view name, f64 value) {
        const_reals_.push_back(value);
        add(name, ExportedFunction{&const_real_trampoline, &const_reals_.back(), default_flags_});
    }

    void bind_const(std::initializer_list<std::string_view> names, f64 value) {
        const_reals_.push_back(value);
        add(names, ExportedFunction{&const_real_trampoline, &const_reals_.back(), default_flags_});
    }

    void bind_const(std::string_view name, std::string_view value) {
        const_strings_.emplace_back(value);
        add(name, ExportedFunction{&const_string_trampoline, &const_strings_.back(), default_flags_});
    }

    void bind_const(std::initializer_list<std::string_view> names, std::string_view value) {
        const_strings_.emplace_back(value);
        add(names, ExportedFunction{&const_string_trampoline, &const_strings_.back(), default_flags_});
    }

    void bind_stub(std::string_view name, f64 value = 0.0) {
        bind_const(name, value);
    }

    void bind_stub(std::initializer_list<std::string_view> names, f64 value = 0.0) {
        bind_const(names, value);
    }

    [[nodiscard]] std::optional<ExportedFunction> find(std::string_view name) const {
        if (!case_insensitive_) {
            if (const auto it = entries_.find(name); it != entries_.end()) {
                return it->second;
            }
            for (const auto& prefix : prefix_aliases_) {
                if (name.starts_with(prefix)) {
                    const std::string_view stripped = name.substr(prefix.size());
                    if (const auto it = entries_.find(stripped); it != entries_.end()) {
                        return it->second;
                    }
                }
            }
            return std::nullopt;
        }

        const std::string lo = ascii_to_lower(name);
        if (const auto it = entries_.find(lo); it != entries_.end()) {
            return it->second;
        }
        for (const auto& prefix : prefix_aliases_) {
            const std::string lo_prefix = ascii_to_lower(prefix);
            if (lo.starts_with(lo_prefix)) {
                const std::string_view stripped = std::string_view(lo).substr(lo_prefix.size());
                if (const auto it = entries_.find(stripped); it != entries_.end()) {
                    return it->second;
                }
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] usize size() const noexcept { return entries_.size(); }

private:
    using OwnedFn = std::function<RValue(const CallContext&)>;

    [[nodiscard]] std::string normalize_key(std::string_view name) const {
        return case_insensitive_ ? ascii_to_lower(name) : std::string(name);
    }

    static RValue owned_fn_trampoline(void* instance, const CallContext& ctx) {
        return (*static_cast<OwnedFn*>(instance))(ctx);
    }

    static RValue const_real_trampoline(void* instance, const CallContext&) {
        return RValue(*static_cast<const f64*>(instance));
    }

    static RValue const_string_trampoline(void* instance, const CallContext&) {
        return RValue(*static_cast<const std::string*>(instance));
    }

    template <typename Callable>
    ExportedFunction make_callable_entry(Callable&& callable) {
        using F = std::decay_t<Callable>;
        if constexpr (std::is_empty_v<F> && std::is_default_constructible_v<F>) {
            return ExportedFunction{&detail::stateless_callable_trampoline<F>, nullptr, default_flags_};
        } else {
            owned_fns_.emplace_back([fn = std::forward<Callable>(callable)](const CallContext& ctx) mutable -> RValue {
                if constexpr (std::is_invocable_v<F&, const CallContext&>) {
                    using Ret = std::invoke_result_t<F&, const CallContext&>;
                    if constexpr (std::is_void_v<Ret>) {
                        fn(ctx);
                        return RValue(0.0);
                    } else {
                        return detail::pack_return(fn(ctx));
                    }
                } else if constexpr (std::is_pointer_v<F> && std::is_function_v<std::remove_pointer_t<F>>) {
                    using Traits = detail::FnTraits<F>;
                    return detail::invoke_marshaled<typename Traits::Ret>(fn, ctx, typename Traits::ArgList{});
                } else {
                    using Traits = detail::MemFnTraits<decltype(&F::operator())>;
                    return detail::invoke_marshaled<typename Traits::Ret>(fn, ctx, typename Traits::ArgList{});
                }
            });
            return ExportedFunction{&owned_fn_trampoline, &owned_fns_.back(), default_flags_};
        }
    }

    bool case_insensitive_ = false;
    u32 default_flags_ = 0;
    std::vector<std::string> prefix_aliases_;
    StringHashMap<ExportedFunction> entries_;
    std::deque<OwnedFn> owned_fns_;
    std::deque<f64> const_reals_;
    std::deque<std::string> const_strings_;
};

} // namespace gm
