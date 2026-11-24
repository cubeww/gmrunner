#include "extensions/core/extension.h"

#include <memory>
#include <string>

namespace gm {

namespace {

class Gm82DragExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "gm82drag"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"gm82drag", "draggerdll"});

        reg.bind("AcceptFiles", [this](const CallContext& ctx) {
            auto& files = ctx.host.ext_window().input_mut().dropped_files;
            enabled_ = ctx.boolean(1, false);
            if (!enabled_) {
                files.clear();
            }
            return RValue(1.0);
        });

        reg.bind("CheckDraggedNum", [this](const CallContext& ctx) {
            const auto& files = ctx.host.ext_window().input_mut().dropped_files;
            if (!enabled_ || files.empty()) {
                return RValue(-1.0);
            }
            return RValue(static_cast<f64>(files.size()));
        });

        reg.bind("CheckDraggedFname", [this](const CallContext& ctx) {
            const auto& files = ctx.host.ext_window().input_mut().dropped_files;
            if (!enabled_ || files.empty()) {
                return RValue(std::string{});
            }
            const i32 idx = ctx.i32_arg(1, 0);
            if (files.size() == 1) {
                return RValue(files[0]);
            }
            if (idx < 0 || static_cast<usize>(idx) >= files.size()) {
                return RValue(std::string{});
            }
            return RValue(files[static_cast<usize>(idx)]);
        });

        reg.bind("ClearLastDrop", [](IExtensionHost& host) {
            host.ext_window().input_mut().dropped_files.clear();
            return 1.0;
        });
    }

    void on_unload(IExtensionHost& host) override {
        enabled_ = false;
        host.ext_window().input_mut().dropped_files.clear();
    }

private:
    bool enabled_ = false;
};

} // namespace

std::unique_ptr<IExtension> create_gm82drag_extension() {
    return std::make_unique<Gm82DragExtension>();
}

} // namespace gm
