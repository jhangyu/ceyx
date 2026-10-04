#include "dng_halide_device.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

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

// Halide error handler (user-approved 2026-10-04; design:
// Halcyon docs/logs/2026-10-04/halide-error-handler-recommendation.md).
// Evidence, Halide v21.0.0 official source:
//  - src/runtime/posix_error_handler.cpp: `WEAK halide_default_error(...)`
//    prints "Error: <msg>" and then calls `abort()`; the file also sets
//    `WEAK halide_error_handler_t error_handler = halide_default_error;`.
//    So with no handler installed, any GPU allocation failure kills the app.
//  - src/InjectHostDevBufferCopies.cpp:520-522 registers
//    `halide_device_free_as_destructor` for each device allocation;
//    src/CodeGen_LLVM.cpp:3688-3698 (create_assertion) branches a failed
//    stage to the destructor block, so device buffers allocated earlier in
//    the same pipeline run are freed when a later stage fails.
// One global handler for every platform and backend: it neither aborts nor
// throws. The runtime then returns its non-zero code to the existing ceyx
// error path (per-photo decode failure). Program-bug errors (bounds checks)
// degrade the same way; the stderr line keeps them visible.
thread_local char g_last_halide_error[512];

void dng_halide_error_handler(void* /*user_context*/, const char* msg) {
    if (msg == nullptr) msg = "(null)";
    std::snprintf(g_last_halide_error, sizeof(g_last_halide_error), "%s", msg);
    std::fprintf(stderr, "[HalideError] %s%s", msg,
                 (*msg != '\0' && msg[std::strlen(msg) - 1] == '\n') ? "" : "\n");
}

void install_halide_error_handler() {
    static std::once_flag once;
    std::call_once(once, [] { halide_set_error_handler(dng_halide_error_handler); });
}

} // namespace

const halide_device_interface_t* dng_halide_gpu_device_interface() {
    install_halide_error_handler();
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

void dng_halide_release_device() {
    // Halide's AOT contract (HalideRuntime.h, halide_device_release): must be
    // called explicitly. Left to the runtime's own destructor, the release
    // runs from this library's unload at process exit, after the GPU driver
    // may already be torn down (Halide issue 8497; 0xC0000409 in the Intel
    // Vulkan driver on Windows). Every backend acquires its context here with
    // create=false, so this never creates one.
    const halide_device_interface_t* device_interface = dng_halide_gpu_device_interface();
    if (device_interface != nullptr) {
        halide_device_release(nullptr, device_interface);
    }
}
