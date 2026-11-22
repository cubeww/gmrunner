#pragma once

#include "base/types.h"
#include "graphics/rhi_types.h"

#include <span>
#include <string_view>

namespace gm {

using GlProcLoader = void* (*)(const char*);

// Unified cross-platform graphics device interface (OpenGL 3.3 Core / GLES 3.0 / WebGL 2.0).
class IGraphicsDevice : public NonCopyable {
public:
    virtual ~IGraphicsDevice() = default;

    [[nodiscard]] virtual Result<void> init(GlProcLoader proc_loader) = 0;
    virtual void shutdown() noexcept = 0;
    [[nodiscard]] virtual const GraphicsCaps& caps() const noexcept = 0;

    // Frame & raster state
    virtual void set_viewport(const ViewportRect& rect) = 0;
    virtual void set_scissor(bool enabled, const ViewportRect& rect = {}) = 0;
    virtual void clear(bool color, bool depth, bool stencil, f32 r, f32 g, f32 b, f32 a, f32 depth_val = 1.0f, i32 stencil_val = 0) = 0;
    virtual void set_blend_state(const BlendState& state) = 0;
    virtual void set_depth_state(const DepthState& state) = 0;
    virtual void set_cull_mode(CullMode mode) = 0;
    virtual void set_color_mask(bool r, bool g, bool b, bool a) = 0;

    // Textures
    [[nodiscard]] virtual Result<TextureHandle> create_texture(const TextureDesc& desc, std::span<const u8> initial_pixels = {}) = 0;
    virtual void update_texture(TextureHandle tex, i32 x, i32 y, i32 w, i32 h, TextureFormat format, std::span<const u8> pixels) = 0;
    virtual void set_texture_sampler(u32 slot, TextureFilter filter, TextureWrap wrap_u, TextureWrap wrap_v) = 0;
    virtual void bind_texture(u32 slot, TextureHandle tex) = 0;
    virtual void destroy_texture(TextureHandle tex) = 0;

    // Framebuffers (Surfaces & MRT)
    [[nodiscard]] virtual Result<FramebufferHandle> create_framebuffer(std::span<const TextureHandle> color_targets, bool with_depth_stencil = true) = 0;
    virtual void bind_framebuffer(FramebufferHandle fbo) = 0; // invalid handle binds default backbuffer
    virtual void destroy_framebuffer(FramebufferHandle fbo) = 0;

    // Buffers
    [[nodiscard]] virtual Result<BufferHandle> create_buffer(BufferTarget target, BufferUsage usage, std::span<const u8> initial_data = {}) = 0;
    virtual void update_buffer(BufferHandle buf, BufferTarget target, usize offset, std::span<const u8> data) = 0;
    virtual void destroy_buffer(BufferHandle buf) = 0;

    // Shaders
    [[nodiscard]] virtual Result<ShaderHandle> create_shader(std::string_view vertex_glsl, std::string_view fragment_glsl) = 0;
    virtual void bind_shader(ShaderHandle shader) = 0;
    virtual void destroy_shader(ShaderHandle shader) = 0;

    // Pixel readback (RGBA8, bottom-up or flipped as requested)
    virtual void read_pixels_rgba8(i32 x, i32 y, i32 w, i32 h, std::span<u8> out_rgba) = 0;
};

} // namespace gm
