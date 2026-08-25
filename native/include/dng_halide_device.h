#pragma once

#include "HalideRuntime.h"

// Platform-adaptive GPU device interface for Halide AOT kernels.
// Returns Metal on Apple, Vulkan on Android, nullptr on unsupported.
const halide_device_interface_t* dng_halide_gpu_device_interface();

// Returns true if GPU acceleration is available on this platform.
bool dng_halide_gpu_available();

// Returns backend name string for logging: "metal", "vulkan", or "unsupported".
const char* dng_halide_gpu_backend_name();
