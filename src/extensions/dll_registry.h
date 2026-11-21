#pragma once

#include "base/types.h"
#include "extensions/extension_host.h"
#include "gml/builtins_catalog.h"
#include "gml/value.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gm {

// GM8 runtime external_define value types: ty_real = 0, ty_string = 1.
enum class ExternalValueType : i32 {
    Real = 0,
    String = 1,
};

struct ExternalBinding {
    i32 id = 0;
    std::string raw_dll_path;
    std::string normalized_dll;
    std::string func_name;
    i32 call_type = 0; // 0 = dll_cdecl, 1 = dll_stdcall (tolerated uniformly)
    ExternalValueType return_type = ExternalValueType::Real;
    std::vector<ExternalValueType> arg_types;
    bool active = true;
};

class IDllShim {
public:
    virtual ~IDllShim() = default;

    [[nodiscard]] virtual std::string_view canonical_name() const noexcept = 0;
    [[nodiscard]] virtual bool matches_dll(std::string_view normalized_dll) const noexcept = 0;
    [[nodiscard]] virtual bool has_function(std::string_view func_name) const noexcept = 0;
    [[nodiscard]] virtual RValue invoke(
        IExtensionHost& host,
        std::string_view func_name,
        std::span<const RValue> args,
        ExternalValueType expected_return) = 0;
    virtual void on_unload(IExtensionHost& host) { (void)host; }
};

class DllRegistry : public NonCopyable {
public:
    DllRegistry();
    ~DllRegistry();

    void register_shim(std::unique_ptr<IDllShim> shim);
    void register_builtin_shims();
    void reset(IExtensionHost* host = nullptr);

    // Normalizes a DLL path/filename to lowercase stem without directory or ".dll" suffix.
    [[nodiscard]] static std::string normalize_dll_name(std::string_view dll_path);

    // Runtime external_define / external_call / external_free / get_function_address
    [[nodiscard]] i32 define_external(
        std::string_view dll_path,
        std::string_view func_name,
        i32 call_type,
        ExternalValueType return_type,
        std::vector<ExternalValueType> arg_types);

    [[nodiscard]] RValue call_external(
        IExtensionHost& host,
        i32 binding_id,
        std::span<const RValue> args);

    void free_external(IExtensionHost& host, std::string_view dll_path);
    [[nodiscard]] static i32 get_function_address(std::string_view name);
    [[nodiscard]] static const BuiltinFuncInfo* callback_by_address(i32 address);

    // Embedded GEX kind=1 DLL dispatch
    [[nodiscard]] RValue call_gex_dll(
        IExtensionHost& host,
        std::string_view dll_filename,
        std::string_view external_name,
        std::span<const RValue> args,
        ExternalValueType return_type);

private:
    [[nodiscard]] IDllShim* find_shim(std::string_view normalized_dll) const noexcept;
    [[nodiscard]] static RValue coerce_result(RValue val, ExternalValueType expected);

    std::vector<std::unique_ptr<IDllShim>> shims_;
    std::unordered_map<i32, ExternalBinding> bindings_;
    i32 next_binding_id_ = 0;
    std::unordered_set<std::string> warned_missing_;
};

// Shared argument extraction helpers for DLL shim implementations.
[[nodiscard]] inline f64 shim_real(std::span<const RValue> args, usize i, f64 def = 0.0) noexcept {
    if (i >= args.size()) return def;
    if (args[i].is_string()) {
        return std::strtod(args[i].as_string().c_str(), nullptr);
    }
    return args[i].as_real();
}

[[nodiscard]] inline i32 shim_i32(std::span<const RValue> args, usize i, i32 def = 0) noexcept {
    return static_cast<i32>(shim_real(args, i, static_cast<f64>(def)));
}

[[nodiscard]] inline bool shim_bool(std::span<const RValue> args, usize i, bool def = false) noexcept {
    return shim_real(args, i, def ? 1.0 : 0.0) >= 0.5;
}

[[nodiscard]] inline std::string shim_str(std::span<const RValue> args, usize i, std::string_view def = {}) {
    if (i >= args.size()) return std::string(def);
    if (args[i].is_string()) return args[i].as_string();
    return format_gm_real(args[i].as_real());
}

} // namespace gm
