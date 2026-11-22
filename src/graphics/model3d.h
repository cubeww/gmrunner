#pragma once

#include "base/math.h"
#include "base/types.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace gm {

// 36-byte fixed-function 3D/2D vertex layout (mirrors D3D8 FVF=338: XYZ | NORMAL | TEX1 | DIFFUSE).
struct Vertex3D {
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 nx = 0.0f;
    f32 ny = 0.0f;
    f32 nz = 0.0f;
    f32 u = 0.0f;
    f32 v = 0.0f;
    u8 r = 255;
    u8 g = 255;
    u8 b = 255;
    u8 a = 255;
};

static_assert(sizeof(Vertex3D) == 36, "Vertex3D must be tightly packed 36 bytes");

// Exact GM8 fixed-function basic 3D shape builders (matching gm82.exe.c sub_565054..sub_566024).
// GM8.1+ records fans center-first and reverses odd strip triangles when storing a model.
// Immediate primitives and GM8.0 models use D3D's native provoking vertex.
void append_d3d_block(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hrepeat, f32 vrepeat,
    ColorRGBA col, bool recorded = false);

void append_d3d_wall(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hrepeat, f32 vrepeat,
    ColorRGBA col, bool recorded = false);

void append_d3d_floor(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hrepeat, f32 vrepeat,
    ColorRGBA col, bool recorded = false);

void append_d3d_cylinder(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hrepeat, f32 vrepeat,
    bool closed, i32 steps,
    ColorRGBA col, bool recorded = false);

void append_d3d_cone(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hrepeat, f32 vrepeat,
    bool closed, i32 steps,
    ColorRGBA col, bool recorded = false);

void append_d3d_ellipsoid(
    std::vector<Vertex3D>& out_tris,
    f32 x1, f32 y1, f32 z1,
    f32 x2, f32 y2, f32 z2,
    f32 hrepeat, f32 vrepeat,
    i32 steps,
    ColorRGBA col, bool recorded = false);

struct Model3DCommand {
    i32 op = 0; // GM8 model opcodes 0..15
    i32 repetitions = 1; // A missing suffix in a model file is a run of zero-initialized commands.
    std::array<f64, 10> p{};
};

struct Model3DMeshBucket {
    std::vector<Vertex3D> verts;
    std::vector<u8> explicit_color; // 1 if vertex has baked color/alpha, 0 if inherits draw_color/draw_alpha
    bool has_explicit = false;
    bool has_uncolored = false;
    u32 vao = 0;
    u32 vbo = 0;
    bool gpu_uploaded = false;
};

struct Model3DData {
    bool exists = false;
    i32 command_count = 0;
    std::vector<Model3DCommand> commands;
    bool dirty = true;
    // The compiled vertex order and packed colors depend on the runner version.
    bool recorded = false;

    Model3DMeshBucket triangles;
    Model3DMeshBucket lines;
    Model3DMeshBucket points;
};

class Model3DManager : public NonCopyable {
public:
    Model3DManager() = default;
    ~Model3DManager();

    void shutdown() noexcept;

    [[nodiscard]] i32 create();
    void destroy(i32 id);
    void clear(i32 id);
    [[nodiscard]] bool exists(i32 id) const noexcept;

    void add_command(i32 id, i32 op, const std::array<f64, 10>& params);
    bool load_from_text(i32 id, std::string_view text);
    [[nodiscard]] std::string save_to_text(i32 id) const;

    // Compiles recorded commands into flat geometry buckets and uploads static VBOs as needed.
    Model3DData* prepare_for_draw(i32 id, bool recorded = false);

private:
    static void release_bucket_gpu(Model3DMeshBucket& bucket) noexcept;
    static void compile_model(Model3DData& model);
    static void upload_bucket_if_static(Model3DMeshBucket& bucket);

    std::vector<Model3DData> models_;
};

} // namespace gm
