#pragma once

#include "base/types.h"
#include "extensions/core/extension_services.h"

#include <unordered_map>
#include <vector>

namespace gm {

// Shared in-memory buffer registry for gm82buf and cross-extension pseudo-address access
// (gm82core include_file_get_buffer, gm82dx8/gm82dx9 surface/light/matrix buffer transfers).
// Uses 32-bit synthetic addresses so GML reals can pass buffer addresses safely on 64-bit targets.
struct GmBuffer {
    std::vector<u8> data;
    usize pos = 0;
    bool error = false;
};

class GmBufferStore final : public IExtensionService {
public:
    static constexpr u32 kPseudoAddrBase = 0x20000000u;

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
        next_id_ = 1;
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
        if (pseudo_addr <= kPseudoAddrBase) return nullptr;
        return find(pseudo_addr - kPseudoAddrBase);
    }

private:
    std::unordered_map<u32, GmBuffer> buffers_;
    u32 next_id_ = 1;
};

} // namespace gm
