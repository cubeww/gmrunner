# Adding a Native Extension Module

Every directory under `src/extensions/modules/<name>/` containing a `CMakeLists.txt` is **auto-discovered** by CMake (`CONFIGURE_DEPENDS`) and compiled into `gm_extensions`. You do **not** need to edit `src/extensions/CMakeLists.txt` or `extension_registry.cpp`.

## Quick Start (2 Files)

Create `src/extensions/modules/myext/`:

### 1. `CMakeLists.txt`

```cmake
gmrunner_add_extension(
    NAME myext
    FACTORY create_myext_extension
    SOURCES myext_extension.cpp
    # Optional arguments:
    # OPTION GMRUNNER_EXT_MYEXT         # Adds a CMake option() toggle
    # DEFAULT ON                        # Default for OPTION
    # PRIORITY 0                        # Higher registers later (wins DLL alias conflicts)
    # THIRD_PARTY_SOURCES engine/*.cpp  # Compiled with warnings suppressed
    # PRIVATE_INCLUDE_DIRS ...
    # LINK_LIBS SDL3::SDL3
    # WIN32_LIBS ws2_32
)
```

### 2. `myext_extension.cpp`

```cpp
#include "extensions/core/extension.h"

#include <memory>
#include <string>

namespace gm {

namespace {

// Plain C / free function: arguments and return values are auto-marshaled at compile time.
double myext_add(double a, double b) {
    return a + b;
}

class MyExtExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "myext"; }

    void on_register(ExtensionRegistrar& reg) override {
        // DLL stems (lowercase, without ".dll") matched by external_define / GEX calls.
        reg.add_dll_aliases({"myext", "myext_legacy"});

        // 1. Compile-time typed free/C function binding (zero heap allocation):
        GM_EXT_BIND(reg, myext_add);
        // or: reg.bind<&myext_add>("myext_add");

        // 2. Member function binding (typed or raw CallContext):
        reg.bind<&MyExtExtension::get_counter>(this, "myext_get_counter");

        // 3. Inline lambda (supports typed args, leading IExtensionHost&, or const CallContext&):
        reg.bind("myext_greet", [](std::string_view name) {
            return std::string("Hello, ") + std::string(name);
        });

        // 4. Constants & no-op stubs:
        reg.bind_const("myext_version", 1.0);
        reg.bind_stub({"myext_noop_a", "myext_noop_b"}, 0.0);
    }

    void on_unload(IExtensionHost&) override {
        counter_ = 0;
    }

private:
    f64 get_counter() { return static_cast<f64>(counter_++); }

    i32 counter_ = 0;
};

} // namespace

std::unique_ptr<IExtension> create_myext_extension() {
    return std::make_unique<MyExtExtension>();
}

} // namespace gm
```

## Advanced Features

- **Cross-extension shared services**: Access shared services such as `GmBufferStore` inside `on_register`:
  ```cpp
  buffers_ = &reg.services().get_or_create<GmBufferStore>();
  ```
- **Case-insensitive export names**: `reg.set_case_insensitive(true);`
- **Shared prefix aliases**: `reg.add_prefix_aliases({"__myext_"});`
- **Per-extension middleware**: Call `reg.set_custom_dispatch(true)`, group exports with `reg.with_flags(kMyFlag, [&](ExportTable& e) { ... })`, and override `IExtension::dispatch(const ExportedFunction&, const CallContext&)`.
- **Dynamic / wildcard export names**: Override `IExtension::resolve_dynamic(std::string_view func_name)`.
- **Large multi-file extensions**: Split bindings across multiple `.cpp` files that each take `ExtensionRegistrar&` (or `ExportTable&`), and put third-party C/C++ library sources in `THIRD_PARTY_SOURCES` so strict engine warning flags only apply to your binding code.
