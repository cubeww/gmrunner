#pragma once

#include "base/types.h"

#include <optional>
#include <span>
#include <string_view>

namespace gm {

struct BuiltinConstantInfo {
    std::string_view name;
    f64 value;
};

struct BuiltinVarInfo {
    u32 id;
    std::string_view name;
    bool is_global;
    bool is_writable;
    bool is_array;
};

struct BuiltinFuncInfo {
    u32 id;
    std::string_view name;
    i32 arg_count; // -1 = variadic
};

class BuiltinsCatalog {
public:
    [[nodiscard]] static std::span<const BuiltinConstantInfo> constants() noexcept;
    [[nodiscard]] static std::span<const BuiltinVarInfo> variables() noexcept;
    [[nodiscard]] static std::span<const BuiltinFuncInfo> functions() noexcept;

    [[nodiscard]] static std::optional<f64> find_constant(std::string_view name);
    [[nodiscard]] static const BuiltinVarInfo* find_variable(std::string_view name);
    [[nodiscard]] static const BuiltinVarInfo* variable_by_id(u32 id) noexcept;
    [[nodiscard]] static const BuiltinFuncInfo* find_function(std::string_view name);
    [[nodiscard]] static const BuiltinFuncInfo* function_by_id(u32 id) noexcept;
};

} // namespace gm
