#pragma once

#include "base/math.h"
#include "base/types.h"
#include "graphics/gl_graphics_device.h"
#include "loader/game_data.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

struct SurfaceObject {
    i32 width = 0;
    i32 height = 0;
    TextureHandle texture;
    FramebufferHandle fbo;
};

struct SpriteGpu {
    std::vector<TextureHandle> subimages;
};

struct FontGpu {
    TextureHandle texture;
    i32 tex_width = 0;
    i32 tex_height = 0;
    i32 line_height = 12;
    bool is_sprite_font = false;
    i32 sprite_index = -1;
    i32 sprite_first = 32;
    bool sprite_prop = false;
    i32 sprite_sep = 0;
};

struct TextTransform {
    f32 xscale = 1.0f;
    f32 yscale = 1.0f;
    f32 angle_deg = 0.0f;
};

class Renderer2D : public NonCopyable {
public:
    Renderer2D() = default;
    ~Renderer2D();

    [[nodiscard]] Result<void> init(GlGraphicsDevice& device);
    void shutdown() noexcept;

    // Asset GPU uploads (also supports incremental dynamic sprite/bg/font additions)
    void upload_game_assets(const GameData& game);
    void upload_sprite(usize sprite_idx, const SpriteData& spr);
    void free_sprite_textures(usize sprite_idx);
    void upload_background(usize bg_idx, const BackgroundData& bg);
    void free_background_texture(usize bg_idx);
    void upload_font(usize font_idx, const FontData& font, VersionMode version);
    void register_sprite_font(usize font_idx, i32 sprite_idx, i32 first_char, bool prop, i32 sep, const GameData& game);

    // Frame & camera control
    void begin_frame(i32 backbuffer_w, i32 backbuffer_h);
    // Logical GM screen and its top-down drawable viewport, including window scaling.
    void set_screen_region(i32 backbuffer_w, i32 backbuffer_h, i32 width, i32 height, const ViewportRect& viewport);
    // Ensures internal logical screen FBO exists and binds it (unless a user surface target is active).
    void begin_screen_pass(i32 canvas_w, i32 canvas_h, i32 backbuffer_w, i32 backbuffer_h, const ViewportRect& present_viewport);
    // Presents the internal screen FBO onto the window backbuffer with letterbox border color.
    void present_screen(u32 border_color_bgr);
    void end_frame();
    void flush();

    // Sets viewport + orthographic camera. If rendering to an FBO, uses FBO Y-orientation.
    void set_projection_ortho(
        f32 view_x, f32 view_y, f32 view_w, f32 view_h, f32 angle_deg,
        i32 port_x, i32 port_y, i32 port_w, i32 port_h);

    void clear_color(u32 gm_bgr, f32 alpha = 1.0f);
    void clear_depth(f32 depth_val = 1.0f);

    // Drawing state
    void set_color(u32 gm_bgr) noexcept { draw_color_ = gm_bgr & 0x00FFFFFFu; }
    [[nodiscard]] u32 color() const noexcept { return draw_color_; }
    void set_alpha(f32 alpha) noexcept { draw_alpha_ = alpha; }
    [[nodiscard]] f32 alpha() const noexcept { return draw_alpha_; }
    void set_font(i32 font_id) noexcept { draw_font_ = font_id; }
    [[nodiscard]] i32 font() const noexcept { return draw_font_; }
    [[nodiscard]] bool using_sprite_font() const noexcept {
        return draw_font_ >= 0 && static_cast<usize>(draw_font_) < fonts_.size() &&
               fonts_[static_cast<usize>(draw_font_)].sprite_index >= 0;
    }
    void set_halign(i32 halign) noexcept { draw_halign_ = halign; }
    void set_valign(i32 valign) noexcept { draw_valign_ = valign; }
    void set_circle_precision(i32 prec) noexcept { circle_precision_ = (prec >= 4 && prec <= 64) ? prec : 24; }
    [[nodiscard]] i32 circle_precision() const noexcept { return circle_precision_; }

    void set_blend_mode(i32 bm_mode);
    void set_blend_mode_ext(i32 src_bm, i32 dst_bm);
    void set_interpolation(bool linear);
    void set_texture_repeat(bool repeat);
    void set_alpha_test(bool enabled, u8 ref_val);
    void set_color_write_enable(bool r, bool g, bool b, bool a);

    // Surfaces (FBOs)
    [[nodiscard]] i32 surface_create(i32 width, i32 height);
    void surface_free(i32 id);
    [[nodiscard]] bool surface_exists(i32 id) const noexcept;
    [[nodiscard]] i32 surface_width(i32 id) const noexcept;
    [[nodiscard]] i32 surface_height(i32 id) const noexcept;
    bool surface_set_target(i32 id);
    bool surface_reset_target();
    [[nodiscard]] i32 current_surface_target() const noexcept { return active_surface_id_; }
    [[nodiscard]] bool rendering_to_fbo() const noexcept { return active_surface_id_ >= 0 || using_screen_fbo_; }
    void draw_surface_ext(i32 id, f32 x, f32 y, f32 xscale, f32 yscale, f32 rot_deg, u32 color_bgr, f32 alpha);
    void draw_surface_part_ext(i32 id, f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha);
    void draw_surface_stretched_ext(i32 id, f32 x, f32 y, f32 w, f32 h, u32 color_bgr, f32 alpha);
    void draw_surface_tiled_ext(i32 id, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha,
                                f32 view_x, f32 view_y, f32 view_w, f32 view_h);
    void draw_surface_general(i32 id, f32 left, f32 top, f32 w, f32 h, f32 x, f32 y,
                              f32 xscale, f32 yscale, f32 rot_deg,
                              u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha);
    bool surface_save(i32 id, std::string_view host_path);
    bool surface_save_part(i32 id, std::string_view host_path, i32 x, i32 y, i32 w, i32 h);
    bool screen_save(std::string_view host_path);
    bool screen_save_part(std::string_view host_path, i32 x, i32 y, i32 w, i32 h);
    bool surface_read_pixels_bgra(i32 id, i32 x, i32 y, i32 w, i32 h, std::vector<u8>& out_bgra);
    bool screen_read_pixels_bgra(i32 x, i32 y, i32 w, i32 h, std::vector<u8>& out_bgra);
    [[nodiscard]] u32 surface_getpixel(i32 id, i32 x, i32 y);
    [[nodiscard]] u32 screen_getpixel(i32 x, i32 y);
    void surface_copy_part(i32 dst_id, f32 x, f32 y, i32 src_id, f32 xs, f32 ys, f32 ws, f32 hs);
    [[nodiscard]] static bool save_bitmap_bgra(const Bitmap32& bmp, std::string_view host_path);

    // Sprites, Backgrounds & Tiles
    void draw_sprite_ext(
        const GameData& game, i32 sprite_id, i32 subimg,
        f32 x, f32 y, f32 xscale, f32 yscale, f32 rot_deg, u32 color_bgr, f32 alpha);
    void draw_sprite_part_ext(
        const GameData& game, i32 sprite_id, i32 subimg,
        f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha);
    void draw_sprite_stretched_ext(
        const GameData& game, i32 sprite_id, i32 subimg,
        f32 x, f32 y, f32 w, f32 h, u32 color_bgr, f32 alpha);
    void draw_sprite_tiled_ext(
        const GameData& game, i32 sprite_id, i32 subimg,
        f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha,
        f32 view_x, f32 view_y, f32 view_w, f32 view_h);
    void draw_sprite_general(
        const GameData& game, i32 sprite_id, i32 subimg,
        f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale,
        f32 rot_deg, u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha);
    void draw_sprite_pos(
        const GameData& game, i32 sprite_id, i32 subimg,
        f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3, f32 x4, f32 y4, f32 alpha);

    void draw_background_ext(
        const GameData& game, i32 bg_id,
        f32 x, f32 y, f32 xscale, f32 yscale, f32 rot_deg, u32 color_bgr, f32 alpha);
    void draw_background_part_ext(
        const GameData& game, i32 bg_id,
        f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha);
    void draw_background_stretched_ext(
        const GameData& game, i32 bg_id,
        f32 x, f32 y, f32 w, f32 h, u32 color_bgr, f32 alpha);
    void draw_background_tiled_ext(
        const GameData& game, i32 bg_id,
        f32 x, f32 y, f32 xscale, f32 yscale, u32 color_bgr, f32 alpha,
        f32 view_x, f32 view_y, f32 view_w, f32 view_h, bool htiled, bool vtiled);
    void draw_background_general(
        const GameData& game, i32 bg_id,
        f32 left, f32 top, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale,
        f32 rot_deg, u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha);

    void draw_tile(
        const GameData& game, i32 bg_id,
        f32 xo, f32 yo, f32 w, f32 h, f32 x, f32 y, f32 xscale, f32 yscale, u32 blend_bgr, f32 alpha);

    // Text & Fonts
    [[nodiscard]] f32 measure_string_width(const GameData& game, std::string_view text) const;
    [[nodiscard]] f32 measure_string_height(const GameData& game, std::string_view text, i32 sep = -1, i32 max_w = -1) const;
    void draw_text_ext_color(
        const GameData& game, f32 x, f32 y, std::string_view text,
        i32 sep, i32 max_w, u32 c1, u32 c2, u32 c3, u32 c4, f32 alpha,
        std::optional<TextTransform> transform = std::nullopt);

    // Primitives
    void draw_point_color(f32 x, f32 y, u32 color_bgr, f32 alpha);
    void draw_line_color(f32 x1, f32 y1, f32 x2, f32 y2, u32 c1, u32 c2, f32 alpha);
    void draw_line_width_color(f32 x1, f32 y1, f32 x2, f32 y2, f32 width, u32 c1, u32 c2, f32 alpha);
    void draw_rectangle_color(f32 x1, f32 y1, f32 x2, f32 y2, u32 c1, u32 c2, u32 c3, u32 c4, bool outline, f32 alpha);
    void draw_triangle_color(f32 x1, f32 y1, f32 x2, f32 y2, f32 x3, f32 y3, u32 c1, u32 c2, u32 c3, bool outline, f32 alpha);
    void draw_ellipse_color(f32 x1, f32 y1, f32 x2, f32 y2, u32 c1, u32 c2, bool outline, f32 alpha);
    void draw_circle_color(f32 x, f32 y, f32 r, u32 c1, u32 c2, bool outline, f32 alpha);
    void draw_arrow(f32 x1, f32 y1, f32 x2, f32 y2, f32 tip_size, u32 color_bgr, f32 alpha);
    void draw_button(f32 x1, f32 y1, f32 x2, f32 y2, bool up, u32 color_bgr, f32 alpha);

    // Immediate primitive builder (draw_primitive_begin / draw_vertex* / draw_primitive_end & d3d_primitive_*)
    void primitive_begin(i32 kind, TextureHandle tex = {});
    void primitive_vertex(f32 x, f32 y, f32 u, f32 v, u32 color_bgr, f32 alpha);
    void primitive_vertex_3d(f32 x, f32 y, f32 z, f32 u, f32 v, u32 color_bgr, f32 alpha);
    void primitive_end();

    // Fixed-function D3D projection, transforms & 3D primitives (used by GM8 2D HUD overlays & 3D scenes)
    void d3d_start();
    void d3d_end();
    [[nodiscard]] bool d3d_active() const noexcept { return d3d_active_; }
    void d3d_set_perspective(bool enable);
    void d3d_set_hidden(bool enable);
    void d3d_set_depth(f32 depth) noexcept { current_draw_z_ = depth; }
    void d3d_set_zwriteenable(bool enable);
    void d3d_set_culling(bool enable);
    void d3d_set_projection(f32 xfrom, f32 yfrom, f32 zfrom, f32 xto, f32 yto, f32 zto, f32 xup, f32 yup, f32 zup);
    void d3d_set_projection_ext(f32 xfrom, f32 yfrom, f32 zfrom, f32 xto, f32 yto, f32 zto,
                                f32 xup, f32 yup, f32 zup, f32 angle_deg, f32 aspect, f32 znear, f32 zfar);
    void d3d_set_projection_ortho(f32 x, f32 y, f32 w, f32 h, f32 angle_deg);
    void d3d_set_projection_perspective(f32 x, f32 y, f32 w, f32 h, f32 angle_deg);
    void d3d_transform_set_identity();
    void d3d_transform_set_translation(f32 xt, f32 yt, f32 zt);
    void d3d_transform_set_scaling(f32 xs, f32 ys, f32 zs);
    void d3d_transform_set_rotation_x(f32 angle_deg);
    void d3d_transform_set_rotation_y(f32 angle_deg);
    void d3d_transform_set_rotation_z(f32 angle_deg);
    void d3d_transform_set_rotation_axis(f32 xa, f32 ya, f32 za, f32 angle_deg);
    void d3d_transform_add_translation(f32 xt, f32 yt, f32 zt);
    void d3d_transform_add_scaling(f32 xs, f32 ys, f32 zs);
    void d3d_transform_add_rotation_x(f32 angle_deg);
    void d3d_transform_add_rotation_y(f32 angle_deg);
    void d3d_transform_add_rotation_z(f32 angle_deg);
    void d3d_transform_add_rotation_axis(f32 xa, f32 ya, f32 za, f32 angle_deg);
    void d3d_transform_stack_clear();
    [[nodiscard]] bool d3d_transform_stack_empty() const noexcept { return transform_stack_.empty(); }
    bool d3d_transform_stack_push();
    bool d3d_transform_stack_pop();
    bool d3d_transform_stack_top();
    bool d3d_transform_stack_discard();
    void d3d_draw_block(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hrepeat, f32 vrepeat);
    void d3d_draw_floor(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hrepeat, f32 vrepeat);
    void d3d_draw_wall(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hrepeat, f32 vrepeat);
    void d3d_draw_cylinder(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hrepeat, f32 vrepeat, bool closed, i32 steps);
    void d3d_draw_cone(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hrepeat, f32 vrepeat, bool closed, i32 steps);
    void d3d_draw_ellipsoid(f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2, TextureHandle tex, f32 hrepeat, f32 vrepeat, i32 steps);

    [[nodiscard]] TextureHandle sprite_texture(i32 sprite_id, i32 subimg) const noexcept;
    [[nodiscard]] TextureHandle background_texture(i32 bg_id) const noexcept;
    [[nodiscard]] TextureHandle surface_texture(i32 surf_id) const noexcept;
    [[nodiscard]] bool texture_exists(TextureHandle tex) const noexcept { return device_ && device_->texture_exists(tex); }
    [[nodiscard]] i32 texture_width(TextureHandle tex) const noexcept { return device_ ? device_->texture_width(tex) : 0; }
    [[nodiscard]] i32 texture_height(TextureHandle tex) const noexcept { return device_ ? device_->texture_height(tex) : 0; }

    // Decodes PNG/BMP/JPG/GIF image bytes from disk into BGRA8 Bitmap32.
    [[nodiscard]] static Result<Bitmap32> decode_image_to_bgra(std::span<const u8> encoded_bytes);

private:
    struct Vertex2D {
        f32 x, y, z;
        f32 u, v;
        u8 r, g, b, a;
    };

    void ensure_batch(PrimitiveTopology topo, TextureHandle tex);
    void ensure_screen_fbo(i32 width, i32 height);
    void bind_active_target_fbo();
    void apply_d3d_depth_cull();
    void push_quad(
        TextureHandle tex,
        f32 x0, f32 y0, f32 u0, f32 v0, ColorRGBA c0,
        f32 x1, f32 y1, f32 u1, f32 v1, ColorRGBA c1,
        f32 x2, f32 y2, f32 u2, f32 v2, ColorRGBA c2,
        f32 x3, f32 y3, f32 u3, f32 v3, ColorRGBA c3);

    void create_builtin_font_texture();
    void apply_screen_projection();
    std::vector<std::string> split_gm_lines(const GameData& game, std::string_view text, f32 max_w) const;
    f32 measure_single_line_width(const GameData& game, std::string_view line) const;
    f32 current_font_line_height(const GameData& game) const;

    GlGraphicsDevice* device_ = nullptr;
    bool initialized_ = false;

    u32 vao_ = 0;
    u32 vbo_ = 0;
    ShaderHandle shader_;
    i32 loc_proj_ = -1;
    i32 loc_tex_ = -1;
    i32 loc_alpha_test_ = -1;
    i32 loc_alpha_ref_ = -1;

    TextureHandle builtin_font_tex_;

    std::vector<Vertex2D> vertices_;
    PrimitiveTopology batch_topo_ = PrimitiveTopology::Triangles;
    TextureHandle batch_tex_{};
    BlendState blend_state_{};
    bool linear_filter_ = false;
    bool tex_repeat_ = false;
    bool alpha_test_enabled_ = false;
    f32 alpha_test_ref_ = 0.0f;

    f32 proj_matrix_[16]{};
    f32 model_matrix_[16]{};
    std::vector<std::array<f32, 16>> transform_stack_;
    f32 current_draw_z_ = 0.0f;
    bool d3d_active_ = false;
    bool d3d_perspective_ = false;
    bool d3d_hidden_ = false;
    bool d3d_zwrite_ = true;
    bool d3d_culling_ = false;

    i32 backbuffer_w_ = 800;
    i32 backbuffer_h_ = 608;
    i32 screen_w_ = 800;
    i32 screen_h_ = 608;
    ViewportRect screen_viewport_{0, 0, 800, 608};
    ViewportRect active_viewport_{0, 0, 800, 608};

    // Persistent logical-resolution screen FBO (for crisp screen captures & transitions)
    TextureHandle screen_fbo_tex_{};
    FramebufferHandle screen_fbo_{};
    i32 screen_fbo_w_ = 0;
    i32 screen_fbo_h_ = 0;
    bool using_screen_fbo_ = false;

    u32 draw_color_ = 0x000000u; // GM8 default draw color is c_black (0)
    f32 draw_alpha_ = 1.0f;
    i32 draw_font_ = -1;
    i32 draw_halign_ = 0;
    i32 draw_valign_ = 0;
    i32 circle_precision_ = 24;

    bool in_primitive_ = false;
    i32 prim_kind_ = 4; // pr_trianglelist
    TextureHandle prim_tex_{};
    std::vector<Vertex2D> prim_verts_;

    std::vector<SpriteGpu> sprites_;
    std::vector<TextureHandle> backgrounds_;
    std::vector<FontGpu> fonts_;

    std::unordered_map<i32, SurfaceObject> surfaces_;
    i32 next_surface_id_ = 0;
    i32 active_surface_id_ = -1;
};

} // namespace gm
