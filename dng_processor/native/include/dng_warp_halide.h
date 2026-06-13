#ifndef DNG_WARP_HALIDE_H_
#define DNG_WARP_HALIDE_H_

#include <cstdint>

#include <dng_auto_ptr.h>
#include <dng_image.h>

class dng_host;
class dng_negative;
class dng_opcode;
class dng_opcode_WarpRectilinear;

struct WarpRectilinearParams {
    uint32_t planes = 0;
    float rad_params[4][4] = {};
    float tan_params[4][2] = {};
    float center_h = 0.5f;
    float center_v = 0.5f;
    float pixel_aspect_ratio = 1.0f;
    double rad_params64[4][4] = {};
    double tan_params64[4][2] = {};
    double center_h64 = 0.5;
    double center_v64 = 0.5;
    double pixel_aspect_ratio64 = 1.0;
    bool is_rad_nop_all = false;
    bool is_tan_nop_all = false;
};

enum class WarpRectilinearMode {
    SDK,
    HALIDE_CPU,
    HALIDE_METAL,
    HALIDE_GPU,  // Platform-adaptive: Metal on Apple, Vulkan on Android
    AUTO
};

bool extractWarpRectilinearParams(const dng_opcode_WarpRectilinear& opcode,
                                  double pixelAspectRatio,
                                  WarpRectilinearParams& params);

bool warp_rectilinear_halide(const uint16_t* src_interleaved_rgb,
                             int width,
                             int height,
                             int planes,
                             const WarpRectilinearParams& params,
                             WarpRectilinearMode mode,
                             uint16_t* dst_interleaved_rgb);

bool demosaic_warp_rectilinear_halide(const uint16_t* src_bayer,
                                      int width,
                                      int height,
                                      const WarpRectilinearParams& params,
                                      WarpRectilinearMode mode,
                                      uint16_t* dst_interleaved_rgb);

// Opaque handle forward-declaration needs the Halide C runtime type.
struct halide_buffer_t;

// Async dispatch of the fused demosaic+WarpRectilinear AOT kernel.
// Returns an opaque handle on successful dispatch (kernel queued on GPU);
// returns nullptr on failure. The caller-owned src_bayer / dst_interleaved_rgb
// pointers MUST remain valid until finish/cancel is called.
//
// This enables Stage3 latency hiding: caller can run CPU-only work
// (e.g. dng_image allocation) while the GPU kernel is in flight.
struct DemosaicWarpHalideHandle;
DemosaicWarpHalideHandle* demosaic_warp_rectilinear_halide_dispatch(
    const uint16_t* src_bayer,
    int width,
    int height,
    const WarpRectilinearParams& params,
    WarpRectilinearMode mode,
    uint16_t* dst_interleaved_rgb);

// Finalizes the dispatched kernel: device_sync() + copy_to_host() into
// the dst pointer that was passed to dispatch. Always destroys the handle.
// Returns false on failure (handle still destroyed).
bool demosaic_warp_rectilinear_halide_finish(DemosaicWarpHalideHandle* handle);

// Cancel/discard a handle without finishing (error path).
void demosaic_warp_rectilinear_halide_cancel(DemosaicWarpHalideHandle* handle);

// Returns the raw Halide device buffer (Stage3 output) from an in-flight handle
// without syncing or copying to host.  The buffer is device-dirty; the GPU may
// still be computing into it.  Caller MUST subsequently call either
// demosaic_warp_rectilinear_halide_finish() (to get host data) or
// demosaic_warp_rectilinear_halide_cancel() (to discard).
// Returns nullptr if handle is invalid.
halide_buffer_t* demosaic_warp_halide_get_device_buffer(DemosaicWarpHalideHandle* handle);

bool apply_warp_rectilinear_to_image(dng_host& host,
                                     dng_negative& negative,
                                     const dng_opcode_WarpRectilinear& opcode,
                                     AutoPtr<dng_image>& image,
                                     WarpRectilinearMode mode);

const char* warpRectilinearModeName(WarpRectilinearMode mode);

// W7-E: idle-time prewarm of the fused demosaic+WarpRectilinear AOT kernel at
// the given actual image size, so the GPU pipeline state (Vulkan on Android,
// Metal on Apple) is built and cached before the first real Stage3 decode of
// that resolution. Dispatches one identity-warp pass on zeroed dummy buffers
// and discards the result. Per-size cached; Android-only body (no-op on other
// platforms so macOS warmup behaviour is unchanged).
void dng_demosaic_warp_prewarm_for_size(int width, int height);

#endif  // DNG_WARP_HALIDE_H_
