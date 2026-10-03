#include "dng_halide_device.h"
#include <cstdlib>

// W5 (2026-08-21, Windows port): Vulkan is the GPU backend on Android, Windows
// and Linux (see CMakeLists.txt AOT_TARGET), so every "Vulkan backend" guard in
// this file covers all three. There is no CPU fallback route
// in the pipeline (dng_pipeline.cpp requireGpuBackend), so a platform that
// falls through to kUnsupported cannot decode at all.
// DNG_FORCE_VULKAN (F-R3-1 MoltenVK arbitration, default OFF): route Apple to
// the Vulkan interface instead of Metal so a Vulkan AOT build can run over
// MoltenVK. OFF keeps the committed Apple=Metal binding unchanged.
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
#include "HalideRuntimeMetal.h"
#include "dng_metal_context.h"  // R1-T2: ceyx strong override of the weak Halide Metal context hooks
#include "render_parameter_upload_cache.h"
#elif defined(__ANDROID__) || defined(_WIN32) || defined(__linux__) || defined(DNG_FORCE_VULKAN)
#include "HalideRuntimeVulkan.h"
// Not declared in any public Halide v21.0.0 header; both are extern "C" WEAK
// definitions in the Vulkan runtime linked on every Vulkan leg.
extern "C" bool halide_vulkan_is_initialized();
extern "C" int halide_vulkan_release_unused_device_allocations(void *user_context);
#endif

namespace {

enum class GpuBackend { kMetal, kVulkan, kUnsupported };

GpuBackend resolve_backend() {
    const char* env = std::getenv("DNG_GPU_BACKEND");
    if (env) {
        if (env[0] == 'm' || env[0] == 'M') return GpuBackend::kMetal;
        if (env[0] == 'v' || env[0] == 'V') return GpuBackend::kVulkan;
        return GpuBackend::kUnsupported;
    }
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
    return GpuBackend::kMetal;
#elif defined(__ANDROID__) || defined(_WIN32) || defined(__linux__) || defined(DNG_FORCE_VULKAN)
    return GpuBackend::kVulkan;
#else
    return GpuBackend::kUnsupported;
#endif
}

GpuBackend cached_backend() {
    static const GpuBackend b = resolve_backend();
    return b;
}

} // namespace

const halide_device_interface_t* dng_halide_gpu_device_interface() {
    switch (cached_backend()) {
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
    case GpuBackend::kMetal:
        return halide_metal_device_interface();
#endif
#if defined(__ANDROID__) || defined(_WIN32) || defined(__linux__) || defined(DNG_FORCE_VULKAN)
    case GpuBackend::kVulkan:
        return halide_vulkan_device_interface();
#endif
    default:
        return nullptr;
    }
}

bool dng_halide_gpu_available() {
    return dng_halide_gpu_device_interface() != nullptr;
}

const char* dng_halide_gpu_backend_name() {
    switch (cached_backend()) {
    case GpuBackend::kMetal:       return "metal";
    case GpuBackend::kVulkan:      return "vulkan";
    case GpuBackend::kUnsupported: return "unsupported";
    }
    return "unknown";
}

DngDeviceReleaseResult dng_halide_release_unused_device_memory() {
    switch (cached_backend()) {
#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)
    case GpuBackend::kMetal:
        if (ceyx::metal_shared_device_handle() == nullptr) {
            return DngDeviceReleaseResult::kSkippedUninitialized;
        }
        // Halide v21.0.0's Metal runtime keeps no allocation pool:
        // halide_metal_device_free releases the MTLBuffer at once. Evidence:
        // Halcyon docs/logs/memory-reclamation-campaign/f3-metal-device-pool.md.
        // The Metal device memory ceyx itself keeps past a decode is the C3
        // parameter cache (released here) and the arena lanes (funnel step 1).
        ceyx::render_parameter_cache_release_all_lanes();
        return DngDeviceReleaseResult::kReleased;
#endif
#if defined(__ANDROID__) || defined(_WIN32) || defined(__linux__) || defined(DNG_FORCE_VULKAN)
    case GpuBackend::kVulkan:
        // Unguarded, the release call creates a Vulkan instance and device in a
        // process that never used the GPU.
        if (!halide_vulkan_is_initialized()) {
            return DngDeviceReleaseResult::kSkippedUninitialized;
        }
        return halide_vulkan_release_unused_device_allocations(nullptr) == 0
                   ? DngDeviceReleaseResult::kReleased
                   : DngDeviceReleaseResult::kError;
#endif
    default:
        return DngDeviceReleaseResult::kSkippedUninitialized;
    }
}
