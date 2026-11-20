#include "graphics/gl_graphics_device.h"

#include "base/log.h"
#include "graphics/gl_headers.h"

#include <vector>

namespace gm {

namespace {

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

    if (white_texture_id_ != 0) {
        glDeleteTextures(1, &white_texture_id_);
        white_texture_id_ = 0;
    }

    initialized_ = false;
}

void GlGraphicsDevice::set_viewport(const ViewportRect& rect) {
    glViewport(rect.x, rect.y, rect.width, rect.height);
}

void GlGraphicsDevice::set_scissor(bool enabled, const ViewportRect& rect) {
    if (enabled) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(rect.x, rect.y, rect.width, rect.height);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
}

void GlGraphicsDevice::clear(bool color, bool depth, bool stencil, f32 r, f32 g, f32 b, f32 a, f32 depth_val, i32 stencil_val) {
    GLbitfield mask = 0;
    if (color) {
        glClearColor(r, g, b, a);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) {
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
}

void GlGraphicsDevice::set_blend_state(const BlendState& state) {
    if (state.enabled) {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(
            to_gl_blend_factor(state.src_rgb),
            to_gl_blend_factor(state.dst_rgb),
            to_gl_blend_factor(state.src_alpha),
            to_gl_blend_factor(state.dst_alpha));
        glBlendEquationSeparate(to_gl_blend_op(state.op_rgb), to_gl_blend_op(state.op_alpha));
    } else {
        glDisable(GL_BLEND);
    }
}

void GlGraphicsDevice::set_depth_state(const DepthState& state) {
    if (state.test_enabled) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(to_gl_compare(state.func));
    } else {
        glDisable(GL_DEPTH_TEST);
    }
    glDepthMask(state.write_enabled ? GL_TRUE : GL_FALSE);
}

void GlGraphicsDevice::set_cull_mode(CullMode mode) {
    if (mode == CullMode::None) {
        glDisable(GL_CULL_FACE);
    } else {
        glEnable(GL_CULL_FACE);
        glCullFace(mode == CullMode::Front ? GL_FRONT : GL_BACK);
    }
}

void GlGraphicsDevice::set_color_mask(bool r, bool g, bool b, bool a) {
    glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE, b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
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

    textures_[id] = TextureMeta{desc.width, desc.height};
    return TextureHandle{id};
}

void GlGraphicsDevice::update_texture(TextureHandle tex, i32 x, i32 y, i32 w, i32 h, TextureFormat format, std::span<const u8> pixels) {
    if (!tex.valid() || pixels.empty()) {
        return;
    }
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
}

void GlGraphicsDevice::set_texture_sampler(TextureHandle tex, TextureFilter filter, TextureWrap wrap_u, TextureWrap wrap_v) {
    if (!tex.valid()) {
        return;
    }
    const GLint gl_filter = (filter == TextureFilter::Linear) ? GL_LINEAR : GL_NEAREST;
    glBindTexture(GL_TEXTURE_2D, tex.id);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gl_filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, to_gl_wrap(wrap_u));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, to_gl_wrap(wrap_v));
    glBindTexture(GL_TEXTURE_2D, 0);
}

void GlGraphicsDevice::bind_texture(u32 slot, TextureHandle tex) {
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(GL_TEXTURE_2D, tex.valid() ? tex.id : white_texture_id_);
}

void GlGraphicsDevice::destroy_texture(TextureHandle tex) {
    if (!tex.valid()) {
        return;
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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (status != GL_FRAMEBUFFER_COMPLETE) {
        if (depth_rbo != 0) glDeleteRenderbuffers(1, &depth_rbo);
        glDeleteFramebuffers(1, &fbo);
        return Error{"GlGraphicsDevice::create_framebuffer incomplete framebuffer"};
    }

    framebuffers_[fbo] = FramebufferEntry{fbo, depth_rbo, static_cast<u32>(color_targets.size())};
    return FramebufferHandle{fbo};
}

void GlGraphicsDevice::bind_framebuffer(FramebufferHandle fbo) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.valid() ? fbo.id : 0u);
}

void GlGraphicsDevice::destroy_framebuffer(FramebufferHandle fbo) {
    if (!fbo.valid()) {
        return;
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
    glUseProgram(shader.valid() ? shader.id : 0u);
}

void GlGraphicsDevice::destroy_shader(ShaderHandle shader) {
    if (!shader.valid()) {
        return;
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
