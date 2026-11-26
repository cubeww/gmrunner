#pragma once

#include "graphics/graphics_device.h"

#include <array>
#include <unordered_map>
#include <vector>

namespace gm {

class GlGraphicsDevice final : public IGraphicsDevice {
public:
    GlGraphicsDevice() = default;
    ~GlGraphicsDevice() override;

    [[nodiscard]] Result<void> init(GlProcLoader proc_loader) override;
    void shutdown() noexcept override;
    void reset_cached_state() noexcept;
    [[nodiscard]] Result<void> reinit_after_context_loss(GlProcLoader proc_loader);
    [[nodiscard]] const GraphicsCaps& caps() const noexcept override { return caps_; }

    void set_viewport(const ViewportRect& rect) override;
    void set_scissor(bool enabled, const ViewportRect& rect = {}) override;
    [[nodiscard]] bool scissor_enabled() const noexcept { return cache_.scissor_valid && cache_.scissor_enabled; }
    [[nodiscard]] const ViewportRect& scissor_rect() const noexcept { return cache_.scissor_rect; }
    void clear(bool color, bool depth, bool stencil, f32 r, f32 g, f32 b, f32 a, f32 depth_val = 1.0f, i32 stencil_val = 0) override;
    void set_blend_state(const BlendState& state) override;
    void set_depth_state(const DepthState& state) override;
    void set_cull_mode(CullMode mode) override;
    void set_color_mask(bool r, bool g, bool b, bool a) override;
    void set_polygon_mode(i32 mode); // 0=point, 1=wireframe, 2=solid
    [[nodiscard]] i32 polygon_mode() const noexcept { return polygon_mode_; }
    void set_depth_bias(f32 bias);

    [[nodiscard]] Result<TextureHandle> create_texture(const TextureDesc& desc, std::span<const u8> initial_pixels = {}) override;
    void update_texture(TextureHandle tex, i32 x, i32 y, i32 w, i32 h, TextureFormat format, std::span<const u8> pixels) override;
    void set_texture_sampler(u32 slot, TextureFilter filter, TextureWrap wrap_u, TextureWrap wrap_v) override;
    void set_texture_sampler_ext(u32 slot, TextureFilter filter, i32 d3d_wrap_u, i32 d3d_wrap_v, u32 border_bgr);
    void bind_texture(u32 slot, TextureHandle tex) override;
    void destroy_texture(TextureHandle tex) override;

    [[nodiscard]] Result<FramebufferHandle> create_framebuffer(std::span<const TextureHandle> color_targets, bool with_depth_stencil = true) override;
    void copy_framebuffer_to_texture(TextureHandle dst, i32 x, i32 y, i32 xs, i32 ys, i32 w, i32 h);
    void bind_framebuffer(FramebufferHandle fbo) override;
    void set_framebuffer_mrt_target(FramebufferHandle fbo, u32 slot, TextureHandle tex);
    bool set_framebuffer_depth_from(FramebufferHandle dst_fbo, FramebufferHandle src_fbo);
    void destroy_framebuffer(FramebufferHandle fbo) override;

    [[nodiscard]] Result<BufferHandle> create_buffer(BufferTarget target, BufferUsage usage, std::span<const u8> initial_data = {}) override;
    void update_buffer(BufferHandle buf, BufferTarget target, usize offset, std::span<const u8> data) override;
    void destroy_buffer(BufferHandle buf) override;

    [[nodiscard]] Result<ShaderHandle> create_shader(std::string_view vertex_glsl, std::string_view fragment_glsl) override;
    void bind_shader(ShaderHandle shader) override;
    void destroy_shader(ShaderHandle shader) override;
    // A nonzero index size uses the element buffer of the bound VAO. GLES needs its
    // CPU indices only when translating D3D flat shading to last-vertex convention.
    void draw_primitives(PrimitiveTopology topology, i32 count, bool flat, u32 instances = 1,
                         u32 index_size = 0, std::span<const u8> index_bytes = {},
                         i32 first_vertex = 0);

    void read_pixels_rgba8(i32 x, i32 y, i32 w, i32 h, std::span<u8> out_rgba) override;

    [[nodiscard]] bool texture_exists(TextureHandle tex) const noexcept {
        return tex.valid() && textures_.contains(tex.id);
    }
    [[nodiscard]] i32 texture_width(TextureHandle tex) const noexcept {
        const auto it = textures_.find(tex.id);
        return it != textures_.end() ? it->second.width : 0;
    }
    [[nodiscard]] i32 texture_height(TextureHandle tex) const noexcept {
        const auto it = textures_.find(tex.id);
        return it != textures_.end() ? it->second.height : 0;
    }

    struct SavedRenderState {
        bool viewport_valid = false;
        ViewportRect viewport{};
        bool scissor_valid = false;
        bool scissor_enabled = false;
        ViewportRect scissor_rect{};
        bool blend_valid = false;
        BlendState blend{};
        bool depth_valid = false;
        DepthState depth{};
        bool cull_valid = false;
        CullMode cull = CullMode::None;
        bool color_mask_valid = false;
        std::array<bool, 4> color_mask{true, true, true, true};
        i32 polygon_mode = 2;
        bool program_valid = false;
        u32 program = 0;
    };

    [[nodiscard]] SavedRenderState save_render_state() const noexcept;
    void restore_render_state(const SavedRenderState& state);

private:
    struct FramebufferEntry {
        u32 fbo = 0;
        u32 depth_rbo = 0;
        u32 color_count = 0;
        u32 mrt_mask = 1; // bit 0..3 for GL_COLOR_ATTACHMENT0..3
    };

    struct TextureMeta {
        i32 width = 0;
        i32 height = 0;
    };

    struct CachedSampler {
        bool valid = false;
        TextureFilter filter = TextureFilter::Nearest;
        i32 wrap_u = 3;
        i32 wrap_v = 3;
        u32 border_bgr = 0;
    };

    struct CachedState {
        bool viewport_valid = false;
        ViewportRect viewport{};
        bool scissor_valid = false;
        bool scissor_enabled = false;
        ViewportRect scissor_rect{};
        bool blend_valid = false;
        BlendState blend{};
        bool depth_valid = false;
        DepthState depth{};
        bool cull_valid = false;
        CullMode cull = CullMode::None;
        bool color_mask_valid = false;
        std::array<bool, 4> color_mask{true, true, true, true};
        bool program_valid = false;
        u32 program = 0;
        bool fbo_valid = false;
        u32 fbo = 0;
        u32 active_unit = 0;
        std::array<u32, 12> bound_tex{};
        std::array<bool, 12> bound_tex_valid{};
        std::array<CachedSampler, 12> samplers{};
    };

    bool initialized_ = false;
    bool depth_write_enabled_ = true;
    i32 polygon_mode_ = 2;
    bool border_clamp_supported_ = false;
    std::array<u32, 12> sampler_ids_{}; // D3D pixel samplers 0..7 and vertex samplers 0..3.
    u32 flat_index_buffer_ = 0;
    std::vector<u32> flat_indices_;
    GraphicsCaps caps_{};
    u32 white_texture_id_ = 0; // 1x1 white fallback texture (gm82dx9 SetNullTexture parity)
    CachedState cache_{};

    std::unordered_map<u32, TextureMeta> textures_;
    std::unordered_map<u32, FramebufferEntry> framebuffers_;
};

} // namespace gm
