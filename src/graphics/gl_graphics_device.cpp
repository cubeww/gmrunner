#include "graphics/gl_graphics_device.h"

#include "base/log.h"
#include "graphics/gl_headers.h"

#include <cstring>
#include <vector>

namespace gm {

namespace {

// Core in desktop GL and GLES 3.2; also provided by EXT/OES_texture_border_clamp.
constexpr GLenum kClampToBorder = 0x812D;
constexpr GLenum kTextureBorderColor = 0x1004;

GLenum to_gl_blend_factor(BlendFactor f) noexcept {
    switch (f) {
        case BlendFactor::Zero:        return GL_ZERO;
        case BlendFactor::One:         return GL_ONE;
        case BlendFactor::SrcColor:    return GL_SRC_COLOR;
        case BlendFactor::InvSrcColor: return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha:    return GL_SRC_ALPHA;
        case BlendFactor::InvSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:    return GL_DST_ALPHA;
        case BlendFactor::InvDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
        case BlendFactor::DstColor:    return GL_DST_COLOR;
        case BlendFactor::InvDstColor: return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlphaSat: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}

GLenum to_gl_blend_op(BlendOp op) noexcept {
    switch (op) {
        case BlendOp::Add:             return GL_FUNC_ADD;
        case BlendOp::Subtract:        return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min:             return GL_MIN;
        case BlendOp::Max:             return GL_MAX;
    }
    return GL_FUNC_ADD;
}

GLenum to_gl_compare(CompareFunc fn) noexcept {
    switch (fn) {
        case CompareFunc::Never:        return GL_NEVER;
        case CompareFunc::Less:         return GL_LESS;
        case CompareFunc::Equal:        return GL_EQUAL;
        case CompareFunc::LessEqual:    return GL_LEQUAL;
        case CompareFunc::Greater:      return GL_GREATER;
        case CompareFunc::NotEqual:     return GL_NOTEQUAL;
        case CompareFunc::GreaterEqual: return GL_GEQUAL;
        case CompareFunc::Always:       return GL_ALWAYS;
    }
    return GL_LEQUAL;
}

GLenum to_gl_wrap(TextureWrap w) noexcept {
    switch (w) {
        case TextureWrap::ClampToEdge:    return GL_CLAMP_TO_EDGE;
        case TextureWrap::Repeat:         return GL_REPEAT;
        case TextureWrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
    }
    return GL_CLAMP_TO_EDGE;
}

GLenum to_gl_buffer_target(BufferTarget t) noexcept {
    return (t == BufferTarget::Index) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
}

GLenum to_gl_buffer_usage(BufferUsage u) noexcept {
    switch (u) {
        case BufferUsage::Static:  return GL_STATIC_DRAW;
        case BufferUsage::Dynamic: return GL_DYNAMIC_DRAW;
        case BufferUsage::Stream:  return GL_STREAM_DRAW;
    }
    return GL_DYNAMIC_DRAW;
}

// Converts BGRA8 bytes to RGBA8 for portable GLES3 / WebGL2 upload (GL_BGRA is not core in GLES3).
std::vector<u8> bgra_to_rgba(std::span<const u8> src) {
    std::vector<u8> dst(src.size());
    for (usize i = 0; i + 3 < src.size(); i += 4) {
        dst[i + 0] = src[i + 2];
        dst[i + 1] = src[i + 1];
        dst[i + 2] = src[i + 0];
        dst[i + 3] = src[i + 3];
    }
    return dst;
}

Result<GLuint> compile_gl_shader(GLenum type, std::string_view src) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
        return Error{"glCreateShader failed"};
    }
    const char* ptr = src.data();
    const GLint len = static_cast<GLint>(src.size());
    glShaderSource(shader, 1, &ptr, &len);
    glCompileShader(shader);

    GLint status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status != GL_TRUE) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        std::string info(static_cast<usize>(log_len > 0 ? log_len : 1), '\0');
        glGetShaderInfoLog(shader, log_len, nullptr, info.data());
        glDeleteShader(shader);
        return Error{"GL shader compile failed: " + info};
    }
    return shader;
}

} // namespace

GlGraphicsDevice::~GlGraphicsDevice() {
    shutdown();
}

Result<void> GlGraphicsDevice::init(GlProcLoader proc_loader) {
    if (initialized_) {
        return Result<void>::success();
    }

#if !GMRUNNER_USE_GLES3
    if (!proc_loader) {
        return Error{"GlGraphicsDevice::init requires a valid GL proc loader on desktop"};
    }
    const int version = gladLoadGL(reinterpret_cast<GLADloadfunc>(proc_loader));
    if (version == 0) {
        return Error{"gladLoadGL failed to initialize OpenGL 3.3 Core functions"};
    }
    caps_.is_gles = false;
#else
    (void)proc_loader;
    caps_.is_gles = true;
#endif

    auto safe_gl_str = [](GLenum name) -> std::string {
        const GLubyte* s = glGetString(name);
        return s ? reinterpret_cast<const char*>(s) : "unknown";
    };

    caps_.vendor = safe_gl_str(GL_VENDOR);
    caps_.renderer = safe_gl_str(GL_RENDERER);
    caps_.version = safe_gl_str(GL_VERSION);
    caps_.glsl_version = safe_gl_str(GL_SHADING_LANGUAGE_VERSION);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &caps_.max_texture_size);
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &caps_.max_color_attachments);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &caps_.max_texture_units);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGenSamplers(static_cast<GLsizei>(sampler_ids_.size()), sampler_ids_.data());
#if !GMRUNNER_USE_GLES3
    glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    border_clamp_supported_ = true;
#else
    GLint major = 0, minor = 0, extension_count = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    border_clamp_supported_ = major > 3 || (major == 3 && minor >= 2);
    glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
    for (GLint i = 0; i < extension_count && !border_clamp_supported_; ++i) {
        const auto* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (name) {
            const std::string_view ext(name);
            border_clamp_supported_ = ext == "GL_EXT_texture_border_clamp" || ext == "GL_OES_texture_border_clamp";
        }
    }
#endif
    set_polygon_mode(2);

    // Create 1x1 opaque white fallback texture (mirrors gm82dx9 SetNullTexture behavior).
    constexpr u8 kWhitePixel[4] = {255, 255, 255, 255};
    glGenTextures(1, &white_texture_id_);
    glBindTexture(GL_TEXTURE_2D, white_texture_id_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, kWhitePixel);
    glBindTexture(GL_TEXTURE_2D, 0);

    initialized_ = true;
    reset_cached_state();

    GM_LOG_INFO("gfx", "Initialized %s", caps_.is_gles ? "OpenGL ES 3.0 / WebGL 2.0" : "OpenGL 3.3 Core");
    GM_LOG_INFO("gfx", "  Vendor:   %s", caps_.vendor.c_str());
    GM_LOG_INFO("gfx", "  Renderer: %s", caps_.renderer.c_str());
    GM_LOG_INFO("gfx", "  Version:  %s (GLSL %s)", caps_.version.c_str(), caps_.glsl_version.c_str());
    GM_LOG_INFO("gfx", "  Limits:   max_tex=%d, max_mrt=%d, max_units=%d",
                caps_.max_texture_size, caps_.max_color_attachments, caps_.max_texture_units);

    return Result<void>::success();
}

void GlGraphicsDevice::shutdown() noexcept {
    if (!initialized_) {
        return;
    }

    for (const auto& [id, entry] : framebuffers_) {
        (void)id;
        if (entry.depth_rbo != 0) glDeleteRenderbuffers(1, &entry.depth_rbo);
        if (entry.fbo != 0) glDeleteFramebuffers(1, &entry.fbo);
    }
    framebuffers_.clear();

    for (const auto& [id, meta] : textures_) {
        (void)meta;
        const GLuint tex = id;
        glDeleteTextures(1, &tex);
    }
    textures_.clear();
    glDeleteSamplers(static_cast<GLsizei>(sampler_ids_.size()), sampler_ids_.data());
    sampler_ids_.fill(0);
    if (flat_index_buffer_) glDeleteBuffers(1, &flat_index_buffer_);
    flat_index_buffer_ = 0;
    flat_indices_.clear();

    if (white_texture_id_ != 0) {
        glDeleteTextures(1, &white_texture_id_);
        white_texture_id_ = 0;
    }

    reset_cached_state();
    initialized_ = false;
}

void GlGraphicsDevice::reset_cached_state() noexcept {
    cache_ = CachedState{};
}

Result<void> GlGraphicsDevice::reinit_after_context_loss(GlProcLoader proc_loader) {
    // Discard stale handles without invoking glDelete* against a lost context.
    framebuffers_.clear();
    textures_.clear();
    sampler_ids_.fill(0);
    flat_index_buffer_ = 0;
    flat_indices_.clear();
    white_texture_id_ = 0;
    initialized_ = false;
    reset_cached_state();
    return init(proc_loader);
}

void GlGraphicsDevice::set_viewport(const ViewportRect& rect) {
    if (cache_.viewport_valid && cache_.viewport == rect) return;
    glViewport(rect.x, rect.y, rect.width, rect.height);
    cache_.viewport = rect;
    cache_.viewport_valid = true;
}

void GlGraphicsDevice::set_scissor(bool enabled, const ViewportRect& rect) {
    if (cache_.scissor_valid && cache_.scissor_enabled == enabled && (!enabled || cache_.scissor_rect == rect)) {
        return;
    }
    if (enabled) {
        if (!cache_.scissor_valid || !cache_.scissor_enabled) glEnable(GL_SCISSOR_TEST);
        glScissor(rect.x, rect.y, rect.width, rect.height);
        cache_.scissor_rect = rect;
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    cache_.scissor_enabled = enabled;
    cache_.scissor_valid = true;
}

void GlGraphicsDevice::clear(bool color, bool depth, bool stencil, f32 r, f32 g, f32 b, f32 a, f32 depth_val, i32 stencil_val) {
    GLbitfield mask = 0;
    if (color) {
        glClearColor(r, g, b, a);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    bool restore_depth_mask = false;
    if (depth) {
        if (!depth_write_enabled_) {
            glDepthMask(GL_TRUE);
            restore_depth_mask = true;
        }
        glClearDepthf(depth_val);
        mask |= GL_DEPTH_BUFFER_BIT;
    }
    if (stencil) {
        glClearStencil(stencil_val);
        mask |= GL_STENCIL_BUFFER_BIT;
    }
    if (mask != 0) {
        glClear(mask);
    }
    if (restore_depth_mask) {
        glDepthMask(GL_FALSE);
    }
}

void GlGraphicsDevice::set_blend_state(const BlendState& state) {
    if (cache_.blend_valid && cache_.blend == state) return;
    if (state.enabled) {
        if (!cache_.blend_valid || !cache_.blend.enabled) glEnable(GL_BLEND);
        glBlendFuncSeparate(
            to_gl_blend_factor(state.src_rgb),
            to_gl_blend_factor(state.dst_rgb),
            to_gl_blend_factor(state.src_alpha),
            to_gl_blend_factor(state.dst_alpha));
        glBlendEquationSeparate(to_gl_blend_op(state.op_rgb), to_gl_blend_op(state.op_alpha));
    } else {
        glDisable(GL_BLEND);
    }
    cache_.blend = state;
    cache_.blend_valid = true;
}

void GlGraphicsDevice::set_depth_state(const DepthState& state) {
    if (cache_.depth_valid && cache_.depth == state) return;
    if (state.test_enabled) {
        if (!cache_.depth_valid || !cache_.depth.test_enabled) glEnable(GL_DEPTH_TEST);
        glDepthFunc(to_gl_compare(state.func));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    depth_write_enabled_ = state.write_enabled;
    glDepthMask(state.write_enabled ? GL_TRUE : GL_FALSE);
    cache_.depth = state;
    cache_.depth_valid = true;
}

void GlGraphicsDevice::set_cull_mode(CullMode mode) {
    if (cache_.cull_valid && cache_.cull == mode) return;
    if (mode == CullMode::None) {
        glDisable(GL_CULL_FACE);
    } else {
        if (!cache_.cull_valid || cache_.cull == CullMode::None) glEnable(GL_CULL_FACE);
        glCullFace(mode == CullMode::Front ? GL_FRONT : GL_BACK);
    }
    cache_.cull = mode;
    cache_.cull_valid = true;
}

void GlGraphicsDevice::set_color_mask(bool r, bool g, bool b, bool a) {
    const std::array<bool, 4> mask{r, g, b, a};
    if (cache_.color_mask_valid && cache_.color_mask == mask) return;
    glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
    cache_.color_mask = mask;
    cache_.color_mask_valid = true;
}

void GlGraphicsDevice::set_polygon_mode(i32 mode) {
    if (polygon_mode_ == mode && initialized_) return;
    polygon_mode_ = mode;
#if !GMRUNNER_USE_GLES3
    GLenum gl_mode = GL_FILL;
    if (mode <= 0) gl_mode = GL_POINT;
    else if (mode == 1) gl_mode = GL_LINE;
    glPolygonMode(GL_FRONT_AND_BACK, gl_mode);
#else
    (void)mode;
#endif
}

void GlGraphicsDevice::draw_primitives(PrimitiveTopology topo, i32 count, bool flat, u32 instances,
                                       u32 index_size, std::span<const u8> index_bytes,
                                       i32 first_vertex) {
    if (count <= 0 || instances == 0 || (index_size != 0 && index_size != 2 && index_size != 4)) return;
    constexpr GLenum gl_modes[]{GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    GLenum mode = gl_modes[static_cast<usize>(topo)];
    const bool reorder = caps_.is_gles && flat && topo != PrimitiveTopology::Points;
    if (reorder) {
        // ES 3.0/WebGL 2 always shade from the last vertex. Rotate each triangle
        // without changing its winding and move each line's first endpoint to the end.
        if (index_size && static_cast<usize>(count) > index_bytes.size() / index_size) return;
        auto emit = [&](i32 vertex) {
            u32 index = static_cast<u32>(vertex + first_vertex);
            if (index_size) {
                index = 0;
                std::memcpy(&index, index_bytes.data() + static_cast<usize>(vertex) * index_size, index_size);
            }
            flat_indices_.push_back(index);
        };
        flat_indices_.clear();
        if (topo == PrimitiveTopology::Lines || topo == PrimitiveTopology::LineStrip) {
            mode = GL_LINES;
            const i32 step = topo == PrimitiveTopology::Lines ? 2 : 1;
            for (i32 i = 0; i < count - 1; i += step) {
                emit(i + 1); emit(i);
            }
        } else {
            mode = GL_TRIANGLES;
            const i32 step = topo == PrimitiveTopology::Triangles ? 3 : 1;
            for (i32 i = 0; i < count - 2; i += step) {
                if (topo == PrimitiveTopology::TriangleFan) {
                    emit(i + 2); emit(0); emit(i + 1);
                } else if (topo == PrimitiveTopology::TriangleStrip && (i & 1)) {
                    emit(i + 2); emit(i + 1); emit(i);
                } else {
                    emit(i + 1); emit(i + 2); emit(i);
                }
            }
        }
        if (flat_indices_.empty()) return;
        if (!flat_index_buffer_) glGenBuffers(1, &flat_index_buffer_);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, flat_index_buffer_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(flat_indices_.size() * sizeof(u32)),
                     flat_indices_.data(), GL_STREAM_DRAW);
        index_size = sizeof(u32);
        count = static_cast<i32>(flat_indices_.size());
    }
    if (index_size) {
        const GLenum type = index_size == 4 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
        if (instances == 1) glDrawElements(mode, count, type, nullptr);
        else glDrawElementsInstanced(mode, count, type, nullptr, static_cast<GLsizei>(instances));
    } else {
        if (instances == 1) glDrawArrays(mode, first_vertex, count);
        else glDrawArraysInstanced(mode, first_vertex, count, static_cast<GLsizei>(instances));
    }
    if (reorder) glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void GlGraphicsDevice::set_depth_bias(f32 bias) {
    if (bias != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, bias * 65536.0f);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(0.0f, 0.0f);
    }
}

Result<TextureHandle> GlGraphicsDevice::create_texture(const TextureDesc& desc, std::span<const u8> initial_pixels) {
    if (desc.width <= 0 || desc.height <= 0) {
        return Error{"GlGraphicsDevice::create_texture invalid dimensions"};
    }

    GLuint id = 0;
    glGenTextures(1, &id);
    if (id == 0) {
        return Error{"glGenTextures failed"};
    }

    glActiveTexture(GL_TEXTURE0);
    cache_.active_unit = 0;
    glBindTexture(GL_TEXTURE_2D, id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    desc.min_filter == TextureFilter::Linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    desc.mag_filter == TextureFilter::Linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, to_gl_wrap(desc.wrap_u));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, to_gl_wrap(desc.wrap_v));

    GLint internal_fmt = GL_RGBA8;
    GLenum pixel_fmt = GL_RGBA;
    std::vector<u8> converted;
    const void* pixel_ptr = initial_pixels.empty() ? nullptr : initial_pixels.data();

    switch (desc.format) {
        case TextureFormat::RGBA8:
            internal_fmt = GL_RGBA8;
            pixel_fmt = GL_RGBA;
            break;
        case TextureFormat::BGRA8:
            internal_fmt = GL_RGBA8;
            pixel_fmt = GL_RGBA;
            if (!initial_pixels.empty()) {
                converted = bgra_to_rgba(initial_pixels);
                pixel_ptr = converted.data();
            }
            break;
        case TextureFormat::R8:
            internal_fmt = GL_R8;
            pixel_fmt = GL_RED;
            break;
        case TextureFormat::Depth24Stencil8:
            internal_fmt = GL_DEPTH24_STENCIL8;
            pixel_fmt = GL_DEPTH_STENCIL;
            break;
    }

    const GLenum pixel_type = (desc.format == TextureFormat::Depth24Stencil8) ? GL_UNSIGNED_INT_24_8 : GL_UNSIGNED_BYTE;
    glTexImage2D(GL_TEXTURE_2D, 0, internal_fmt, desc.width, desc.height, 0, pixel_fmt, pixel_type, pixel_ptr);
    glBindTexture(GL_TEXTURE_2D, 0);
    cache_.bound_tex_valid[0] = false;

    textures_[id] = TextureMeta{desc.width, desc.height};
    return TextureHandle{id};
}

void GlGraphicsDevice::update_texture(TextureHandle tex, i32 x, i32 y, i32 w, i32 h, TextureFormat format, std::span<const u8> pixels) {
    if (!tex.valid() || pixels.empty()) {
        return;
    }
    glActiveTexture(GL_TEXTURE0);
    cache_.active_unit = 0;
    glBindTexture(GL_TEXTURE_2D, tex.id);
    if (format == TextureFormat::BGRA8) {
        auto rgba = bgra_to_rgba(pixels);
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else if (format == TextureFormat::R8) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    cache_.bound_tex_valid[0] = false;
}

void GlGraphicsDevice::set_texture_sampler(u32 slot, TextureFilter filter, TextureWrap wrap_u, TextureWrap wrap_v) {
    auto d3d_wrap = [](TextureWrap w) {
        switch (w) {
            case TextureWrap::Repeat: return 1;
            case TextureWrap::MirroredRepeat: return 2;
            default: return 3;
        }
    };
    set_texture_sampler_ext(slot, filter, d3d_wrap(wrap_u), d3d_wrap(wrap_v), 0);
}

void GlGraphicsDevice::set_texture_sampler_ext(u32 slot, TextureFilter filter, i32 d3d_wrap_u, i32 d3d_wrap_v, u32 border_bgr) {
    if (slot >= sampler_ids_.size()) {
        return;
    }
    auto& cs = cache_.samplers[slot];
    if (cs.valid && cs.filter == filter && cs.wrap_u == d3d_wrap_u && cs.wrap_v == d3d_wrap_v &&
        cs.border_bgr == border_bgr) {
        return;
    }
    auto d3d_to_gl_wrap = [this](i32 mode) noexcept -> GLenum {
        switch (mode) {
            case 1: return GL_REPEAT;          // D3DTADDRESS_WRAP
            case 2: return GL_MIRRORED_REPEAT; // D3DTADDRESS_MIRROR
            case 3: return GL_CLAMP_TO_EDGE;   // D3DTADDRESS_CLAMP
            case 4: return border_clamp_supported_ ? kClampToBorder : GL_CLAMP_TO_EDGE;
            default: return GL_CLAMP_TO_EDGE;
        }
    };
    const GLint gl_filter = (filter == TextureFilter::Linear) ? GL_LINEAR : GL_NEAREST;
    const GLuint sampler = sampler_ids_[slot];
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, gl_filter);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, gl_filter);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, static_cast<GLint>(d3d_to_gl_wrap(d3d_wrap_u)));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, static_cast<GLint>(d3d_to_gl_wrap(d3d_wrap_v)));
    if (border_clamp_supported_ && (d3d_wrap_u == 4 || d3d_wrap_v == 4)) {
        const f32 bc[4] = {
            static_cast<f32>(border_bgr & 0xFFu) / 255.0f,
            static_cast<f32>((border_bgr >> 8u) & 0xFFu) / 255.0f,
            static_cast<f32>((border_bgr >> 16u) & 0xFFu) / 255.0f,
            1.0f,
        };
        glSamplerParameterfv(sampler, kTextureBorderColor, bc);
    }
    glBindSampler(slot, sampler);
    cs.valid = true;
    cs.filter = filter;
    cs.wrap_u = d3d_wrap_u;
    cs.wrap_v = d3d_wrap_v;
    cs.border_bgr = border_bgr;
}

void GlGraphicsDevice::bind_texture(u32 slot, TextureHandle tex) {
    const u32 gl_id = tex.valid() ? tex.id : white_texture_id_;
    if (slot < cache_.bound_tex.size() && cache_.bound_tex_valid[slot] && cache_.bound_tex[slot] == gl_id) {
        return;
    }
    if (cache_.active_unit != slot) {
        glActiveTexture(GL_TEXTURE0 + slot);
        cache_.active_unit = slot;
    }
    glBindTexture(GL_TEXTURE_2D, gl_id);
    if (slot < cache_.bound_tex.size()) {
        cache_.bound_tex[slot] = gl_id;
        cache_.bound_tex_valid[slot] = true;
    }
}

void GlGraphicsDevice::destroy_texture(TextureHandle tex) {
    if (!tex.valid()) {
        return;
    }
    for (usize i = 0; i < cache_.bound_tex.size(); ++i) {
        if (cache_.bound_tex_valid[i] && cache_.bound_tex[i] == tex.id) {
            cache_.bound_tex_valid[i] = false;
        }
    }
    const GLuint id = tex.id;
    glDeleteTextures(1, &id);
    textures_.erase(tex.id);
}

Result<FramebufferHandle> GlGraphicsDevice::create_framebuffer(std::span<const TextureHandle> color_targets, bool with_depth_stencil) {
    if (color_targets.empty()) {
        return Error{"GlGraphicsDevice::create_framebuffer requires at least one color attachment"};
    }
    const auto meta_it = textures_.find(color_targets[0].id);
    if (meta_it == textures_.end()) {
        return Error{"GlGraphicsDevice::create_framebuffer unknown color texture handle"};
    }

    GLuint fbo = 0;
    GLint previous_framebuffer = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_framebuffer);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    std::vector<GLenum> draw_buffers;
    draw_buffers.reserve(color_targets.size());
    for (usize i = 0; i < color_targets.size(); ++i) {
        const GLenum attachment = static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + i);
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, color_targets[i].id, 0);
        draw_buffers.push_back(attachment);
    }
    glDrawBuffers(static_cast<GLsizei>(draw_buffers.size()), draw_buffers.data());

    GLuint depth_rbo = 0;
    if (with_depth_stencil) {
        glGenRenderbuffers(1, &depth_rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, depth_rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, meta_it->second.width, meta_it->second.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth_rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous_framebuffer));

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (depth_rbo != 0) glDeleteRenderbuffers(1, &depth_rbo);
        glDeleteFramebuffers(1, &fbo);
        return Error{"GlGraphicsDevice::create_framebuffer incomplete framebuffer"};
    }

    const u32 init_mask = (color_targets.size() >= 32) ? 0xFFFFFFFFu : ((1u << color_targets.size()) - 1u);
    framebuffers_[fbo] = FramebufferEntry{fbo, depth_rbo, static_cast<u32>(color_targets.size()), init_mask};
    return FramebufferHandle{fbo};
}

void GlGraphicsDevice::bind_framebuffer(FramebufferHandle fbo) {
    const u32 gl_id = fbo.valid() ? fbo.id : 0u;
    if (cache_.fbo_valid && cache_.fbo == gl_id) return;
    glBindFramebuffer(GL_FRAMEBUFFER, gl_id);
    cache_.fbo = gl_id;
    cache_.fbo_valid = true;
}

void GlGraphicsDevice::set_framebuffer_mrt_target(FramebufferHandle fbo, u32 slot, TextureHandle tex) {
    if (!fbo.valid() || slot < 1 || slot > 3) return;
    auto it = framebuffers_.find(fbo.id);
    if (it == framebuffers_.end()) return;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.id);

    const GLenum attachment = static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + slot);
    if (tex.valid()) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, tex.id, 0);
        it->second.mrt_mask |= (1u << slot);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, 0, 0);
        it->second.mrt_mask &= ~(1u << slot);
    }

    GLenum bufs[4] = {
        GL_COLOR_ATTACHMENT0,
        static_cast<GLenum>((it->second.mrt_mask & 2u) ? GL_COLOR_ATTACHMENT1 : GL_NONE),
        static_cast<GLenum>((it->second.mrt_mask & 4u) ? GL_COLOR_ATTACHMENT2 : GL_NONE),
        static_cast<GLenum>((it->second.mrt_mask & 8u) ? GL_COLOR_ATTACHMENT3 : GL_NONE),
    };
    const GLsizei count = (it->second.mrt_mask > 1u) ? 4 : 1;
    glDrawBuffers(count, bufs);

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
}

bool GlGraphicsDevice::set_framebuffer_depth_from(FramebufferHandle dst_fbo, FramebufferHandle src_fbo) {
    if (!dst_fbo.valid() || !src_fbo.valid()) return false;
    auto dst_it = framebuffers_.find(dst_fbo.id);
    auto src_it = framebuffers_.find(src_fbo.id);
    if (dst_it == framebuffers_.end() || src_it == framebuffers_.end() || src_it->second.depth_rbo == 0) {
        return false;
    }

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo.id);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, src_it->second.depth_rbo);
    const bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    if (!ok && dst_it->second.depth_rbo != 0) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, dst_it->second.depth_rbo);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
    return ok;
}

void GlGraphicsDevice::destroy_framebuffer(FramebufferHandle fbo) {
    if (!fbo.valid()) {
        return;
    }
    if (cache_.fbo_valid && cache_.fbo == fbo.id) {
        cache_.fbo_valid = false;
    }
    const auto it = framebuffers_.find(fbo.id);
    if (it != framebuffers_.end()) {
        if (it->second.depth_rbo != 0) glDeleteRenderbuffers(1, &it->second.depth_rbo);
        if (it->second.fbo != 0) glDeleteFramebuffers(1, &it->second.fbo);
        framebuffers_.erase(it);
    }
}

Result<BufferHandle> GlGraphicsDevice::create_buffer(BufferTarget target, BufferUsage usage, std::span<const u8> initial_data) {
    GLuint id = 0;
    glGenBuffers(1, &id);
    if (id == 0) {
        return Error{"glGenBuffers failed"};
    }
    const GLenum gl_target = to_gl_buffer_target(target);
    glBindBuffer(gl_target, id);
    glBufferData(
        gl_target,
        static_cast<GLsizeiptr>(initial_data.size()),
        initial_data.empty() ? nullptr : initial_data.data(),
        to_gl_buffer_usage(usage));
    glBindBuffer(gl_target, 0);
    return BufferHandle{id};
}

void GlGraphicsDevice::update_buffer(BufferHandle buf, BufferTarget target, usize offset, std::span<const u8> data) {
    if (!buf.valid() || data.empty()) {
        return;
    }
    const GLenum gl_target = to_gl_buffer_target(target);
    glBindBuffer(gl_target, buf.id);
    glBufferSubData(gl_target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(data.size()), data.data());
    glBindBuffer(gl_target, 0);
}

void GlGraphicsDevice::destroy_buffer(BufferHandle buf) {
    if (!buf.valid()) {
        return;
    }
    const GLuint id = buf.id;
    glDeleteBuffers(1, &id);
}

Result<ShaderHandle> GlGraphicsDevice::create_shader(std::string_view vertex_glsl, std::string_view fragment_glsl) {
    auto vs_res = compile_gl_shader(GL_VERTEX_SHADER, vertex_glsl);
    if (!vs_res) return vs_res.error();

    auto fs_res = compile_gl_shader(GL_FRAGMENT_SHADER, fragment_glsl);
    if (!fs_res) {
        glDeleteShader(vs_res.value());
        return fs_res.error();
    }

    const GLuint program = glCreateProgram();
    glAttachShader(program, vs_res.value());
    glAttachShader(program, fs_res.value());
    glLinkProgram(program);

    glDeleteShader(vs_res.value());
    glDeleteShader(fs_res.value());

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        std::string info(static_cast<usize>(log_len > 0 ? log_len : 1), '\0');
        glGetProgramInfoLog(program, log_len, nullptr, info.data());
        glDeleteProgram(program);
        return Error{"GL shader link failed: " + info};
    }

    return ShaderHandle{program};
}

void GlGraphicsDevice::bind_shader(ShaderHandle shader) {
    const u32 gl_id = shader.valid() ? shader.id : 0u;
    if (cache_.program_valid && cache_.program == gl_id) return;
    glUseProgram(gl_id);
    cache_.program = gl_id;
    cache_.program_valid = true;
}

void GlGraphicsDevice::destroy_shader(ShaderHandle shader) {
    if (!shader.valid()) {
        return;
    }
    if (cache_.program_valid && cache_.program == shader.id) {
        cache_.program_valid = false;
    }
    glDeleteProgram(shader.id);
}

void GlGraphicsDevice::read_pixels_rgba8(i32 x, i32 y, i32 w, i32 h, std::span<u8> out_rgba) {
    if (w <= 0 || h <= 0) {
        return;
    }
    if (out_rgba.size() < static_cast<usize>(w) * static_cast<usize>(h) * 4u) {
        return;
    }
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out_rgba.data());
}

} // namespace gm
