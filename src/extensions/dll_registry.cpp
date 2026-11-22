#include "extensions/dll_registry.h"

#include "base/encoding.h"
#include "base/log.h"
#include "extensions/buffer_store.h"
#include "extensions/shim_cleanmem.h"
#include "extensions/shim_gm82buf.h"
#include "extensions/shim_gm82core.h"
#include "extensions/shim_gm82drag.h"
#include "extensions/shim_gm82dx.h"
#include "extensions/shim_gm82joy.h"
#include "extensions/shim_gm82test.h"
#include "extensions/shim_gmfmod.h"
#include "extensions/shim_maizemusic.h"
#include "extensions/shim_supersound.h"

#include <cstdlib>

namespace gm {

DllRegistry::DllRegistry() = default;
DllRegistry::~DllRegistry() = default;

void DllRegistry::register_shim(std::unique_ptr<IDllShim> shim) {
    if (shim) {
        shims_.push_back(std::move(shim));
    }
}

void DllRegistry::register_builtin_shims() {
    shims_.clear();
    auto buffers = std::make_shared<GmBufferStore>();
    register_shim(create_gmfmod_shim());
    register_shim(create_supersound_shim());
    register_shim(create_maizemusic_shim());
    register_shim(create_cleanmem_shim());
    register_shim(create_gm82core_shim(buffers));
    register_shim(create_gm82buf_shim(buffers));
    register_shim(create_gm82joy_shim());
    register_shim(create_gm82drag_shim());
    register_shim(create_gm82test_shim());
    register_shim(create_gm82dx_shim(buffers));
}

void DllRegistry::reset(IExtensionHost* host) {
    if (host) {
        for (auto& shim : shims_) {
            shim->on_unload(*host);
        }
    }
    bindings_.clear();
    next_binding_id_ = 0;
    warned_missing_.clear();
}

std::string DllRegistry::normalize_dll_name(std::string_view dll_path) {
    usize slash = dll_path.find_last_of("\\/");
    std::string_view base = (slash != std::string_view::npos) ? dll_path.substr(slash + 1) : dll_path;
    std::string lo = ascii_to_lower(base);
    if (lo.ends_with(".dll") || lo.ends_with(".so") || lo.ends_with(".dylib")) {
        const usize dot = lo.rfind('.');
        if (dot != std::string::npos) {
            lo.resize(dot);
        }
    }
    return lo;
}

IDllShim* DllRegistry::find_shim(std::string_view normalized_dll) const noexcept {
    for (const auto& shim : shims_) {
        if (shim->matches_dll(normalized_dll)) {
            return shim.get();
        }
    }
    return nullptr;
}

RValue DllRegistry::coerce_result(RValue val, ExternalValueType expected) {
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

i32 DllRegistry::define_external(
    std::string_view dll_path,
    std::string_view func_name,
    i32 call_type,
    ExternalValueType return_type,
    std::vector<ExternalValueType> arg_types) {
    ExternalBinding b{};
    b.id = next_binding_id_++;
    b.raw_dll_path = std::string(dll_path);
    b.normalized_dll = normalize_dll_name(dll_path);
    b.func_name = std::string(func_name);
    b.call_type = call_type;
    b.return_type = return_type;
    b.arg_types = std::move(arg_types);
    b.active = true;

    const bool matched = (find_shim(b.normalized_dll) != nullptr);
    if (Logger::trace_flags().dll_calls) {
        GM_LOG_TRACE("ext", "external_define id=%d dll='%s' (%s) fn='%s' argc=%zu shimmed=%s",
                     b.id, b.raw_dll_path.c_str(), b.normalized_dll.c_str(),
                     b.func_name.c_str(), b.arg_types.size(), matched ? "yes" : "no");
    }

    const i32 id = b.id;
    bindings_[id] = std::move(b);
    return id;
}

RValue DllRegistry::call_external(
    IExtensionHost& host,
    i32 binding_id,
    std::span<const RValue> args) {
    const auto it = bindings_.find(binding_id);
    if (it == bindings_.end() || !it->second.active) {
        return RValue(0.0);
    }
    const auto& b = it->second;

    // Coerce incoming arguments according to declared parameter types when provided.
    std::vector<RValue> coerced;
    coerced.reserve(args.size());
    for (usize i = 0; i < args.size(); ++i) {
        if (i < b.arg_types.size()) {
            if (b.arg_types[i] == ExternalValueType::String && !args[i].is_string()) {
                coerced.emplace_back(format_gm_real(args[i].as_real()));
            } else if (b.arg_types[i] == ExternalValueType::Real && args[i].is_string()) {
                coerced.emplace_back(std::strtod(args[i].as_string().c_str(), nullptr));
            } else {
                coerced.push_back(args[i]);
            }
        } else {
            coerced.push_back(args[i]);
        }
    }

    IDllShim* shim = find_shim(b.normalized_dll);
    if (shim) {
        if (Logger::trace_flags().dll_calls) {
            GM_LOG_TRACE("ext", "external_call [%s] %s (argc=%zu)",
                         b.normalized_dll.c_str(), b.func_name.c_str(), coerced.size());
        }
        RValue res = shim->invoke(host, b.func_name, coerced, b.return_type);
        return coerce_result(std::move(res), b.return_type);
    }

    const std::string key = b.normalized_dll + "::" + b.func_name;
    if (warned_missing_.insert(key).second) {
        GM_LOG_WARN("ext", "No native shim for external_call %s::%s (returning default)",
                    b.normalized_dll.c_str(), b.func_name.c_str());
    }
    return (b.return_type == ExternalValueType::String) ? RValue(std::string{}) : RValue(0.0);
}

void DllRegistry::free_external(IExtensionHost& host, std::string_view dll_path) {
    const std::string norm = normalize_dll_name(dll_path);
    if (Logger::trace_flags().dll_calls) {
        GM_LOG_TRACE("ext", "external_free '%s' (%s)", std::string(dll_path).c_str(), norm.c_str());
    }
    if (IDllShim* shim = find_shim(norm)) {
        shim->on_unload(host);
    }
    for (auto& [id, b] : bindings_) {
        if (b.normalized_dll == norm) {
            b.active = false;
        }
    }
}

namespace {

// These opaque values can be passed through GML reals on every target. Only
// shims interpret them, through IExtensionHost::ext_call_function.
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

i32 DllRegistry::get_function_address(std::string_view name) {
    if (name == "action_set_relative") return -1; // synthesized by the D&D compiler
    const auto* func = BuiltinsCatalog::find_function(callback_name(name));
    return func ? kCallbackBase + static_cast<i32>(func->id) : -1;
}

const BuiltinFuncInfo* DllRegistry::callback_by_address(i32 address) {
    if (address < kCallbackBase) return nullptr;
    const auto* func = BuiltinsCatalog::function_by_id(static_cast<u32>(address - kCallbackBase));
    return func && get_function_address(func->name) == address ? func : nullptr;
}

RValue DllRegistry::call_gex_dll(
    IExtensionHost& host,
    std::string_view dll_filename,
    std::string_view external_name,
    std::span<const RValue> args,
    ExternalValueType return_type) {
    const std::string norm = normalize_dll_name(dll_filename);
    IDllShim* shim = find_shim(norm);
    if (shim) {
        if (Logger::trace_flags().dll_calls) {
            GM_LOG_TRACE("ext", "gex_dll_call [%s] %s (argc=%zu)",
                         norm.c_str(), std::string(external_name).c_str(), args.size());
        }
        return coerce_result(shim->invoke(host, external_name, args, return_type), return_type);
    }

    const std::string key = norm + "::" + std::string(external_name);
    if (warned_missing_.insert(key).second) {
        GM_LOG_WARN("ext", "No native shim for GEX DLL call %s::%s",
                    norm.c_str(), std::string(external_name).c_str());
    }
    return (return_type == ExternalValueType::String) ? RValue(std::string{}) : RValue(0.0);
}

} // namespace gm
