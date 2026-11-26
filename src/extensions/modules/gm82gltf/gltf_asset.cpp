#include "extensions/modules/gm82gltf/gltf_asset.h"

#include "base/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <variant>

namespace gm {

namespace {

// ---------------------------------------------------------------------------
// Minimal RFC 8259 JSON parser preserving object key insertion order
// ---------------------------------------------------------------------------

struct JsonValue;
using JsonArray  = std::vector<JsonValue>;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    std::variant<std::nullptr_t, bool, f64, std::string, JsonArray, JsonObject> data{nullptr};

    [[nodiscard]] bool is_bool() const noexcept { return std::holds_alternative<bool>(data); }
    [[nodiscard]] bool is_number() const noexcept { return std::holds_alternative<f64>(data); }
    [[nodiscard]] bool is_string() const noexcept { return std::holds_alternative<std::string>(data); }
    [[nodiscard]] bool is_array() const noexcept { return std::holds_alternative<JsonArray>(data); }
    [[nodiscard]] bool is_object() const noexcept { return std::holds_alternative<JsonObject>(data); }

    [[nodiscard]] bool as_bool() const noexcept { return std::get<bool>(data); }
    [[nodiscard]] f64 as_number() const noexcept { return std::get<f64>(data); }
    [[nodiscard]] const std::string& as_string() const noexcept { return std::get<std::string>(data); }
    [[nodiscard]] const JsonArray& as_array() const noexcept { return std::get<JsonArray>(data); }
    [[nodiscard]] const JsonObject& as_object() const noexcept { return std::get<JsonObject>(data); }

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept {
        if (!is_object()) return nullptr;
        for (const auto& [k, v] : as_object()) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

void append_utf8(std::string& out, u32 cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6u)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0x10FFFFu) {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18u)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view src) noexcept : src_(src) {}

    [[nodiscard]] std::optional<JsonValue> parse() {
        skip_ws();
        auto val = parse_value(0);
        if (!val) return std::nullopt;
        skip_ws_and_nul();
        if (pos_ != src_.size()) return std::nullopt;
        return val;
    }

private:
    static constexpr int kMaxDepth = 128;

    void skip_ws() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    void skip_ws_and_nul() noexcept {
        while (pos_ < src_.size()) {
            const char c = src_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\0') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    [[nodiscard]] bool parse_hex4(u32& out) noexcept {
        if (pos_ + 4 > src_.size()) return false;
        u32 v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = src_[pos_++];
            v <<= 4u;
            if (c >= '0' && c <= '9') v |= static_cast<u32>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<u32>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<u32>(c - 'A' + 10);
            else return false;
        }
        out = v;
        return true;
    }

    [[nodiscard]] std::optional<std::string> parse_string() {
        if (pos_ >= src_.size() || src_[pos_] != '"') return std::nullopt;
        ++pos_;
        std::string out;
        while (pos_ < src_.size()) {
            const unsigned char ch = static_cast<unsigned char>(src_[pos_++]);
            if (ch == '"') {
                return out;
            }
            if (ch < 0x20u) {
                return std::nullopt;
            }
            if (ch != '\\') {
                out.push_back(static_cast<char>(ch));
                continue;
            }
            if (pos_ >= src_.size()) return std::nullopt;
            const char esc = src_[pos_++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    u32 cp = 0;
                    if (!parse_hex4(cp)) return std::nullopt;
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                        if (pos_ + 2 <= src_.size() && src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
                            pos_ += 2;
                            u32 lo = 0;
                            if (!parse_hex4(lo)) return std::nullopt;
                            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                                cp = 0x10000u + (((cp - 0xD800u) << 10u) | (lo - 0xDC00u));
                            } else {
                                append_utf8(out, 0xFFFDu);
                                cp = lo;
                            }
                        } else {
                            cp = 0xFFFDu;
                        }
                    }
                    append_utf8(out, cp);
                    break;
                }
                default:
                    return std::nullopt;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<f64> parse_number() {
        const usize start = pos_;
        if (pos_ < src_.size() && src_[pos_] == '-') ++pos_;
        if (pos_ >= src_.size()) return std::nullopt;
        if (src_[pos_] == '0') {
            ++pos_;
        } else if (src_[pos_] >= '1' && src_[pos_] <= '9') {
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        } else {
            return std::nullopt;
        }
        if (pos_ < src_.size() && src_[pos_] == '.') {
            ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return std::nullopt;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
            if (pos_ >= src_.size() || src_[pos_] < '0' || src_[pos_] > '9') return std::nullopt;
            while (pos_ < src_.size() && src_[pos_] >= '0' && src_[pos_] <= '9') ++pos_;
        }
        std::string tmp(src_.substr(start, pos_ - start));
        char* end = nullptr;
        const f64 val = std::strtod(tmp.c_str(), &end);
        if (!end || end != tmp.c_str() + tmp.size() || !std::isfinite(val)) {
            return std::nullopt;
        }
        return val;
    }

    [[nodiscard]] std::optional<JsonValue> parse_value(int depth) {
        if (depth > kMaxDepth || pos_ >= src_.size()) return std::nullopt;
        const char c = src_[pos_];
        if (c == '{') {
            ++pos_;
            JsonObject obj;
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return JsonValue{std::move(obj)};
            }
            while (true) {
                skip_ws();
                auto key = parse_string();
                if (!key) return std::nullopt;
                skip_ws();
                if (pos_ >= src_.size() || src_[pos_] != ':') return std::nullopt;
                ++pos_;
                skip_ws();
                auto val = parse_value(depth + 1);
                if (!val) return std::nullopt;
                // Overwrite duplicate key in place to preserve ArrayHashMap semantics.
                bool replaced = false;
                for (auto& kv : obj) {
                    if (kv.first == *key) {
                        kv.second = std::move(*val);
                        replaced = true;
                        break;
                    }
                }
                if (!replaced) {
                    obj.emplace_back(std::move(*key), std::move(*val));
                }
                skip_ws();
                if (pos_ >= src_.size()) return std::nullopt;
                if (src_[pos_] == '}') {
                    ++pos_;
                    return JsonValue{std::move(obj)};
                }
                if (src_[pos_] != ',') return std::nullopt;
                ++pos_;
            }
        }
        if (c == '[') {
            ++pos_;
            JsonArray arr;
            skip_ws();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return JsonValue{std::move(arr)};
            }
            while (true) {
                skip_ws();
                auto val = parse_value(depth + 1);
                if (!val) return std::nullopt;
                arr.push_back(std::move(*val));
                skip_ws();
                if (pos_ >= src_.size()) return std::nullopt;
                if (src_[pos_] == ']') {
                    ++pos_;
                    return JsonValue{std::move(arr)};
                }
                if (src_[pos_] != ',') return std::nullopt;
                ++pos_;
            }
        }
        if (c == '"') {
            auto s = parse_string();
            if (!s) return std::nullopt;
            return JsonValue{std::move(*s)};
        }
        if (c == 't') {
            if (src_.substr(pos_, 4) == "true") {
                pos_ += 4;
                return JsonValue{true};
            }
            return std::nullopt;
        }
        if (c == 'f') {
            if (src_.substr(pos_, 5) == "false") {
                pos_ += 5;
                return JsonValue{false};
            }
            return std::nullopt;
        }
        if (c == 'n') {
            if (src_.substr(pos_, 4) == "null") {
                pos_ += 4;
                return JsonValue{nullptr};
            }
            return std::nullopt;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            auto n = parse_number();
            if (!n) return std::nullopt;
            return JsonValue{*n};
        }
        return std::nullopt;
    }

    std::string_view src_;
    usize pos_ = 0;
};

// ---------------------------------------------------------------------------
// JSON field decoding helpers
// ---------------------------------------------------------------------------

[[nodiscard]] bool read_usize(const JsonValue& v, usize& out) noexcept {
    if (!v.is_number()) return false;
    const f64 n = v.as_number();
    if (!std::isfinite(n) || n < 0.0 || n > static_cast<f64>(std::numeric_limits<usize>::max())) {
        return false;
    }
    out = static_cast<usize>(n);
    return true;
}

[[nodiscard]] bool opt_usize(const JsonValue& obj, std::string_view key, std::optional<usize>& out) noexcept {
    const JsonValue* v = obj.find(key);
    if (!v) return true;
    usize tmp = 0;
    if (!read_usize(*v, tmp)) return false;
    out = tmp;
    return true;
}

[[nodiscard]] bool req_usize(const JsonValue& obj, std::string_view key, usize& out) noexcept {
    const JsonValue* v = obj.find(key);
    return v && read_usize(*v, out);
}

[[nodiscard]] bool def_usize(const JsonValue& obj, std::string_view key, usize& out, usize def_val) noexcept {
    out = def_val;
    const JsonValue* v = obj.find(key);
    if (!v) return true;
    return read_usize(*v, out);
}

[[nodiscard]] bool opt_string(const JsonValue& obj, std::string_view key, std::optional<std::string>& out) {
    const JsonValue* v = obj.find(key);
    if (!v) return true;
    if (!v->is_string()) return false;
    out = v->as_string();
    return true;
}

[[nodiscard]] bool req_string(const JsonValue& obj, std::string_view key, std::string& out) {
    const JsonValue* v = obj.find(key);
    if (!v || !v->is_string()) return false;
    out = v->as_string();
    return true;
}

[[nodiscard]] bool def_string(const JsonValue& obj, std::string_view key, std::string& out, std::string_view def_val) {
    out = std::string(def_val);
    const JsonValue* v = obj.find(key);
    if (!v) return true;
    if (!v->is_string()) return false;
    out = v->as_string();
    return true;
}

[[nodiscard]] bool def_bool(const JsonValue& obj, std::string_view key, bool& out, bool def_val) noexcept {
    out = def_val;
    const JsonValue* v = obj.find(key);
    if (!v) return true;
    if (!v->is_bool()) return false;
    out = v->as_bool();
    return true;
}

[[nodiscard]] bool def_f32(const JsonValue& obj, std::string_view key, f32& out, f32 def_val) noexcept {
    out = def_val;
    const JsonValue* v = obj.find(key);
    if (!v) return true;
    if (!v->is_number()) return false;
    out = static_cast<f32>(v->as_number());
    return true;
}

template <usize N>
[[nodiscard]] bool read_f32_array(const JsonValue& v, std::array<f32, N>& out) noexcept {
    if (!v.is_array() || v.as_array().size() != N) return false;
    for (usize i = 0; i < N; ++i) {
        if (!v.as_array()[i].is_number()) return false;
        out[i] = static_cast<f32>(v.as_array()[i].as_number());
    }
    return true;
}

[[nodiscard]] bool read_usize_vector(const JsonValue& v, std::vector<usize>& out) {
    if (!v.is_array()) return false;
    out.clear();
    out.reserve(v.as_array().size());
    for (const auto& elem : v.as_array()) {
        usize val = 0;
        if (!read_usize(elem, val)) return false;
        out.push_back(val);
    }
    return true;
}

[[nodiscard]] bool read_f32_vector(const JsonValue& v, std::vector<f32>& out) {
    if (!v.is_array()) return false;
    out.clear();
    out.reserve(v.as_array().size());
    for (const auto& elem : v.as_array()) {
        if (!elem.is_number()) return false;
        out.push_back(static_cast<f32>(elem.as_number()));
    }
    return true;
}

[[nodiscard]] bool read_attribute_map(const JsonValue& v, GltfMesh::AttributeList& out) {
    if (!v.is_object()) return false;
    out.clear();
    out.reserve(v.as_object().size());
    for (const auto& [k, val] : v.as_object()) {
        usize idx = 0;
        if (!read_usize(val, idx)) return false;
        out.emplace_back(k, idx);
    }
    return true;
}

[[nodiscard]] bool read_texture_info(const JsonValue& v, GltfTextureInfo& out) noexcept {
    if (!v.is_object()) return false;
    return req_usize(v, "index", out.index) && def_usize(v, "texCoord", out.tex_coord, 0);
}

[[nodiscard]] u32 read_u32_le(const u8* p) noexcept {
    u32 v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

[[nodiscard]] bool decode_gltf_json(const JsonValue& root, GltfAsset& out) {
    if (!root.is_object()) return false;

    // Asset info (required in glTF 2.0).
    const JsonValue* asset_v = root.find("asset");
    if (!asset_v || !asset_v->is_object()) return false;
    if (!req_string(*asset_v, "version", out.asset.version)) return false;
    if (!opt_string(*asset_v, "copyright", out.asset.copyright)) return false;
    if (!opt_string(*asset_v, "generator", out.asset.generator)) return false;
    if (!opt_string(*asset_v, "minVersion", out.asset.min_version)) return false;

    // Reject files that require extensions (matches gm82gltf reference).
    if (const JsonValue* req_ext = root.find("extensionsRequired")) {
        if (!req_ext->is_array()) return false;
        if (!req_ext->as_array().empty()) return false;
    }

    if (!opt_usize(root, "scene", out.default_scene)) return false;

    if (const JsonValue* buffers_v = root.find("buffers")) {
        if (!buffers_v->is_array()) return false;
        out.buffers.reserve(buffers_v->as_array().size());
        for (const auto& bv : buffers_v->as_array()) {
            if (!bv.is_object()) return false;
            GltfBuffer b{};
            if (!opt_string(bv, "uri", b.uri)) return false;
            if (!req_usize(bv, "byteLength", b.byte_length)) return false;
            if (!opt_string(bv, "name", b.name)) return false;
            out.buffers.push_back(std::move(b));
        }
    }

    if (const JsonValue* bvs_v = root.find("bufferViews")) {
        if (!bvs_v->is_array()) return false;
        out.buffer_views.reserve(bvs_v->as_array().size());
        for (const auto& bv_v : bvs_v->as_array()) {
            if (!bv_v.is_object()) return false;
            GltfBufferView bv{};
            if (!req_usize(bv_v, "buffer", bv.buffer)) return false;
            if (!def_usize(bv_v, "byteOffset", bv.byte_offset, 0)) return false;
            if (!req_usize(bv_v, "byteLength", bv.byte_length)) return false;
            if (!opt_usize(bv_v, "byteStride", bv.byte_stride)) return false;
            if (!opt_usize(bv_v, "target", bv.target)) return false;
            if (!opt_string(bv_v, "name", bv.name)) return false;
            out.buffer_views.push_back(std::move(bv));
        }
    }

    if (const JsonValue* accs_v = root.find("accessors")) {
        if (!accs_v->is_array()) return false;
        out.accessors.reserve(accs_v->as_array().size());
        for (const auto& av : accs_v->as_array()) {
            if (!av.is_object()) return false;
            GltfAccessor a{};
            if (!opt_usize(av, "bufferView", a.buffer_view)) return false;
            if (!def_usize(av, "byteOffset", a.byte_offset, 0)) return false;
            if (!req_usize(av, "componentType", a.component_type)) return false;
            if (!def_bool(av, "normalized", a.normalized, false)) return false;
            if (!req_usize(av, "count", a.count)) return false;
            if (!req_string(av, "type", a.type)) return false;
            if (!opt_string(av, "name", a.name)) return false;
            if (const JsonValue* sp_v = av.find("sparse")) {
                if (!sp_v->is_object()) return false;
                GltfAccessor::Sparse sp{};
                if (!req_usize(*sp_v, "count", sp.count)) return false;
                const JsonValue* idx_v = sp_v->find("indices");
                const JsonValue* val_v = sp_v->find("values");
                if (!idx_v || !idx_v->is_object() || !val_v || !val_v->is_object()) return false;
                if (!req_usize(*idx_v, "bufferView", sp.indices.buffer_view)) return false;
                if (!def_usize(*idx_v, "byteOffset", sp.indices.byte_offset, 0)) return false;
                if (!req_usize(*idx_v, "componentType", sp.indices.component_type)) return false;
                if (!req_usize(*val_v, "bufferView", sp.values.buffer_view)) return false;
                if (!def_usize(*val_v, "byteOffset", sp.values.byte_offset, 0)) return false;
                a.sparse = sp;
            }
            out.accessors.push_back(std::move(a));
        }
    }

    if (const JsonValue* anims_v = root.find("animations")) {
        if (!anims_v->is_array()) return false;
        out.animations.reserve(anims_v->as_array().size());
        for (const auto& av : anims_v->as_array()) {
            if (!av.is_object()) return false;
            GltfAnimation anim{};
            if (!opt_string(av, "name", anim.name)) return false;
            const JsonValue* chs_v = av.find("channels");
            const JsonValue* smps_v = av.find("samplers");
            if (!chs_v || !chs_v->is_array() || !smps_v || !smps_v->is_array()) return false;
            anim.channels.reserve(chs_v->as_array().size());
            for (const auto& cv : chs_v->as_array()) {
                if (!cv.is_object()) return false;
                GltfAnimation::Channel ch{};
                if (!req_usize(cv, "sampler", ch.sampler)) return false;
                const JsonValue* tv = cv.find("target");
                if (!tv || !tv->is_object()) return false;
                if (!opt_usize(*tv, "node", ch.target.node)) return false;
                if (!req_string(*tv, "path", ch.target.path)) return false;
                anim.channels.push_back(std::move(ch));
            }
            anim.samplers.reserve(smps_v->as_array().size());
            for (const auto& sv : smps_v->as_array()) {
                if (!sv.is_object()) return false;
                GltfAnimation::Sampler smp{};
                if (!req_usize(sv, "input", smp.input)) return false;
                if (!def_string(sv, "interpolation", smp.interpolation, "LINEAR")) return false;
                if (!req_usize(sv, "output", smp.output)) return false;
                anim.samplers.push_back(std::move(smp));
            }
            out.animations.push_back(std::move(anim));
        }
    }

    if (const JsonValue* cams_v = root.find("cameras")) {
        if (!cams_v->is_array()) return false;
        out.cameras.reserve(cams_v->as_array().size());
        for (const auto& cv : cams_v->as_array()) {
            if (!cv.is_object()) return false;
            GltfCamera cam{};
            if (!req_string(cv, "type", cam.type)) return false;
            if (const JsonValue* pv = cv.find("perspective")) {
                if (!pv->is_object()) return false;
                GltfCamera::Perspective p{};
                if (!def_f32(*pv, "aspectRatio", p.aspect_ratio, 0.0f)) return false;
                if (!def_f32(*pv, "yfov", p.yfov, 0.0f)) return false;
                if (!def_f32(*pv, "zfar", p.zfar, 0.0f)) return false;
                if (!def_f32(*pv, "znear", p.znear, 0.0f)) return false;
                cam.perspective = p;
            }
            if (const JsonValue* ov = cv.find("orthographic")) {
                if (!ov->is_object()) return false;
                GltfCamera::Orthographic o{};
                if (!def_f32(*ov, "xmag", o.xmag, 0.0f)) return false;
                if (!def_f32(*ov, "ymag", o.ymag, 0.0f)) return false;
                if (!def_f32(*ov, "zfar", o.zfar, 0.0f)) return false;
                if (!def_f32(*ov, "znear", o.znear, 0.0f)) return false;
                cam.orthographic = o;
            }
            out.cameras.push_back(std::move(cam));
        }
    }

    if (const JsonValue* imgs_v = root.find("images")) {
        if (!imgs_v->is_array()) return false;
        out.images.reserve(imgs_v->as_array().size());
        for (const auto& iv : imgs_v->as_array()) {
            if (!iv.is_object()) return false;
            GltfImage img{};
            if (!opt_string(iv, "uri", img.uri)) return false;
            if (!opt_string(iv, "mimeType", img.mime_type)) return false;
            if (!opt_usize(iv, "bufferView", img.buffer_view)) return false;
            if (!opt_string(iv, "name", img.name)) return false;
            out.images.push_back(std::move(img));
        }
    }

    if (const JsonValue* mats_v = root.find("materials")) {
        if (!mats_v->is_array()) return false;
        out.materials.reserve(mats_v->as_array().size());
        for (const auto& mv : mats_v->as_array()) {
            if (!mv.is_object()) return false;
            GltfMaterial mat{};
            if (!opt_string(mv, "name", mat.name)) return false;
            if (const JsonValue* pbr_v = mv.find("pbrMetallicRoughness")) {
                if (!pbr_v->is_object()) return false;
                if (const JsonValue* bcf = pbr_v->find("baseColorFactor")) {
                    if (!read_f32_array(*bcf, mat.pbr.base_color_factor)) return false;
                }
                if (const JsonValue* bct = pbr_v->find("baseColorTexture")) {
                    GltfTextureInfo ti{};
                    if (!read_texture_info(*bct, ti)) return false;
                    mat.pbr.base_color_texture = ti;
                }
                if (!def_f32(*pbr_v, "metallicFactor", mat.pbr.metallic_factor, 1.0f)) return false;
                if (!def_f32(*pbr_v, "roughnessFactor", mat.pbr.roughness_factor, 1.0f)) return false;
                if (const JsonValue* mrt = pbr_v->find("metallicRoughnessTexture")) {
                    GltfTextureInfo ti{};
                    if (!read_texture_info(*mrt, ti)) return false;
                    mat.pbr.metallic_roughness_texture = ti;
                }
            }
            if (const JsonValue* nt_v = mv.find("normalTexture")) {
                if (!nt_v->is_object()) return false;
                GltfNormalTextureInfo nt{};
                if (!req_usize(*nt_v, "index", nt.index)) return false;
                if (!def_usize(*nt_v, "texCoord", nt.tex_coord, 0)) return false;
                if (!def_f32(*nt_v, "scale", nt.scale, 1.0f)) return false;
                mat.normal_texture = nt;
            }
            if (const JsonValue* ot_v = mv.find("occlusionTexture")) {
                if (!ot_v->is_object()) return false;
                GltfOcclusionTextureInfo ot{};
                if (!req_usize(*ot_v, "index", ot.index)) return false;
                if (!def_usize(*ot_v, "texCoord", ot.tex_coord, 0)) return false;
                if (!def_f32(*ot_v, "strength", ot.strength, 1.0f)) return false;
                mat.occlusion_texture = ot;
            }
            if (const JsonValue* et_v = mv.find("emissiveTexture")) {
                GltfTextureInfo et{};
                if (!read_texture_info(*et_v, et)) return false;
                mat.emissive_texture = et;
            }
            if (const JsonValue* ef_v = mv.find("emissiveFactor")) {
                std::array<f32, 3> ef{};
                if (!read_f32_array(*ef_v, ef)) return false;
                mat.emissive_factor = {ef[0], ef[1], ef[2], 1.0f};
            }
            if (!def_string(mv, "alphaMode", mat.alpha_mode, "OPAQUE")) return false;
            if (!def_f32(mv, "alphaCutoff", mat.alpha_cutoff, 0.5f)) return false;
            if (!def_bool(mv, "doubleSided", mat.double_sided, false)) return false;
            out.materials.push_back(std::move(mat));
        }
    }

    if (const JsonValue* meshes_v = root.find("meshes")) {
        if (!meshes_v->is_array()) return false;
        out.meshes.reserve(meshes_v->as_array().size());
        for (const auto& mv : meshes_v->as_array()) {
            if (!mv.is_object()) return false;
            GltfMesh mesh{};
            if (!opt_string(mv, "name", mesh.name)) return false;
            const JsonValue* prims_v = mv.find("primitives");
            if (!prims_v || !prims_v->is_array()) return false;
            mesh.primitives.reserve(prims_v->as_array().size());
            for (const auto& pv : prims_v->as_array()) {
                if (!pv.is_object()) return false;
                GltfMesh::Primitive prim{};
                const JsonValue* attrs_v = pv.find("attributes");
                if (!attrs_v || !read_attribute_map(*attrs_v, prim.attributes)) return false;
                if (!opt_usize(pv, "indices", prim.indices)) return false;
                if (!opt_usize(pv, "material", prim.material)) return false;
                if (!def_usize(pv, "mode", prim.mode, 4)) return false;
                if (const JsonValue* targets_v = pv.find("targets")) {
                    if (!targets_v->is_array()) return false;
                    prim.targets.reserve(targets_v->as_array().size());
                    for (const auto& tv : targets_v->as_array()) {
                        GltfMesh::AttributeList tmap;
                        if (!read_attribute_map(tv, tmap)) return false;
                        prim.targets.push_back(std::move(tmap));
                    }
                }
                mesh.primitives.push_back(std::move(prim));
            }
            if (const JsonValue* wv = mv.find("weights")) {
                if (!read_f32_vector(*wv, mesh.weights)) return false;
                mesh.has_weights = true;
            }
            out.meshes.push_back(std::move(mesh));
        }
    }

    if (const JsonValue* nodes_v = root.find("nodes")) {
        if (!nodes_v->is_array()) return false;
        out.nodes.reserve(nodes_v->as_array().size());
        for (const auto& nv : nodes_v->as_array()) {
            if (!nv.is_object()) return false;
            GltfNode node{};
            if (!opt_usize(nv, "camera", node.camera)) return false;
            if (const JsonValue* ch_v = nv.find("children")) {
                if (!read_usize_vector(*ch_v, node.children)) return false;
            }
            if (!opt_usize(nv, "skin", node.skin)) return false;
            if (const JsonValue* m_v = nv.find("matrix")) {
                std::array<f32, 16> mat{};
                if (!read_f32_array(*m_v, mat)) return false;
                node.matrix = mat;
            }
            if (!opt_usize(nv, "mesh", node.mesh)) return false;
            if (const JsonValue* r_v = nv.find("rotation")) {
                if (!read_f32_array(*r_v, node.rotation)) return false;
            }
            if (const JsonValue* s_v = nv.find("scale")) {
                if (!read_f32_array(*s_v, node.scale)) return false;
            }
            if (const JsonValue* t_v = nv.find("translation")) {
                if (!read_f32_array(*t_v, node.translation)) return false;
            }
            if (!opt_string(nv, "name", node.name)) return false;
            if (const JsonValue* w_v = nv.find("weights")) {
                if (!read_f32_vector(*w_v, node.weights)) return false;
                node.has_weights = true;
            }
            out.nodes.push_back(std::move(node));
        }
    }

    if (const JsonValue* smps_v = root.find("samplers")) {
        if (!smps_v->is_array()) return false;
        out.samplers.reserve(smps_v->as_array().size());
        for (const auto& sv : smps_v->as_array()) {
            if (!sv.is_object()) return false;
            GltfSampler s{};
            if (!opt_usize(sv, "magFilter", s.mag_filter)) return false;
            if (!opt_usize(sv, "minFilter", s.min_filter)) return false;
            if (!def_usize(sv, "wrapS", s.wrap_s, 10497)) return false;
            if (!def_usize(sv, "wrapT", s.wrap_t, 10497)) return false;
            if (!opt_string(sv, "name", s.name)) return false;
            out.samplers.push_back(std::move(s));
        }
    }

    if (const JsonValue* scenes_v = root.find("scenes")) {
        if (!scenes_v->is_array()) return false;
        out.scenes.reserve(scenes_v->as_array().size());
        for (const auto& sv : scenes_v->as_array()) {
            if (!sv.is_object()) return false;
            GltfScene sc{};
            if (const JsonValue* ns_v = sv.find("nodes")) {
                if (!read_usize_vector(*ns_v, sc.nodes)) return false;
            }
            if (!opt_string(sv, "name", sc.name)) return false;
            out.scenes.push_back(std::move(sc));
        }
    }

    if (const JsonValue* skins_v = root.find("skins")) {
        if (!skins_v->is_array()) return false;
        out.skins.reserve(skins_v->as_array().size());
        for (const auto& sk_v : skins_v->as_array()) {
            if (!sk_v.is_object()) return false;
            GltfSkin sk{};
            if (!opt_usize(sk_v, "inverseBindMatrices", sk.inverse_bind_matrices)) return false;
            if (!opt_usize(sk_v, "skeleton", sk.skeleton)) return false;
            const JsonValue* j_v = sk_v.find("joints");
            if (!j_v || !read_usize_vector(*j_v, sk.joints)) return false;
            if (!opt_string(sk_v, "name", sk.name)) return false;
            out.skins.push_back(std::move(sk));
        }
    }

    if (const JsonValue* texs_v = root.find("textures")) {
        if (!texs_v->is_array()) return false;
        out.textures.reserve(texs_v->as_array().size());
        for (const auto& tv : texs_v->as_array()) {
            if (!tv.is_object()) return false;
            GltfTexture t{};
            if (!opt_usize(tv, "sampler", t.sampler)) return false;
            if (!opt_usize(tv, "source", t.source)) return false;
            if (!opt_string(tv, "name", t.name)) return false;
            out.textures.push_back(std::move(t));
        }
    }

    return true;
}

[[nodiscard]] bool read_floats_bounded(
    const std::vector<u8>& buf,
    usize byte_offset,
    usize float_count,
    f32* dst) noexcept {
    const usize byte_len = float_count * sizeof(f32);
    if (byte_offset > buf.size() || byte_len > buf.size() - byte_offset) {
        return false;
    }
    std::memcpy(dst, buf.data() + byte_offset, byte_len);
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Public GLB parser & glTF 2.0 helpers
// ---------------------------------------------------------------------------

std::optional<GltfAsset> parse_glb(std::span<const u8> bytes) {
    if (bytes.size() < 12) return std::nullopt;

    const u32 magic = read_u32_le(bytes.data() + 0);
    const u32 version = read_u32_le(bytes.data() + 4);
    const u32 file_length = read_u32_le(bytes.data() + 8);
    if (magic != 0x46546C67u || version != 2u) return std::nullopt;
    if (file_length < 12u || file_length > bytes.size()) return std::nullopt;

    std::optional<std::string_view> json_chunk;
    std::optional<std::span<const u8>> bin_chunk;

    usize offset = 12;
    while (offset < file_length) {
        if (file_length - offset < 8u) return std::nullopt;
        const u32 chunk_length = read_u32_le(bytes.data() + offset);
        const u32 chunk_type = read_u32_le(bytes.data() + offset + 4);
        offset += 8;
        if (chunk_length > file_length - offset) return std::nullopt;

        if (chunk_type == 0x4E4F534Au) { // "JSON"
            if (json_chunk.has_value()) return std::nullopt;
            json_chunk = std::string_view(reinterpret_cast<const char*>(bytes.data() + offset), chunk_length);
        } else if (chunk_type == 0x004E4942u) { // "BIN\0"
            if (bin_chunk.has_value()) return std::nullopt;
            bin_chunk = bytes.subspan(offset, chunk_length);
        }
        offset += chunk_length;
    }

    if (!json_chunk.has_value()) return std::nullopt;

    JsonParser parser(*json_chunk);
    auto parsed_json = parser.parse();
    if (!parsed_json) {
        GM_LOG_WARN("ext", "gm82gltf: failed to parse GLB JSON chunk");
        return std::nullopt;
    }

    GltfAsset gltf{};
    if (!decode_gltf_json(*parsed_json, gltf)) {
        GM_LOG_WARN("ext", "gm82gltf: failed to decode glTF JSON schema");
        return std::nullopt;
    }

    // Bind embedded GLB BIN chunk to buffer 0; external URIs / additional buffers are unsupported.
    gltf.buffer_data.resize(gltf.buffers.size());
    for (usize i = 0; i < gltf.buffers.size(); ++i) {
        const auto& buf = gltf.buffers[i];
        if (i == 0 && bin_chunk.has_value() && !buf.uri.has_value()) {
            if (bin_chunk->size() < buf.byte_length) {
                return std::nullopt;
            }
            gltf.buffer_data[0].assign(bin_chunk->begin(), bin_chunk->end());
            continue;
        }
        return std::nullopt;
    }

    // Compute node parent links from children arrays.
    for (usize i = 0; i < gltf.nodes.size(); ++i) {
        for (const usize child : gltf.nodes[i].children) {
            if (child < gltf.nodes.size()) {
                gltf.nodes[child].parent = i;
            }
        }
    }

    // Copy mesh default morph weights into nodes that omit explicit weights.
    for (auto& node : gltf.nodes) {
        if (!node.has_weights && node.mesh.has_value() && *node.mesh < gltf.meshes.size()) {
            const auto& mesh = gltf.meshes[*node.mesh];
            if (mesh.has_weights) {
                node.weights = mesh.weights;
                node.has_weights = true;
            }
        }
    }

    return gltf;
}

usize gltf_component_type_size(usize component_type) noexcept {
    switch (component_type) {
        case 5120: return 1; // BYTE
        case 5121: return 1; // UNSIGNED_BYTE
        case 5122: return 2; // SHORT
        case 5123: return 2; // UNSIGNED_SHORT
        case 5125: return 4; // UNSIGNED_INT
        case 5126: return 4; // FLOAT
        default:   return 0;
    }
}

usize gltf_type_component_count(std::string_view type) noexcept {
    if (type == "SCALAR") return 1;
    if (type == "VEC2")   return 2;
    if (type == "VEC3")   return 3;
    if (type == "VEC4")   return 4;
    if (type == "MAT2")   return 4;
    if (type == "MAT3")   return 9;
    if (type == "MAT4")   return 16;
    return 0;
}

usize gltf_accessor_packed_stride(const GltfAccessor& accessor) noexcept {
    return gltf_component_type_size(accessor.component_type) * gltf_type_component_count(accessor.type);
}

usize gltf_accessor_effective_stride(const GltfAsset& gltf, const GltfAccessor& accessor) noexcept {
    const usize packed = gltf_accessor_packed_stride(accessor);
    if (accessor.buffer_view.has_value() && *accessor.buffer_view < gltf.buffer_views.size()) {
        const auto& bv = gltf.buffer_views[*accessor.buffer_view];
        if (bv.byte_stride.has_value()) {
            return *bv.byte_stride;
        }
    }
    return packed;
}

usize gltf_accessor_byte_size(const GltfAsset& gltf, const GltfAccessor& accessor) noexcept {
    return accessor.count * gltf_accessor_effective_stride(gltf, accessor);
}

std::array<f32, 16> gltf_quat_to_matrix(const std::array<f32, 4>& q) noexcept {
    // Matches DirectXMath XMMatrixRotationQuaternion / gm82gltf create_rotation:
    // row-major D3D matrix (= column-major OpenGL matrix).
    const f32 x = q[0];
    const f32 y = q[1];
    const f32 z = q[2];
    const f32 w = q[3];

    const f32 x2 = x + x;
    const f32 y2 = y + y;
    const f32 z2 = z + z;

    const f32 xx = x * x2;
    const f32 xy = x * y2;
    const f32 xz = x * z2;
    const f32 yy = y * y2;
    const f32 yz = y * z2;
    const f32 zz = z * z2;
    const f32 wx = w * x2;
    const f32 wy = w * y2;
    const f32 wz = w * z2;

    return {
        1.0f - (yy + zz), xy + wz,          xz - wy,          0.0f,
        xy - wz,          1.0f - (xx + zz), yz + wx,          0.0f,
        xz + wy,          yz - wx,          1.0f - (xx + yy), 0.0f,
        0.0f,             0.0f,             0.0f,             1.0f,
    };
}

std::array<f32, 16> gltf_mul_mat4(const std::array<f32, 16>& a, const std::array<f32, 16>& b) noexcept {
    std::array<f32, 16> out{};
    for (usize y = 0; y < 4; ++y) {
        for (usize x = 0; x < 4; ++x) {
            out[y * 4 + x] =
                a[y * 4 + 0] * b[0 * 4 + x] +
                a[y * 4 + 1] * b[1 * 4 + x] +
                a[y * 4 + 2] * b[2 * 4 + x] +
                a[y * 4 + 3] * b[3 * 4 + x];
        }
    }
    return out;
}

std::array<f32, 16> gltf_node_local_matrix(const GltfNode& node) noexcept {
    if (node.matrix.has_value()) {
        return *node.matrix;
    }
    const std::array<f32, 16> scaling{
        node.scale[0], 0.0f,          0.0f,          0.0f,
        0.0f,          node.scale[1], 0.0f,          0.0f,
        0.0f,          0.0f,          node.scale[2], 0.0f,
        0.0f,          0.0f,          0.0f,          1.0f,
    };
    const std::array<f32, 16> rotation = gltf_quat_to_matrix(node.rotation);
    const std::array<f32, 16> translation{
        1.0f,                0.0f,                0.0f,                0.0f,
        0.0f,                1.0f,                0.0f,                0.0f,
        0.0f,                0.0f,                1.0f,                0.0f,
        node.translation[0], node.translation[1], node.translation[2], 1.0f,
    };
    return gltf_mul_mat4(gltf_mul_mat4(scaling, rotation), translation);
}

std::array<f32, 16> gltf_node_full_matrix(
    const GltfAsset& gltf,
    usize node_id,
    std::optional<usize> skeleton) noexcept {
    if (node_id >= gltf.nodes.size()) {
        return std::array<f32, 16>{};
    }
    const GltfNode* node = &gltf.nodes[node_id];
    std::array<f32, 16> mat = gltf_node_local_matrix(*node);
    usize hops = 0;
    while (node->parent.has_value() && hops < gltf.nodes.size()) {
        const usize parent_id = *node->parent;
        if (skeleton.has_value() && parent_id == *skeleton) {
            break;
        }
        if (parent_id >= gltf.nodes.size()) {
            return std::array<f32, 16>{};
        }
        node = &gltf.nodes[parent_id];
        mat = gltf_mul_mat4(mat, gltf_node_local_matrix(*node));
        ++hops;
    }
    return mat;
}

std::optional<std::span<const u8>> gltf_accessor_direct_span(
    const GltfAsset& gltf,
    usize accessor_id) noexcept {
    if (accessor_id >= gltf.accessors.size()) return std::nullopt;
    const auto& acc = gltf.accessors[accessor_id];
    if (!acc.buffer_view.has_value() || *acc.buffer_view >= gltf.buffer_views.size()) {
        return std::nullopt;
    }
    const auto& bv = gltf.buffer_views[*acc.buffer_view];
    if (bv.buffer >= gltf.buffer_data.size()) return std::nullopt;
    const auto& buf = gltf.buffer_data[bv.buffer];
    const usize offset = bv.byte_offset + acc.byte_offset;
    if (offset > buf.size()) return std::nullopt;
    const usize want = gltf_accessor_byte_size(gltf, acc);
    const usize avail = buf.size() - offset;
    return std::span<const u8>(buf.data() + offset, std::min(want, avail));
}

bool gltf_materialize_sparse_accessor(
    const GltfAsset& gltf,
    usize accessor_id,
    std::vector<u8>& out_data) {
    if (accessor_id >= gltf.accessors.size()) return false;
    const auto& acc = gltf.accessors[accessor_id];
    if (!acc.sparse.has_value()) return false;
    const auto& sparse = *acc.sparse;

    const usize packed_stride = gltf_accessor_packed_stride(acc);
    const usize stride = gltf_accessor_effective_stride(gltf, acc);
    if (packed_stride == 0 || stride < packed_stride) return false;
    const usize total_size = acc.count * stride;

    out_data.assign(total_size, 0u);
    if (acc.buffer_view.has_value()) {
        if (*acc.buffer_view >= gltf.buffer_views.size()) return false;
        const auto& bv = gltf.buffer_views[*acc.buffer_view];
        if (bv.buffer >= gltf.buffer_data.size()) return false;
        const auto& buf = gltf.buffer_data[bv.buffer];
        const usize offset = bv.byte_offset + acc.byte_offset;
        if (offset > buf.size() || total_size > buf.size() - offset) return false;
        if (total_size > 0) {
            std::memcpy(out_data.data(), buf.data() + offset, total_size);
        }
    }

    if (sparse.indices.buffer_view >= gltf.buffer_views.size() ||
        sparse.values.buffer_view >= gltf.buffer_views.size()) {
        return false;
    }
    const auto& idx_bv = gltf.buffer_views[sparse.indices.buffer_view];
    const auto& val_bv = gltf.buffer_views[sparse.values.buffer_view];
    if (idx_bv.buffer >= gltf.buffer_data.size() || val_bv.buffer >= gltf.buffer_data.size()) {
        return false;
    }
    const auto& idx_buf = gltf.buffer_data[idx_bv.buffer];
    const auto& val_buf = gltf.buffer_data[val_bv.buffer];
    const usize idx_base = idx_bv.byte_offset + sparse.indices.byte_offset;
    const usize val_base = val_bv.byte_offset + sparse.values.byte_offset;

    const usize idx_elem_size = gltf_component_type_size(sparse.indices.component_type);
    if (idx_elem_size == 0 ||
        (sparse.indices.component_type != 5121 &&
         sparse.indices.component_type != 5123 &&
         sparse.indices.component_type != 5125)) {
        return false;
    }
    if (idx_base > idx_buf.size() || sparse.count * idx_elem_size > idx_buf.size() - idx_base) {
        return false;
    }
    if (val_base > val_buf.size() || sparse.count * packed_stride > val_buf.size() - val_base) {
        return false;
    }

    for (usize i = 0; i < sparse.count; ++i) {
        usize index = 0;
        const u8* ip = idx_buf.data() + idx_base + i * idx_elem_size;
        if (sparse.indices.component_type == 5121) {
            index = ip[0];
        } else if (sparse.indices.component_type == 5123) {
            u16 v = 0;
            std::memcpy(&v, ip, sizeof(v));
            index = v;
        } else {
            u32 v = 0;
            std::memcpy(&v, ip, sizeof(v));
            index = v;
        }
        if (index >= acc.count) return false;
        std::memcpy(
            out_data.data() + index * stride,
            val_buf.data() + val_base + i * packed_stride,
            packed_stride);
    }
    return true;
}

std::optional<std::span<const u8>> gltf_texture_image_bytes(
    const GltfAsset& gltf,
    usize texture_id) noexcept {
    if (texture_id >= gltf.textures.size()) return std::nullopt;
    const auto& tex = gltf.textures[texture_id];
    if (!tex.source.has_value() || *tex.source >= gltf.images.size()) return std::nullopt;
    const auto& img = gltf.images[*tex.source];
    if (!img.buffer_view.has_value() || *img.buffer_view >= gltf.buffer_views.size()) return std::nullopt;
    const auto& bv = gltf.buffer_views[*img.buffer_view];
    if (bv.buffer >= gltf.buffer_data.size()) return std::nullopt;
    const auto& buf = gltf.buffer_data[bv.buffer];
    if (bv.byte_offset > buf.size() || bv.byte_length > buf.size() - bv.byte_offset) {
        return std::nullopt;
    }
    return std::span<const u8>(buf.data() + bv.byte_offset, bv.byte_length);
}

std::optional<bool> gltf_apply_animation(
    GltfAsset& gltf,
    usize animation_id,
    f64 time) {
    if (animation_id >= gltf.animations.size()) return std::nullopt;
    const auto& animation = gltf.animations[animation_id];
    bool done = true;

    for (const auto& channel : animation.channels) {
        if (!channel.target.node.has_value() || *channel.target.node >= gltf.nodes.size()) continue;
        if (channel.sampler >= animation.samplers.size()) continue;

        GltfNode& node = gltf.nodes[*channel.target.node];
        const bool is_rotation = (channel.target.path == "rotation");
        const bool is_translation = (channel.target.path == "translation");
        const bool is_scale = (channel.target.path == "scale");
        const bool is_weights = (channel.target.path == "weights");

        f32* out_ptr = nullptr;
        usize out_len = 0;
        if (is_rotation) {
            out_ptr = node.rotation.data();
            out_len = 4;
        } else if (is_translation) {
            out_ptr = node.translation.data();
            out_len = 3;
        } else if (is_scale) {
            out_ptr = node.scale.data();
            out_len = 3;
        } else if (is_weights) {
            if (node.weights.empty()) continue;
            out_ptr = node.weights.data();
            out_len = node.weights.size();
        } else {
            continue;
        }

        const auto& sampler = animation.samplers[channel.sampler];
        if (sampler.interpolation == "CUBICSPLINE") {
            continue;
        }
        const bool is_linear = (sampler.interpolation == "LINEAR");

        if (sampler.input >= gltf.accessors.size() || sampler.output >= gltf.accessors.size()) continue;
        const auto& in_acc = gltf.accessors[sampler.input];
        const auto& out_acc = gltf.accessors[sampler.output];
        if (in_acc.component_type != 5126 || out_acc.component_type != 5126 || in_acc.count == 0) continue;
        if (!in_acc.buffer_view.has_value() || !out_acc.buffer_view.has_value()) continue;
        if (*in_acc.buffer_view >= gltf.buffer_views.size() ||
            *out_acc.buffer_view >= gltf.buffer_views.size()) {
            continue;
        }
        const auto& in_bv = gltf.buffer_views[*in_acc.buffer_view];
        const auto& out_bv = gltf.buffer_views[*out_acc.buffer_view];
        if (in_bv.buffer >= gltf.buffer_data.size() || out_bv.buffer >= gltf.buffer_data.size()) continue;

        const auto& in_buf = gltf.buffer_data[in_bv.buffer];
        const auto& out_buf = gltf.buffer_data[out_bv.buffer];
        const usize in_base = in_bv.byte_offset + in_acc.byte_offset;
        const usize out_base = out_bv.byte_offset + out_acc.byte_offset;
        const usize in_stride = in_bv.byte_stride.value_or(sizeof(f32));
        const usize out_elem_stride = is_weights
            ? (out_bv.byte_stride.value_or(sizeof(f32)) * out_len)
            : out_bv.byte_stride.value_or(sizeof(f32) * out_len);

        if (in_base > in_buf.size() || (in_acc.count - 1) * in_stride + sizeof(f32) > in_buf.size() - in_base) {
            continue;
        }

        auto read_keyframe_time = [&](usize idx) noexcept -> f32 {
            f32 t = 0.0f;
            std::memcpy(&t, in_buf.data() + in_base + idx * in_stride, sizeof(f32));
            return t;
        };

        // Animated TRS nodes must use TRS instead of a static matrix.
        if (!is_weights) {
            node.matrix.reset();
        }

        bool found = false;
        std::vector<f32> cur_vals(out_len, 0.0f);
        std::vector<f32> next_vals(out_len, 0.0f);

        for (usize i = 0; i < in_acc.count; ++i) {
            const f32 keyframe_next = read_keyframe_time(i);
            if (time < static_cast<f64>(keyframe_next)) {
                done = false;
                const usize prev = (i != 0) ? (i - 1) : 0;
                const usize cur_off = out_base + out_elem_stride * prev;
                if (!read_floats_bounded(out_buf, cur_off, out_len, cur_vals.data())) {
                    found = true;
                    break;
                }
                if (is_linear && i != 0) {
                    const usize next_off = out_base + out_elem_stride * i;
                    if (!read_floats_bounded(out_buf, next_off, out_len, next_vals.data())) {
                        std::memcpy(out_ptr, cur_vals.data(), out_len * sizeof(f32));
                        found = true;
                        break;
                    }
                    const f32 keyframe_prev = read_keyframe_time(prev);
                    const f64 denom = static_cast<f64>(keyframe_next) - static_cast<f64>(keyframe_prev);
                    const f64 t = (denom > 0.0) ? ((time - static_cast<f64>(keyframe_prev)) / denom) : 0.0;
                    const f32 tf = static_cast<f32>(t);
                    if (!is_rotation) {
                        for (usize j = 0; j < out_len; ++j) {
                            out_ptr[j] = cur_vals[j] + (next_vals[j] - cur_vals[j]) * tf;
                        }
                    } else {
                        f32 dot = cur_vals[0] * next_vals[0] +
                                  cur_vals[1] * next_vals[1] +
                                  cur_vals[2] * next_vals[2] +
                                  cur_vals[3] * next_vals[3];
                        const f32 sign = (dot < 0.0f) ? -1.0f : 1.0f;
                        const f32 abs_dot = std::clamp(std::fabs(dot), 0.0f, 1.0f);
                        const f32 angle = std::acos(abs_dot);
                        if (angle > 0.001f) {
                            const f32 sin_a = std::sin(angle);
                            const f32 w0 = std::sin(angle * (1.0f - tf)) / sin_a;
                            const f32 w1 = sign * (std::sin(angle * tf) / sin_a);
                            for (usize j = 0; j < 4; ++j) {
                                out_ptr[j] = w0 * cur_vals[j] + w1 * next_vals[j];
                            }
                        } else {
                            f32 norm_sq = 0.0f;
                            for (usize j = 0; j < 4; ++j) {
                                out_ptr[j] = cur_vals[j] + (sign * next_vals[j] - cur_vals[j]) * tf;
                                norm_sq += out_ptr[j] * out_ptr[j];
                            }
                            if (norm_sq > 1e-12f) {
                                const f32 inv_len = 1.0f / std::sqrt(norm_sq);
                                for (usize j = 0; j < 4; ++j) out_ptr[j] *= inv_len;
                            }
                        }
                    }
                } else {
                    std::memcpy(out_ptr, cur_vals.data(), out_len * sizeof(f32));
                }
                found = true;
                break;
            }
        }

        if (!found) {
            const usize last_off = out_base + out_elem_stride * (in_acc.count - 1);
            if (read_floats_bounded(out_buf, last_off, out_len, cur_vals.data())) {
                std::memcpy(out_ptr, cur_vals.data(), out_len * sizeof(f32));
            }
        }
    }

    return done;
}

std::optional<f64> gltf_compute_animation_length(
    const GltfAsset& gltf,
    usize animation_id) noexcept {
    if (animation_id >= gltf.animations.size()) return std::nullopt;
    const auto& animation = gltf.animations[animation_id];
    f32 max_t = 0.0f;
    for (const auto& channel : animation.channels) {
        if (channel.sampler >= animation.samplers.size()) continue;
        const auto& sampler = animation.samplers[channel.sampler];
        if (sampler.input >= gltf.accessors.size()) continue;
        const auto& in_acc = gltf.accessors[sampler.input];
        if (in_acc.component_type != 5126 || in_acc.count == 0 || !in_acc.buffer_view.has_value()) continue;
        if (*in_acc.buffer_view >= gltf.buffer_views.size()) continue;
        const auto& in_bv = gltf.buffer_views[*in_acc.buffer_view];
        if (in_bv.buffer >= gltf.buffer_data.size()) continue;
        const auto& in_buf = gltf.buffer_data[in_bv.buffer];
        const usize stride = in_bv.byte_stride.value_or(sizeof(f32));
        const usize last_off = in_bv.byte_offset + in_acc.byte_offset + (in_acc.count - 1) * stride;
        f32 last_t = 0.0f;
        if (read_floats_bounded(in_buf, last_off, 1, &last_t)) {
            max_t = std::max(max_t, last_t);
        }
    }
    return static_cast<f64>(max_t);
}

bool gltf_compute_skin_matrices(
    const GltfAsset& gltf,
    usize skin_id,
    std::vector<std::array<f32, 16>>& out_matrices) {
    if (skin_id >= gltf.skins.size()) return false;
    const auto& skin = gltf.skins[skin_id];

    const std::vector<u8>* ibm_buf = nullptr;
    usize ibm_base = 0;
    usize ibm_stride = 64;
    usize ibm_count = 0;
    if (skin.inverse_bind_matrices.has_value() && *skin.inverse_bind_matrices < gltf.accessors.size()) {
        const auto& ibm_acc = gltf.accessors[*skin.inverse_bind_matrices];
        if (ibm_acc.buffer_view.has_value() && *ibm_acc.buffer_view < gltf.buffer_views.size()) {
            const auto& ibm_bv = gltf.buffer_views[*ibm_acc.buffer_view];
            if (ibm_bv.buffer < gltf.buffer_data.size()) {
                ibm_buf = &gltf.buffer_data[ibm_bv.buffer];
                ibm_base = ibm_bv.byte_offset + ibm_acc.byte_offset;
                ibm_stride = ibm_bv.byte_stride.value_or(64);
                ibm_count = ibm_acc.count;
            }
        }
    }

    out_matrices.clear();
    out_matrices.reserve(skin.joints.size());
    for (usize i = 0; i < skin.joints.size(); ++i) {
        std::array<f32, 16> mat = gltf_node_full_matrix(gltf, skin.joints[i], skin.skeleton);
        if (ibm_buf && i < ibm_count) {
            std::array<f32, 16> ibm{};
            if (read_floats_bounded(*ibm_buf, ibm_base + i * ibm_stride, 16, ibm.data())) {
                mat = gltf_mul_mat4(ibm, mat);
            }
        }
        out_matrices.push_back(mat);
    }
    return true;
}

} // namespace gm
