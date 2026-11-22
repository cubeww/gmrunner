#pragma once

#include "base/types.h"
#include "graphics/dx9_shader_transpiler.h"
#include "graphics/gl_graphics_device.h"

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

struct Dx9DrawContext {
    const f32* world_matrix = nullptr;
    const f32* view_matrix = nullptr;
    const f32* proj_matrix = nullptr;
    const f32* model_tint = nullptr; // vec4 RGBA
    bool rendering_to_fbo = false;
    ViewportRect viewport{};
    bool tex_blending = true;
    i32 texcoord_mode = 0;
    bool alpha_test_enabled = false;
    f32 alpha_test_ref = 0.0f;
    i32 alpha_test_func = 5;
    bool global_linear_filter = false;
    bool global_tex_repeat = false;
    bool flat_shading = false;
};

struct Dx9SamplerState {
    TextureHandle texture{};
    bool has_explicit_tex = false;
    bool linear_filter = false;
    bool has_explicit_filter = false;
    i32 wrap_u = 3; // D3DTADDRESS_CLAMP
    i32 wrap_v = 3;
    u32 border_bgr = 0;
    bool has_explicit_wrap = false;
};

struct Dx9VertexElement {
    u16 stream = 0;
    u16 offset = 0;
    u8 type = 0;       // D3DDECLTYPE
    u8 usage = 0;      // D3DDECLUSAGE
    u8 usage_index = 0;
};

class Dx9Pipeline : public NonCopyable {
public:
    Dx9Pipeline() = default;
    ~Dx9Pipeline();

    void init(GlGraphicsDevice& device);
    void shutdown() noexcept;

    // Shader lifecycle (returns > 0 handle on success, -1/-2/-3 on error matching gm82dx9).
    [[nodiscard]] i32 create_vertex_shader(std::span<const u8> bytes);
    [[nodiscard]] i32 create_pixel_shader(std::span<const u8> bytes);
    bool destroy_vertex_shader(i32 id);
    bool destroy_pixel_shader(i32 id);

    // Shader binding (returns true on success, false if unknown ID).
    bool set_vertex_shader(i32 id);
    bool set_pixel_shader(i32 id);
    void reset_vertex_shader() noexcept { active_vs_ = 0; }
    void reset_pixel_shader() noexcept { active_ps_ = 0; }
    [[nodiscard]] bool has_active_shader() const noexcept { return active_vs_ > 0 || active_ps_ > 0; }
    [[nodiscard]] i32 active_vertex_shader() const noexcept { return active_vs_; }
    [[nodiscard]] i32 active_pixel_shader() const noexcept { return active_ps_; }
    void set_active_shaders_raw(i32 vs_id, i32 ps_id) noexcept { active_vs_ = vs_id; active_ps_ = ps_id; }

    // CTAB reflection queries (returns -4 / noone if not found).
    [[nodiscard]] i32 get_vconst(std::string_view name) const noexcept;
    [[nodiscard]] i32 get_vconst_size(std::string_view name) const noexcept;
    [[nodiscard]] i32 get_pconst(std::string_view name) const noexcept;
    [[nodiscard]] i32 get_pconst_size(std::string_view name) const noexcept;

    // Shader constant register updates.
    void set_vs_const_f(u32 reg, const f32* data, u32 vec4_count);
    void set_ps_const_f(u32 reg, const f32* data, u32 vec4_count);
    void set_vs_const_i(u32 reg, const i32* data, u32 ivec4_count);
    void set_ps_const_i(u32 reg, const i32* data, u32 ivec4_count);
    void set_vs_const_b(u32 reg, const bool* data, u32 count);
    void set_ps_const_b(u32 reg, const bool* data, u32 count);

    // Multi-stage sampler state (PS stages 0..7, VS stages 0..3).
    void set_ps_sampler_tex(i32 stage, TextureHandle tex);
    void set_vs_sampler_tex(i32 stage, TextureHandle tex);
    void set_ps_sampler_filter(i32 stage, bool linear);
    void set_vs_sampler_filter(i32 stage, bool linear);
    void set_ps_sampler_wrap(i32 stage, i32 wrap_u, i32 wrap_v, u32 border_bgr);
    void set_vs_sampler_wrap(i32 stage, i32 wrap_u, i32 wrap_v, u32 border_bgr);
    void clear_stage0_overrides() noexcept;
    void clear_stage0_filter_override() noexcept { ps_samplers_[0].has_explicit_filter = false; }
    void clear_stage0_wrap_override() noexcept { ps_samplers_[0].has_explicit_wrap = false; }
    [[nodiscard]] const Dx9SamplerState& ps_sampler(i32 stage) const noexcept {
        return ps_samplers_[static_cast<usize>(std::clamp(stage, 0, 7))];
    }

    // Binds the active linked custom shader program, uploads uniforms & samplers.
    // Returns false if linking failed (caller may fall back to fixed-function shader).
    bool bind_and_upload(const Dx9DrawContext& ctx, TextureHandle stage0_draw_tex);

    // Custom Vertex Formats, Vertex Buffers, Index Buffers & Instancing
    void vertex_format_begin();
    void vertex_format_add_custom(i32 decl_type, i32 usage, i32 slot);
    [[nodiscard]] i32 vertex_format_end();
    [[nodiscard]] i32 vertex_format_get_size(i32 vformat_id, i32 slot) const noexcept;
    bool vertex_format_delete(i32 vformat_id);

    [[nodiscard]] i32 create_vertex_buffer(std::span<const u8> bytes, u32 stride);
    [[nodiscard]] i32 vertex_buffer_get_size(i32 vbuf_id) const noexcept;
    bool vertex_bind_buffer(i32 vbuf_id, i32 slot);
    bool vertex_delete_buffer(i32 vbuf_id);

    [[nodiscard]] i32 create_index_buffer(std::span<const u8> bytes, i32 d3d_format);
    [[nodiscard]] i32 index_buffer_get_size(i32 ibuf_id) const noexcept;
    [[nodiscard]] i32 index_buffer_get_format(i32 ibuf_id) const noexcept;
    bool index_set_buffer(i32 ibuf_id);
    bool index_delete_buffer(i32 ibuf_id);

    void set_instance_count(u32 count) noexcept { instance_count_ = count; instancing_enabled_ = (count > 0); }
    void set_instances_per_vertex(i32 slot, u32 rate) noexcept {
        if (slot >= 0 && slot < 16) stream_instance_rate_[static_cast<usize>(slot)] = rate;
    }
    void reset_instancing() noexcept {
        instancing_enabled_ = false;
        instance_count_ = 0;
        stream_instance_rate_.fill(0);
    }

    // Draws a custom vertex buffer using the specified vertex format, primitive type, and texture.
    // If no custom shader is bound, uses the provided fixed-function fallback binder callback.
    bool draw_vertex_buffer(
        i32 vbuf_id,
        i32 vformat_id,
        i32 d3d_prim_type,
        TextureHandle tex,
        i32 vert_count,
        bool indexed,
        const Dx9DrawContext& ctx,
        ShaderHandle ff_shader_fallback = {});

private:
    struct LinkedProgram {
        ShaderHandle shader;
        bool failed = false;

        // Cached uniform locations
        i32 loc_yflip = -1;
        i32 loc_viewport = -1;
        i32 loc_vc = -1;
        i32 loc_vi = -1;
        i32 loc_vb = -1;
        i32 loc_pc = -1;
        i32 loc_pi = -1;
        i32 loc_pb = -1;
        std::array<i32, 8> loc_ps_samp{-1,-1,-1,-1,-1,-1,-1,-1};
        std::array<i32, 4> loc_vs_samp{-1,-1,-1,-1};

        // FF-compat uniforms (used when vs_id == 0 or ps_id == 0)
        i32 loc_world = -1;
        i32 loc_view = -1;
        i32 loc_proj = -1;
        i32 loc_model_tint = -1;
        i32 loc_tex_blending = -1;
        i32 loc_texcoord_mode = -1;
        i32 loc_alpha_test = -1;
        i32 loc_alpha_ref = -1;
        i32 loc_alpha_func = -1;

        u32 vs_max_f = 0;
        u32 vs_max_i = 0;
        u32 vs_max_b = 0;
        u32 ps_max_f = 0;
        u32 ps_max_i = 0;
        u32 ps_max_b = 0;
        u8 ps_sampler_mask = 0;
        u8 vs_sampler_mask = 0;
    };

    struct VertexFormat {
        std::vector<Dx9VertexElement> elements;
        std::array<i32, 16> slot_sizes{};
    };

    struct GpuVertexBuffer {
        u32 vbo = 0;
        u32 stride = 0;
        std::vector<u8> cpu_copy; // Retained for GLES3 D3DCOLOR swizzle fallback if needed
        u32 swizzled_vbo = 0;
        u32 size_bytes = 0;
    };

    struct GpuIndexBuffer {
        u32 ibo = 0;
        i32 d3d_format = 101; // D3DFMT_INDEX16
        u32 size_bytes = 0;
        std::vector<u8> cpu_copy; // GLES fixed-function flat shading needs the original index sequence.
    };

    static u64 program_key(i32 vs_id, i32 ps_id) noexcept {
        return (static_cast<u64>(static_cast<u32>(vs_id)) << 32u) | static_cast<u64>(static_cast<u32>(ps_id));
    }

    LinkedProgram* get_or_link_program(i32 vs_id, i32 ps_id);
    void invalidate_programs_using(i32 shader_id, bool is_vs);
    void apply_ctab_defaults(const Dx9ParsedShader& sh);

    GlGraphicsDevice* device_ = nullptr;
    u32 custom_vao_ = 0;

    i32 next_vs_id_ = 0;
    i32 next_ps_id_ = 0;
    i32 active_vs_ = 0;
    i32 active_ps_ = 0;
    std::unordered_map<i32, Dx9ParsedShader> vertex_shaders_;
    std::unordered_map<i32, Dx9ParsedShader> pixel_shaders_;
    std::unordered_map<u64, LinkedProgram> program_cache_;

    // D3D9 device constant register banks
    std::array<f32, 256 * 4> vs_const_f_{};
    std::array<i32, 16 * 4>  vs_const_i_{};
    std::array<i32, 16>      vs_const_b_{};
    std::array<f32, 224 * 4> ps_const_f_{};
    std::array<i32, 16 * 4>  ps_const_i_{};
    std::array<i32, 16>      ps_const_b_{};

    std::array<Dx9SamplerState, 8> ps_samplers_{};
    std::array<Dx9SamplerState, 4> vs_samplers_{};

    // Custom vertex formats & GPU buffers
    std::vector<Dx9VertexElement> vfmt_building_;
    std::array<i32, 16> vfmt_building_offsets_{};
    std::array<u8, 16> vfmt_building_usage_idx_{};
    i32 next_vfmt_id_ = 0;
    i32 next_vbuf_id_ = 0;
    i32 next_ibuf_id_ = 0;
    std::unordered_map<i32, VertexFormat> vformats_;
    std::unordered_map<i32, GpuVertexBuffer> vbuffers_;
    std::unordered_map<i32, GpuIndexBuffer> ibuffers_;
    std::array<i32, 16> bound_streams_{};
    i32 bound_ibuf_ = -1;
    bool instancing_enabled_ = false;
    u32 instance_count_ = 0;
    std::array<u32, 16> stream_instance_rate_{};
};

} // namespace gm
