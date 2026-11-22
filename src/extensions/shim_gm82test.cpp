#include "extensions/shim_gm82test.h"

#include <string>
#include <vector>

namespace gm {

namespace {

class Gm82TestShim final : public IDllShim {
public:
    std::string_view canonical_name() const noexcept override { return "gm82test"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "inst_meta" || normalized_dll == "gm82test";
    }

    bool has_function(std::string_view fn) const noexcept override {
        return fn == "__gm82_inst_meta_init" ||
               fn == "__gm82_inst_meta_final" ||
               fn == "variable_instance_names_start" ||
               fn == "variable_instance_names_next" ||
               fn == "__gm82_var_name_count" ||
               fn == "__gm82_var_name_get";
    }

    void on_unload(IExtensionHost&) override {
        iter_names_.clear();
        iter_index_ = 0;
    }

    RValue invoke(
        IExtensionHost& host,
        std::string_view fn,
        std::span<const RValue> args,
        ExternalValueType) override {
        if (fn == "__gm82_inst_meta_init") {
            iter_names_.clear();
            iter_index_ = 0;
            return RValue(0.0);
        }
        if (fn == "__gm82_inst_meta_final") {
            iter_names_.clear();
            iter_index_ = 0;
            return RValue(0.0);
        }
        if (fn == "variable_instance_names_start") {
            const i32 inst_id = shim_i32(args, 0, -1);
            iter_names_ = host.ext_instance_var_names(inst_id);
            iter_index_ = 0;
            return RValue(static_cast<f64>(iter_names_.size()));
        }
        if (fn == "variable_instance_names_next") {
            if (iter_index_ >= iter_names_.size()) {
                return RValue(std::string{});
            }
            return RValue(iter_names_[iter_index_++]);
        }
        if (fn == "__gm82_var_name_count") {
            return RValue(static_cast<f64>(host.ext_var_name_count()));
        }
        if (fn == "__gm82_var_name_get") {
            return RValue(host.ext_var_name_get(shim_i32(args, 0, -1)));
        }
        return RValue(0.0);
    }

private:
    std::vector<std::string> iter_names_;
    usize iter_index_ = 0;
};

} // namespace

std::unique_ptr<IDllShim> create_gm82test_shim() {
    return std::make_unique<Gm82TestShim>();
}

} // namespace gm
