#include "extensions/core/extension.h"

#include <memory>
#include <string>
#include <vector>

namespace gm {

namespace {

class Gm82TestExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "gm82test"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_aliases({"inst_meta", "gm82test"});

        reg.bind<&Gm82TestExtension::reset_iter>(this, {"__gm82_inst_meta_init", "__gm82_inst_meta_final"});
        reg.bind<&Gm82TestExtension::var_names_start>(this, "variable_instance_names_start");
        reg.bind<&Gm82TestExtension::var_names_next>(this, "variable_instance_names_next");
        reg.bind("__gm82_var_name_count", [](IExtensionHost& host) {
            return host.ext_var_name_count();
        });
        reg.bind("__gm82_var_name_get", [](const CallContext& ctx) {
            return RValue(ctx.host.ext_var_name_get(ctx.i32_arg(0, -1)));
        });
    }

    void on_unload(IExtensionHost&) override {
        reset_iter();
    }

private:
    f64 reset_iter() {
        iter_names_.clear();
        iter_index_ = 0;
        return 0.0;
    }

    f64 var_names_start(const CallContext& ctx) {
        const i32 inst_id = ctx.i32_arg(0, -1);
        iter_names_ = ctx.host.ext_instance_var_names(inst_id);
        iter_index_ = 0;
        return static_cast<f64>(iter_names_.size());
    }

    std::string var_names_next() {
        if (iter_index_ >= iter_names_.size()) {
            return {};
        }
        return iter_names_[iter_index_++];
    }

    std::vector<std::string> iter_names_;
    usize iter_index_ = 0;
};

} // namespace

std::unique_ptr<IExtension> create_gm82test_extension() {
    return std::make_unique<Gm82TestExtension>();
}

} // namespace gm
