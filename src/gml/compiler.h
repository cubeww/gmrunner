#pragma once

#include "base/types.h"
#include "gml/bytecode.h"
#include "gml/symbol_table.h"
#include "loader/game_data.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

struct CompileErrorInfo {
    std::string unit_name;
    std::string message;
    std::string source_snippet;
};

struct CompileStats {
    usize scripts_compiled = 0;
    usize ext_gml_compiled = 0;
    usize expr_consts_compiled = 0;
    usize triggers_compiled = 0;
    usize object_events_compiled = 0;
    usize timeline_moments_compiled = 0;
    usize room_codes_compiled = 0;
    usize instance_codes_compiled = 0;
    usize library_codes_compiled = 0;
    usize total_instructions = 0;
    usize total_errors = 0;
};

struct CompiledProgram {
    SymbolTable symbols;
    CompileStats stats;
    std::vector<CompileErrorInfo> errors;

    std::vector<BytecodeChunk> const_init_chunks;
    std::vector<std::optional<BytecodeChunk>> script_chunks;
    std::vector<BytecodeChunk> ext_gml_chunks;
    std::vector<std::optional<BytecodeChunk>> trigger_chunks;
    std::vector<std::array<std::unordered_map<i32, BytecodeChunk>, 12>> object_event_chunks;
    std::vector<std::unordered_map<i32, BytecodeChunk>> timeline_chunks;
    std::vector<std::optional<BytecodeChunk>> room_creation_chunks;
    std::vector<std::unordered_map<i32, BytecodeChunk>> instance_creation_chunks; // room -> placed instance ID
    std::vector<BytecodeChunk> library_init_chunks;
};

class GmlCompiler {
public:
    // Compiles a GML statement sequence (script, event, room/instance creation code, execute_string).
    [[nodiscard]] static Result<BytecodeChunk> compile_chunk(
        std::string_view debug_name,
        std::string_view source,
        SymbolTable& symbols);

    // Compiles a single GML expression (trigger condition or expression constant) ending with Return.
    [[nodiscard]] static Result<BytecodeChunk> compile_expression(
        std::string_view debug_name,
        std::string_view expr_source,
        SymbolTable& symbols);

    // Compiles an entire loaded GameData into a CompiledProgram.
    [[nodiscard]] static CompiledProgram compile_game(const GameData& game);
};

void print_compile_summary(const CompiledProgram& prog, bool verbose = false);

} // namespace gm
