#include "extensions/shim_cleanmem.h"

#include "base/encoding.h"

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

class CleanMemShim final : public IDllShim {
public:
    std::string_view canonical_name() const noexcept override { return "CleanMem"; }

    bool matches_dll(std::string_view normalized_dll) const noexcept override {
        return normalized_dll == "cleanmem";
    }

    bool has_function(std::string_view func_name) const noexcept override {
        const std::string lo = ascii_to_lower(func_name);
        return lo == "halo_shg_clean" || lo == "halo_shg_get_mem" || lo == "cleanmem";
    }

    RValue invoke(
        IExtensionHost&,
        std::string_view func_name,
        std::span<const RValue>,
        ExternalValueType) override {
        const std::string lo = ascii_to_lower(func_name);
        if (lo == "halo_shg_clean" || lo == "cleanmem") {
            return RValue(1.0);
        }
        if (lo == "halo_shg_get_mem") {
            return RValue(query_process_memory_bytes());
        }
        return RValue(0.0);
    }
};

} // namespace

std::unique_ptr<IDllShim> create_cleanmem_shim() {
    return std::make_unique<CleanMemShim>();
}

} // namespace gm
