#include "graphics/dx9_pipeline.h"

#include "base/log.h"
#include "graphics/gl_headers.h"

#include <algorithm>
#include <cstring>

namespace gm {

namespace {

i32 decl_type_byte_size(i32 decl_type) noexcept {
    switch (decl_type) {
        case 0:  // FLOAT1
        case 4:  // D3DCOLOR
        case 5:  // UBYTE4
        case 6:  // SHORT2
        case 8:  // UBYTE4N
        case 9:  // SHORT2N
        case 11: // USHORT2N
        case 13: // UDEC3
        case 14: // DEC3N
        case 15: // FLOAT16_2
            return 4;
        case 1:  // FLOAT2
        case 7:  // SHORT4
        case 10: // SHORT4N
        case 12: // USHORT4N
        case 16: // FLOAT16_4
            return 8;
        case 2:  // FLOAT3
            return 12;
        case 3:  // FLOAT4
            return 16;
        default:
            return 0;
    }
}

} // namespace

Dx9Pipeline::~Dx9Pipeline() {
    shutdown();
}

void Dx9Pipeline::init(GlGraphicsDevice& device) {
    shutdown();
    device_ = &device;
    glGenVertexArrays(1, &custom_vao_);
    bound_streams_.fill(-1);
    stream_instance_rate_.fill(0);
    bound_ibuf_ = -1;
}

void Dx9Pipeline::shutdown() noexcept {
    if (device_) {
        for (auto& [k, lp] : program_cache_) {
            if (lp.shader.valid()) {
                device_->destroy_shader(lp.shader);
            }
        }
    }
    program_cache_.clear();
    vertex_shaders_.clear();
    pixel_shaders_.clear();

    for (auto& [id, vb] : vbuffers_) {
        if (vb.vbo != 0) glDeleteBuffers(1, &vb.vbo);
        if (vb.swizzled_vbo != 0) glDeleteBuffers(1, &vb.swizzled_vbo);
    }
    vbuffers_.clear();

    for (auto& [id, ib] : ibuffers_) {
        if (ib.ibo != 0) glDeleteBuffers(1, &ib.ibo);
    }
    ibuffers_.clear();
    vformats_.clear();

    if (custom_vao_ != 0) {
        glDeleteVertexArrays(1, &custom_vao_);
        custom_vao_ = 0;
    }

    active_vs_ = 0;
    active_ps_ = 0;
    next_vs_id_ = 0;
    next_ps_id_ = 0;
    next_vfmt_id_ = 0;
    next_vbuf_id_ = 0;
    next_ibuf_id_ = 0;
    bound_streams_.fill(-1);
    bound_ibuf_ = -1;
    reset_instancing();
    ps_samplers_ = {};
    vs_samplers_ = {};
    device_ = nullptr;
}

i32 Dx9Pipeline::create_vertex_shader(std::span<const u8> bytes) {
    Dx9ParsedShader parsed{};
    std::string err;
    const i32 rc = parse_d3d9_shader(bytes, Dx9ShaderStage::Vertex, parsed, &err);
    if (rc != 0) {
        GM_LOG_WARN("gfx", "dx9 create_vertex_shader failed (rc=%d): %s", rc, err.c_str());
        return rc;
    }
    const i32 id = ++next_vs_id_;
    GM_LOG_DEBUG("gfx", "dx9 created vertex shader id=%d (vs_%u_%u, consts=%zu, max_c=%u)",
                 id, parsed.major_version, parsed.minor_version, parsed.constants.size(), parsed.max_const_f);
    vertex_shaders_.emplace(id, std::move(parsed));
    // Pre-warm GLSL transpilation + GPU compile/link with FF-compat PS to avoid first-draw hitching.
    if (device_) {
        (void)get_or_link_program(id, 0);
    }
    return id;
}

i32 Dx9Pipeline::create_pixel_shader(std::span<const u8> bytes) {
    Dx9ParsedShader parsed{};
    std::string err;
    const i32 rc = parse_d3d9_shader(bytes, Dx9ShaderStage::Pixel, parsed, &err);
    if (rc != 0) {
        GM_LOG_WARN("gfx", "dx9 create_pixel_shader failed (rc=%d): %s", rc, err.c_str());
        return rc;
    }
    const i32 id = ++next_ps_id_;
    GM_LOG_DEBUG("gfx", "dx9 created pixel shader id=%d (ps_%u_%u, consts=%zu, max_c=%u, smask=0x%x)",
                 id, parsed.major_version, parsed.minor_version, parsed.constants.size(), parsed.max_const_f, parsed.sampler_mask);
    pixel_shaders_.emplace(id, std::move(parsed));
    // Pre-warm GLSL transpilation + GPU compile/link with FF-compat VS to avoid first-draw hitching.
    if (device_) {
        (void)get_or_link_program(0, id);
    }
    return id;
}

void Dx9Pipeline::invalidate_programs_using(i32 shader_id, bool is_vs) {
    for (auto it = program_cache_.begin(); it != program_cache_.end();) {
        const i32 vs_id = static_cast<i32>(it->first >> 32u);
        const i32 ps_id = static_cast<i32>(it->first & 0xFFFFFFFFu);
        if ((is_vs && vs_id == shader_id) || (!is_vs && ps_id == shader_id)) {
            if (device_ && it->second.shader.valid()) {
                device_->destroy_shader(it->second.shader);
            }
            it = program_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

bool Dx9Pipeline::destroy_vertex_shader(i32 id) {
    auto it = vertex_shaders_.find(id);
    if (it == vertex_shaders_.end()) return false;
    if (active_vs_ == id) active_vs_ = 0;
    invalidate_programs_using(id, true);
    vertex_shaders_.erase(it);
    return true;
}

bool Dx9Pipeline::destroy_pixel_shader(i32 id) {
    auto it = pixel_shaders_.find(id);
    if (it == pixel_shaders_.end()) return false;
    if (active_ps_ == id) active_ps_ = 0;
    invalidate_programs_using(id, false);
    pixel_shaders_.erase(it);
    return true;
}

void Dx9Pipeline::apply_ctab_defaults(const Dx9ParsedShader& sh) {
    const bool is_vs = (sh.stage == Dx9ShaderStage::Vertex);
    for (const auto& c : sh.constants) {
        if (!c.has_default || c.register_count == 0) continue;
        if (c.register_set == Dx9RegisterSet::Float4 && !c.default_floats.empty()) {
            if (is_vs) set_vs_const_f(c.register_index, c.default_floats.data(), c.register_count);
            else       set_ps_const_f(c.register_index, c.default_floats.data(), c.register_count);
        } else if (c.register_set == Dx9RegisterSet::Int4 && !c.default_ints.empty()) {
            if (is_vs) set_vs_const_i(c.register_index, c.default_ints.data(), c.register_count);
            else       set_ps_const_i(c.register_index, c.default_ints.data(), c.register_count);
        } else if (c.register_set == Dx9RegisterSet::Bool && !c.default_bools.empty()) {
            std::array<bool, 16> tmp{};
            const u32 n = std::min<u32>(16u, static_cast<u32>(c.default_bools.size()));
            for (u32 i = 0; i < n; ++i) tmp[i] = (c.default_bools[i] != 0);
            if (is_vs) set_vs_const_b(c.register_index, tmp.data(), n);
            else       set_ps_const_b(c.register_index, tmp.data(), n);
        }
    }
}

bool Dx9Pipeline::set_vertex_shader(i32 id) {
    if (id <= 0) return false;
    auto it = vertex_shaders_.find(id);
    if (it == vertex_shaders_.end()) return false;
    active_vs_ = id;
    apply_ctab_defaults(it->second);
    return true;
}

bool Dx9Pipeline::set_pixel_shader(i32 id) {
    if (id <= 0) return false;
    auto it = pixel_shaders_.find(id);
    if (it == pixel_shaders_.end()) return false;
    active_ps_ = id;
    apply_ctab_defaults(it->second);
    return true;
}

i32 Dx9Pipeline::get_vconst(std::string_view name) const noexcept {
    auto it = vertex_shaders_.find(active_vs_);
    if (it == vertex_shaders_.end()) return -4;
    const auto* c = it->second.find_constant(name);
    return c ? static_cast<i32>(c->register_index) : -4;
}

i32 Dx9Pipeline::get_vconst_size(std::string_view name) const noexcept {
    auto it = vertex_shaders_.find(active_vs_);
    if (it == vertex_shaders_.end()) return -4;
    const auto* c = it->second.find_constant(name);
    return c ? static_cast<i32>(c->register_count) : -4;
}

i32 Dx9Pipeline::get_pconst(std::string_view name) const noexcept {
    auto it = pixel_shaders_.find(active_ps_);
    if (it == pixel_shaders_.end()) return -4;
    const auto* c = it->second.find_constant(name);
    return c ? static_cast<i32>(c->register_index) : -4;
}

i32 Dx9Pipeline::get_pconst_size(std::string_view name) const noexcept {
    auto it = pixel_shaders_.find(active_ps_);
    if (it == pixel_shaders_.end()) return -4;
    const auto* c = it->second.find_constant(name);
    return c ? static_cast<i32>(c->register_count) : -4;
}

void Dx9Pipeline::set_vs_const_f(u32 reg, const f32* data, u32 vec4_count) {
    if (!data || reg >= 256u) return;
    const u32 n = std::min(vec4_count, 256u - reg);
    std::memcpy(&vs_const_f_[reg * 4u], data, n * 16u);
}

void Dx9Pipeline::set_ps_const_f(u32 reg, const f32* data, u32 vec4_count) {
    if (!data || reg >= 224u) return;
    const u32 n = std::min(vec4_count, 224u - reg);
    std::memcpy(&ps_const_f_[reg * 4u], data, n * 16u);
}

void Dx9Pipeline::set_vs_const_i(u32 reg, const i32* data, u32 ivec4_count) {
    if (!data || reg >= 16u) return;
    const u32 n = std::min(ivec4_count, 16u - reg);
    std::memcpy(&vs_const_i_[reg * 4u], data, n * 16u);
}

void Dx9Pipeline::set_ps_const_i(u32 reg, const i32* data, u32 ivec4_count) {
    if (!data || reg >= 16u) return;
    const u32 n = std::min(ivec4_count, 16u - reg);
    std::memcpy(&ps_const_i_[reg * 4u], data, n * 16u);
}

void Dx9Pipeline::set_vs_const_b(u32 reg, const bool* data, u32 count) {
    if (!data || reg >= 16u) return;
    const u32 n = std::min(count, 16u - reg);
    for (u32 i = 0; i < n; ++i) vs_const_b_[reg + i] = data[i] ? 1 : 0;
}

void Dx9Pipeline::set_ps_const_b(u32 reg, const bool* data, u32 count) {
    if (!data || reg >= 16u) return;
    const u32 n = std::min(count, 16u - reg);
    for (u32 i = 0; i < n; ++i) ps_const_b_[reg + i] = data[i] ? 1 : 0;
}

void Dx9Pipeline::set_ps_sampler_tex(i32 stage, TextureHandle tex) {
    if (stage < 0 || stage >= 8) return;
    auto& s = ps_samplers_[static_cast<usize>(stage)];
    s.texture = tex;
    s.has_explicit_tex = true;
}

void Dx9Pipeline::set_vs_sampler_tex(i32 stage, TextureHandle tex) {
    if (stage < 0 || stage >= 4) return;
    auto& s = vs_samplers_[static_cast<usize>(stage)];
    s.texture = tex;
    s.has_explicit_tex = true;
}

void Dx9Pipeline::set_ps_sampler_filter(i32 stage, bool linear) {
    if (stage < 0 || stage >= 8) return;
    auto& s = ps_samplers_[static_cast<usize>(stage)];
    s.linear_filter = linear;
    s.has_explicit_filter = true;
}

void Dx9Pipeline::set_vs_sampler_filter(i32 stage, bool linear) {
    if (stage < 0 || stage >= 4) return;
    auto& s = vs_samplers_[static_cast<usize>(stage)];
    s.linear_filter = linear;
    s.has_explicit_filter = true;
}

void Dx9Pipeline::set_ps_sampler_wrap(i32 stage, i32 wrap_u, i32 wrap_v, u32 border_bgr) {
    if (stage < 0 || stage >= 8) return;
    auto& s = ps_samplers_[static_cast<usize>(stage)];
    s.wrap_u = wrap_u;
    s.wrap_v = wrap_v;
    s.border_bgr = border_bgr & 0x00FFFFFFu;
    s.has_explicit_wrap = true;
}

void Dx9Pipeline::set_vs_sampler_wrap(i32 stage, i32 wrap_u, i32 wrap_v, u32 border_bgr) {
    if (stage < 0 || stage >= 4) return;
    auto& s = vs_samplers_[static_cast<usize>(stage)];
    s.wrap_u = wrap_u;
    s.wrap_v = wrap_v;
    s.border_bgr = border_bgr & 0x00FFFFFFu;
    s.has_explicit_wrap = true;
}

void Dx9Pipeline::clear_stage0_overrides() noexcept {
    ps_samplers_[0].has_explicit_filter = false;
    ps_samplers_[0].has_explicit_wrap = false;
}

Dx9Pipeline::LinkedProgram* Dx9Pipeline::get_or_link_program(i32 vs_id, i32 ps_id) {
    if (!device_) return nullptr;
    const u64 key = program_key(vs_id, ps_id);
    if (auto it = program_cache_.find(key); it != program_cache_.end()) {
        return it->second.failed ? nullptr : &it->second;
    }

    const bool gles3 = device_->caps().is_gles;
    std::string vs_glsl;
    std::unordered_set<std::string> vs_outputs;
    const Dx9ParsedShader* vs_parsed = nullptr;
    const Dx9ParsedShader* ps_parsed = nullptr;

    if (vs_id > 0) {
        auto it = vertex_shaders_.find(vs_id);
        if (it == vertex_shaders_.end()) return nullptr;
        vs_parsed = &it->second;
        auto res = transpile_d3d9_to_glsl(*vs_parsed, gles3, nullptr);
        if (!res) {
            GM_LOG_WARN("gfx", "dx9 VS transpile error (id=%d): %s", vs_id, res.error_message().c_str());
            program_cache_[key].failed = true;
            return nullptr;
        }
        vs_glsl = std::move(res.value());
        vs_outputs = vs_parsed->output_semantics;
    } else {
        vs_glsl = build_ff_compat_vs_glsl(gles3);
        vs_outputs = ff_compat_vs_output_semantics();
    }

    std::string ps_glsl;
    if (ps_id > 0) {
        auto it = pixel_shaders_.find(ps_id);
        if (it == pixel_shaders_.end()) return nullptr;
        ps_parsed = &it->second;
        auto res = transpile_d3d9_to_glsl(*ps_parsed, gles3, &vs_outputs);
        if (!res) {
            GM_LOG_WARN("gfx", "dx9 PS transpile error (id=%d): %s", ps_id, res.error_message().c_str());
            program_cache_[key].failed = true;
            return nullptr;
        }
        ps_glsl = std::move(res.value());
    } else {
        ps_glsl = build_ff_compat_ps_glsl(gles3);
    }

    auto sh_res = device_->create_shader(vs_glsl, ps_glsl);
    if (!sh_res) {
        GM_LOG_ERROR("gfx", "dx9 GLSL link failed (vs=%d, ps=%d): %s", vs_id, ps_id, sh_res.error_message().c_str());
        GM_LOG_DEBUG("gfx", "--- Generated VS ---\n%s", vs_glsl.c_str());
        GM_LOG_DEBUG("gfx", "--- Generated PS ---\n%s", ps_glsl.c_str());
        program_cache_[key].failed = true;
        return nullptr;
    }

    LinkedProgram lp{};
    lp.shader = sh_res.value();
    const GLuint pid = lp.shader.id;

    lp.loc_yflip = glGetUniformLocation(pid, "u_gm_yflip");
    lp.loc_pixel_center_offset = glGetUniformLocation(pid, "u_pixel_center_offset");
    lp.loc_viewport = glGetUniformLocation(pid, "u_gm_viewport");
    lp.loc_vc = glGetUniformLocation(pid, "u_vc");
    lp.loc_vi = glGetUniformLocation(pid, "u_vi");
    lp.loc_vb = glGetUniformLocation(pid, "u_vb");
    lp.loc_pc = glGetUniformLocation(pid, "u_pc");
    lp.loc_pi = glGetUniformLocation(pid, "u_pi");
    lp.loc_pb = glGetUniformLocation(pid, "u_pb");
    lp.loc_world = glGetUniformLocation(pid, "u_world");
    lp.loc_view = glGetUniformLocation(pid, "u_view");
    lp.loc_proj = glGetUniformLocation(pid, "u_proj");
    lp.loc_model_tint = glGetUniformLocation(pid, "u_model_tint");
    lp.loc_tex_blending = glGetUniformLocation(pid, "u_tex_blending");
    lp.loc_texcoord_mode = glGetUniformLocation(pid, "u_texcoord_mode");
    lp.loc_alpha_test = glGetUniformLocation(pid, "u_alpha_test");
    lp.loc_alpha_ref = glGetUniformLocation(pid, "u_alpha_ref");
    lp.loc_alpha_func = glGetUniformLocation(pid, "u_alpha_func");

    for (u32 s = 0; s < 8; ++s) {
        const std::string uname = "u_s" + std::to_string(s);
        lp.loc_ps_samp[s] = glGetUniformLocation(pid, uname.c_str());
    }
    for (u32 s = 0; s < 4; ++s) {
        const std::string uname = "u_vs" + std::to_string(s);
        lp.loc_vs_samp[s] = glGetUniformLocation(pid, uname.c_str());
    }

    if (vs_parsed) {
        lp.vs_max_f = vs_parsed->max_const_f;
        lp.vs_max_i = vs_parsed->max_const_i;
        lp.vs_max_b = vs_parsed->max_const_b;
        lp.vs_sampler_mask = vs_parsed->sampler_mask;
    }
    if (ps_parsed) {
        lp.ps_max_f = ps_parsed->max_const_f;
        lp.ps_max_i = ps_parsed->max_const_i;
        lp.ps_max_b = ps_parsed->max_const_b;
        lp.ps_sampler_mask = ps_parsed->sampler_mask;
    } else {
        lp.ps_sampler_mask = 1u;
    }

    GM_LOG_DEBUG("gfx", "dx9 linked GLSL program (vs=%d, ps=%d) -> GL prog %u", vs_id, ps_id, pid);
    auto [ins_it, _] = program_cache_.emplace(key, lp);
    return &ins_it->second;
}

bool Dx9Pipeline::bind_and_upload(const Dx9DrawContext& ctx, TextureHandle stage0_draw_tex) {
    if (!device_ || !has_active_shader()) return false;
    LinkedProgram* lp = get_or_link_program(active_vs_, active_ps_);
    if (!lp) return false;

    device_->bind_shader(lp->shader);
    if (lp->loc_texcoord_mode >= 0) glUniform1i(lp->loc_texcoord_mode, ctx.texcoord_mode);

    const f32 yflip = ctx.rendering_to_fbo ? -1.0f : 1.0f;
    if (lp->loc_yflip >= 0) glUniform1f(lp->loc_yflip, yflip);
    if (lp->loc_pixel_center_offset >= 0) glUniform2fv(lp->loc_pixel_center_offset, 1, ctx.pixel_center_offset.data());
    if (lp->loc_viewport >= 0) {
        glUniform4f(lp->loc_viewport,
                    static_cast<f32>(ctx.viewport.x), static_cast<f32>(ctx.viewport.y),
                    static_cast<f32>(ctx.viewport.width), static_cast<f32>(ctx.viewport.height));
    }

    // Fixed-function compat uniforms (present when vs_id == 0 or ps_id == 0).
    if (lp->loc_world >= 0 && ctx.world_matrix) glUniformMatrix4fv(lp->loc_world, 1, GL_FALSE, ctx.world_matrix);
    if (lp->loc_view >= 0 && ctx.view_matrix)   glUniformMatrix4fv(lp->loc_view, 1, GL_FALSE, ctx.view_matrix);
    if (lp->loc_proj >= 0 && ctx.proj_matrix)   glUniformMatrix4fv(lp->loc_proj, 1, GL_FALSE, ctx.proj_matrix);
    if (lp->loc_model_tint >= 0 && ctx.model_tint) {
        glUniform4fv(lp->loc_model_tint, 1, ctx.model_tint);
    }
    if (lp->loc_tex_blending >= 0) glUniform1i(lp->loc_tex_blending, ctx.tex_blending ? 1 : 0);
    if (lp->loc_alpha_test >= 0)   glUniform1i(lp->loc_alpha_test, ctx.alpha_test_enabled ? 1 : 0);
    if (lp->loc_alpha_ref >= 0)    glUniform1f(lp->loc_alpha_ref, ctx.alpha_test_ref);
    if (lp->loc_alpha_func >= 0)   glUniform1i(lp->loc_alpha_func, ctx.alpha_test_func);

    // Custom VS/PS register banks.
    if (lp->loc_vc >= 0 && lp->vs_max_f > 0) glUniform4fv(lp->loc_vc, static_cast<GLsizei>(lp->vs_max_f), vs_const_f_.data());
    if (lp->loc_vi >= 0 && lp->vs_max_i > 0) glUniform4iv(lp->loc_vi, static_cast<GLsizei>(lp->vs_max_i), vs_const_i_.data());
    if (lp->loc_vb >= 0 && lp->vs_max_b > 0) glUniform1iv(lp->loc_vb, static_cast<GLsizei>(lp->vs_max_b), vs_const_b_.data());
    if (lp->loc_pc >= 0 && lp->ps_max_f > 0) glUniform4fv(lp->loc_pc, static_cast<GLsizei>(lp->ps_max_f), ps_const_f_.data());
    if (lp->loc_pi >= 0 && lp->ps_max_i > 0) glUniform4iv(lp->loc_pi, static_cast<GLsizei>(lp->ps_max_i), ps_const_i_.data());
    if (lp->loc_pb >= 0 && lp->ps_max_b > 0) glUniform1iv(lp->loc_pb, static_cast<GLsizei>(lp->ps_max_b), ps_const_b_.data());

    // Bind PS samplers on texture units 0..7.
    for (u32 s = 0; s < 8; ++s) {
        if (lp->loc_ps_samp[s] < 0) continue;
        auto& st = ps_samplers_[s];
        TextureHandle tex{};
        if (s == 0) {
            if (stage0_draw_tex.valid()) {
                tex = stage0_draw_tex;
                st.texture = stage0_draw_tex;
                st.has_explicit_tex = false;
            } else {
                tex = st.has_explicit_tex ? st.texture : TextureHandle{};
            }
        } else {
            tex = st.texture;
        }
        const bool lin = (s == 0 && !st.has_explicit_filter) ? ctx.global_linear_filter : st.linear_filter;
        const i32 wu = (s == 0 && !st.has_explicit_wrap) ? (ctx.global_tex_repeat ? 1 : 3) : st.wrap_u;
        const i32 wv = (s == 0 && !st.has_explicit_wrap) ? (ctx.global_tex_repeat ? 1 : 3) : st.wrap_v;
        device_->set_texture_sampler_ext(s, lin ? TextureFilter::Linear : TextureFilter::Nearest, wu, wv, st.border_bgr);
        device_->bind_texture(s, tex);
        glUniform1i(lp->loc_ps_samp[s], static_cast<GLint>(s));
    }

    // Bind VS samplers on texture units 8..11.
    for (u32 s = 0; s < 4; ++s) {
        if (lp->loc_vs_samp[s] < 0) continue;
        const auto& st = vs_samplers_[s];
        device_->set_texture_sampler_ext(8u + s, st.linear_filter ? TextureFilter::Linear : TextureFilter::Nearest,
                                         st.wrap_u, st.wrap_v, st.border_bgr);
        device_->bind_texture(8u + s, st.texture);
        glUniform1i(lp->loc_vs_samp[s], static_cast<GLint>(8u + s));
    }

    glActiveTexture(GL_TEXTURE0);
    return true;
}

// ---------------------------------------------------------------------------
// Custom Vertex Formats, Vertex Buffers, Index Buffers & Instancing
// ---------------------------------------------------------------------------

void Dx9Pipeline::vertex_format_begin() {
    vfmt_building_.clear();
    vfmt_building_offsets_.fill(0);
    vfmt_building_usage_idx_.fill(0);
}

void Dx9Pipeline::vertex_format_add_custom(i32 decl_type, i32 usage, i32 slot) {
    if (slot < 0 || slot >= 16) return;
    const u8 u = static_cast<u8>(std::clamp(usage, 0, 15));
    Dx9VertexElement el{};
    el.stream = static_cast<u16>(slot);
    el.offset = static_cast<u16>(vfmt_building_offsets_[static_cast<usize>(slot)]);
    el.type = static_cast<u8>(std::clamp(decl_type, 0, 17));
    el.usage = u;
    el.usage_index = vfmt_building_usage_idx_[u]++;
    vfmt_building_.push_back(el);
    vfmt_building_offsets_[static_cast<usize>(slot)] += decl_type_byte_size(decl_type);
}

i32 Dx9Pipeline::vertex_format_end() {
    const i32 id = ++next_vfmt_id_;
    vformats_[id] = VertexFormat{vfmt_building_, vfmt_building_offsets_};
    return id;
}

i32 Dx9Pipeline::vertex_format_get_size(i32 vformat_id, i32 slot) const noexcept {
    if (slot < 0 || slot >= 16) return -1;
    auto it = vformats_.find(vformat_id);
    if (it == vformats_.end()) return -1;
    return it->second.slot_sizes[static_cast<usize>(slot)];
}

bool Dx9Pipeline::vertex_format_delete(i32 vformat_id) {
    return vformats_.erase(vformat_id) > 0;
}

i32 Dx9Pipeline::create_vertex_buffer(std::span<const u8> bytes, u32 stride) {
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    if (vbo == 0) return -1;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()),
                 bytes.empty() ? nullptr : bytes.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    GpuVertexBuffer rec{};
    rec.vbo = vbo;
    rec.stride = std::max(1u, stride);
    rec.size_bytes = static_cast<u32>(bytes.size());
#if GMRUNNER_USE_GLES3
    rec.cpu_copy.assign(bytes.begin(), bytes.end());
#endif
    const i32 id = ++next_vbuf_id_;
    vbuffers_.emplace(id, std::move(rec));
    return id;
}

i32 Dx9Pipeline::vertex_buffer_get_size(i32 vbuf_id) const noexcept {
    auto it = vbuffers_.find(vbuf_id);
    return it == vbuffers_.end() ? -1 : static_cast<i32>(it->second.size_bytes);
}

bool Dx9Pipeline::vertex_bind_buffer(i32 vbuf_id, i32 slot) {
    if (slot < 0 || slot >= 16) return false;
    if (vbuf_id < 0) {
        bound_streams_[static_cast<usize>(slot)] = -1;
        return true;
    }
    if (!vbuffers_.contains(vbuf_id)) return false;
    bound_streams_[static_cast<usize>(slot)] = vbuf_id;
    return true;
}

bool Dx9Pipeline::vertex_delete_buffer(i32 vbuf_id) {
    auto it = vbuffers_.find(vbuf_id);
    if (it == vbuffers_.end()) return false;
    if (it->second.vbo != 0) glDeleteBuffers(1, &it->second.vbo);
    if (it->second.swizzled_vbo != 0) glDeleteBuffers(1, &it->second.swizzled_vbo);
    for (auto& s : bound_streams_) {
        if (s == vbuf_id) s = -1;
    }
    vbuffers_.erase(it);
    return true;
}

i32 Dx9Pipeline::create_index_buffer(std::span<const u8> bytes, i32 d3d_format) {
    GLuint ibo = 0;
    glGenBuffers(1, &ibo);
    if (ibo == 0) return -1;
    glBindBuffer(GL_ARRAY_BUFFER, ibo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(bytes.size()),
                 bytes.empty() ? nullptr : bytes.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const i32 id = ++next_ibuf_id_;
    GpuIndexBuffer buffer{ibo, d3d_format, static_cast<u32>(bytes.size()), {}};
    if (device_->caps().is_gles) buffer.cpu_copy.assign(bytes.begin(), bytes.end());
    ibuffers_.emplace(id, std::move(buffer));
    return id;
}

i32 Dx9Pipeline::index_buffer_get_size(i32 ibuf_id) const noexcept {
    auto it = ibuffers_.find(ibuf_id);
    return it == ibuffers_.end() ? -1 : static_cast<i32>(it->second.size_bytes);
}

i32 Dx9Pipeline::index_buffer_get_format(i32 ibuf_id) const noexcept {
    auto it = ibuffers_.find(ibuf_id);
    return it == ibuffers_.end() ? -1 : it->second.d3d_format;
}

bool Dx9Pipeline::index_set_buffer(i32 ibuf_id) {
    if (ibuf_id < 0) {
        bound_ibuf_ = -1;
        return true;
    }
    if (!ibuffers_.contains(ibuf_id)) return false;
    bound_ibuf_ = ibuf_id;
    return true;
}

bool Dx9Pipeline::index_delete_buffer(i32 ibuf_id) {
    auto it = ibuffers_.find(ibuf_id);
    if (it == ibuffers_.end()) return false;
    if (it->second.ibo != 0) glDeleteBuffers(1, &it->second.ibo);
    if (bound_ibuf_ == ibuf_id) bound_ibuf_ = -1;
    ibuffers_.erase(it);
    return true;
}

bool Dx9Pipeline::draw_vertex_buffer(
    i32 vbuf_id,
    i32 vformat_id,
    i32 d3d_prim_type,
    TextureHandle tex,
    i32 vert_count,
    bool indexed,
    const Dx9DrawContext& ctx,
    ShaderHandle ff_shader_fallback) {
    if (!device_ || custom_vao_ == 0 || vert_count <= 0) return false;
    auto bit = vbuffers_.find(vbuf_id);
    auto fit = vformats_.find(vformat_id);
    if (bit == vbuffers_.end() || fit == vformats_.end()) return false;

    const GpuIndexBuffer* ib = nullptr;
    if (indexed) {
        auto iit = ibuffers_.find(bound_ibuf_);
        if (iit == ibuffers_.end()) return false;
        ib = &iit->second;
    }

    // Bind shader (custom DX9 shader if active, otherwise fixed-function fallback).
    set_ps_sampler_tex(0, tex);
    if (has_active_shader()) {
        if (!bind_and_upload(ctx, tex)) return false;
    } else if (ff_shader_fallback.valid()) {
        device_->bind_shader(ff_shader_fallback);
        const auto& st0 = ps_samplers_[0];
        const bool lin = st0.has_explicit_filter ? st0.linear_filter : ctx.global_linear_filter;
        const i32 wu = st0.has_explicit_wrap ? st0.wrap_u : (ctx.global_tex_repeat ? 1 : 3);
        const i32 wv = st0.has_explicit_wrap ? st0.wrap_v : (ctx.global_tex_repeat ? 1 : 3);
        device_->set_texture_sampler_ext(0, lin ? TextureFilter::Linear : TextureFilter::Nearest, wu, wv, st0.border_bgr);
        device_->bind_texture(0, tex);
    } else {
        return false;
    }

    bound_streams_[0] = vbuf_id;
    glBindVertexArray(custom_vao_);

    // Clear all 16 attribute slots first so stale streams don't leak across formats.
    for (GLuint loc = 0; loc < 16; ++loc) {
        glDisableVertexAttribArray(loc);
        glVertexAttribDivisor(loc, 0);
    }

    // Default color attribute to white if format omits COLOR0.
    glVertexAttrib4f(3, 1.0f, 1.0f, 1.0f, 1.0f);

    const auto vs = vertex_shaders_.find(active_vs_);
    for (const auto& el : fit->second.elements) {
        i32 loc = dx9_semantic_to_attrib_location(el.usage, el.usage_index);
        if (vs != vertex_shaders_.end()) {
            const auto input = std::ranges::find_if(vs->second.inputs, [&](const Dx9DclSemantic& d) {
                return d.usage == el.usage && d.usage_index == el.usage_index;
            });
            if (input == vs->second.inputs.end()) continue;
            loc = input->attrib_location;
        }
        if (loc < 0 || loc >= 16 || el.stream >= 16) continue;

        const i32 stream_vbuf_id = bound_streams_[el.stream];
        auto sit = vbuffers_.find(stream_vbuf_id);
        if (sit == vbuffers_.end()) continue;

        GLuint use_vbo = sit->second.vbo;
        const GLsizei stride = static_cast<GLsizei>(sit->second.stride);
        const void* ptr = reinterpret_cast<const void*>(static_cast<uintptr_t>(el.offset));

        GLint comps = 4;
        GLenum gl_type = GL_FLOAT;
        GLboolean normalized = GL_FALSE;
        switch (el.type) {
            case 0:  comps = 1; gl_type = GL_FLOAT; break;
            case 1:  comps = 2; gl_type = GL_FLOAT; break;
            case 2:  comps = 3; gl_type = GL_FLOAT; break;
            case 3:  comps = 4; gl_type = GL_FLOAT; break;
            case 4: // D3DCOLOR (BGRA8 normalized)
#if !GMRUNNER_USE_GLES3
                comps = GL_BGRA;
                gl_type = GL_UNSIGNED_BYTE;
                normalized = GL_TRUE;
#else
                if (sit->second.swizzled_vbo == 0 && !sit->second.cpu_copy.empty()) {
                    std::vector<u8> swiz = sit->second.cpu_copy;
                    for (usize off = el.offset; off + 3 < swiz.size(); off += sit->second.stride) {
                        std::swap(swiz[off + 0], swiz[off + 2]);
                    }
                    glGenBuffers(1, &sit->second.swizzled_vbo);
                    glBindBuffer(GL_ARRAY_BUFFER, sit->second.swizzled_vbo);
                    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(swiz.size()), swiz.data(), GL_STATIC_DRAW);
                }
                if (sit->second.swizzled_vbo != 0) use_vbo = sit->second.swizzled_vbo;
                comps = 4;
                gl_type = GL_UNSIGNED_BYTE;
                normalized = GL_TRUE;
#endif
                break;
            case 5:  comps = 4; gl_type = GL_UNSIGNED_BYTE; normalized = GL_FALSE; break;
            case 6:  comps = 2; gl_type = GL_SHORT; normalized = GL_FALSE; break;
            case 7:  comps = 4; gl_type = GL_SHORT; normalized = GL_FALSE; break;
            case 8:  comps = 4; gl_type = GL_UNSIGNED_BYTE; normalized = GL_TRUE; break;
            case 9:  comps = 2; gl_type = GL_SHORT; normalized = GL_TRUE; break;
            case 10: comps = 4; gl_type = GL_SHORT; normalized = GL_TRUE; break;
            case 11: comps = 2; gl_type = GL_UNSIGNED_SHORT; normalized = GL_TRUE; break;
            case 12: comps = 4; gl_type = GL_UNSIGNED_SHORT; normalized = GL_TRUE; break;
            case 15: comps = 2; gl_type = GL_HALF_FLOAT; normalized = GL_FALSE; break;
            case 16: comps = 4; gl_type = GL_HALF_FLOAT; normalized = GL_FALSE; break;
            default: continue;
        }

        glBindBuffer(GL_ARRAY_BUFFER, use_vbo);
        glEnableVertexAttribArray(static_cast<GLuint>(loc));
        glVertexAttribPointer(static_cast<GLuint>(loc), comps, gl_type, normalized, stride, ptr);
        if (instancing_enabled_ && stream_instance_rate_[el.stream] > 0) {
            glVertexAttribDivisor(static_cast<GLuint>(loc), stream_instance_rate_[el.stream]);
        }
    }

    const auto topo = (d3d_prim_type >= 1 && d3d_prim_type <= 6)
                        ? static_cast<PrimitiveTopology>(d3d_prim_type - 1) : PrimitiveTopology::Triangles;
    const u32 instances = instancing_enabled_ && instance_count_ > 0 ? instance_count_ : 1;
    const bool flat = ctx.flat_shading && !has_active_shader();
    if (indexed && ib) {
        const u32 index_size = ib->d3d_format == 102 ? 4u : 2u;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib->ibo);
        device_->draw_primitives(topo, vert_count, flat, instances, index_size, ib->cpu_copy);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        device_->draw_primitives(topo, vert_count, flat, instances);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    return true;
}

} // namespace gm
