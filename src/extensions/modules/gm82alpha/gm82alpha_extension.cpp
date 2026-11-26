#include "extensions/core/extension.h"

#include <memory>

namespace gm {

namespace {

class Gm82AlphaExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "gm82alpha"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_alias("gm82alpha");
        reg.add_prefix_alias("__gm82alpha_");

        reg.bind("setcomposite", [](const CallContext& ctx) {
            const bool enable = ctx.real(1, 0.0) >= 0.5;
            ctx.host.ext_window().set_composite_alpha(enable);
            return RValue(0.0);
        });

        reg.bind("setchromakey", [](const CallContext& ctx) {
            const bool enable = ctx.real(1, 0.0) >= 0.5;
            const u32 color = static_cast<u32>(static_cast<i64>(ctx.real(2, 0.0))) & 0x00FFFFFFu;
            ctx.host.ext_window().set_chromakey(enable, color);
            return RValue(0.0);
        });
    }

    void on_unload(IExtensionHost& host) override {
        host.ext_window().set_composite_alpha(false);
        host.ext_window().set_chromakey(false, 0);
        host.ext_renderer().set_alpha_buffer(false);
    }
};

} // namespace

std::unique_ptr<IExtension> create_gm82alpha_extension() {
    return std::make_unique<Gm82AlphaExtension>();
}

} // namespace gm
