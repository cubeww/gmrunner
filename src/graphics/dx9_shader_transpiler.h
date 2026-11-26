#pragma once

#include "base/types.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gm {

enum class Dx9ShaderStage : u8 {
    Vertex = 0,
    Pixel = 1,
};

// D3DXREGISTER_SET values in CTAB
enum class Dx9RegisterSet : u16 {
    Bool = 0,
    Int4 = 1,
    Float4 = 2,
    Sampler = 3,
};

struct Dx9ConstantInfo {
    std::string name;
    Dx9RegisterSet register_set = Dx9RegisterSet::Float4;
    u16 register_index = 0;
    u16 register_count = 0;
    u16 type_class = 0;
    u16 base_type = 0;
    u16 rows = 1;
    u16 cols = 1;
    bool has_default = false;
    std::vector<f32> default_floats; // 4 * register_count for Float4
    std::vector<i32> default_ints;   // 4 * register_count for Int4
    std::vector<u8> default_bools;   // register_count for Bool
};

struct Dx9DclSemantic {
    u8 usage = 0;       // D3DDECLUSAGE (0=POS, 3=NORMAL, 5=TEXCOORD, 10=COLOR, ...)
    u8 usage_index = 0; // 0..15
    u16 reg_num = 0;    // vN or oN
    u8 write_mask = 0xF;
    i32 attrib_location = -1;
};

struct Dx9ParsedShader {
    Dx9ShaderStage stage = Dx9ShaderStage::Pixel;
    u8 major_version = 3;
    u8 minor_version = 0;

    // Raw bytecode copy (kept so PS can be emitted with exact VS varying linkage).
    std::vector<u32> words;

    // Reflected CTAB uniform table (name -> info).
    std::vector<Dx9ConstantInfo> constants;
    std::unordered_map<std::string, usize> constant_by_name;

    // Reflected register extents (for compact GLSL uniform arrays).
    u32 max_const_f = 0; // 0 if unused, else highest_reg + 1
    u32 max_const_i = 0;
    u32 max_const_b = 0;
    bool uses_const_f = false;
    bool uses_const_i = false;
    bool uses_const_b = false;
    bool uses_rel_const_f = false;
    u8 sampler_mask = 0; // bit N = uses sampler sN (0..7 for PS, 0..3 for VS)
    u8 mrt_mask = 1;     // bit N = writes oCN (0..3 for PS)
    bool writes_depth = false;

    // Semantic interfaces for VS/PS linkage.
    std::vector<Dx9DclSemantic> inputs;
    std::vector<Dx9DclSemantic> outputs;
    std::unordered_set<std::string> output_semantics; // Canonical names like "TEXCOORD0", "COLOR0"

    [[nodiscard]] const Dx9ConstantInfo* find_constant(std::string_view name) const noexcept {
        const auto it = constant_by_name.find(std::string(name));
        return it != constant_by_name.end() ? &constants[it->second] : nullptr;
    }
};

// Maps a D3DDECLUSAGE + usage_index to canonical GLSL vertex attribute location (0..15).
// Matches Renderer's Vertex3D layout: 0=POSITION0, 1=NORMAL0, 2=TEXCOORD0, 3=COLOR0.
[[nodiscard]] i32 dx9_semantic_to_attrib_location(u8 usage, u8 usage_index) noexcept;

// Returns canonical varying name (e.g. "TEXCOORD0", "COLOR0", "NORMAL0").
[[nodiscard]] std::string dx9_semantic_name(u8 usage, u8 usage_index);

// gm82dx9 applies this container check before handing file/buffer bytecode to D3D9.
[[nodiscard]] bool dx9_shader_bounds_check(std::span<const u8> bytes) noexcept;

// Validates and parses D3D9 SM1/SM2/SM3 binary bytecode.
// Returns error code: 0 = ok, -1 = invalid instruction, -2 = short stream, -3 = wrong stage or corrupt bytecode.
[[nodiscard]] i32 parse_d3d9_shader(
    std::span<const u8> bytes,
    Dx9ShaderStage expected_stage,
    Dx9ParsedShader& out_shader,
    std::string* out_error = nullptr);

// Emits GLSL 330 core (desktop) or GLSL 300 es (GLES3/WebGL2) source for a parsed D3D9 shader.
// For Pixel shaders, `vs_available_semantics` lists the canonical varying names exported by the
// paired Vertex shader (any PS input not in that set is safely defaulted to vec4(0,0,0,1)).
[[nodiscard]] Result<std::string> transpile_d3d9_to_glsl(
    const Dx9ParsedShader& shader,
    bool gles3,
    const std::unordered_set<std::string>* vs_available_semantics = nullptr);

// Built-in compatibility shaders for pairing fixed-function VS with custom PS, or custom VS with FF PS.
[[nodiscard]] const char* ff_texcoord_glsl() noexcept;
[[nodiscard]] std::string build_ff_compat_vs_glsl(bool gles3);
[[nodiscard]] std::string build_ff_compat_ps_glsl(bool gles3);
[[nodiscard]] std::unordered_set<std::string> ff_compat_vs_output_semantics();

} // namespace gm
