#pragma once

#include <cstdint>

#include "HalideRuntime.h"

// Platform-adaptive GPU device interface for Halide AOT kernels.
// Returns Metal on Apple, Vulkan on Android, nullptr on unsupported.
const halide_device_interface_t* dng_halide_gpu_device_interface();

// Returns true if GPU acceleration is available on this platform.
bool dng_halide_gpu_available();

// Returns backend name string for logging: "metal", "vulkan", or "unsupported".
const char* dng_halide_gpu_backend_name();

// Result of one idle device-memory release pass. Same meaning on every backend
// (PARITY.md clause 1):
//   kReleased              the backend's unused device memory was handed back;
//   kSkippedUninitialized  no GPU runtime state exists in this process, so there
//                          was nothing to release -- and nothing was created;
//   kError                 the backend reported a failure (nothing is retried).
enum class DngDeviceReleaseResult : int32_t {
    kReleased = 0,
    kSkippedUninitialized = 1,
    kError = 2,
};

// Step 3 of ceyx_native_idle_shrink. The caller guarantees decode quiescence
// (raw_ffi_api.h clause (e)). Never creates a GPU instance or device.
DngDeviceReleaseResult dng_halide_release_unused_device_memory();

// Destroys this process's GPU device context (ceyx_native_release_gpu, the
// contract is in raw_ffi_api.h). Same call on every backend; a no-op that
// creates nothing when no context exists.
void dng_halide_release_device();
