#include "gml/symbol_table.h"

#include "base/log.h"

#include <cctype>
#include <cstdlib>

namespace gm {

namespace {

std::string_view trim_sv(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool try_parse_plain_number(std::string_view sv, f64& out) noexcept {
    sv = trim_sv(sv);
    if (sv.empty()) {
        return false;
    }

    // Check GM8 $hex literal (with optional leading '-')
    bool neg = false;
    std::string_view work = sv;
    if (work.front() == '-') {
        neg = true;
        work.remove_prefix(1);
    } else if (work.front() == '+') {
        work.remove_prefix(1);
    }
    if (!work.empty() && work.front() == '$') {
        work.remove_prefix(1);
        if (work.empty()) return false;
        u64 val = 0;
        for (char c : work) {
            if (c >= '0' && c <= '9') {
                val = (val << 4u) | static_cast<u64>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                val = (val << 4u) | static_cast<u64>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                val = (val << 4u) | static_cast<u64>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        out = neg ? -static_cast<f64>(val) : static_cast<f64>(val);
        return true;
    }

    std::string tmp(sv);
    char* end_ptr = nullptr;
    const f64 parsed = std::strtod(tmp.c_str(), &end_ptr);
    if (end_ptr == tmp.c_str() || *end_ptr != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

} // namespace

std::unordered_map<std::string, std::string> SymbolTable::split_gex_gml_defines(std::string_view gml_text) {
    std::unordered_map<std::string, std::string> result;
    std::string current_name;
    std::string current_body;
    bool in_define = false;

    usize pos = 0;
    while (pos <= gml_text.size()) {
        usize line_end = gml_text.find('\n', pos);
        if (line_end == std::string_view::npos) {
            line_end = gml_text.size();
        }
        std::string_view line = gml_text.substr(pos, line_end - pos);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }

        const std::string_view trimmed = trim_sv(line);
        if (trimmed.starts_with("#define")) {
            const bool valid_sep = (trimmed.size() == 7) ||
                                   std::isspace(static_cast<unsigned char>(trimmed[7]));
            if (valid_sep) {
                if (in_define && !current_name.empty()) {
                    result[current_name] = std::move(current_body);
                    current_body.clear();
                }
                std::string_view name_part = trim_sv(trimmed.substr(7));
                // Strip optional trailing comment after #define name
                const usize slash_pos = name_part.find("//");
                if (slash_pos != std::string_view::npos) {
                    name_part = trim_sv(name_part.substr(0, slash_pos));
                }
                current_name = std::string(name_part);
                current_body.clear();
                in_define = true;
                if (line_end == gml_text.size()) break;
                pos = line_end + 1;
                continue;
            }
        }

        if (in_define) {
            current_body.append(line);
            current_body.push_back('\n');
        }

        if (line_end == gml_text.size()) {
            break;
        }
        pos = line_end + 1;
    }

    if (in_define && !current_name.empty()) {
        result[current_name] = std::move(current_body);
    }

    return result;
}

void SymbolTable::register_resource_constant(std::string_view name, i32 index) {
    if (name.empty()) return;
    // Earlier resource categories win on collision (Objects -> Sprites -> ... -> Triggers).
    constants_.try_emplace(std::string(name), ConstantBinding{false, 0u, static_cast<f64>(index), {}});
}

void SymbolTable::register_user_constant(std::string_view name, std::string_view raw_value) {
    const std::string_view trimmed_name = trim_sv(name);
    if (trimmed_name.empty()) return;
    const std::string key(trimmed_name);
    if (constants_.contains(key)) {
        return; // Resource names take precedence over user constants.
    }

    f64 num = 0.0;
    if (try_parse_plain_number(raw_value, num)) {
        constants_.emplace(key, ConstantBinding{false, 0u, num, {}});
    } else {
        const u32 slot = static_cast<u32>(expr_constants_.size());
        std::string src(trim_sv(raw_value));
        expr_constants_.push_back(ExpressionConstantEntry{slot, key, src});
        constants_.emplace(key, ConstantBinding{true, slot, 0.0, std::move(src)});
    }
}

void SymbolTable::populate_from_game(const GameData& game) {
    callables_.clear();
    constants_.clear();
    ext_gml_funcs_.clear();
    ext_dll_funcs_.clear();
    expr_constants_.clear();

    // 1. Register built-in functions first (lowest callable precedence; scripts & extensions can shadow).
    for (const auto& fn : BuiltinsCatalog::functions()) {
        callables_[std::string(fn.name)] = CallableTarget{
            CallableKind::Builtin,
            fn.id,
            fn.arg_count,
            std::string(fn.name),
        };
    }

    // 2. Register game scripts (override built-ins if same name).
    for (usize i = 0; i < game.scripts.size(); ++i) {
        const auto& sc = game.scripts[i];
        if (!sc.exists || sc.name.empty()) continue;
        callables_[sc.name] = CallableTarget{
            CallableKind::Script,
            static_cast<u32>(i),
            -1, // Scripts accept 0..16 arguments dynamically
            sc.name,
        };
    }

    // 3. Register embedded extension functions & constants (highest callable precedence,
    // allowing gm82joy/gm82snd/gm82core to cleanly shadow built-in joystick_*/sound_*/game_restart).
    for (usize ei = 0; ei < game.extensions.size(); ++ei) {
        const auto& ext = game.extensions[ei];
        for (usize ii = 0; ii < ext.includes.size(); ++ii) {
            const auto& inc = ext.includes[ii];

            std::unordered_map<std::string, std::string> gml_defines;
            if (inc.kind == 2 && !inc.payload.empty()) {
                const std::string_view text(
                    reinterpret_cast<const char*>(inc.payload.data()),
                    inc.payload.size());
                gml_defines = split_gex_gml_defines(text);
            }

            for (const auto& fn : inc.functions) {
                const std::string ext_sym = fn.external_name.empty() ? fn.name : fn.external_name;
                if (inc.kind == 2) {
                    const u32 gid = static_cast<u32>(ext_gml_funcs_.size());
                    std::string body;
                    if (auto it = gml_defines.find(ext_sym); it != gml_defines.end()) {
                        body = it->second;
                    } else if (auto it2 = gml_defines.find(fn.name); it2 != gml_defines.end()) {
                        body = it2->second;
                    }
                    ext_gml_funcs_.push_back(ExtensionGmlFunction{
                        gid,
                        static_cast<u32>(ei),
                        static_cast<u32>(ii),
                        fn.name,
                        ext_sym,
                        std::move(body),
                    });
                    if (!fn.name.empty()) {
                        callables_[fn.name] = CallableTarget{
                            CallableKind::ExtensionGml,
                            gid,
                            -1, // GEX GML scripts accept variable args (e.g. buffer_create([filename]))
                            fn.name,
                        };
                    }
                } else if (inc.kind == 1) {
                    const u32 did = static_cast<u32>(ext_dll_funcs_.size());
                    ext_dll_funcs_.push_back(ExtensionDllFunction{
                        did,
                        static_cast<u32>(ei),
                        static_cast<u32>(ii),
                        inc.filename,
                        fn.name,
                        ext_sym,
                        fn.call_type,
                        fn.arg_count,
                        fn.arg_types,
                        fn.return_type,
                    });
                    if (!fn.name.empty()) {
                        callables_[fn.name] = CallableTarget{
                            CallableKind::ExtensionDll,
                            did,
                            fn.arg_count,
                            fn.name,
                        };
                    }
                }
            }
        }
    }

    // 4. Register resource name constants in exact GM8 precedence order:
    //    Objects -> Sprites -> Sounds -> Backgrounds -> Paths -> Fonts -> Timelines -> Scripts -> Rooms -> Triggers (1-based)
    for (usize i = 0; i < game.objects.size(); ++i) {
        if (game.objects[i].exists) register_resource_constant(game.objects[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.sprites.size(); ++i) {
        if (game.sprites[i].exists) register_resource_constant(game.sprites[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.sounds.size(); ++i) {
        if (game.sounds[i].exists) register_resource_constant(game.sounds[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.backgrounds.size(); ++i) {
        if (game.backgrounds[i].exists) register_resource_constant(game.backgrounds[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.paths.size(); ++i) {
        if (game.paths[i].exists) register_resource_constant(game.paths[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.fonts.size(); ++i) {
        if (game.fonts[i].exists) register_resource_constant(game.fonts[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.timelines.size(); ++i) {
        if (game.timelines[i].exists) register_resource_constant(game.timelines[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.scripts.size(); ++i) {
        if (game.scripts[i].exists) register_resource_constant(game.scripts[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.rooms.size(); ++i) {
        if (game.rooms[i].exists) register_resource_constant(game.rooms[i].name, static_cast<i32>(i));
    }
    for (usize i = 0; i < game.triggers.size(); ++i) {
        if (game.triggers[i].exists) {
            // Triggers are 1-based constants in GM8!
            register_resource_constant(game.triggers[i].const_name, static_cast<i32>(i + 1));
        }
    }

    // 5. Register user-defined & extension constants.
    for (const auto& c : game.constants) {
        register_user_constant(c.name, c.value);
    }
    for (const auto& ext : game.extensions) {
        for (const auto& inc : ext.includes) {
            for (const auto& c : inc.constants) {
                register_user_constant(c.name, c.value);
            }
        }
    }

    // 6. Register 331 built-in constants (lowest constant precedence).
    for (const auto& c : BuiltinsCatalog::constants()) {
        constants_.try_emplace(std::string(c.name), ConstantBinding{false, 0u, c.value, {}});
    }
}

const CallableTarget* SymbolTable::find_callable(std::string_view name) const {
    const auto it = callables_.find(std::string(name));
    return (it != callables_.end()) ? &it->second : nullptr;
}

const ConstantBinding* SymbolTable::find_constant(std::string_view name) const {
    const auto it = constants_.find(std::string(name));
    return (it != constants_.end()) ? &it->second : nullptr;
}

const BuiltinVarInfo* SymbolTable::find_builtin_var(std::string_view name) const {
    return BuiltinsCatalog::find_variable(name);
}

u32 SymbolTable::intern_user_var(std::string_view name) {
    const std::string key(name);
    if (const auto it = user_var_ids_.find(key); it != user_var_ids_.end()) {
        return it->second;
    }
    const u32 id = kUserVarIdBase + static_cast<u32>(user_var_names_.size());
    user_var_names_.push_back(key);
    user_var_ids_.emplace(key, id);
    return id;
}

std::optional<u32> SymbolTable::find_user_var(std::string_view name) const {
    const auto it = user_var_ids_.find(std::string(name));
    if (it == user_var_ids_.end()) return std::nullopt;
    return it->second;
}

std::string_view SymbolTable::user_var_name(u32 var_id) const noexcept {
    if (var_id < kUserVarIdBase) {
        if (const auto* bv = BuiltinsCatalog::variable_by_id(var_id)) {
            return bv->name;
        }
        return {};
    }
    const usize idx = static_cast<usize>(var_id - kUserVarIdBase);
    return (idx < user_var_names_.size()) ? std::string_view(user_var_names_[idx]) : std::string_view{};
}

} // namespace gm
