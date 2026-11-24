#pragma once

#include "base/types.h"

#include <concepts>
#include <memory>
#include <unordered_map>
#include <utility>

namespace gm {

// Base interface for cross-extension shared services (e.g. GmBufferStore).
// Registered lazily via ExtensionServices::get_or_create<T>() and reset
// uniformly when ExtensionRegistry::reset() runs.
class IExtensionService {
public:
    virtual ~IExtensionService() = default;
    virtual void reset() {}
};

// Lightweight, zero-RTTI typed service container owned by ExtensionRegistry.
class ExtensionServices : public NonCopyable {
public:
    ExtensionServices() = default;
    ~ExtensionServices() = default;

    template <std::derived_from<IExtensionService> T, typename... Args>
    [[nodiscard]] T& get_or_create(Args&&... args) {
        const void* key = type_key<T>();
        if (const auto it = services_.find(key); it != services_.end()) {
            return *static_cast<T*>(it->second.get());
        }
        auto svc = std::make_unique<T>(std::forward<Args>(args)...);
        T& ref = *svc;
        services_.emplace(key, std::move(svc));
        return ref;
    }

    template <std::derived_from<IExtensionService> T>
    [[nodiscard]] T* find() noexcept {
        const auto it = services_.find(type_key<T>());
        return it != services_.end() ? static_cast<T*>(it->second.get()) : nullptr;
    }

    template <std::derived_from<IExtensionService> T>
    [[nodiscard]] const T* find() const noexcept {
        const auto it = services_.find(type_key<T>());
        return it != services_.end() ? static_cast<const T*>(it->second.get()) : nullptr;
    }

    void reset_all() {
        for (auto& [_, svc] : services_) {
            if (svc) svc->reset();
        }
    }

    void clear() noexcept {
        services_.clear();
    }

private:
    template <typename T>
    [[nodiscard]] static const void* type_key() noexcept {
        static constexpr char kTag = 0;
        return &kTag;
    }

    std::unordered_map<const void*, std::unique_ptr<IExtensionService>> services_;
};

} // namespace gm
