#include "dng_halide_device.h"
#include <cstdlib>

#if defined(__APPLE__)
#include "HalideRuntimeMetal.h"
#elif defined(__ANDROID__)
#include "HalideRuntimeVulkan.h"
#endif

namespace {

enum class GpuBackend { kMetal, kVulkan, kCpu };

GpuBackend resolve_backend() {
    const char* env = std::getenv("DNG_GPU_BACKEND");
    if (env) {
        if (env[0] == 'c' || env[0] == 'C') return GpuBackend::kCpu;
        if (env[0] == 'm' || env[0] == 'M') return GpuBackend::kMetal;
        if (env[0] == 'v' || env[0] == 'V') return GpuBackend::kVulkan;
    }
#if defined(__APPLE__)
    return GpuBackend::kMetal;
#elif defined(__ANDROID__)
    return GpuBackend::kVulkan;
#else
    return GpuBackend::kCpu;
#endif
}

GpuBackend cached_backend() {
    static const GpuBackend b = resolve_backend();
    return b;
}

} // namespace

const halide_device_interface_t* dng_halide_gpu_device_interface() {
    switch (cached_backend()) {
#if defined(__APPLE__)
    case GpuBackend::kMetal:
        return halide_metal_device_interface();
#endif
#if defined(__ANDROID__)
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
    case GpuBackend::kMetal:  return "metal";
    case GpuBackend::kVulkan: return "vulkan";
    case GpuBackend::kCpu:    return "cpu";
    }
    return "unknown";
}
