#include "extensions/core/extension.h"
#include "extensions/modules/gm82gltf/gltf_asset.h"
#include "extensions/shared/buffer_store.h"

#include "base/log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gm {

namespace {

[[nodiscard]] std::optional<usize> to_index(f64 v) noexcept {
    if (!std::isfinite(v) || v < 0.0) return std::nullopt;
    const f64 floored = std::trunc(v);
    if (floored > static_cast<f64>(std::numeric_limits<usize>::max() / 2)) return std::nullopt;
    return static_cast<usize>(floored);
}

[[nodiscard]] bool is_absolute_path_str(std::string_view p) noexcept {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    return p.size() >= 2 && p[1] == ':';
}

class Gm82GltfExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "gm82gltf"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_alias("gm82gltf");
        buffers_ = &reg.services().get_or_create<GmBufferStore>();

        // Lifecycle
        reg.bind<&Gm82GltfExtension::reset_all>(this, "__gltf_reset");
        reg.bind<&Gm82GltfExtension::load_glb>(this, "__gltf_load");
        reg.bind<&Gm82GltfExtension::destroy_glb>(this, "__gltf_destroy");

        // Animations
        reg.bind<&Gm82GltfExtension::animation_count>(this, "gltf_animation_count");
        reg.bind<&Gm82GltfExtension::get_animation>(this, "gltf_get_animation");
        reg.bind<&Gm82GltfExtension::animation_name>(this, "gltf_animation_name");
        reg.bind<&Gm82GltfExtension::animate>(this, "__gltf_animate");
        reg.bind<&Gm82GltfExtension::animation_length>(this, "__gltf_animation_length");

        // Skins
        reg.bind<&Gm82GltfExtension::skin_skeleton>(this, "gltf_skin_skeleton");
        reg.bind<&Gm82GltfExtension::skin_joint_count>(this, "gltf_skin_joint_count");
        reg.bind<&Gm82GltfExtension::skin_joints>(this, "gltf_skin_joints");

        // Scenes
        reg.bind<&Gm82GltfExtension::default_scene>(this, "gltf_scene");
        reg.bind<&Gm82GltfExtension::scene_count>(this, "gltf_scene_count");
        reg.bind<&Gm82GltfExtension::get_scene>(this, "gltf_get_scene");
        reg.bind<&Gm82GltfExtension::scene_name>(this, "gltf_scene_name");
        reg.bind<&Gm82GltfExtension::scene_node_count>(this, "gltf_scene_node_count");
        reg.bind<&Gm82GltfExtension::scene_node>(this, "gltf_scene_node");

        // Nodes
        reg.bind<&Gm82GltfExtension::node_count>(this, "gltf_node_count");
        reg.bind<&Gm82GltfExtension::get_node>(this, "gltf_get_node");
        reg.bind<&Gm82GltfExtension::node_name>(this, "gltf_node_name");
        reg.bind<&Gm82GltfExtension::node_tx>(this, "gltf_node_tx");
        reg.bind<&Gm82GltfExtension::node_ty>(this, "gltf_node_ty");
        reg.bind<&Gm82GltfExtension::node_tz>(this, "gltf_node_tz");
        reg.bind<&Gm82GltfExtension::node_rx>(this, "gltf_node_rx");
        reg.bind<&Gm82GltfExtension::node_ry>(this, "gltf_node_ry");
        reg.bind<&Gm82GltfExtension::node_rz>(this, "gltf_node_rz");
        reg.bind<&Gm82GltfExtension::node_rw>(this, "gltf_node_rw");
        reg.bind<&Gm82GltfExtension::node_sx>(this, "gltf_node_sx");
        reg.bind<&Gm82GltfExtension::node_sy>(this, "gltf_node_sy");
        reg.bind<&Gm82GltfExtension::node_sz>(this, "gltf_node_sz");
        reg.bind<&Gm82GltfExtension::node_matrix_pointer>(this, "gltf_node_matrix_pointer");
        reg.bind<&Gm82GltfExtension::node_parent>(this, "gltf_node_parent");
        reg.bind<&Gm82GltfExtension::node_child_count>(this, "gltf_node_child_count");
        reg.bind<&Gm82GltfExtension::node_child>(this, "gltf_node_child");
        reg.bind<&Gm82GltfExtension::node_camera>(this, "gltf_node_camera");
        reg.bind<&Gm82GltfExtension::node_mesh>(this, "gltf_node_mesh");
        reg.bind<&Gm82GltfExtension::node_skin>(this, "gltf_node_skin");
        reg.bind<&Gm82GltfExtension::node_weight_count>(this, "gltf_node_weight_count");
        reg.bind<&Gm82GltfExtension::node_sorted_morph>(this, "gltf_node_sorted_morph");
        reg.bind<&Gm82GltfExtension::node_sorted_weights_spaced_pointer>(this, "gltf_node_sorted_weights_spaced_pointer");

        // Cameras
        reg.bind<&Gm82GltfExtension::camera_type>(this, "gltf_camera_type");
        reg.bind<&Gm82GltfExtension::camera_aspect>(this, "gltf_camera_aspect");
        reg.bind<&Gm82GltfExtension::camera_yfov>(this, "gltf_camera_yfov");
        reg.bind<&Gm82GltfExtension::camera_zfar>(this, "gltf_camera_zfar");
        reg.bind<&Gm82GltfExtension::camera_znear>(this, "gltf_camera_znear");

        // Meshes & Primitives
        reg.bind<&Gm82GltfExtension::mesh_count>(this, "gltf_mesh_count");
        reg.bind<&Gm82GltfExtension::mesh_primitive_count>(this, "gltf_mesh_primitive_count");
        reg.bind<&Gm82GltfExtension::mesh_primitive_material>(this, "gltf_mesh_primitive_material");
        reg.bind<&Gm82GltfExtension::mesh_primitive_mode>(this, "gltf_mesh_primitive_mode");
        reg.bind<&Gm82GltfExtension::mesh_primitive_indices_accessor>(this, "gltf_mesh_primitive_indices_accessor");
        reg.bind<&Gm82GltfExtension::mesh_primitive_attribute_count>(this, "gltf_mesh_primitive_attribute_count");
        reg.bind<&Gm82GltfExtension::mesh_primitive_attribute_semantic>(this, "gltf_mesh_primitive_attribute_semantic");
        reg.bind<&Gm82GltfExtension::mesh_primitive_attribute_accessor>(this, "gltf_mesh_primitive_attribute_accessor");
        reg.bind<&Gm82GltfExtension::mesh_primitive_morph_count>(this, "gltf_mesh_primitive_morph_count");
        reg.bind<&Gm82GltfExtension::mesh_primitive_morph>(this, "gltf_mesh_primitive_morph");
        reg.bind<&Gm82GltfExtension::mesh_primitive_morph_attribute_count>(this, "gltf_mesh_primitive_morph_attribute_count");
        reg.bind<&Gm82GltfExtension::mesh_primitive_morph_attribute_semantic>(this, "gltf_mesh_primitive_morph_attribute_semantic");
        reg.bind<&Gm82GltfExtension::mesh_primitive_morph_attribute_accessor>(this, "gltf_mesh_primitive_morph_attribute_accessor");

        // Accessors
        reg.bind<&Gm82GltfExtension::accessor_type>(this, "gltf_accessor_type");
        reg.bind<&Gm82GltfExtension::accessor_component_type>(this, "gltf_accessor_component_type");
        reg.bind<&Gm82GltfExtension::accessor_normalized>(this, "gltf_accessor_normalized");
        reg.bind<&Gm82GltfExtension::accessor_stride>(this, "gltf_accessor_stride");
        reg.bind<&Gm82GltfExtension::accessor_copy>(this, "gltf_accessor_copy");
        reg.bind<&Gm82GltfExtension::accessor_pointer>(this, "gltf_accessor_pointer");
        reg.bind<&Gm82GltfExtension::accessor_size>(this, "gltf_accessor_size");

        // Materials
        reg.bind<&Gm82GltfExtension::material_base_texture>(this, "gltf_material_base_texture");
        reg.bind<&Gm82GltfExtension::material_roughness>(this, "gltf_material_roughness");
        reg.bind<&Gm82GltfExtension::material_metalness>(this, "gltf_material_metalness");
        reg.bind<&Gm82GltfExtension::material_roughness_texture>(this, "gltf_material_roughness_texture");
        reg.bind<&Gm82GltfExtension::material_roughness_texcoord>(this, "gltf_material_roughness_texcoord");
        reg.bind<&Gm82GltfExtension::material_base_color_pointer>(this, "gltf_material_base_color_pointer");
        reg.bind<&Gm82GltfExtension::material_base_texcoord>(this, "gltf_material_base_texcoord");
        reg.bind<&Gm82GltfExtension::material_normal_texture>(this, "gltf_material_normal_texture");
        reg.bind<&Gm82GltfExtension::material_normal_texcoord>(this, "gltf_material_normal_texcoord");
        reg.bind<&Gm82GltfExtension::material_normal_scale>(this, "gltf_material_normal_scale");
        reg.bind<&Gm82GltfExtension::material_occlusion_texture>(this, "gltf_material_occlusion_texture");
        reg.bind<&Gm82GltfExtension::material_occlusion_texcoord>(this, "gltf_material_occlusion_texcoord");
        reg.bind<&Gm82GltfExtension::material_occlusion_strength>(this, "gltf_material_occlusion_strength");
        reg.bind<&Gm82GltfExtension::material_emissive_texture>(this, "gltf_material_emissive_texture");
        reg.bind<&Gm82GltfExtension::material_emissive_texcoord>(this, "gltf_material_emissive_texcoord");
        reg.bind<&Gm82GltfExtension::material_emissive_color_pointer>(this, "gltf_material_emissive_color_pointer");
        reg.bind<&Gm82GltfExtension::material_alpha_mode>(this, "gltf_material_alpha_mode");
        reg.bind<&Gm82GltfExtension::material_alpha_cutoff>(this, "gltf_material_alpha_cutoff");
        reg.bind<&Gm82GltfExtension::material_double_sided>(this, "gltf_material_double_sided");

        // Textures
        reg.bind<&Gm82GltfExtension::texture_count>(this, "gltf_texture_count");
        reg.bind<&Gm82GltfExtension::texture_wrap_h>(this, "gltf_texture_wrap_h");
        reg.bind<&Gm82GltfExtension::texture_wrap_v>(this, "gltf_texture_wrap_v");
        reg.bind<&Gm82GltfExtension::texture_interpolation>(this, "gltf_texture_interpolation");
        reg.bind<&Gm82GltfExtension::texture_type>(this, "gltf_texture_type");
        reg.bind<&Gm82GltfExtension::texture_copy>(this, "gltf_texture_copy");
        reg.bind<&Gm82GltfExtension::texture_save>(this, "gltf_texture_save");
        reg.bind<&Gm82GltfExtension::texture_size>(this, "gltf_texture_size");
    }

    void on_unload(IExtensionHost&) override {
        clear_state();
    }

private:
    struct LoadedGlb {
        GltfAsset gltf;
        std::vector<u32> accessor_views;
        std::vector<u32> base_color_views;
        std::vector<u32> emissive_color_views;
    };

    void unregister_glb_views(LoadedGlb& rec) {
        if (!buffers_) return;
        for (u32 addr : rec.accessor_views) {
            if (addr != 0u) buffers_->unregister_view(addr);
        }
        for (u32 addr : rec.base_color_views) {
            if (addr != 0u) buffers_->unregister_view(addr);
        }
        for (u32 addr : rec.emissive_color_views) {
            if (addr != 0u) buffers_->unregister_view(addr);
        }
    }

    void clear_state() {
        if (buffers_) {
            for (auto& [_, rec] : gltfs_) {
                if (rec) unregister_glb_views(*rec);
            }
            if (node_matrix_view_ != 0u) buffers_->unregister_view(node_matrix_view_);
            if (skin_matrices_view_ != 0u) buffers_->unregister_view(skin_matrices_view_);
            if (sorted_weights_view_ != 0u) buffers_->unregister_view(sorted_weights_view_);
            if (sparse_data_view_ != 0u) buffers_->unregister_view(sparse_data_view_);
        }
        gltfs_.clear();
        next_id_ = 1;
        node_matrix_view_ = 0u;
        skin_matrices_view_ = 0u;
        sorted_weights_view_ = 0u;
        sparse_data_view_ = 0u;
        skin_matrices_.clear();
        sorted_weights_.clear();
        sorted_weight_ids_.clear();
        sparse_data_.clear();
    }

    [[nodiscard]] u32 update_scratch_view(u32& slot, std::span<const u8> span) {
        if (!buffers_) return 0u;
        if (slot == 0u) {
            slot = buffers_->register_view(span);
        } else {
            buffers_->update_view(slot, span);
        }
        return slot;
    }

    [[nodiscard]] LoadedGlb* find_loaded(f64 gltf_id) noexcept {
        if (!std::isfinite(gltf_id)) return nullptr;
        const auto it = gltfs_.find(static_cast<i32>(gltf_id));
        return it != gltfs_.end() ? it->second.get() : nullptr;
    }

    [[nodiscard]] GltfAsset* find_gltf(f64 gltf_id) noexcept {
        auto* rec = find_loaded(gltf_id);
        return rec ? &rec->gltf : nullptr;
    }

    [[nodiscard]] const GltfNode* find_node(f64 gltf_id, f64 node_id) noexcept {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(node_id);
        if (!gltf || !idx || *idx >= gltf->nodes.size()) return nullptr;
        return &gltf->nodes[*idx];
    }

    [[nodiscard]] const GltfCamera* find_camera(f64 gltf_id, f64 camera_id) noexcept {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(camera_id);
        if (!gltf || !idx || *idx >= gltf->cameras.size()) return nullptr;
        return &gltf->cameras[*idx];
    }

    [[nodiscard]] const GltfMesh* find_mesh(f64 gltf_id, f64 mesh_id) noexcept {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(mesh_id);
        if (!gltf || !idx || *idx >= gltf->meshes.size()) return nullptr;
        return &gltf->meshes[*idx];
    }

    [[nodiscard]] const GltfMesh::Primitive* find_primitive(f64 gltf_id, f64 mesh_id, f64 prim_id) noexcept {
        const auto* mesh = find_mesh(gltf_id, mesh_id);
        const auto idx = to_index(prim_id);
        if (!mesh || !idx || *idx >= mesh->primitives.size()) return nullptr;
        return &mesh->primitives[*idx];
    }

    [[nodiscard]] const GltfAccessor* find_accessor(f64 gltf_id, f64 accessor_id) noexcept {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(accessor_id);
        if (!gltf || !idx || *idx >= gltf->accessors.size()) return nullptr;
        return &gltf->accessors[*idx];
    }

    [[nodiscard]] const GltfMaterial* find_material(f64 gltf_id, f64 material_id) noexcept {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(material_id);
        if (!gltf || !idx || *idx >= gltf->materials.size()) return nullptr;
        return &gltf->materials[*idx];
    }

    [[nodiscard]] const GltfSampler* find_sampler_for_texture(f64 gltf_id, f64 texture_id) noexcept {
        const auto* gltf = find_gltf(gltf_id);
        const auto tid = to_index(texture_id);
        if (!gltf || !tid || *tid >= gltf->textures.size()) return nullptr;
        const auto& tex = gltf->textures[*tid];
        if (!tex.sampler.has_value() || *tex.sampler >= gltf->samplers.size()) return nullptr;
        return &gltf->samplers[*tex.sampler];
    }

    bool setup_sorted_weights(f64 gltf_id, f64 node_id) {
        const auto* node = find_node(gltf_id, node_id);
        if (!node || !node->has_weights) return false;
        const auto& weights = node->weights;
        sorted_weight_ids_.resize(weights.size());
        for (usize i = 0; i < weights.size(); ++i) {
            sorted_weight_ids_[i] = i;
        }
        std::stable_sort(sorted_weight_ids_.begin(), sorted_weight_ids_.end(), [&](usize a, usize b) {
            return weights[a] > weights[b];
        });
        sorted_weights_.assign(weights.size() * 4u, 0.0f);
        for (usize i = 0; i < sorted_weight_ids_.size(); ++i) {
            sorted_weights_[i * 4u] = weights[sorted_weight_ids_[i]];
        }
        return true;
    }

    // ---- Lifecycle ----

    f64 reset_all() {
        clear_state();
        return 0.0;
    }

    f64 load_glb(IExtensionHost& host, std::string_view working_directory, std::string_view filename) {
        if (filename.empty()) return -1.0;

        auto& vfs = host.ext_vfs();
        Result<std::vector<u8>> bytes = Error{"not loaded"};
        if (is_absolute_path_str(filename)) {
            bytes = vfs.read_all_bytes(filename);
        } else {
            if (!working_directory.empty()) {
                std::string combined(working_directory);
                if (combined.back() != '/' && combined.back() != '\\') {
                    combined.push_back('/');
                }
                combined.append(filename);
                bytes = vfs.read_all_bytes(combined);
            }
            if (!bytes) {
                bytes = vfs.read_all_bytes(filename);
            }
        }
        if (!bytes) {
            GM_LOG_WARN("ext", "gm82gltf: failed to open '%.*s'",
                        static_cast<int>(filename.size()), filename.data());
            return -1.0;
        }

        auto parsed = parse_glb(bytes.value());
        if (!parsed) {
            GM_LOG_WARN("ext", "gm82gltf: failed to parse GLB '%.*s'",
                        static_cast<int>(filename.size()), filename.data());
            return -1.0;
        }

        auto rec = std::make_unique<LoadedGlb>();
        rec->gltf = std::move(*parsed);
        rec->accessor_views.assign(rec->gltf.accessors.size(), 0u);
        rec->base_color_views.assign(rec->gltf.materials.size(), 0u);
        rec->emissive_color_views.assign(rec->gltf.materials.size(), 0u);

        const i32 id = next_id_++;
        gltfs_.emplace(id, std::move(rec));
        return static_cast<f64>(id);
    }

    f64 destroy_glb(f64 gltf_id) {
        if (!std::isfinite(gltf_id)) return 0.0;
        const auto it = gltfs_.find(static_cast<i32>(gltf_id));
        if (it != gltfs_.end()) {
            if (it->second) unregister_glb_views(*it->second);
            gltfs_.erase(it);
        }
        return 0.0;
    }

    // ---- Animations ----

    f64 animation_count(f64 gltf_id) {
        const auto* gltf = find_gltf(gltf_id);
        return gltf ? static_cast<f64>(gltf->animations.size()) : -1.0;
    }

    f64 get_animation(f64 gltf_id, std::string_view name) {
        const auto* gltf = find_gltf(gltf_id);
        if (!gltf) return -1.0;
        for (usize i = 0; i < gltf->animations.size(); ++i) {
            if (gltf->animations[i].name.has_value() && *gltf->animations[i].name == name) {
                return static_cast<f64>(i);
            }
        }
        return -1.0;
    }

    std::string_view animation_name(f64 gltf_id, f64 anim_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(anim_id);
        if (!gltf || !idx || *idx >= gltf->animations.size() || !gltf->animations[*idx].name.has_value()) {
            return {};
        }
        return *gltf->animations[*idx].name;
    }

    f64 animate(f64 gltf_id, f64 anim_id, f64 time) {
        auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(anim_id);
        if (!gltf || !idx) return -1.0;
        const auto done = gltf_apply_animation(*gltf, *idx, time);
        if (!done) return -1.0;
        return *done ? 1.0 : 0.0;
    }

    f64 animation_length(f64 gltf_id, f64 anim_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(anim_id);
        if (!gltf || !idx) return -1.0;
        const auto len = gltf_compute_animation_length(*gltf, *idx);
        return len.value_or(-1.0);
    }

    // ---- Skins ----

    f64 skin_skeleton(f64 gltf_id, f64 skin_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(skin_id);
        if (!gltf || !idx || *idx >= gltf->skins.size() || !gltf->skins[*idx].skeleton.has_value()) {
            return -1.0;
        }
        return static_cast<f64>(*gltf->skins[*idx].skeleton);
    }

    f64 skin_joint_count(f64 gltf_id, f64 skin_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(skin_id);
        if (!gltf || !idx || *idx >= gltf->skins.size()) return -1.0;
        return static_cast<f64>(gltf->skins[*idx].joints.size());
    }

    f64 skin_joints(f64 gltf_id, f64 skin_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(skin_id);
        if (!gltf || !idx) return -1.0;
        if (!gltf_compute_skin_matrices(*gltf, *idx, skin_matrices_)) return -1.0;
        const std::span<const u8> bytes(
            reinterpret_cast<const u8*>(skin_matrices_.data()),
            skin_matrices_.size() * sizeof(std::array<f32, 16>));
        return static_cast<f64>(update_scratch_view(skin_matrices_view_, bytes));
    }

    // ---- Scenes ----

    f64 default_scene(f64 gltf_id) {
        const auto* gltf = find_gltf(gltf_id);
        if (!gltf || !gltf->default_scene.has_value()) return -1.0;
        return static_cast<f64>(*gltf->default_scene);
    }

    f64 scene_count(f64 gltf_id) {
        const auto* gltf = find_gltf(gltf_id);
        return gltf ? static_cast<f64>(gltf->scenes.size()) : -1.0;
    }

    f64 get_scene(f64 gltf_id, std::string_view name) {
        const auto* gltf = find_gltf(gltf_id);
        if (!gltf) return -1.0;
        for (usize i = 0; i < gltf->scenes.size(); ++i) {
            if (gltf->scenes[i].name.has_value() && *gltf->scenes[i].name == name) {
                return static_cast<f64>(i);
            }
        }
        return -1.0;
    }

    std::string_view scene_name(f64 gltf_id, f64 scene_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(scene_id);
        if (!gltf || !idx || *idx >= gltf->scenes.size() || !gltf->scenes[*idx].name.has_value()) {
            return {};
        }
        return *gltf->scenes[*idx].name;
    }

    f64 scene_node_count(f64 gltf_id, f64 scene_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(scene_id);
        if (!gltf || !idx || *idx >= gltf->scenes.size()) return -1.0;
        return static_cast<f64>(gltf->scenes[*idx].nodes.size());
    }

    f64 scene_node(f64 gltf_id, f64 scene_id, f64 node_idx) {
        const auto* gltf = find_gltf(gltf_id);
        const auto sidx = to_index(scene_id);
        const auto nidx = to_index(node_idx);
        if (!gltf || !sidx || !nidx || *sidx >= gltf->scenes.size()) return -1.0;
        const auto& nodes = gltf->scenes[*sidx].nodes;
        if (*nidx >= nodes.size()) return -1.0;
        return static_cast<f64>(nodes[*nidx]);
    }

    // ---- Nodes ----

    f64 node_count(f64 gltf_id) {
        const auto* gltf = find_gltf(gltf_id);
        return gltf ? static_cast<f64>(gltf->nodes.size()) : -1.0;
    }

    f64 get_node(f64 gltf_id, std::string_view name) {
        const auto* gltf = find_gltf(gltf_id);
        if (!gltf) return -1.0;
        for (usize i = 0; i < gltf->nodes.size(); ++i) {
            if (gltf->nodes[i].name.has_value() && *gltf->nodes[i].name == name) {
                return static_cast<f64>(i);
            }
        }
        return -1.0;
    }

    std::string_view node_name(f64 gltf_id, f64 node_id) {
        const auto* node = find_node(gltf_id, node_id);
        if (!node || !node->name.has_value()) return {};
        return *node->name;
    }

    f64 node_tx(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->translation[0]) : -1.0; }
    f64 node_ty(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->translation[1]) : -1.0; }
    f64 node_tz(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->translation[2]) : -1.0; }
    f64 node_rx(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->rotation[0]) : -1.0; }
    f64 node_ry(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->rotation[1]) : -1.0; }
    f64 node_rz(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->rotation[2]) : -1.0; }
    f64 node_rw(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->rotation[3]) : -1.0; }
    f64 node_sx(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->scale[0]) : -1.0; }
    f64 node_sy(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->scale[1]) : -1.0; }
    f64 node_sz(f64 gltf_id, f64 node_id) { const auto* n = find_node(gltf_id, node_id); return n ? static_cast<f64>(n->scale[2]) : -1.0; }

    f64 node_matrix_pointer(f64 gltf_id, f64 node_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(node_id);
        if (!gltf || !idx || *idx >= gltf->nodes.size()) return -1.0;
        node_matrix_ = gltf_node_full_matrix(*gltf, *idx, std::nullopt);
        const std::span<const u8> bytes(
            reinterpret_cast<const u8*>(node_matrix_.data()),
            sizeof(node_matrix_));
        return static_cast<f64>(update_scratch_view(node_matrix_view_, bytes));
    }

    f64 node_parent(f64 gltf_id, f64 node_id) {
        const auto* n = find_node(gltf_id, node_id);
        return (n && n->parent.has_value()) ? static_cast<f64>(*n->parent) : -1.0;
    }

    f64 node_child_count(f64 gltf_id, f64 node_id) {
        const auto* n = find_node(gltf_id, node_id);
        return n ? static_cast<f64>(n->children.size()) : -1.0;
    }

    f64 node_child(f64 gltf_id, f64 node_id, f64 child_idx) {
        const auto* n = find_node(gltf_id, node_id);
        const auto idx = to_index(child_idx);
        if (!n || !idx || *idx >= n->children.size()) return -1.0;
        return static_cast<f64>(n->children[*idx]);
    }

    f64 node_camera(f64 gltf_id, f64 node_id) {
        const auto* n = find_node(gltf_id, node_id);
        return (n && n->camera.has_value()) ? static_cast<f64>(*n->camera) : -1.0;
    }

    f64 node_mesh(f64 gltf_id, f64 node_id) {
        const auto* n = find_node(gltf_id, node_id);
        return (n && n->mesh.has_value()) ? static_cast<f64>(*n->mesh) : -1.0;
    }

    f64 node_skin(f64 gltf_id, f64 node_id) {
        const auto* n = find_node(gltf_id, node_id);
        return (n && n->skin.has_value()) ? static_cast<f64>(*n->skin) : -1.0;
    }

    f64 node_weight_count(f64 gltf_id, f64 node_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(node_id);
        if (!gltf || !idx || *idx >= gltf->nodes.size()) return -1.0;
        const auto& n = gltf->nodes[*idx];
        if (n.has_weights) return static_cast<f64>(n.weights.size());
        if (!n.mesh.has_value() || *n.mesh >= gltf->meshes.size()) return -1.0;
        const auto& mesh = gltf->meshes[*n.mesh];
        return mesh.has_weights ? static_cast<f64>(mesh.weights.size()) : 0.0;
    }

    f64 node_sorted_morph(f64 gltf_id, f64 node_id, f64 morph_id) {
        if (!setup_sorted_weights(gltf_id, node_id)) return -1.0;
        const auto idx = to_index(morph_id);
        if (!idx || *idx >= sorted_weight_ids_.size()) return -1.0;
        return static_cast<f64>(sorted_weight_ids_[*idx]);
    }

    f64 node_sorted_weights_spaced_pointer(f64 gltf_id, f64 node_id) {
        if (!setup_sorted_weights(gltf_id, node_id)) return 0.0;
        const std::span<const u8> bytes(
            reinterpret_cast<const u8*>(sorted_weights_.data()),
            sorted_weights_.size() * sizeof(f32));
        return static_cast<f64>(update_scratch_view(sorted_weights_view_, bytes));
    }

    // ---- Cameras ----

    std::string_view camera_type(f64 gltf_id, f64 camera_id) {
        const auto* cam = find_camera(gltf_id, camera_id);
        return cam ? std::string_view(cam->type) : std::string_view{};
    }

    f64 camera_aspect(f64 gltf_id, f64 camera_id) {
        const auto* cam = find_camera(gltf_id, camera_id);
        return (cam && cam->perspective.has_value()) ? static_cast<f64>(cam->perspective->aspect_ratio) : -1.0;
    }

    f64 camera_yfov(f64 gltf_id, f64 camera_id) {
        const auto* cam = find_camera(gltf_id, camera_id);
        return (cam && cam->perspective.has_value()) ? static_cast<f64>(cam->perspective->yfov) : -1.0;
    }

    f64 camera_zfar(f64 gltf_id, f64 camera_id) {
        const auto* cam = find_camera(gltf_id, camera_id);
        return (cam && cam->perspective.has_value()) ? static_cast<f64>(cam->perspective->zfar) : -1.0;
    }

    f64 camera_znear(f64 gltf_id, f64 camera_id) {
        const auto* cam = find_camera(gltf_id, camera_id);
        return (cam && cam->perspective.has_value()) ? static_cast<f64>(cam->perspective->znear) : -1.0;
    }

    // ---- Meshes & Primitives ----

    f64 mesh_count(f64 gltf_id) {
        const auto* gltf = find_gltf(gltf_id);
        return gltf ? static_cast<f64>(gltf->meshes.size()) : -1.0;
    }

    f64 mesh_primitive_count(f64 gltf_id, f64 mesh_id) {
        const auto* mesh = find_mesh(gltf_id, mesh_id);
        return mesh ? static_cast<f64>(mesh->primitives.size()) : -1.0;
    }

    f64 mesh_primitive_material(f64 gltf_id, f64 mesh_id, f64 prim_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        return (prim && prim->material.has_value()) ? static_cast<f64>(*prim->material) : -1.0;
    }

    f64 mesh_primitive_mode(f64 gltf_id, f64 mesh_id, f64 prim_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        return prim ? static_cast<f64>(prim->mode) : -1.0;
    }

    f64 mesh_primitive_indices_accessor(f64 gltf_id, f64 mesh_id, f64 prim_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        return (prim && prim->indices.has_value()) ? static_cast<f64>(*prim->indices) : -1.0;
    }

    f64 mesh_primitive_attribute_count(f64 gltf_id, f64 mesh_id, f64 prim_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        return prim ? static_cast<f64>(prim->attributes.size()) : -1.0;
    }

    std::string_view mesh_primitive_attribute_semantic(f64 gltf_id, f64 mesh_id, f64 prim_id, f64 attr_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        const auto idx = to_index(attr_id);
        if (!prim || !idx || *idx >= prim->attributes.size()) return {};
        return prim->attributes[*idx].first;
    }

    f64 mesh_primitive_attribute_accessor(f64 gltf_id, f64 mesh_id, f64 prim_id, f64 attr_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        const auto idx = to_index(attr_id);
        if (!prim || !idx || *idx >= prim->attributes.size()) return -1.0;
        return static_cast<f64>(prim->attributes[*idx].second);
    }

    f64 mesh_primitive_morph_count(f64 gltf_id, f64 mesh_id, f64 prim_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        return prim ? static_cast<f64>(prim->targets.size()) : -1.0;
    }

    f64 mesh_primitive_morph(f64 gltf_id, f64 mesh_id, f64 prim_id, f64 morph_id, f64 attr_slot) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        const auto midx = to_index(morph_id);
        const auto aidx = to_index(attr_slot);
        if (!prim || !midx || !aidx || *midx >= prim->targets.size() || *aidx >= 3u) return -1.0;
        static constexpr std::array<std::string_view, 3> kMorphSemantics{"POSITION", "NORMAL", "TANGENT"};
        const std::string_view want = kMorphSemantics[*aidx];
        for (const auto& [k, v] : prim->targets[*midx]) {
            if (k == want) return static_cast<f64>(v);
        }
        return -1.0;
    }

    f64 mesh_primitive_morph_attribute_count(f64 gltf_id, f64 mesh_id, f64 prim_id, f64 morph_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        const auto midx = to_index(morph_id);
        if (!prim || !midx || *midx >= prim->targets.size()) return -1.0;
        return static_cast<f64>(prim->targets[*midx].size());
    }

    std::string_view mesh_primitive_morph_attribute_semantic(
        f64 gltf_id, f64 mesh_id, f64 prim_id, f64 morph_id, f64 attr_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        const auto midx = to_index(morph_id);
        const auto aidx = to_index(attr_id);
        if (!prim || !midx || !aidx || *midx >= prim->targets.size()) return {};
        const auto& target = prim->targets[*midx];
        if (*aidx >= target.size()) return {};
        return target[*aidx].first;
    }

    f64 mesh_primitive_morph_attribute_accessor(
        f64 gltf_id, f64 mesh_id, f64 prim_id, f64 morph_id, f64 attr_id) {
        const auto* prim = find_primitive(gltf_id, mesh_id, prim_id);
        const auto midx = to_index(morph_id);
        const auto aidx = to_index(attr_id);
        if (!prim || !midx || !aidx || *midx >= prim->targets.size()) return -1.0;
        const auto& target = prim->targets[*midx];
        if (*aidx >= target.size()) return -1.0;
        return static_cast<f64>(target[*aidx].second);
    }

    // ---- Accessors ----

    std::string_view accessor_type(f64 gltf_id, f64 accessor_id) {
        const auto* acc = find_accessor(gltf_id, accessor_id);
        return acc ? std::string_view(acc->type) : std::string_view{};
    }

    f64 accessor_component_type(f64 gltf_id, f64 accessor_id) {
        const auto* acc = find_accessor(gltf_id, accessor_id);
        return acc ? static_cast<f64>(acc->component_type) : -1.0;
    }

    f64 accessor_normalized(f64 gltf_id, f64 accessor_id) {
        const auto* acc = find_accessor(gltf_id, accessor_id);
        return acc ? (acc->normalized ? 1.0 : 0.0) : -1.0;
    }

    f64 accessor_stride(f64 gltf_id, f64 accessor_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(accessor_id);
        if (!gltf || !idx || *idx >= gltf->accessors.size()) return -1.0;
        return static_cast<f64>(gltf_accessor_effective_stride(*gltf, gltf->accessors[*idx]));
    }

    f64 accessor_size(f64 gltf_id, f64 accessor_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(accessor_id);
        if (!gltf || !idx || *idx >= gltf->accessors.size()) return -1.0;
        return static_cast<f64>(gltf_accessor_byte_size(*gltf, gltf->accessors[*idx]));
    }

    f64 accessor_copy(f64 gltf_id, f64 accessor_id, f64 dest_address, f64 dest_size) {
        if (!buffers_) return -1.0;
        const auto* gltf = find_gltf(gltf_id);
        const auto idx = to_index(accessor_id);
        const auto want_len = to_index(dest_size);
        if (!gltf || !idx || !want_len || *idx >= gltf->accessors.size()) return -1.0;

        const u32 addr = static_cast<u32>(static_cast<i64>(dest_address));
        auto* dst = buffers_->find_by_address(addr);
        if (!dst || dst->data.size() < *want_len) return -1.0;

        const auto& acc = gltf->accessors[*idx];
        if (acc.sparse.has_value()) {
            std::vector<u8> tmp;
            if (!gltf_materialize_sparse_accessor(*gltf, *idx, tmp)) return -1.0;
            if (tmp.size() != *want_len) return -1.0;
            if (*want_len > 0) std::memcpy(dst->data.data(), tmp.data(), *want_len);
            return 0.0;
        }

        const auto span = gltf_accessor_direct_span(*gltf, *idx);
        if (!span || span->size() != *want_len) return -1.0;
        if (*want_len > 0) std::memcpy(dst->data.data(), span->data(), *want_len);
        return 0.0;
    }

    f64 accessor_pointer(f64 gltf_id, f64 accessor_id) {
        if (!buffers_) return -1.0;
        auto* rec = find_loaded(gltf_id);
        const auto idx = to_index(accessor_id);
        if (!rec || !idx || *idx >= rec->gltf.accessors.size()) return -1.0;

        const auto& acc = rec->gltf.accessors[*idx];
        if (acc.sparse.has_value()) {
            if (!gltf_materialize_sparse_accessor(rec->gltf, *idx, sparse_data_)) return -1.0;
            return static_cast<f64>(update_scratch_view(sparse_data_view_, sparse_data_));
        }

        u32& cached = rec->accessor_views[*idx];
        if (cached != 0u) return static_cast<f64>(cached);

        const auto span = gltf_accessor_direct_span(rec->gltf, *idx);
        if (!span) return -1.0;
        cached = buffers_->register_view(*span);
        return static_cast<f64>(cached);
    }

    // ---- Materials ----

    f64 material_base_texture(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->pbr.base_color_texture.has_value()) ? static_cast<f64>(m->pbr.base_color_texture->index) : -1.0;
    }

    f64 material_roughness(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return m ? static_cast<f64>(m->pbr.roughness_factor) : -1.0;
    }

    f64 material_metalness(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return m ? static_cast<f64>(m->pbr.metallic_factor) : -1.0;
    }

    f64 material_roughness_texture(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->pbr.metallic_roughness_texture.has_value())
            ? static_cast<f64>(m->pbr.metallic_roughness_texture->index) : -1.0;
    }

    f64 material_roughness_texcoord(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->pbr.metallic_roughness_texture.has_value())
            ? static_cast<f64>(m->pbr.metallic_roughness_texture->tex_coord) : -1.0;
    }

    f64 material_base_color_pointer(f64 gltf_id, f64 material_id) {
        if (!buffers_) return -1.0;
        auto* rec = find_loaded(gltf_id);
        const auto idx = to_index(material_id);
        if (!rec || !idx || *idx >= rec->gltf.materials.size()) return -1.0;
        u32& cached = rec->base_color_views[*idx];
        if (cached == 0u) {
            const auto& col = rec->gltf.materials[*idx].pbr.base_color_factor;
            cached = buffers_->register_view(std::span<const u8>(
                reinterpret_cast<const u8*>(col.data()), sizeof(col)));
        }
        return static_cast<f64>(cached);
    }

    f64 material_base_texcoord(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->pbr.base_color_texture.has_value()) ? static_cast<f64>(m->pbr.base_color_texture->tex_coord) : -1.0;
    }

    f64 material_normal_texture(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->normal_texture.has_value()) ? static_cast<f64>(m->normal_texture->index) : -1.0;
    }

    f64 material_normal_texcoord(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->normal_texture.has_value()) ? static_cast<f64>(m->normal_texture->tex_coord) : -1.0;
    }

    f64 material_normal_scale(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->normal_texture.has_value()) ? static_cast<f64>(m->normal_texture->scale) : -1.0;
    }

    f64 material_occlusion_texture(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->occlusion_texture.has_value()) ? static_cast<f64>(m->occlusion_texture->index) : -1.0;
    }

    f64 material_occlusion_texcoord(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->occlusion_texture.has_value()) ? static_cast<f64>(m->occlusion_texture->tex_coord) : -1.0;
    }

    f64 material_occlusion_strength(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->occlusion_texture.has_value()) ? static_cast<f64>(m->occlusion_texture->strength) : -1.0;
    }

    f64 material_emissive_texture(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->emissive_texture.has_value()) ? static_cast<f64>(m->emissive_texture->index) : -1.0;
    }

    f64 material_emissive_texcoord(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return (m && m->emissive_texture.has_value()) ? static_cast<f64>(m->emissive_texture->tex_coord) : -1.0;
    }

    f64 material_emissive_color_pointer(f64 gltf_id, f64 material_id) {
        if (!buffers_) return -1.0;
        auto* rec = find_loaded(gltf_id);
        const auto idx = to_index(material_id);
        if (!rec || !idx || *idx >= rec->gltf.materials.size()) return -1.0;
        u32& cached = rec->emissive_color_views[*idx];
        if (cached == 0u) {
            const auto& col = rec->gltf.materials[*idx].emissive_factor;
            cached = buffers_->register_view(std::span<const u8>(
                reinterpret_cast<const u8*>(col.data()), sizeof(col)));
        }
        return static_cast<f64>(cached);
    }

    std::string_view material_alpha_mode(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return m ? std::string_view(m->alpha_mode) : std::string_view{};
    }

    f64 material_alpha_cutoff(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return m ? static_cast<f64>(m->alpha_cutoff) : -1.0;
    }

    f64 material_double_sided(f64 gltf_id, f64 material_id) {
        const auto* m = find_material(gltf_id, material_id);
        return m ? (m->double_sided ? 1.0 : 0.0) : -1.0;
    }

    // ---- Textures ----

    f64 texture_count(f64 gltf_id) {
        const auto* gltf = find_gltf(gltf_id);
        return gltf ? static_cast<f64>(gltf->textures.size()) : -1.0;
    }

    f64 texture_wrap_h(f64 gltf_id, f64 texture_id) {
        const auto* s = find_sampler_for_texture(gltf_id, texture_id);
        return s ? static_cast<f64>(s->wrap_s) : 10497.0;
    }

    f64 texture_wrap_v(f64 gltf_id, f64 texture_id) {
        const auto* s = find_sampler_for_texture(gltf_id, texture_id);
        return s ? static_cast<f64>(s->wrap_t) : 10497.0;
    }

    f64 texture_interpolation(f64 gltf_id, f64 texture_id) {
        const auto* s = find_sampler_for_texture(gltf_id, texture_id);
        if (!s) return 10497.0;
        return (!s->mag_filter.has_value() || *s->mag_filter != 9728u) ? 1.0 : 0.0;
    }

    std::string_view texture_type(f64 gltf_id, f64 texture_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto tid = to_index(texture_id);
        if (!gltf || !tid || *tid >= gltf->textures.size()) return {};
        const auto& tex = gltf->textures[*tid];
        if (!tex.source.has_value() || *tex.source >= gltf->images.size()) return {};
        const auto& img = gltf->images[*tex.source];
        return img.mime_type.has_value() ? std::string_view(*img.mime_type) : std::string_view{};
    }

    f64 texture_copy(f64 gltf_id, f64 texture_id, f64 dest_address, f64 dest_size) {
        if (!buffers_) return -1.0;
        const auto* gltf = find_gltf(gltf_id);
        const auto tid = to_index(texture_id);
        const auto want_len = to_index(dest_size);
        if (!gltf || !tid || !want_len) return -1.0;

        const auto img_bytes = gltf_texture_image_bytes(*gltf, *tid);
        if (!img_bytes || img_bytes->size() != *want_len) return -1.0;

        const u32 addr = static_cast<u32>(static_cast<i64>(dest_address));
        auto* dst = buffers_->find_by_address(addr);
        if (!dst || dst->data.size() < *want_len) return -1.0;
        if (*want_len > 0) std::memcpy(dst->data.data(), img_bytes->data(), *want_len);
        return 0.0;
    }

    f64 texture_save(IExtensionHost& host, f64 gltf_id, f64 texture_id, std::string_view filename) {
        const auto* gltf = find_gltf(gltf_id);
        const auto tid = to_index(texture_id);
        if (!gltf || !tid || filename.empty()) return -1.0;

        const auto img_bytes = gltf_texture_image_bytes(*gltf, *tid);
        if (!img_bytes) return -1.0;
        return host.ext_vfs().write_all_bytes(filename, *img_bytes) ? 0.0 : -1.0;
    }

    f64 texture_size(f64 gltf_id, f64 texture_id) {
        const auto* gltf = find_gltf(gltf_id);
        const auto tid = to_index(texture_id);
        if (!gltf || !tid) return -1.0;
        const auto img_bytes = gltf_texture_image_bytes(*gltf, *tid);
        return img_bytes ? static_cast<f64>(img_bytes->size()) : -1.0;
    }

    GmBufferStore* buffers_ = nullptr;
    std::unordered_map<i32, std::unique_ptr<LoadedGlb>> gltfs_;
    i32 next_id_ = 1;

    // Separate reusable scratch buffers so skin joints and node matrices never alias.
    std::array<f32, 16> node_matrix_{};
    u32 node_matrix_view_ = 0u;
    std::vector<std::array<f32, 16>> skin_matrices_;
    u32 skin_matrices_view_ = 0u;
    std::vector<f32> sorted_weights_;
    std::vector<usize> sorted_weight_ids_;
    u32 sorted_weights_view_ = 0u;
    std::vector<u8> sparse_data_;
    u32 sparse_data_view_ = 0u;
};

} // namespace

std::unique_ptr<IExtension> create_gm82gltf_extension() {
    return std::make_unique<Gm82GltfExtension>();
}

} // namespace gm
