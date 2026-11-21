#pragma once

#include "base/types.h"
#include "gml/builtins_catalog.h"
#include "loader/game_data.h"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

inline constexpr u32 kUserVarIdBase = 100000u;

enum class CallableKind : u8 {
    Builtin = 0,
    Script = 1,
    ExtensionGml = 2,
    ExtensionDll = 3,
};

struct CallableTarget {
    CallableKind kind = CallableKind::Builtin;
    u32 id = 0;
    i32 arg_count = -1; // -1 = variadic
    std::string name;
};

struct ExtensionGmlFunction {
    u32 id = 0;
    u32 extension_index = 0;
    u32 include_index = 0;
    std::string name;
    std::string external_name;
    std::string source;
};

struct ExtensionDllFunction {
    u32 id = 0;
    u32 extension_index = 0;
    u32 include_index = 0;
    std::string dll_filename;
    std::string name;
    std::string external_name;
    i32 call_type = 12; // 11=stdcall, 12=cdecl
    i32 arg_count = 0;
    std::vector<i32> arg_types;
    i32 return_type = 2;
};

struct ConstantBinding {
    bool is_expression = false;
    u32 expr_slot = 0; // Index into expression_constants() when is_expression == true
    f64 numeric_value = 0.0;
    std::string expr_source;
};

struct ExpressionConstantEntry {
    u32 slot = 0;
    std::string name;
    std::string expr_source;
};

// Central symbol & identifier table for a loaded GM 8.0 / 8.1 / 8.2 game.
// Implements exact GM8 resolution precedence:
//   Callables: Extensions -> Scripts -> Builtins
//   Identifiers: Resources -> User/Extension Constants -> Builtin Constants -> Variables
class SymbolTable {
public:
    SymbolTable() = default;

    void populate_from_game(const GameData& game);

    // Resolution queries
    [[nodiscard]] const CallableTarget* find_callable(std::string_view name) const;
    [[nodiscard]] const ConstantBinding* find_constant(std::string_view name) const;
    [[nodiscard]] const BuiltinVarInfo* find_builtin_var(std::string_view name) const;

    // Variable interning (IDs >= 100000, matching GM8 runner & gm82test reflection)
    [[nodiscard]] u32 intern_user_var(std::string_view name);
    [[nodiscard]] std::optional<u32> find_user_var(std::string_view name) const;
    [[nodiscard]] std::string_view user_var_name(u32 var_id) const noexcept;
    [[nodiscard]] usize user_var_count() const noexcept { return user_var_names_.size(); }

    // Extension callable tables populated during populate_from_game()
    [[nodiscard]] const std::vector<ExtensionGmlFunction>& extension_gml_functions() const noexcept {
        return ext_gml_funcs_;
    }
    [[nodiscard]] const std::vector<ExtensionDllFunction>& extension_dll_functions() const noexcept {
        return ext_dll_funcs_;
    }
    [[nodiscard]] const std::vector<ExpressionConstantEntry>& expression_constants() const noexcept {
        return expr_constants_;
    }

    // Splits a GM8 GEX kind=2 .gml file into map<define_name, gml_body>.
    [[nodiscard]] static std::unordered_map<std::string, std::string> split_gex_gml_defines(std::string_view gml_text);

private:
    void register_resource_constant(std::string_view name, i32 index);
    void register_user_constant(std::string_view name, std::string_view raw_value);

    std::unordered_map<std::string, CallableTarget> callables_;
    std::unordered_map<std::string, ConstantBinding> constants_;
    std::unordered_map<std::string, u32> user_var_ids_;
    std::vector<std::string> user_var_names_;

    std::vector<ExtensionGmlFunction> ext_gml_funcs_;
    std::vector<ExtensionDllFunction> ext_dll_funcs_;
    std::vector<ExpressionConstantEntry> expr_constants_;
};

} // namespace gm
