#pragma once

#include "graphics/graphics_device.h"

#include <unordered_map>
#include <vector>

namespace gm {

class GlGraphicsDevice final : public IGraphicsDevice {
public:
    GlGraphicsDevice() = default;
    ~GlGraphicsDevice() override;

    [[nodiscard]] Result<void> init(GlProcLoader proc_loader) override;
    void shutdown() noexcept override;
    [[nodiscard]] const GraphicsCaps& caps() const noexcept override { return caps_; }

    void set_viewport(const ViewportRect& rect) override;
    void set_scissor(bool enabled, const ViewportRect& rect = {}) override;
    void clear(bool color, bool depth, bool stencil, f32 r, f32 g, f32 b, f32 a, f32 depth_val = 1.0f, i32 stencil_val = 0) override;
    void set_blend_state(const BlendState& state) override;
    void set_depth_state(const DepthState& state) override;
    void set_cull_mode(CullMode mode) override;
    void set_color_mask(bool r, bool g, bool b, bool a) override;

    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc& desc, std::span<const u8> initial_pixels = {}) override;
    void update_texture(TextureHandle tex, i32 x, i32 y, i32 w, i32 h, TextureFormat format, std::span<const u8> pixels) override;
    void set_texture_sampler(TextureHandle tex, TextureFilter filter, TextureWrap wrap_u, TextureWrap wrap_v) override;
    void bind_texture(u32 slot, TextureHandle tex) override;
    void destroy_texture(TextureHandle tex) override;

    [[nodiscard]] Result<FramebufferHandle> create_framebuffer(std::span<const TextureHandle> color_targets, bool with_depth_stencil = true) override;
    void bind_framebuffer(FramebufferHandle fbo) override;
    void destroy_framebuffer(FramebufferHandle fbo) override;

    [[nodiscard]] Result<BufferHandle> create_buffer(BufferTarget target, BufferUsage usage, std::span<const u8> initial_data = {}) override;
    void update_buffer(BufferHandle buf, BufferTarget target, usize offset, std::span<const u8> data) override;
    void destroy_buffer(BufferHandle buf) override;

    [[nodiscard]] Result<ShaderHandle> create_shader(std::string_view vertex_glsl, std::string_view fragment_glsl) override;
    void bind_shader(ShaderHandle shader) override;
    void destroy_shader(ShaderHandle shader) override;

    void read_pixels_rgba8(i32 x, i32 y, i32 w, i32 h, std::span<u8> out_rgba) override;

private:
    struct FramebufferEntry {
        u32 fbo = 0;
        u32 depth_rbo = 0;
        u32 color_count = 0;
    };

    struct TextureMeta {
        i32 width = 0;
        i32 height = 0;
    };

    bool initialized_ = false;
    GraphicsCaps caps_{};
    u32 white_texture_id_ = 0; // 1x1 white fallback texture (gm82dx9 SetNullTexture parity)

    std::unordered_map<u32, TextureMeta> textures_;
    std::unordered_map<u32, FramebufferEntry> framebuffers_;
};

} // namespace gm
