#pragma once

#include "base/types.h"
#include "extensions/core/extension_services.h"

#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace gm {

// Shared in-memory buffer registry for gm82buf and cross-extension pseudo-address access
// (gm82core include_file_get_buffer, gm82dx8/gm82dx9 surface/light/matrix buffer transfers,
// and gm82gltf accessor/matrix/material pointer views).
// Uses 32-bit synthetic addresses so GML reals can pass buffer addresses safely on 64-bit targets.
struct GmBuffer {
    std::vector<u8> data;
    usize pos = 0;
    bool error = false;
};

class GmBufferStore final : public IExtensionService {
public:
    static constexpr u32 kPseudoAddrBase = 0x20000000u;
    static constexpr u32 kViewAddrBase   = 0x30000000u;

    void reset() override {
        clear_all();
    }

    u32 create() {
        const u32 id = next_id_++;
        buffers_.emplace(id, GmBuffer{});
        return id;
    }

    bool destroy(u32 id) {
        return buffers_.erase(id) > 0;
    }

    void clear_all() {
        buffers_.clear();
        views_.clear();
        next_id_ = 1;
        next_view_id_ = 1;
    }

    [[nodiscard]] bool exists(u32 id) const noexcept {
        return buffers_.contains(id);
    }

    [[nodiscard]] GmBuffer* find(u32 id) noexcept {
        const auto it = buffers_.find(id);
        return it != buffers_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] const GmBuffer* find(u32 id) const noexcept {
        const auto it = buffers_.find(id);
        return it != buffers_.end() ? &it->second : nullptr;
    }

    [[nodiscard]] u32 pseudo_address(u32 id) const noexcept {
        const auto* b = find(id);
        if (!b || b->data.empty()) return 0u;
        return kPseudoAddrBase + id;
    }

    [[nodiscard]] GmBuffer* find_by_address(u32 pseudo_addr) noexcept {
        if (pseudo_addr <= kPseudoAddrBase || pseudo_addr >= kViewAddrBase) return nullptr;
        return find(pseudo_addr - kPseudoAddrBase);
    }

    [[nodiscard]] const GmBuffer* find_by_address(u32 pseudo_addr) const noexcept {
        if (pseudo_addr <= kPseudoAddrBase || pseudo_addr >= kViewAddrBase) return nullptr;
        return find(pseudo_addr - kPseudoAddrBase);
    }

    // Registers a non-owning read-only byte span under a 32-bit synthetic address so
    // extensions such as gm82gltf can pass raw memory slices into gm82dx9 buffer APIs.
    [[nodiscard]] u32 register_view(std::span<const u8> data) {
        const u32 id = next_view_id_++;
        views_.emplace(id, data);
        return kViewAddrBase + id;
    }

    void update_view(u32 view_addr, std::span<const u8> data) {
        if (view_addr <= kViewAddrBase) return;
        views_[view_addr - kViewAddrBase] = data;
    }

    bool unregister_view(u32 view_addr) {
        if (view_addr <= kViewAddrBase) return false;
        return views_.erase(view_addr - kViewAddrBase) > 0;
    }

    // Resolves either a writable GmBuffer pseudo-address or a read-only external view address.
    [[nodiscard]] std::optional<std::span<const u8>> find_read_address(u32 pseudo_addr) const noexcept {
        if (pseudo_addr > kViewAddrBase) {
            const auto it = views_.find(pseudo_addr - kViewAddrBase);
            if (it == views_.end()) return std::nullopt;
            return it->second;
        }
        if (const auto* b = find_by_address(pseudo_addr)) {
            return std::span<const u8>(b->data);
        }
        return std::nullopt;
    }

private:
    std::unordered_map<u32, GmBuffer> buffers_;
    std::unordered_map<u32, std::span<const u8>> views_;
    u32 next_id_ = 1;
    u32 next_view_id_ = 1;
};

} // namespace gm
