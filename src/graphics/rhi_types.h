#pragma once

#include "base/types.h"

#include <array>
#include <string>

namespace gm {

struct TextureHandle {
    u32 id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    constexpr bool operator==(const TextureHandle&) const noexcept = default;
};

struct BufferHandle {
    u32 id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    constexpr bool operator==(const BufferHandle&) const noexcept = default;
};

struct ShaderHandle {
    u32 id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    constexpr bool operator==(const ShaderHandle&) const noexcept = default;
};

struct FramebufferHandle {
    u32 id = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return id != 0; }
    constexpr bool operator==(const FramebufferHandle&) const noexcept = default;
};

enum class TextureFormat : u8 {
    RGBA8 = 0,
    BGRA8 = 1,
    R8 = 2,
    Depth24Stencil8 = 3,
};

enum class TextureFilter : u8 {
    Nearest = 0,
    Linear = 1,
};

enum class TextureWrap : u8 {
    ClampToEdge = 0,
    Repeat = 1,
    MirroredRepeat = 2,
};

enum class BlendFactor : u8 {
    Zero = 0,
    One,
    SrcColor,
    InvSrcColor,
    SrcAlpha,
    InvSrcAlpha,
    DstAlpha,
    InvDstAlpha,
    DstColor,
    InvDstColor,
    SrcAlphaSat,
};

enum class BlendOp : u8 {
    Add = 0,
    Subtract,
    ReverseSubtract,
    Min,
    Max,
};

enum class CompareFunc : u8 {
    Never = 0,
    Less,
    Equal,
    LessEqual,
    Greater,
    NotEqual,
    GreaterEqual,
    Always,
};

enum class CullMode : u8 {
    None = 0,
    Front,
    Back,
};

enum class PrimitiveTopology : u8 {
    Points = 0,
    Lines,
    LineStrip,
    Triangles,
    TriangleStrip,
    TriangleFan,
};

enum class BufferTarget : u8 {
    Vertex = 0,
    Index,
};

enum class BufferUsage : u8 {
    Static = 0,
    Dynamic,
    Stream,
};

struct BlendState {
    bool enabled = true;
    BlendFactor src_rgb = BlendFactor::SrcAlpha;
    BlendFactor dst_rgb = BlendFactor::InvSrcAlpha;
    BlendFactor src_alpha = BlendFactor::SrcAlpha;
    BlendFactor dst_alpha = BlendFactor::InvSrcAlpha;
    BlendOp op_rgb = BlendOp::Add;
    BlendOp op_alpha = BlendOp::Add;
    constexpr bool operator==(const BlendState&) const noexcept = default;
};

struct DepthState {
    bool test_enabled = false;
    bool write_enabled = true;
    CompareFunc func = CompareFunc::LessEqual;
    constexpr bool operator==(const DepthState&) const noexcept = default;
};

struct ViewportRect {
    i32 x = 0;
    i32 y = 0;
    i32 width = 0;
    i32 height = 0;
    constexpr bool operator==(const ViewportRect&) const noexcept = default;
};

struct TextureDesc {
    i32 width = 0;
    i32 height = 0;
    TextureFormat format = TextureFormat::RGBA8;
    TextureFilter min_filter = TextureFilter::Nearest;
    TextureFilter mag_filter = TextureFilter::Nearest;
    TextureWrap wrap_u = TextureWrap::ClampToEdge;
    TextureWrap wrap_v = TextureWrap::ClampToEdge;
};

struct GraphicsCaps {
    std::string vendor;
    std::string renderer;
    std::string version;
    std::string glsl_version;
    i32 max_texture_size = 0;
    i32 max_color_attachments = 1;
    i32 max_texture_units = 8;
    bool is_gles = false;
};

} // namespace gm
