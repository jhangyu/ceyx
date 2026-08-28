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
#elif defined(__ANDROID__) || defined(_WIN32) || defined(__linux__) || defined(DNG_FORCE_VULKAN)
#include "HalideRuntimeVulkan.h"
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
