#pragma once

#include "base/types.h"
#include "extensions/core/extension_context.h"
#include "extensions/core/extension_exports.h"
#include "extensions/core/extension_host.h"
#include "extensions/core/extension_services.h"

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gm {

// Builder passed to IExtension::on_register() so each extension declaratively
// configures its DLL aliases, shared services, dispatch policy, and exported functions.
class ExtensionRegistrar : public NonCopyable {
public:
    ExtensionRegistrar(ExportTable& exports, ExtensionServices& services) noexcept
        : exports_(exports), services_(services) {}

    void add_dll_alias(std::string_view alias) {
        if (!alias.empty()) {
            dll_aliases_.emplace_back(alias);
        }
    }

    void add_dll_aliases(std::initializer_list<std::string_view> aliases) {
        for (std::string_view a : aliases) {
            add_dll_alias(a);
        }
    }

    void set_case_insensitive(bool enabled) noexcept { exports_.set_case_insensitive(enabled); }
    void add_prefix_alias(std::string_view prefix) { exports_.add_prefix_alias(prefix); }
    void add_prefix_aliases(std::initializer_list<std::string_view> prefixes) {
        exports_.add_prefix_aliases(prefixes);
    }
    void set_custom_dispatch(bool enabled) noexcept { custom_dispatch_ = enabled; }

    [[nodiscard]] ExtensionServices& services() noexcept { return services_; }
    [[nodiscard]] ExportTable& exports() noexcept { return exports_; }

    template <typename Fn>
    void with_flags(u32 flags, Fn&& body) {
        exports_.with_flags(flags, std::forward<Fn>(body));
    }

    void add(std::string_view name, ExportedFunction fn) {
        exports_.add(name, fn);
    }

    void add(std::initializer_list<std::string_view> names, ExportedFunction fn) {
        exports_.add(names, fn);
    }

    template <auto Fn>
    void bind(std::string_view name) {
        exports_.bind<Fn>(name);
    }

    template <auto Fn>
    void bind(std::initializer_list<std::string_view> names) {
        exports_.bind<Fn>(names);
    }

    template <auto Method, typename T>
    void bind(T* instance, std::string_view name) {
        exports_.bind<Method>(instance, name);
    }

    template <auto Method, typename T>
    void bind(T* instance, std::initializer_list<std::string_view> names) {
        exports_.bind<Method>(instance, names);
    }

    template <typename Callable>
    void bind(std::string_view name, Callable&& callable) {
        exports_.bind(name, std::forward<Callable>(callable));
    }

    template <typename Callable>
    void bind(std::initializer_list<std::string_view> names, Callable&& callable) {
        exports_.bind(names, std::forward<Callable>(callable));
    }

    void bind_const(std::string_view name, f64 value) { exports_.bind_const(name, value); }
    void bind_const(std::initializer_list<std::string_view> names, f64 value) { exports_.bind_const(names, value); }
    void bind_const(std::string_view name, std::string_view value) { exports_.bind_const(name, value); }
    void bind_const(std::initializer_list<std::string_view> names, std::string_view value) {
        exports_.bind_const(names, value);
    }
    void bind_stub(std::string_view name, f64 value = 0.0) { exports_.bind_stub(name, value); }
    void bind_stub(std::initializer_list<std::string_view> names, f64 value = 0.0) {
        exports_.bind_stub(names, value);
    }

    [[nodiscard]] const std::vector<std::string>& dll_aliases() const noexcept { return dll_aliases_; }
    [[nodiscard]] bool custom_dispatch() const noexcept { return custom_dispatch_; }

private:
    ExportTable& exports_;
    ExtensionServices& services_;
    std::vector<std::string> dll_aliases_;
    bool custom_dispatch_ = false;
};

// Base interface implemented by every native extension module.
class IExtension {
public:
    virtual ~IExtension() = default;

    [[nodiscard]] virtual std::string_view canonical_name() const noexcept = 0;
    virtual void on_register(ExtensionRegistrar& reg) = 0;

    // Optional lifecycle & fallback hooks.
    virtual void on_unload(IExtensionHost& host) { (void)host; }

    [[nodiscard]] virtual bool matches_dll_fallback(std::string_view normalized_dll) const noexcept {
        (void)normalized_dll;
        return false;
    }

    [[nodiscard]] virtual std::optional<ExportedFunction> resolve_dynamic(std::string_view func_name) {
        (void)func_name;
        return std::nullopt;
    }

    [[nodiscard]] virtual RValue dispatch(const ExportedFunction& fn, const CallContext& ctx) {
        return fn.invoke(fn.instance, ctx);
    }
};

} // namespace gm

// Convenience macros for mass-binding C/free functions or class methods by identifier name.
#define GM_EXT_BIND(reg, fn) (reg).bind<&fn>(#fn)
#define GM_EXT_BIND_MEMBER(reg, obj, cls, method) (reg).bind<&cls::method>((obj), #method)
