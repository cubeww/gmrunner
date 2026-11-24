#include "extensions/core/extension_registry.h"

#include "base/encoding.h"
#include "base/log.h"

#include <array>
#include <cstdlib>
#include <utility>

namespace gm {

namespace {

constexpr usize kStackCoerceLimit = 16;

[[nodiscard]] bool needs_coercion(
    std::span<const ExternalValueType> arg_types,
    std::span<const RValue> args) noexcept {
    const usize n = std::min(arg_types.size(), args.size());
    for (usize i = 0; i < n; ++i) {
        if (arg_types[i] == ExternalValueType::String && !args[i].is_string()) return true;
        if (arg_types[i] == ExternalValueType::Real && args[i].is_string()) return true;
    }
    return false;
}

[[nodiscard]] RValue coerce_arg(const RValue& arg, ExternalValueType expected) {
    if (expected == ExternalValueType::String && !arg.is_string()) {
        return RValue(format_gm_real(arg.as_real()));
    }
    if (expected == ExternalValueType::Real && arg.is_string()) {
        return RValue(std::strtod(arg.as_string().c_str(), nullptr));
    }
    return arg;
}

// These opaque values can be passed through GML reals on every target. Only
// extension modules interpret them, through IExtensionHost::ext_call_function.
constexpr i32 kCallbackBase = 0x10000000;

std::string_view callback_name(std::string_view name) {
    // The original GM8.1 table points each of these spellings to the same routine.
    static constexpr std::pair<std::string_view, std::string_view> aliases[] = {
        {"make_color", "make_color_rgb"}, {"move_bounce", "move_bounce_solid"},
        {"sprite_name", "sprite_get_name"}, {"background_name", "background_get_name"},
        {"sound_name", "sound_get_name"}, {"font_name", "font_get_name"},
        {"script_name", "script_get_name"}, {"path_name", "path_get_name"},
        {"timeline_name", "timeline_get_name"}, {"object_name", "object_get_name"},
        {"room_name", "room_get_name"}, {"part_type_color", "part_type_color3"},
        {"part_type_alpha", "part_type_alpha3"},
        {"action_path_old", "action_highscore"}, {"action_draw_font", "action_highscore"},
        {"action_draw_font_old", "action_highscore"}, {"action_fill_color", "action_highscore"},
        {"action_line_color", "action_highscore"},
    };
    for (const auto& [alias, canonical] : aliases) {
        if (name == alias) return canonical;
    }
    return name;
}

} // namespace

ExtensionRegistry::ExtensionRegistry() = default;
ExtensionRegistry::~ExtensionRegistry() = default;

void ExtensionRegistry::register_extension(std::unique_ptr<IExtension> ext) {
    if (!ext) return;

    auto entry = std::make_unique<RegisteredExtension>();
    entry->ext = std::move(ext);

    ExtensionRegistrar registrar(entry->exports, services_);
    entry->ext->on_register(registrar);
    entry->custom_dispatch = registrar.custom_dispatch();

    RegisteredExtension* raw = entry.get();
    for (const std::string& raw_alias : registrar.dll_aliases()) {
        std::string norm = normalize_dll_name(raw_alias);
        if (norm.empty()) continue;
        if (const auto it = dll_aliases_.find(norm); it != dll_aliases_.end()) {
            GM_LOG_INFO("ext", "Extension '%.*s' overrides DLL alias '%s' (previously '%.*s')",
                        static_cast<int>(raw->ext->canonical_name().size()), raw->ext->canonical_name().data(),
                        norm.c_str(),
                        static_cast<int>(it->second->ext->canonical_name().size()),
                        it->second->ext->canonical_name().data());
        }
        dll_aliases_[std::move(norm)] = raw;
    }

    extensions_.push_back(std::move(entry));
}

void ExtensionRegistry::clear_extensions() {
    dll_aliases_.clear();
    extensions_.clear();
    services_.clear();
    bindings_.clear();
    gex_bindings_.clear();
    warned_missing_.clear();
}

void ExtensionRegistry::log_registered_summary() const {
    GM_LOG_DEBUG("ext", "Registered %zu extension module(s) (%zu static exports, %zu DLL aliases)",
                 extensions_.size(), total_export_count(), dll_aliases_.size());
}

void ExtensionRegistry::reset(IExtensionHost* host) {
    if (host) {
        for (auto& entry : extensions_) {
            if (entry && entry->ext) {
                entry->ext->on_unload(*host);
            }
        }
    }
    services_.reset_all();
    bindings_.clear();
    gex_bindings_.clear();
    warned_missing_.clear();
}

std::string ExtensionRegistry::normalize_dll_name(std::string_view dll_path) {
    const usize slash = dll_path.find_last_of("\\/");
    const std::string_view base = (slash != std::string_view::npos) ? dll_path.substr(slash + 1) : dll_path;
    std::string lo = ascii_to_lower(base);
    if (lo.ends_with(".dll") || lo.ends_with(".so") || lo.ends_with(".dylib")) {
        const usize dot = lo.rfind('.');
        if (dot != std::string::npos) {
            lo.resize(dot);
        }
    }
    return lo;
}

ExtensionRegistry::RegisteredExtension* ExtensionRegistry::find_extension(
    std::string_view normalized_dll) const noexcept {
    if (const auto it = dll_aliases_.find(normalized_dll); it != dll_aliases_.end()) {
        return it->second;
    }
    for (const auto& entry : extensions_) {
        if (entry && entry->ext && entry->ext->matches_dll_fallback(normalized_dll)) {
            return entry.get();
        }
    }
    return nullptr;
}

ExtensionRegistry::ResolvedExport ExtensionRegistry::resolve_export(
    std::string_view normalized_dll,
    std::string_view func_name) const {
    RegisteredExtension* owner = find_extension(normalized_dll);
    if (!owner) {
        return {};
    }
    if (auto fn = owner->exports.find(func_name)) {
        return ResolvedExport{owner, *fn};
    }
    if (auto dyn = owner->ext->resolve_dynamic(func_name)) {
        return ResolvedExport{owner, *dyn};
    }
    return ResolvedExport{owner, ExportedFunction{}};
}

RValue ExtensionRegistry::coerce_result(RValue val, ExternalValueType expected) {
    if (expected == ExternalValueType::String) {
        if (!val.is_string()) {
            return RValue(format_gm_real(val.as_real()));
        }
        return val;
    }
    if (val.is_string()) {
        return RValue(std::strtod(val.as_string().c_str(), nullptr));
    }
    return val;
}

void ExtensionRegistry::warn_missing_once(
    std::string_view normalized_dll,
    std::string_view func_name,
    bool has_ext,
    const char* call_kind) {
    std::string key;
    key.reserve(normalized_dll.size() + 2 + func_name.size());
    key.append(normalized_dll);
    key.append("::");
    key.append(func_name);
    if (warned_missing_.insert(key).second) {
        if (has_ext) {
            GM_LOG_WARN("ext", "Unhandled export for %s %s (returning default)", call_kind, key.c_str());
        } else {
            GM_LOG_WARN("ext", "No native extension for %s %s (returning default)", call_kind, key.c_str());
        }
    }
}

RValue ExtensionRegistry::invoke_with_coercion(
    IExtensionHost& host,
    const ResolvedExport& resolved,
    std::string_view normalized_dll,
    std::string_view func_name,
    std::span<const ExternalValueType> arg_types,
    ExternalValueType return_type,
    std::span<const RValue> args,
    const char* trace_tag) {
    if (!resolved.has_function()) {
        warn_missing_once(normalized_dll, func_name, resolved.has_extension(), trace_tag);
        return (return_type == ExternalValueType::String) ? RValue(std::string{}) : RValue(0.0);
    }

    if (Logger::trace_flags().dll_calls) {
        GM_LOG_TRACE("ext", "%s [%.*s] %.*s (argc=%zu)",
                     trace_tag,
                     static_cast<int>(normalized_dll.size()), normalized_dll.data(),
                     static_cast<int>(func_name.size()), func_name.data(),
                     args.size());
    }

    if (!needs_coercion(arg_types, args)) {
        const CallContext ctx{host, func_name, args, return_type};
        return coerce_result(resolved.invoke(ctx), return_type);
    }

    if (args.size() <= kStackCoerceLimit) {
        std::array<RValue, kStackCoerceLimit> stack_args{};
        for (usize i = 0; i < args.size(); ++i) {
            stack_args[i] = (i < arg_types.size()) ? coerce_arg(args[i], arg_types[i]) : args[i];
        }
        const CallContext ctx{host, func_name, std::span<const RValue>(stack_args.data(), args.size()), return_type};
        return coerce_result(resolved.invoke(ctx), return_type);
    }

    std::vector<RValue> heap_args;
    heap_args.reserve(args.size());
    for (usize i = 0; i < args.size(); ++i) {
        heap_args.push_back((i < arg_types.size()) ? coerce_arg(args[i], arg_types[i]) : args[i]);
    }
    const CallContext ctx{host, func_name, heap_args, return_type};
    return coerce_result(resolved.invoke(ctx), return_type);
}

void ExtensionRegistry::bind_gex_functions(std::span<const ExtensionDllFunction> funcs) {
    gex_bindings_.clear();
    gex_bindings_.reserve(funcs.size());

    const bool audit = (Logger::level() <= LogLevel::Debug);
    usize missing_count = 0;

    for (const auto& fn : funcs) {
        BoundGexCall bound{};
        bound.normalized_dll = normalize_dll_name(fn.dll_filename);
        bound.external_name = fn.external_name;
        bound.return_type = (fn.return_type == 1) ? ExternalValueType::String : ExternalValueType::Real;
        bound.arg_types.reserve(fn.arg_types.size());
        for (i32 t : fn.arg_types) {
            // GEX arg type codes: 1 = string, 2 = real.
            bound.arg_types.push_back((t == 1) ? ExternalValueType::String : ExternalValueType::Real);
        }
        bound.resolved = resolve_export(bound.normalized_dll, bound.external_name);

        if (audit && !bound.resolved.has_function()) {
            ++missing_count;
            GM_LOG_DEBUG("ext", "Unimplemented GEX DLL export: %s::%s (ext_dll_id=%u, dll_handled=%s)",
                         bound.normalized_dll.c_str(),
                         bound.external_name.c_str(),
                         fn.id,
                         bound.resolved.has_extension() ? "yes" : "no");
        }

        gex_bindings_.push_back(std::move(bound));
    }

    if (audit && !gex_bindings_.empty()) {
        GM_LOG_DEBUG("ext", "Pre-bound %zu GEX DLL export(s) (%zu missing)",
                     gex_bindings_.size(), missing_count);
    }
}

RValue ExtensionRegistry::call_gex_bound(
    IExtensionHost& host,
    u32 ext_dll_id,
    std::span<const RValue> args) {
    if (ext_dll_id >= gex_bindings_.size()) {
        return RValue(0.0);
    }
    const auto& b = gex_bindings_[ext_dll_id];
    return invoke_with_coercion(
        host, b.resolved, b.normalized_dll, b.external_name,
        b.arg_types, b.return_type, args, "gex_dll_call");
}

RValue ExtensionRegistry::call_gex_dll(
    IExtensionHost& host,
    std::string_view dll_filename,
    std::string_view external_name,
    std::span<const RValue> args,
    ExternalValueType return_type) {
    const std::string norm = normalize_dll_name(dll_filename);
    const ResolvedExport resolved = resolve_export(norm, external_name);
    return invoke_with_coercion(
        host, resolved, norm, external_name, {}, return_type, args, "gex_dll_call");
}

i32 ExtensionRegistry::define_external(
    std::string_view dll_path,
    std::string_view func_name,
    i32 call_type,
    ExternalValueType return_type,
    std::vector<ExternalValueType> arg_types) {
    ExternalBinding b{};
    b.id = static_cast<i32>(bindings_.size());
    b.raw_dll_path = std::string(dll_path);
    b.normalized_dll = normalize_dll_name(dll_path);
    b.func_name = std::string(func_name);
    b.call_type = call_type;
    b.return_type = return_type;
    b.arg_types = std::move(arg_types);
    b.resolved = resolve_export(b.normalized_dll, b.func_name);
    b.active = true;

    if (Logger::trace_flags().dll_calls) {
        GM_LOG_TRACE("ext", "external_define id=%d dll='%s' (%s) fn='%s' argc=%zu resolved=%s",
                     b.id, b.raw_dll_path.c_str(), b.normalized_dll.c_str(),
                     b.func_name.c_str(), b.arg_types.size(),
                     b.resolved.has_function() ? "yes" : (b.resolved.has_extension() ? "dll-only" : "no"));
    }

    const i32 id = b.id;
    bindings_.push_back(std::move(b));
    return id;
}

RValue ExtensionRegistry::call_external(
    IExtensionHost& host,
    i32 binding_id,
    std::span<const RValue> args) {
    if (binding_id < 0 || static_cast<usize>(binding_id) >= bindings_.size()) {
        return RValue(0.0);
    }
    const auto& b = bindings_[static_cast<usize>(binding_id)];
    if (!b.active) {
        return RValue(0.0);
    }
    return invoke_with_coercion(
        host, b.resolved, b.normalized_dll, b.func_name,
        b.arg_types, b.return_type, args, "external_call");
}

void ExtensionRegistry::free_external(IExtensionHost& host, std::string_view dll_path) {
    const std::string norm = normalize_dll_name(dll_path);
    if (Logger::trace_flags().dll_calls) {
        GM_LOG_TRACE("ext", "external_free '%.*s' (%s)",
                     static_cast<int>(dll_path.size()), dll_path.data(), norm.c_str());
    }
    if (RegisteredExtension* ext = find_extension(norm)) {
        ext->ext->on_unload(host);
    }
    for (auto& b : bindings_) {
        if (b.normalized_dll == norm) {
            b.active = false;
            b.resolved = {};
        }
    }
}

i32 ExtensionRegistry::get_function_address(std::string_view name) {
    if (name == "action_set_relative") return -1; // synthesized by the D&D compiler
    const auto* func = BuiltinsCatalog::find_function(callback_name(name));
    return func ? kCallbackBase + static_cast<i32>(func->id) : -1;
}

const BuiltinFuncInfo* ExtensionRegistry::callback_by_address(i32 address) {
    if (address < kCallbackBase) return nullptr;
    const auto* func = BuiltinsCatalog::function_by_id(static_cast<u32>(address - kCallbackBase));
    return func && get_function_address(func->name) == address ? func : nullptr;
}

bool ExtensionRegistry::has_extension(std::string_view dll_path) const noexcept {
    return find_extension(normalize_dll_name(dll_path)) != nullptr;
}

bool ExtensionRegistry::has_function(std::string_view dll_path, std::string_view func_name) {
    return resolve_export(normalize_dll_name(dll_path), func_name).has_function();
}

usize ExtensionRegistry::total_export_count() const noexcept {
    usize total = 0;
    for (const auto& entry : extensions_) {
        if (entry) total += entry->exports.size();
    }
    return total;
}

} // namespace gm
