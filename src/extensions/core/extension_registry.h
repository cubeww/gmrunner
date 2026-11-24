#pragma once

#include "base/types.h"
#include "extensions/core/extension.h"
#include "gml/builtins_catalog.h"
#include "gml/symbol_table.h"
#include "gml/value.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gm {

class ExtensionRegistry : public NonCopyable {
public:
    ExtensionRegistry();
    ~ExtensionRegistry();

    void register_extension(std::unique_ptr<IExtension> ext);
    // Defined by CMake-generated generated_extension_registry.cpp in gm_extensions
    // so gm_extensions -> gm_ext_* -> gm_extensions_core remains strictly acyclic
    // for single-pass static linkers (e.g. GNU ld on Linux).
    void register_compiled_extensions();
    void clear_extensions();
    void log_registered_summary() const;
    void reset(IExtensionHost* host = nullptr);

    // Normalizes a DLL path/filename to lowercase stem without directory or ".dll/.so/.dylib" suffix.
    [[nodiscard]] static std::string normalize_dll_name(std::string_view dll_path);

    // Pre-binds all embedded GEX kind=1 DLL exports for O(1) zero-lookup hot-path invocation.
    void bind_gex_functions(std::span<const ExtensionDllFunction> funcs);

    [[nodiscard]] RValue call_gex_bound(
        IExtensionHost& host,
        u32 ext_dll_id,
        std::span<const RValue> args);

    // Name-based GEX kind=1 DLL dispatch (used for startup init_func / shutdown final_func).
    [[nodiscard]] RValue call_gex_dll(
        IExtensionHost& host,
        std::string_view dll_filename,
        std::string_view external_name,
        std::span<const RValue> args,
        ExternalValueType return_type);

    // Runtime external_define / external_call / external_free / get_function_address.
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

    // Introspection & shared services access.
    [[nodiscard]] ExtensionServices& services() noexcept { return services_; }
    [[nodiscard]] bool has_extension(std::string_view dll_path) const noexcept;
    [[nodiscard]] bool has_function(std::string_view dll_path, std::string_view func_name);
    [[nodiscard]] usize extension_count() const noexcept { return extensions_.size(); }
    [[nodiscard]] usize total_export_count() const noexcept;

private:
    struct RegisteredExtension {
        std::unique_ptr<IExtension> ext;
        ExportTable exports;
        bool custom_dispatch = false;
    };

    struct ResolvedExport {
        RegisteredExtension* owner = nullptr;
        ExportedFunction fn{};

        [[nodiscard]] bool has_extension() const noexcept { return owner != nullptr; }
        [[nodiscard]] bool has_function() const noexcept { return owner != nullptr && static_cast<bool>(fn); }

        [[nodiscard]] RValue invoke(const CallContext& ctx) const {
            if (owner->custom_dispatch) {
                return owner->ext->dispatch(fn, ctx);
            }
            return fn.invoke(fn.instance, ctx);
        }
    };

    struct ExternalBinding {
        i32 id = 0;
        std::string raw_dll_path;
        std::string normalized_dll;
        std::string func_name;
        i32 call_type = 0;
        ExternalValueType return_type = ExternalValueType::Real;
        std::vector<ExternalValueType> arg_types;
        ResolvedExport resolved{};
        bool active = true;
    };

    struct BoundGexCall {
        std::string normalized_dll;
        std::string external_name;
        ExternalValueType return_type = ExternalValueType::Real;
        std::vector<ExternalValueType> arg_types;
        ResolvedExport resolved{};
    };

    [[nodiscard]] RegisteredExtension* find_extension(std::string_view normalized_dll) const noexcept;
    [[nodiscard]] ResolvedExport resolve_export(std::string_view normalized_dll, std::string_view func_name) const;
    [[nodiscard]] static RValue coerce_result(RValue val, ExternalValueType expected);
    [[nodiscard]] RValue invoke_with_coercion(
        IExtensionHost& host,
        const ResolvedExport& resolved,
        std::string_view normalized_dll,
        std::string_view func_name,
        std::span<const ExternalValueType> arg_types,
        ExternalValueType return_type,
        std::span<const RValue> args,
        const char* trace_tag);
    void warn_missing_once(
        std::string_view normalized_dll,
        std::string_view func_name,
        bool has_extension,
        const char* call_kind);

    ExtensionServices services_;
    std::vector<std::unique_ptr<RegisteredExtension>> extensions_;
    StringHashMap<RegisteredExtension*> dll_aliases_;
    std::vector<ExternalBinding> bindings_;
    std::vector<BoundGexCall> gex_bindings_;
    std::unordered_set<std::string> warned_missing_;
};

} // namespace gm
