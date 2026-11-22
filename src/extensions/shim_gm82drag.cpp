#include "extensions/shim_gm82drag.h"

namespace gm {

namespace {

class Gm82DragShim final : public IDllShim {
public:
    std::string_view canonical_name() const noexcept override { return "gm82drag"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "gm82drag" || normalized_dll == "draggerdll";
    }

    bool has_function(std::string_view fn) const noexcept override {
        return fn == "AcceptFiles" ||
               fn == "CheckDraggedNum" ||
               fn == "CheckDraggedFname" ||
               fn == "ClearLastDrop";
    }

    void on_unload(IExtensionHost& host) override {
        enabled_ = false;
        host.ext_window().input_mut().dropped_files.clear();
    }

    RValue invoke(
        IExtensionHost& host,
        std::string_view fn,
        std::span<const RValue> args,
        ExternalValueType) override {
        auto& files = host.ext_window().input_mut().dropped_files;

        if (fn == "AcceptFiles") {
            enabled_ = shim_bool(args, 1, false);
            if (!enabled_) {
                files.clear();
            }
            return RValue(1.0);
        }
        if (fn == "CheckDraggedNum") {
            if (!enabled_ || files.empty()) {
                return RValue(-1.0);
            }
            return RValue(static_cast<f64>(files.size()));
        }
        if (fn == "CheckDraggedFname") {
            if (!enabled_ || files.empty()) {
                return RValue(std::string{});
            }
            const i32 idx = shim_i32(args, 1, 0);
            if (files.size() == 1) {
                return RValue(files[0]);
            }
            if (idx < 0 || static_cast<usize>(idx) >= files.size()) {
                return RValue(std::string{});
            }
            return RValue(files[static_cast<usize>(idx)]);
        }
        if (fn == "ClearLastDrop") {
            files.clear();
            return RValue(1.0);
        }
        return RValue(0.0);
    }

private:
    bool enabled_ = false;
};

} // namespace

std::unique_ptr<IDllShim> create_gm82drag_shim() {
    return std::make_unique<Gm82DragShim>();
}

} // namespace gm
