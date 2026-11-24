#include "extensions/core/extension.h"

#include <memory>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <cstdio>
#include <unistd.h>
#endif

namespace gm {

namespace {

f64 query_process_memory_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<f64>(pmc.WorkingSetSize);
    }
    return 32.0 * 1024.0 * 1024.0;
#elif defined(__APPLE__)
    mach_task_basic_info info{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS) {
        return static_cast<f64>(info.resident_size);
    }
    return 32.0 * 1024.0 * 1024.0;
#elif defined(__linux__)
    long rss_pages = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        long total_pages = 0;
        if (std::fscanf(f, "%ld %ld", &total_pages, &rss_pages) == 2) {
            std::fclose(f);
            return static_cast<f64>(rss_pages) * static_cast<f64>(sysconf(_SC_PAGESIZE));
        }
        std::fclose(f);
    }
    return 32.0 * 1024.0 * 1024.0;
#else
    return 32.0 * 1024.0 * 1024.0;
#endif
}

class CleanMemExtension final : public IExtension {
public:
    std::string_view canonical_name() const noexcept override { return "CleanMem"; }

    void on_register(ExtensionRegistrar& reg) override {
        reg.add_dll_alias("cleanmem");
        reg.set_case_insensitive(true);
        reg.bind_const({"halo_shg_clean", "cleanmem"}, 1.0);
        reg.bind<&query_process_memory_bytes>("halo_shg_get_mem");
    }
};

} // namespace

std::unique_ptr<IExtension> create_cleanmem_extension() {
    return std::make_unique<CleanMemExtension>();
}

} // namespace gm
