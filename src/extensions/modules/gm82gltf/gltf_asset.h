#pragma once

#include "base/types.h"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gm {

struct GltfTextureInfo {
    usize index = 0;
    usize tex_coord = 0;
};

struct GltfNormalTextureInfo {
    usize index = 0;
    usize tex_coord = 0;
    f32 scale = 1.0f;
};

struct GltfOcclusionTextureInfo {
    usize index = 0;
    usize tex_coord = 0;
    f32 strength = 1.0f;
};

struct GltfAccessor {
    struct Sparse {
        struct Indices {
            usize buffer_view = 0;
            usize byte_offset = 0;
            usize component_type = 0;
        };
        struct Values {
            usize buffer_view = 0;
            usize byte_offset = 0;
        };
        usize count = 0;
        Indices indices{};
        Values values{};
    };

    std::optional<usize> buffer_view;
    usize byte_offset = 0;
    usize component_type = 0;
    bool normalized = false;
    usize count = 0;
    std::string type;
    std::optional<Sparse> sparse;
    std::optional<std::string> name;
};

struct GltfAnimation {
    struct Channel {
        struct Target {
            std::optional<usize> node;
            std::string path;
        };
        usize sampler = 0;
        Target target{};
    };

    struct Sampler {
        usize input = 0;
        std::string interpolation = "LINEAR";
        usize output = 0;
    };

    std::vector<Channel> channels;
    std::vector<Sampler> samplers;
    std::optional<std::string> name;
};

struct GltfAssetInfo {
    std::optional<std::string> copyright;
    std::optional<std::string> generator;
    std::string version;
    std::optional<std::string> min_version;
};

struct GltfBuffer {
    std::optional<std::string> uri;
    usize byte_length = 0;
    std::optional<std::string> name;
};

struct GltfBufferView {
    usize buffer = 0;
    usize byte_offset = 0;
    usize byte_length = 0;
    std::optional<usize> byte_stride;
    std::optional<usize> target;
    std::optional<std::string> name;
};

struct GltfCamera {
    struct Orthographic {
        f32 xmag = 0.0f;
        f32 ymag = 0.0f;
        f32 zfar = 0.0f;
        f32 znear = 0.0f;
    };
    struct Perspective {
        f32 aspect_ratio = 0.0f;
        f32 yfov = 0.0f;
        f32 zfar = 0.0f;
        f32 znear = 0.0f;
    };

    std::optional<Orthographic> orthographic;
    std::optional<Perspective> perspective;
    std::string type;
};

struct GltfImage {
    std::optional<std::string> uri;
    std::optional<std::string> mime_type;
    std::optional<usize> buffer_view;
    std::optional<std::string> name;
};

struct GltfMaterial {
    struct PbrMetallicRoughness {
        std::array<f32, 4> base_color_factor{1.0f, 1.0f, 1.0f, 1.0f};
        std::optional<GltfTextureInfo> base_color_texture;
        f32 metallic_factor = 1.0f;
        f32 roughness_factor = 1.0f;
        std::optional<GltfTextureInfo> metallic_roughness_texture;
    };

    std::optional<std::string> name;
    PbrMetallicRoughness pbr{};
    std::optional<GltfNormalTextureInfo> normal_texture;
    std::optional<GltfOcclusionTextureInfo> occlusion_texture;
    std::optional<GltfTextureInfo> emissive_texture;
    // Padded to 4 floats so 16-byte uniform buffer reads stay in bounds.
    std::array<f32, 4> emissive_factor{0.0f, 0.0f, 0.0f, 1.0f};
    std::string alpha_mode = "OPAQUE";
    f32 alpha_cutoff = 0.5f;
    bool double_sided = false;
};

struct GltfMesh {
    using AttributeList = std::vector<std::pair<std::string, usize>>;

    struct Primitive {
        AttributeList attributes;
        std::optional<usize> indices;
        std::optional<usize> material;
        usize mode = 4;
        std::vector<AttributeList> targets;
    };

    std::vector<Primitive> primitives;
    std::vector<f32> weights;
    bool has_weights = false;
    std::optional<std::string> name;
};

struct GltfNode {
    std::optional<usize> camera;
    std::vector<usize> children;
    std::optional<usize> skin;
    std::optional<std::array<f32, 16>> matrix;
    std::optional<usize> mesh;
    std::array<f32, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
    std::array<f32, 3> scale{1.0f, 1.0f, 1.0f};
    std::array<f32, 3> translation{0.0f, 0.0f, 0.0f};
    std::optional<std::string> name;
    std::vector<f32> weights;
    bool has_weights = false;
    std::optional<usize> parent;
};

struct GltfSampler {
    std::optional<usize> mag_filter;
    std::optional<usize> min_filter;
    usize wrap_s = 10497;
    usize wrap_t = 10497;
    std::optional<std::string> name;
};

struct GltfScene {
    std::vector<usize> nodes;
    std::optional<std::string> name;
};

struct GltfSkin {
    std::optional<usize> inverse_bind_matrices;
    std::optional<usize> skeleton;
    std::vector<usize> joints;
    std::optional<std::string> name;
};

struct GltfTexture {
    std::optional<usize> sampler;
    std::optional<usize> source;
    std::optional<std::string> name;
};

struct GltfAsset {
    GltfAssetInfo asset{};
    std::vector<GltfAccessor> accessors;
    std::vector<GltfAnimation> animations;
    std::vector<GltfBuffer> buffers;
    std::vector<GltfBufferView> buffer_views;
    std::vector<GltfCamera> cameras;
    std::vector<GltfImage> images;
    std::vector<GltfMaterial> materials;
    std::vector<GltfMesh> meshes;
    std::vector<GltfNode> nodes;
    std::vector<GltfSampler> samplers;
    std::optional<usize> default_scene;
    std::vector<GltfScene> scenes;
    std::vector<GltfSkin> skins;
    std::vector<GltfTexture> textures;

    // Raw binary buffer payloads (index 0 is the GLB BIN chunk).
    std::vector<std::vector<u8>> buffer_data;
};

// Parses a binary glTF 2.0 (.glb) container into a self-contained GltfAsset.
[[nodiscard]] std::optional<GltfAsset> parse_glb(std::span<const u8> bytes);

// glTF 2.0 / gm82gltf math, accessor, and animation helpers.
[[nodiscard]] usize gltf_component_type_size(usize component_type) noexcept;
[[nodiscard]] usize gltf_type_component_count(std::string_view type) noexcept;
[[nodiscard]] usize gltf_accessor_packed_stride(const GltfAccessor& accessor) noexcept;
[[nodiscard]] usize gltf_accessor_effective_stride(const GltfAsset& gltf, const GltfAccessor& accessor) noexcept;
[[nodiscard]] usize gltf_accessor_byte_size(const GltfAsset& gltf, const GltfAccessor& accessor) noexcept;

[[nodiscard]] std::array<f32, 16> gltf_quat_to_matrix(const std::array<f32, 4>& q) noexcept;
[[nodiscard]] std::array<f32, 16> gltf_mul_mat4(const std::array<f32, 16>& a, const std::array<f32, 16>& b) noexcept;
[[nodiscard]] std::array<f32, 16> gltf_node_local_matrix(const GltfNode& node) noexcept;
[[nodiscard]] std::array<f32, 16> gltf_node_full_matrix(
    const GltfAsset& gltf,
    usize node_id,
    std::optional<usize> skeleton = std::nullopt) noexcept;

// Resolves a regular (non-sparse) accessor slice directly inside buffer_data.
[[nodiscard]] std::optional<std::span<const u8>> gltf_accessor_direct_span(
    const GltfAsset& gltf,
    usize accessor_id) noexcept;

// Materializes a sparse accessor into `out_data`. Returns true on success.
[[nodiscard]] bool gltf_materialize_sparse_accessor(
    const GltfAsset& gltf,
    usize accessor_id,
    std::vector<u8>& out_data);

// Resolves the encoded image bytes referenced by a texture.
[[nodiscard]] std::optional<std::span<const u8>> gltf_texture_image_bytes(
    const GltfAsset& gltf,
    usize texture_id) noexcept;

// Applies an animation at `time` (seconds). Returns true when all channels are past their last keyframe.
[[nodiscard]] std::optional<bool> gltf_apply_animation(
    GltfAsset& gltf,
    usize animation_id,
    f64 time);

// Returns the maximum keyframe timestamp (seconds) across all channels of an animation.
[[nodiscard]] std::optional<f64> gltf_compute_animation_length(
    const GltfAsset& gltf,
    usize animation_id) noexcept;

// Computes all joint matrices (inverseBindMatrix * fullNodeTransform) for a skin.
[[nodiscard]] bool gltf_compute_skin_matrices(
    const GltfAsset& gltf,
    usize skin_id,
    std::vector<std::array<f32, 16>>& out_matrices);

} // namespace gm
