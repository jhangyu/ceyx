/*
---
file_summary: >
  Stage3 WarpRectilinear 橋接層。負責從 DNG SDK opcode 取出 warp 參數、建立 tile clipping grid，
  並 dispatch 到 Halide AOT 或明確指定的 CPU reference-compatible 測試路徑。

notes:
  - `warp_rectilinear_halide()` 是純 buffer 入口；`apply_warp_rectilinear_to_image()` 是 `dng_image` 入口。
  - Halide kernel 依賴 tile clipping grid 來對齊 SDK resample 邊界與減少無效取樣。
  - 現行行為固定走 Halide warp，無 SDK fallback。

structs:
  - name: "WarpRuntimeParams"
    description: "每次 warp 執行時計算的中心點、半徑與像素比例快取。"
    lines: "178-185"
  - name: "TileClippingGrid"
    description: "每個 tile 的 source sampling 範圍快取，供 CPU/Halide 共用。"
    lines: "187-199"

functions:
  - name: "cubicWeight"
    description: "Bicubic kernel 權重函式。"
    lines: "114-124"
  - name: "normalize4"
    description: "將 4-tap 權重正規化。"
    lines: "126-138"
  - name: "computeBicubicWeights"
    description: "預先計算單一 subsample phase 的 bicubic 權重。"
    lines: "140-146"
  - name: "quantizeSubsampleIndex"
    description: "將小數位移量量化成 SDK 相容的 subsample index。"
    lines: "151-159"
  - name: "bicubicWeightsForSubsample"
    description: "回傳指定 subsample phase 的 4-tap 權重表。"
    lines: "161-176"
  - name: "buildRuntimeParams"
    description: "由影像尺寸與 opcode 參數推導 warp 執行期幾何參數。"
    lines: "220-249"
  - name: "computeSdkTileExtent"
    description: "推導接近 SDK 行為的 tile size。"
    lines: "251-255"
  - name: "warpPlaneIndex"
    description: "將輸出 channel 映射到 warp plane index。"
    lines: "260-265"
  - name: "getSrcPixelPosition"
    description: "把目標座標反算回 source 座標，套用 radial/tangential distortion。"
    lines: "270-306"
  - name: "buildTileClippingGrid"
    description: "預估每個 tile 所需的 source sampling bounds。"
    lines: "311-385"
  - name: "warpRectilinearCpu"
    description: "CPU reference；用 clipping grid + bicubic resample 執行 warp。"
    lines: "390-467"
  - name: "imageToInterleaved"
    description: "把 `dng_image` 讀成 uint16 interleaved buffer。"
    lines: "472-494"
  - name: "writeInterleavedToImage"
    description: "把 interleaved buffer 寫回 `dng_image`。"
    lines: "499-517"
  - name: "copyHalideOutputToHost"
    description: "將 Halide device output 同步回 caller-owned host buffer，並依 centralized timing config 拆分 sync/copy。"
    lines: "522-527"
  - name: "getOrGrowZeroBuf"
    description: "Lazy-zero mmap arena reused for the warp ABI placeholder buffers; avoids per-call ~292 MB zero-fill."
    lines: "632-652"
  - name: "runWarpHalideAot"
    description: "建立 Halide Buffer 與 tile bounds，呼叫 rectilinear_warp AOT kernel。"
    lines: "654-787"
  - name: "runDemosaicWarpHalideAot"
    description: "建立 Bayer input 與 RGB output Halide Buffer，呼叫 fused demosaic+WarpRectilinear AOT kernel。"
    lines: "789-874"
  - name: "demosaic_warp_rectilinear_halide_dispatch / _finish / _cancel"
    description: "Phase 8.2.1 Path D — 拆 fused kernel 為 async dispatch + finish；caller 可在 GPU sync window 內並行做 CPU 工作。Handle 採 PIMPL：file-scope `DemosaicWarpHalideHandle{void*}` 包 anon-ns `DemosaicWarpHalideAsyncImpl`。"
    lines: "結尾段；以 `// --- Phase 8.2.1 Path D` 註解標記"
  - name: "warpRectilinearModeName"
    description: "列舉值轉字串。"
    lines: "737-745"
  - name: "extractWarpRectilinearParams"
    description: "從 SDK opcode 取出 plane/radial/tangential 參數並填入 runtime struct。"
    lines: "748-789"
  - name: "warp_rectilinear_halide"
    description: "buffer 入口；依 mode dispatch Halide GPU/AUTO；CPU reference 僅限明確 HALIDE_CPU。"
    lines: "791-833"
  - name: "demosaic_warp_rectilinear_halide"
    description: "Bayer fused demosaic+WarpRectilinear buffer 入口；正式 Stage3 fast path。"
    lines: "835-872"
  - name: "LazyZeroBuf"
    description: "T9 (Gotcha #62)：Android prewarm dummy buffer 的 mmap MAP_ANON lazy-zero RAII 容器（calloc fallback），消除 std::vector(N,0) eager memset。"
    lines: "879-902"
  - name: "dng_demosaic_warp_prewarm_for_size"
    description: "W7-E Android-only：以 actual-size LazyZeroBuf dummy 預建 fused demosaic+warp Vulkan pipeline；per-size cache、[Warmup] s3 markers。macOS no-op。"
    lines: "905-936"
  - name: "apply_warp_rectilinear_to_image"
    description: "Stage3 正式入口；處理 bit-exact SDK fallback、image <-> buffer 轉換與 timing。"
    lines: "979-1028"
---
*/
#include "dng_warp_halide.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>
#if defined(__ANDROID__)
// W7-E prewarm per-size cache (Android-only body).
#include <unordered_set>
#endif
#if defined(__ANDROID__)
// C0 diagnostic timing (Stage3-Perf-C0) — Android-only, log-only.
#include <chrono>
#include <cstdio>
#endif

#include "HalideBuffer.h"
#include "dng_demosaic_warp.h"
#include "rectilinear_warp.h"
#include <cstdlib>
#include <cstring>

#include <dng_host.h>
#include <dng_lens_correction.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_rect.h>
#include <dng_utils.h>
// W4 (2026-08-21, Windows port): POSIX mapping headers are unavailable under
// MSVC/clang-cl; getOrGrowZeroBuf below uses VirtualAlloc there. (The
// LazyZeroBuf helper further down is already inside an __ANDROID__ guard.)
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using Halide::Runtime::Buffer;

// CFA phase normalization: red_x / red_y are the column / row parity of the
// red CFA site (see dng_mosaic_halide.h). Anything outside {0,1} is a caller
// bug; clamp to the RGGB default rather than feeding the kernel garbage.
inline int normalizeCfaPhase(int v) {
    return (v == 1) ? 1 : 0;
}

float cubicWeight(float x) {
    const float a = -0.75f;
    x = std::fabs(x);
    if (x >= 2.0f) {
        return 0.0f;
    }
    if (x >= 1.0f) {
        return (((a * x - 5.0f * a) * x + 8.0f * a) * x - 4.0f * a);
    }
    return (((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f);
}

void normalize4(float* w) {
    const float sum = w[0] + w[1] + w[2] + w[3];
    if (sum == 0.0f) {
        w[0] = 0.0f;
        w[1] = 1.0f;
        w[2] = 0.0f;
        w[3] = 0.0f;
        return;
    }
    for (int i = 0; i < 4; ++i) {
        w[i] /= sum;
    }
}

void computeBicubicWeights(float fract, float* weights) {
    weights[0] = cubicWeight(-1.0f - fract);
    weights[1] = cubicWeight(0.0f - fract);
    weights[2] = cubicWeight(1.0f - fract);
    weights[3] = cubicWeight(2.0f - fract);
    normalize4(weights);
}

constexpr int kResampleSubsampleBits2D = 5;
constexpr int kResampleSubsampleCount2D = 1 << kResampleSubsampleBits2D;

inline int quantizeSubsampleIndex(double fract) {
    int idx = static_cast<int>(std::floor(fract * static_cast<double>(kResampleSubsampleCount2D)));
    if (idx < 0) {
        idx = 0;
    } else if (idx >= kResampleSubsampleCount2D) {
        idx = kResampleSubsampleCount2D - 1;
    }
    return idx;
}

const float* bicubicWeightsForSubsample(int subsampleIndex) {
    static float table[kResampleSubsampleCount2D][4];
    static bool initialized = false;

    if (!initialized) {
        for (int i = 0; i < kResampleSubsampleCount2D; ++i) {
            const float fract = static_cast<float>(i) /
                                static_cast<float>(kResampleSubsampleCount2D);
            computeBicubicWeights(fract, table[i]);
        }
        initialized = true;
    }

    const int clamped = std::max(0, std::min(kResampleSubsampleCount2D - 1, subsampleIndex));
    return table[clamped];
}

struct WarpRuntimeParams {
    double center_x = 0.0;
    double center_y = 0.0;
    double norm_radius = 1.0;
    double inv_norm_radius = 1.0;
    double pixel_scale_v = 1.0;
    double pixel_scale_v_inv = 1.0;
};

struct TileClippingGrid {
    int tile_width = 1;
    int tile_height = 1;
    int tiles_x = 1;
    int tiles_y = 1;
    std::vector<int32_t> bounds;  // [kind(4), tile_x, tile_y]

    int32_t get(int kind, int tx, int ty) const {
        const size_t idx = static_cast<size_t>(kind) +
                           4u * (static_cast<size_t>(tx) + static_cast<size_t>(tiles_x) * ty);
        return bounds[idx];
    }
};

WarpRuntimeParams buildRuntimeParams(int width, int height, const WarpRectilinearParams& params) {
    WarpRuntimeParams runtime;

    const dng_rect bounds(0, 0, height, width);
    runtime.center_x = Lerp_real64(static_cast<double>(bounds.l),
                                   static_cast<double>(bounds.r),
                                   params.center_h64);
    runtime.center_y = Lerp_real64(static_cast<double>(bounds.t),
                                   static_cast<double>(bounds.b),
                                   params.center_v64);

    runtime.pixel_scale_v = 1.0 / params.pixel_aspect_ratio64;
    runtime.pixel_scale_v_inv = params.pixel_aspect_ratio64;

    dng_rect square_bounds(bounds);
    square_bounds.b = square_bounds.t +
                      Round_int32(runtime.pixel_scale_v * static_cast<double>(square_bounds.H()));

    const dng_point_real64 square_center(
        Lerp_real64(static_cast<double>(square_bounds.t),
                    static_cast<double>(square_bounds.b),
                    params.center_v64),
        Lerp_real64(static_cast<double>(square_bounds.l),
                    static_cast<double>(square_bounds.r),
                    params.center_h64));

    runtime.norm_radius = MaxDistancePointToRect(square_center, square_bounds);
    runtime.inv_norm_radius = 1.0 / runtime.norm_radius;
    return runtime;
}

int computeSdkTileExtent(int extent) {
    if (extent <= 0) {
        return 1;
    }
    constexpr int kMaxTileExtent = 256;
    const int count = (extent + kMaxTileExtent - 1) / kMaxTileExtent;
    return std::max(1, (extent + count - 1) / count);
}

inline int warpPlaneIndex(int channel, const WarpRectilinearParams& params) {
    if (params.planes == 0 || params.planes == 1) {
        return 0;
    }
    if (channel >= 0 && channel < static_cast<int>(params.planes)) {
        return channel;
    }
    return 0;
}

void getSrcPixelPosition(double dst_x,
                         double dst_y,
                         int plane,
                         const WarpRuntimeParams& runtime,
                         const WarpRectilinearParams& params,
                         double& src_x,
                         double& src_y) {
    const double* rad = params.rad_params64[plane];
    const double* tan = params.tan_params64[plane];
    const double diff_x = dst_x - runtime.center_x;
    const double diff_y = dst_y - runtime.center_y;
    const double diff_norm_x = diff_x * runtime.inv_norm_radius;
    const double diff_norm_y = diff_y * runtime.inv_norm_radius;
    const double diff_scaled_x = diff_norm_x;
    const double diff_scaled_y = diff_norm_y * runtime.pixel_scale_v;
    const double rr = std::min(diff_scaled_x * diff_scaled_x + diff_scaled_y * diff_scaled_y, 1.0);

    const double ratio = rad[0] + rr * (rad[1] + rr * (rad[2] + rr * rad[3]));

    const double tan_v = tan[0] * (rr + 2.0 * diff_scaled_y * diff_scaled_y) +
                         2.0 * tan[1] * diff_scaled_x * diff_scaled_y;
    const double tan_h = tan[1] * (rr + 2.0 * diff_scaled_x * diff_scaled_x) +
                         2.0 * tan[0] * diff_scaled_x * diff_scaled_y;

    if (params.is_tan_nop_all) {
        src_x = runtime.center_x + (dst_x - runtime.center_x) * ratio;
        src_y = runtime.center_y + (dst_y - runtime.center_y) * ratio;
        return;
    }

    if (params.is_rad_nop_all) {
        src_x = dst_x + runtime.norm_radius * tan_h;
        src_y = dst_y + runtime.norm_radius * tan_v * runtime.pixel_scale_v_inv;
        return;
    }

    src_x = runtime.center_x + runtime.norm_radius * (diff_norm_x * ratio + tan_h);
    src_y = runtime.center_y +
            runtime.norm_radius * (diff_norm_y * ratio + tan_v * runtime.pixel_scale_v_inv);
}

TileClippingGrid buildTileClippingGrid(int width,
                                       int height,
                                       int dst_planes,
                                       const WarpRuntimeParams& runtime,
                                       const WarpRectilinearParams& params) {
    TileClippingGrid grid;
    grid.tile_width = computeSdkTileExtent(width);
    grid.tile_height = computeSdkTileExtent(height);
    grid.tiles_x = std::max(1, (width + grid.tile_width - 1) / grid.tile_width);
    grid.tiles_y = std::max(1, (height + grid.tile_height - 1) / grid.tile_height);
    grid.bounds.assign(static_cast<size_t>(4 * grid.tiles_x * grid.tiles_y), 0);

    constexpr int kKernelRadius = 2;
    constexpr int kBicubicWidth = 4;

    for (int ty = 0; ty < grid.tiles_y; ++ty) {
        const int t = ty * grid.tile_height;
        const int b = std::min(height, t + grid.tile_height);
        for (int tx = 0; tx < grid.tiles_x; ++tx) {
            const int l = tx * grid.tile_width;
            const int r = std::min(width, l + grid.tile_width);

            int32_t xMin = std::numeric_limits<int32_t>::max();
            int32_t xMax = std::numeric_limits<int32_t>::min();
            int32_t yMin = std::numeric_limits<int32_t>::max();
            int32_t yMax = std::numeric_limits<int32_t>::min();

            for (int plane = 0; plane < dst_planes; ++plane) {
                const int p = warpPlaneIndex(plane, params);

                for (int c = l; c < r; ++c) {
                    double sx = 0.0;
                    double sy = 0.0;
                    getSrcPixelPosition(static_cast<double>(c), static_cast<double>(t), p, runtime, params, sx, sy);
                    yMin = std::min(yMin, static_cast<int32_t>(std::floor(sy)));

                    getSrcPixelPosition(static_cast<double>(c),
                                        static_cast<double>(b - 1),
                                        p,
                                        runtime,
                                        params,
                                        sx,
                                        sy);
                    yMax = std::max(yMax, static_cast<int32_t>(std::ceil(sy)));
                }

                for (int rr = t; rr < b; ++rr) {
                    double sx = 0.0;
                    double sy = 0.0;
                    getSrcPixelPosition(static_cast<double>(l), static_cast<double>(rr), p, runtime, params, sx, sy);
                    xMin = std::min(xMin, static_cast<int32_t>(std::floor(sx)));

                    getSrcPixelPosition(static_cast<double>(r - 1),
                                        static_cast<double>(rr),
                                        p,
                                        runtime,
                                        params,
                                        sx,
                                        sy);
                    xMax = std::max(xMax, static_cast<int32_t>(std::ceil(sx)));
                }
            }

            const int32_t hMin = xMin - kKernelRadius;
            const int32_t vMin = yMin - kKernelRadius;
            const int32_t hMax = xMax + kKernelRadius - kBicubicWidth;
            const int32_t vMax = yMax + kKernelRadius - kBicubicWidth;

            const size_t base = 4u * (static_cast<size_t>(tx) + static_cast<size_t>(grid.tiles_x) * ty);
            grid.bounds[base + 0] = hMin;
            grid.bounds[base + 1] = hMax;
            grid.bounds[base + 2] = vMin;
            grid.bounds[base + 3] = vMax;
        }
    }

    return grid;
}

void warpRectilinearCpu(const uint16_t* src,
                        int width,
                        int height,
                        int planes,
                        const WarpRectilinearParams& params,
                        const TileClippingGrid& tile_grid,
                        uint16_t* dst) {
    const WarpRuntimeParams runtime = buildRuntimeParams(width, height, params);

    auto sample = [&](int x, int y, int c) -> float {
        const int clamped_x = std::max(0, std::min(width - 1, x));
        const int clamped_y = std::max(0, std::min(height - 1, y));
        const size_t idx = (static_cast<size_t>(clamped_y) * width + clamped_x) * planes + c;
        return static_cast<float>(src[idx]);
    };

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int tile_x = std::min(x / tile_grid.tile_width, tile_grid.tiles_x - 1);
            const int tile_y = std::min(y / tile_grid.tile_height, tile_grid.tiles_y - 1);
            const int min_base_x = tile_grid.get(0, tile_x, tile_y);
            const int max_base_x = tile_grid.get(1, tile_x, tile_y);
            const int min_base_y = tile_grid.get(2, tile_x, tile_y);
            const int max_base_y = tile_grid.get(3, tile_x, tile_y);

            for (int c = 0; c < planes; ++c) {
                const int plane = warpPlaneIndex(c, params);
                double src_x = 0.0;
                double src_y = 0.0;
                getSrcPixelPosition(static_cast<double>(x),
                                    static_cast<double>(y),
                                    plane,
                                    runtime,
                                    params,
                                    src_x,
                                    src_y);

                const double src_x_floor = std::floor(src_x);
                const double src_y_floor = std::floor(src_y);

                int base_x = static_cast<int>(src_x_floor) - 1;
                int base_y = static_cast<int>(src_y_floor) - 1;
                int frac_x_idx = quantizeSubsampleIndex(src_x - src_x_floor);
                int frac_y_idx = quantizeSubsampleIndex(src_y - src_y_floor);

                if (base_x < min_base_x) {
                    base_x = min_base_x;
                    frac_x_idx = 0;
                } else if (base_x > max_base_x) {
                    base_x = max_base_x;
                    frac_x_idx = 0;
                }

                if (base_y < min_base_y) {
                    base_y = min_base_y;
                    frac_y_idx = 0;
                } else if (base_y > max_base_y) {
                    base_y = max_base_y;
                    frac_y_idx = 0;
                }

                const float* weights_x = bicubicWeightsForSubsample(frac_x_idx);
                const float* weights_y = bicubicWeightsForSubsample(frac_y_idx);

                float total = 0.0f;
                for (int ky = 0; ky < 4; ++ky) {
                    const int sy = base_y + ky;
                    float row = 0.0f;
                    for (int kx = 0; kx < 4; ++kx) {
                        row += weights_x[kx] * sample(base_x + kx, sy, c);
                    }
                    total += weights_y[ky] * row;
                }

                const uint16_t out = static_cast<uint16_t>(
                    std::max(0.0f, std::min(65535.0f, total + 0.5f)));
                dst[(static_cast<size_t>(y) * width + x) * planes + c] = out;
            }
        }
    }
}

bool imageToInterleaved(dng_image* image,
                        std::vector<uint16_t>& data,
                        uint32_t& width,
                        uint32_t& height,
                        uint32_t& planes) {
    if (!image || image->PixelType() != ttShort) {
        return false;
    }
    width = image->Width();
    height = image->Height();
    planes = image->Planes();
    data.resize(static_cast<size_t>(width) * height * planes);

    dng_pixel_buffer buffer;
    buffer.fArea = image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = planes;
    buffer.fPixelType = ttShort;
    buffer.fPixelSize = sizeof(uint16_t);
    buffer.fData = data.data();
    buffer.fRowStep = static_cast<int32>(width * planes);
    buffer.fColStep = static_cast<int32>(planes);
    buffer.fPlaneStep = 1;
    image->Get(buffer);
    return true;
}

bool writeInterleavedToImage(dng_image* image,
                             const std::vector<uint16_t>& data,
                             uint32_t width,
                             uint32_t height,
                             uint32_t planes) {
    if (!image || image->Width() != width || image->Height() != height || image->Planes() != planes) {
        return false;
    }

    dng_pixel_buffer buffer;
    buffer.fArea = image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = planes;
    buffer.fPixelType = ttShort;
    buffer.fPixelSize = sizeof(uint16_t);
    buffer.fData = const_cast<uint16_t*>(data.data());
    buffer.fRowStep = static_cast<int32>(width * planes);
    buffer.fColStep = static_cast<int32>(planes);
    buffer.fPlaneStep = 1;
    image->Put(buffer);
    return true;
}

bool copyHalideOutputToHost(Buffer<uint16_t>& dst_buf) {
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    return true;
}

// T4-A: Shared static zero buffer for the warp coord ABI placeholder.
// Avoids allocating + zero-filling ~73 MB on every call.
// Phase 10 Sprint D-B: mmap MAP_ANON lazy zero pages. The Halide kernel
// never reads these inputs when precompute_coords=false, so committed
// physical pages stay near zero RSS. Avoids the ~290ms eager zero-fill
// of a 73M-element int32 buffer (=~292MB) on every first call.
static std::mutex s_zero_buf_mutex;
static int32_t*   s_zero_buf_ptr   = nullptr;
static size_t     s_zero_buf_bytes = 0;

const int32_t* getOrGrowZeroBuf(int width, int height) {
    const size_t needed_elems = static_cast<size_t>(width) * height * 3u;
    const size_t needed_bytes = needed_elems * sizeof(int32_t);
    std::lock_guard<std::mutex> lock(s_zero_buf_mutex);
    if (needed_bytes > s_zero_buf_bytes) {
        if (s_zero_buf_ptr) {
#if defined(_WIN32)
            VirtualFree(s_zero_buf_ptr, 0, MEM_RELEASE);
#else
            munmap(s_zero_buf_ptr, s_zero_buf_bytes);
#endif
            s_zero_buf_ptr = nullptr;
            s_zero_buf_bytes = 0;
        }
#if defined(_WIN32)
        // MEM_RESERVE|MEM_COMMIT gives lazily-backed zero pages, matching the
        // MAP_ANON property this buffer depends on (the kernel never reads it
        // when precompute_coords=false, so pages stay uncommitted).
        void* p = VirtualAlloc(nullptr, needed_bytes, MEM_RESERVE | MEM_COMMIT,
                               PAGE_READWRITE);
        if (p == nullptr) {
            p = std::calloc(needed_elems, sizeof(int32_t));
        }
#else
        void* p = mmap(nullptr, needed_bytes,
                       PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            p = std::calloc(needed_elems, sizeof(int32_t));
        }
#endif
        s_zero_buf_ptr = static_cast<int32_t*>(p);
        s_zero_buf_bytes = needed_bytes;
    }
    return s_zero_buf_ptr;
}

bool runWarpHalideAot(const uint16_t* src_interleaved_rgb,
                      int width,
                      int height,
                      int planes,
                      const WarpRuntimeParams& runtime,
                      const WarpRectilinearParams& params,
                      const TileClippingGrid& tile_grid,
                      uint16_t* dst_interleaved_rgb) {
    if (!src_interleaved_rgb || !dst_interleaved_rgb || width <= 0 || height <= 0 || planes <= 0) {
        return false;
    }

    // Fast path: RGB interleaved input/output (common Stage3 path).
    Buffer<uint16_t> src_buf =
        Buffer<uint16_t>::make_interleaved(const_cast<uint16_t*>(src_interleaved_rgb), width, height, planes);
    Buffer<uint16_t> dst_buf =
        Buffer<uint16_t>::make_interleaved(dst_interleaved_rgb, width, height, planes);

    const int plane_count = static_cast<int>(std::max<uint32_t>(1, params.planes));
    std::vector<float> rad_storage(static_cast<size_t>(4 * plane_count));
    std::vector<float> tan_storage(static_cast<size_t>(2 * plane_count));

    for (int plane = 0; plane < plane_count; ++plane) {
        for (int i = 0; i < 4; ++i) {
            rad_storage[static_cast<size_t>(plane * 4 + i)] = params.rad_params[plane][i];
        }
        for (int i = 0; i < 2; ++i) {
            tan_storage[static_cast<size_t>(plane * 2 + i)] = params.tan_params[plane][i];
        }
    }

    Buffer<float> rad_buf(rad_storage.data(), 4, plane_count);
    Buffer<float> tan_buf(tan_storage.data(), 2, plane_count);
    Buffer<int32_t> tile_bounds_buf(const_cast<int32_t*>(tile_grid.bounds.data()),
                                    4,
                                    tile_grid.tiles_x,
                                    tile_grid.tiles_y);

    // The rectilinear_warp ABI lists 4 coord buffers as Inputs (base_x/base_y/
    // frac_x/frac_y), but the production kernel never reads them. Feed a shared
    // lazy-zero mmap placeholder for all four — no per-call zero-fill (~73 MB
    // saved).
    const int32_t* zero_ptr = getOrGrowZeroBuf(width, height);
    Buffer<int32_t> base_x_buf(const_cast<int32_t*>(zero_ptr), width, height, 3);
    Buffer<int32_t> base_y_buf = base_x_buf;
    Buffer<int32_t> frac_x_buf = base_x_buf;
    Buffer<int32_t> frac_y_buf = base_x_buf;

    // Be explicit for GPU backends: inputs are host-authored, output host is stale
    // until we copy back after kernel execution.
    src_buf.set_host_dirty();
    rad_buf.set_host_dirty();
    tan_buf.set_host_dirty();
    tile_bounds_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int result = rectilinear_warp(src_buf.raw_buffer(),
                                        rad_buf.raw_buffer(),
                                        tan_buf.raw_buffer(),
                                        tile_bounds_buf.raw_buffer(),
                                        static_cast<int32_t>(params.planes),
                                        static_cast<float>(runtime.center_x),
                                        static_cast<float>(runtime.center_y),
                                        static_cast<float>(runtime.norm_radius),
                                        static_cast<float>(runtime.inv_norm_radius),
                                        static_cast<float>(runtime.pixel_scale_v),
                                        static_cast<float>(runtime.pixel_scale_v_inv),
                                        params.is_rad_nop_all ? 1 : 0,
                                        params.is_tan_nop_all ? 1 : 0,
                                        static_cast<int32_t>(tile_grid.tile_width),
                                        static_cast<int32_t>(tile_grid.tile_height),
                                        base_x_buf.raw_buffer(),
                                        base_y_buf.raw_buffer(),
                                        frac_x_buf.raw_buffer(),
                                        frac_y_buf.raw_buffer(),
                                        dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    if (!copyHalideOutputToHost(dst_buf)) {
        return false;
    }

    return true;
}

bool runDemosaicWarpHalideAot(const uint16_t* src_bayer,
                              int width,
                              int height,
                              const WarpRuntimeParams& runtime,
                              const WarpRectilinearParams& params,
                              const TileClippingGrid& tile_grid,
                              uint16_t* dst_interleaved_rgb,
                              int red_x,
                              int red_y) {
    if (!src_bayer || !dst_interleaved_rgb || width <= 0 || height <= 0) {
        return false;
    }

    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(src_bayer), width, height);
    Buffer<uint16_t> dst_buf =
        Buffer<uint16_t>::make_interleaved(dst_interleaved_rgb, width, height, 3);

    const int plane_count = static_cast<int>(std::max<uint32_t>(1, params.planes));
    std::vector<float> rad_storage(static_cast<size_t>(4 * plane_count));
    std::vector<float> tan_storage(static_cast<size_t>(2 * plane_count));

    for (int plane = 0; plane < plane_count; ++plane) {
        for (int i = 0; i < 4; ++i) {
            rad_storage[static_cast<size_t>(plane * 4 + i)] = params.rad_params[plane][i];
        }
        for (int i = 0; i < 2; ++i) {
            tan_storage[static_cast<size_t>(plane * 2 + i)] = params.tan_params[plane][i];
        }
    }

    Buffer<float> rad_buf(rad_storage.data(), 4, plane_count);
    Buffer<float> tan_buf(tan_storage.data(), 2, plane_count);
    Buffer<int32_t> tile_bounds_buf(const_cast<int32_t*>(tile_grid.bounds.data()),
                                    4,
                                    tile_grid.tiles_x,
                                    tile_grid.tiles_y);

    src_buf.set_host_dirty();
    rad_buf.set_host_dirty();
    tan_buf.set_host_dirty();
    tile_bounds_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int result = dng_demosaic_warp(src_buf.raw_buffer(),
                                         rad_buf.raw_buffer(),
                                         tan_buf.raw_buffer(),
                                         tile_bounds_buf.raw_buffer(),
                                         static_cast<int32_t>(params.planes),
                                         static_cast<float>(runtime.center_x),
                                         static_cast<float>(runtime.center_y),
                                         static_cast<float>(runtime.norm_radius),
                                         static_cast<float>(runtime.inv_norm_radius),
                                         static_cast<float>(runtime.pixel_scale_v),
                                         static_cast<float>(runtime.pixel_scale_v_inv),
                                         params.is_rad_nop_all ? 1 : 0,
                                         params.is_tan_nop_all ? 1 : 0,
                                         static_cast<int32_t>(tile_grid.tile_width),
                                         static_cast<int32_t>(tile_grid.tile_height),
                                         normalizeCfaPhase(red_x),
                                         normalizeCfaPhase(red_y),
                                         dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    if (!copyHalideOutputToHost(dst_buf)) {
        return false;
    }

    return true;
}

// Holds in-flight state for an async demosaic+warp dispatch.
// Lives inside the anonymous namespace because it embeds TileClippingGrid.
struct DemosaicWarpHalideAsyncImpl {
    TileClippingGrid tile_grid;
    std::vector<float> rad_storage;
    std::vector<float> tan_storage;
    Buffer<uint16_t> src_buf;
    Buffer<uint16_t> dst_buf;
    Buffer<float> rad_buf;
    Buffer<float> tan_buf;
    Buffer<int32_t> tile_bounds_buf;
#if defined(__ANDROID__)
    // C0 diagnostic (Stage3-Perf-C0): time spent in the async kernel submit
    // (dng_demosaic_warp dispatch call). Captured in _dispatch, consumed in
    // _finish where sync_wait/d2h are measured. Android-only, log-only — no
    // effect on the numeric path; macOS path is byte-for-byte unchanged.
    double c0_submit_ms = 0.0;
#endif
};

}  // namespace

// PIMPL handle: header-visible forward decl, body holds an anon-ns Impl*.
struct DemosaicWarpHalideHandle {
    void* impl = nullptr;
};

const char* warpRectilinearModeName(WarpRectilinearMode mode) {
    switch (mode) {
        case WarpRectilinearMode::SDK: return "sdk";
        case WarpRectilinearMode::HALIDE_CPU: return "halide-cpu";
        case WarpRectilinearMode::HALIDE_METAL: return "halide-metal";
        case WarpRectilinearMode::HALIDE_GPU: return "HALIDE_GPU";
        case WarpRectilinearMode::AUTO: return "auto";
    }
    return "unknown";
}

bool extractWarpRectilinearParams(const dng_opcode_WarpRectilinear& opcode,
                                  double pixelAspectRatio,
                                  WarpRectilinearParams& params) {
    const dng_warp_params_rectilinear& warp = opcode.WarpParams();
    if (warp.fPlanes == 0 || warp.fPlanes > 4) {
        return false;
    }

    params = WarpRectilinearParams{};
    params.planes = warp.fPlanes;
    params.center_h = static_cast<float>(warp.fCenter.h);
    params.center_v = static_cast<float>(warp.fCenter.v);
    params.pixel_aspect_ratio = static_cast<float>(pixelAspectRatio);
    params.center_h64 = warp.fCenter.h;
    params.center_v64 = warp.fCenter.v;
    params.pixel_aspect_ratio64 = pixelAspectRatio;
    params.is_rad_nop_all = true;
    params.is_tan_nop_all = true;

    for (uint32_t plane = 0; plane < warp.fPlanes; ++plane) {
        for (int i = 0; i < 4; ++i) {
            params.rad_params[plane][i] = static_cast<float>(warp.fRadParams[plane][i]);
            params.rad_params64[plane][i] = warp.fRadParams[plane][i];
        }
        for (int i = 0; i < 2; ++i) {
            params.tan_params[plane][i] = static_cast<float>(warp.fTanParams[plane][i]);
            params.tan_params64[plane][i] = warp.fTanParams[plane][i];
        }

        const bool is_rad_nop =
            (warp.fRadParams[plane][0] == 1.0 &&
             warp.fRadParams[plane][1] == 0.0 &&
             warp.fRadParams[plane][2] == 0.0 &&
             warp.fRadParams[plane][3] == 0.0);
        const bool is_tan_nop =
            (warp.fTanParams[plane][0] == 0.0 &&
             warp.fTanParams[plane][1] == 0.0);
        params.is_rad_nop_all = params.is_rad_nop_all && is_rad_nop;
        params.is_tan_nop_all = params.is_tan_nop_all && is_tan_nop;
    }
    return true;
}

bool warp_rectilinear_halide(const uint16_t* src_interleaved_rgb,
                             int width,
                             int height,
                             int planes,
                             const WarpRectilinearParams& params,
                             WarpRectilinearMode mode,
                             uint16_t* dst_interleaved_rgb) {
    if (!src_interleaved_rgb || !dst_interleaved_rgb || width <= 0 || height <= 0 || planes <= 0) {
        return false;
    }

    if (mode == WarpRectilinearMode::SDK) {
        return false;
    }

    const WarpRuntimeParams runtime = buildRuntimeParams(width, height, params);
    const TileClippingGrid tile_grid = buildTileClippingGrid(width, height, planes, runtime, params);
    if (mode == WarpRectilinearMode::HALIDE_CPU) {
        warpRectilinearCpu(src_interleaved_rgb,
                           width,
                           height,
                           planes,
                           params,
                           tile_grid,
                           dst_interleaved_rgb);
        return true;
    }

    if (mode == WarpRectilinearMode::HALIDE_METAL ||
        mode == WarpRectilinearMode::HALIDE_GPU ||
        mode == WarpRectilinearMode::AUTO) {
        return runWarpHalideAot(src_interleaved_rgb,
                                width,
                                height,
                                planes,
                                runtime,
                                params,
                                tile_grid,
                                dst_interleaved_rgb);
    }

    return false;
}

bool demosaic_warp_rectilinear_halide(const uint16_t* src_bayer,
                                      int width,
                                      int height,
                                      const WarpRectilinearParams& params,
                                      WarpRectilinearMode mode,
                                      uint16_t* dst_interleaved_rgb,
                                      int red_x,
                                      int red_y) {
    if (!src_bayer || !dst_interleaved_rgb || width <= 0 || height <= 0) {
        return false;
    }
    if (mode == WarpRectilinearMode::SDK) {
        return false;
    }
    if (mode == WarpRectilinearMode::HALIDE_CPU) {
        return false;
    }

    const WarpRuntimeParams runtime = buildRuntimeParams(width, height, params);
    const TileClippingGrid tile_grid = buildTileClippingGrid(width, height, 3, runtime, params);
    if (mode == WarpRectilinearMode::HALIDE_METAL ||
        mode == WarpRectilinearMode::HALIDE_GPU ||
        mode == WarpRectilinearMode::AUTO) {
        if (runDemosaicWarpHalideAot(src_bayer,
                                     width,
                                     height,
                                     runtime,
                                     params,
                                     tile_grid,
                                     dst_interleaved_rgb,
                                     red_x,
                                     red_y)) {
            return true;
        }
        if (mode == WarpRectilinearMode::HALIDE_METAL ||
            mode == WarpRectilinearMode::HALIDE_GPU) {
            return false;
        }
    }

    return false;
}

#if defined(__ANDROID__)
// W7-C T9 (Gotcha #62): lazy-zero scratch for warmup dummy buffers. mmap
// MAP_ANON gives zero-pages committed only on first touch (the Halide H2D
// upload), avoiding std::vector(N,0)'s eager full-buffer memset. munmap (or
// free for the calloc fallback) releases on scope exit.
struct LazyZeroBuf {
    void* ptr = nullptr;
    size_t bytes = 0;
    bool mmaped = false;
    explicit LazyZeroBuf(size_t n) : bytes(n) {
        ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            ptr = std::calloc(1, bytes);  // OS-zeroed fallback
            mmaped = false;
        } else {
            mmaped = true;
        }
    }
    ~LazyZeroBuf() {
        if (mmaped && ptr != nullptr) {
            ::munmap(ptr, bytes);
        } else if (ptr != nullptr) {
            std::free(ptr);
        }
    }
    LazyZeroBuf(const LazyZeroBuf&) = delete;
    LazyZeroBuf& operator=(const LazyZeroBuf&) = delete;
};
#endif  // __ANDROID__

void dng_demosaic_warp_prewarm_for_size(int width, int height) {
#if defined(__ANDROID__)
    // W7-E: build the fused demosaic+WarpRectilinear GPU pipeline at the actual
    // image size during idle warmup so the first real Stage3 decode of this
    // resolution does not pay the Vulkan pipeline-state creation + first large
    // dispatch tax (cold S3 ~291ms vs warm dispatch ~167ms). Mirrors the
    // halide_prewarm_polynomial3_for_size pattern: per-size cache + identity
    // dispatch on zeroed dummy buffers, result discarded.
    if (width <= 0 || height <= 0) {
        return;
    }

    static std::mutex cache_mu;
    static std::unordered_set<uint64_t> warmed;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) |
                         static_cast<uint64_t>(static_cast<uint32_t>(height));
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        if (warmed.count(key)) {
            return;
        }
    }

    // Identity warp params (no radial / tangential distortion). This drives the
    // exact dng_demosaic_warp AOT entry the production async path dispatches, so
    // the Vulkan pipeline cached here is the one the real decode reuses.
    WarpRectilinearParams params{};
    params.planes = 3;
    params.is_rad_nop_all = true;
    params.is_tan_nop_all = true;
    for (int plane = 0; plane < 3; ++plane) {
        params.rad_params[plane][0] = 1.0f;  // r^0 coeff = 1 => identity radial
    }

    const size_t bayerElems = static_cast<size_t>(width) * static_cast<size_t>(height);
    const size_t rgbElems = bayerElems * 3u;

    // Gotcha #62: std::vector(N, 0) eager value-inits trivially-constructible
    // elements (~bayer 49MB + dst 146MB here = ~1.9s of pure zero-fill memset
    // for the 6048x4024 case). The dummy content is irrelevant (output
    // discarded); the GPU only needs a readable buffer to H2D-upload. mmap
    // MAP_ANON hands back lazy zero-pages committed on first touch (during the
    // Halide upload), so the eager memset disappears. calloc fallback keeps the
    // OS-zero guarantee if mmap fails.
    LazyZeroBuf dummy_bayer(bayerElems * sizeof(uint16_t));
    LazyZeroBuf dummy_dst(rgbElems * sizeof(uint16_t));

    // Crash-attribution markers (T5b): let the on-device log pinpoint which
    // prewarm stage faulted if the GPU pipeline build throws.
    std::fprintf(stderr, "[Warmup] s3 begin %dx%d\n", width, height);
    std::fflush(stderr);
    if (dummy_bayer.ptr != nullptr && dummy_dst.ptr != nullptr) {
        (void)demosaic_warp_rectilinear_halide(static_cast<uint16_t*>(dummy_bayer.ptr),
                                               width,
                                               height,
                                               params,
                                               WarpRectilinearMode::AUTO,
                                               static_cast<uint16_t*>(dummy_dst.ptr),
                                               // Prewarm only builds the GPU
                                               // pipeline state; the CFA phase
                                               // does not affect codegen, so
                                               // RGGB is fine here.
                                               /*red_x=*/0, /*red_y=*/0);
    }
    std::fprintf(stderr, "[Warmup] s3 done\n");
    std::fflush(stderr);

    // Mark warmed regardless of result — even a failed dispatch means the GPU
    // attempted pipeline creation; do not retry on every warmup call.
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        warmed.insert(key);
    }
#else
    (void)width;
    (void)height;
#endif
}

bool apply_warp_rectilinear_to_image(dng_host& host,
                                     dng_negative& negative,
                                     const dng_opcode_WarpRectilinear& opcode,
                                     AutoPtr<dng_image>& image,
                                     WarpRectilinearMode mode) {
    if (mode == WarpRectilinearMode::SDK) {
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t planes = 0;
    std::vector<uint16_t> src_data;
    if (!imageToInterleaved(image.Get(), src_data, width, height, planes)) {
        return false;
    }

    WarpRectilinearParams params;
    if (!extractWarpRectilinearParams(opcode,
                                      negative.PixelAspectRatio(),
                                      params)) {
        return false;
    }

    std::vector<uint16_t> dst_data(src_data.size());
    if (!warp_rectilinear_halide(src_data.data(),
                                 static_cast<int>(width),
                                 static_cast<int>(height),
                                 static_cast<int>(planes),
                                 params,
                                 mode,
                                 dst_data.data())) {
        return false;
    }

    if (!writeInterleavedToImage(image.Get(), dst_data, width, height, planes)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Phase 8.2.1 Path D — async dispatch/finish for Stage3 latency hiding.
//
// Splits the fused demosaic+WarpRectilinear AOT call so the caller can run
// CPU-only work (e.g. dng_image allocation, Stage4 prep) while the Metal
// kernel is in flight on the GPU. The handle owns all buffers / scratch the
// kernel still references; caller-owned src/dst pointers must remain valid
// until finish (or cancel) is called.
// ---------------------------------------------------------------------------

DemosaicWarpHalideHandle* demosaic_warp_rectilinear_halide_dispatch(
    const uint16_t* src_bayer,
    int width,
    int height,
    const WarpRectilinearParams& params,
    WarpRectilinearMode mode,
    uint16_t* dst_interleaved_rgb,
    int red_x,
    int red_y) {
    if (!src_bayer || !dst_interleaved_rgb || width <= 0 || height <= 0) {
        return nullptr;
    }
    if (mode == WarpRectilinearMode::SDK) {
        return nullptr;
    }
    if (mode == WarpRectilinearMode::HALIDE_CPU) {
        return nullptr;
    }
    if (mode != WarpRectilinearMode::HALIDE_METAL &&
        mode != WarpRectilinearMode::HALIDE_GPU &&
        mode != WarpRectilinearMode::AUTO) {
        return nullptr;
    }

    auto impl = std::make_unique<DemosaicWarpHalideAsyncImpl>();

    const WarpRuntimeParams runtime = buildRuntimeParams(width, height, params);
    impl->tile_grid =
        buildTileClippingGrid(width, height, 3, runtime, params);

    impl->src_buf =
        Buffer<uint16_t>(const_cast<uint16_t*>(src_bayer), width, height);
    impl->dst_buf = Buffer<uint16_t>::make_interleaved(
        dst_interleaved_rgb, width, height, 3);

    const int plane_count =
        static_cast<int>(std::max<uint32_t>(1, params.planes));
    impl->rad_storage.assign(static_cast<size_t>(4 * plane_count), 0.0f);
    impl->tan_storage.assign(static_cast<size_t>(2 * plane_count), 0.0f);
    for (int plane = 0; plane < plane_count; ++plane) {
        for (int i = 0; i < 4; ++i) {
            impl->rad_storage[static_cast<size_t>(plane * 4 + i)] =
                params.rad_params[plane][i];
        }
        for (int i = 0; i < 2; ++i) {
            impl->tan_storage[static_cast<size_t>(plane * 2 + i)] =
                params.tan_params[plane][i];
        }
    }
    impl->rad_buf = Buffer<float>(impl->rad_storage.data(), 4, plane_count);
    impl->tan_buf = Buffer<float>(impl->tan_storage.data(), 2, plane_count);
    impl->tile_bounds_buf = Buffer<int32_t>(impl->tile_grid.bounds.data(),
                                            4,
                                            impl->tile_grid.tiles_x,
                                            impl->tile_grid.tiles_y);

    impl->src_buf.set_host_dirty();
    impl->rad_buf.set_host_dirty();
    impl->tan_buf.set_host_dirty();
    impl->tile_bounds_buf.set_host_dirty();
    impl->dst_buf.set_host_dirty(false);

#if defined(__ANDROID__)
    const auto c0_submit_t0 = std::chrono::steady_clock::now();
#endif
    const int result = dng_demosaic_warp(
        impl->src_buf.raw_buffer(),
        impl->rad_buf.raw_buffer(),
        impl->tan_buf.raw_buffer(),
        impl->tile_bounds_buf.raw_buffer(),
        static_cast<int32_t>(params.planes),
        static_cast<float>(runtime.center_x),
        static_cast<float>(runtime.center_y),
        static_cast<float>(runtime.norm_radius),
        static_cast<float>(runtime.inv_norm_radius),
        static_cast<float>(runtime.pixel_scale_v),
        static_cast<float>(runtime.pixel_scale_v_inv),
        params.is_rad_nop_all ? 1 : 0,
        params.is_tan_nop_all ? 1 : 0,
        static_cast<int32_t>(impl->tile_grid.tile_width),
        static_cast<int32_t>(impl->tile_grid.tile_height),
        normalizeCfaPhase(red_x),
        normalizeCfaPhase(red_y),
        impl->dst_buf.raw_buffer());
    if (result != 0) {
        return nullptr;
    }
#if defined(__ANDROID__)
    impl->c0_submit_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - c0_submit_t0).count();
#endif

    auto* handle = new DemosaicWarpHalideHandle();
    handle->impl = impl.release();
    return handle;
}

bool demosaic_warp_rectilinear_halide_finish(DemosaicWarpHalideHandle* handle) {
    if (!handle) {
        return false;
    }
    std::unique_ptr<DemosaicWarpHalideAsyncImpl> impl(
        static_cast<DemosaicWarpHalideAsyncImpl*>(handle->impl));
    delete handle;
    if (!impl) {
        return false;
    }

#if defined(__ANDROID__)
    // C0 diagnostic: split the readback into device_sync (GPU compute wait)
    // vs copy_to_host (pure D2H transfer). NOTE (Gotcha #70): inserting an
    // explicit device_sync here moves GPU compute time out of copy_to_host
    // and into sync_wait, so these numbers are a *diagnostic decomposition*,
    // not a production baseline. The fused kernel is a single Halide AOT entry
    // (dng_demosaic_warp), so demosaic-producer vs warp-consumer GPU dispatches
    // cannot be timed separately at the bridge — sync_wait is the whole fused
    // GPU compute. Log-only; numeric output is identical to copy_to_host alone.
    const auto c0_sync_t0 = std::chrono::steady_clock::now();
    impl->dst_buf.device_sync();
    const auto c0_sync_t1 = std::chrono::steady_clock::now();
    const bool c0_ok = copyHalideOutputToHost(impl->dst_buf);
    const auto c0_sync_t2 = std::chrono::steady_clock::now();
    const double c0_sync_wait_ms =
        std::chrono::duration<double, std::milli>(c0_sync_t1 - c0_sync_t0).count();
    const double c0_d2h_ms =
        std::chrono::duration<double, std::milli>(c0_sync_t2 - c0_sync_t1).count();
    std::fprintf(stderr,
                 "[Stage3-Perf-C0] submit=%.2f sync_wait=%.2f d2h=%.2f\n",
                 impl->c0_submit_ms, c0_sync_wait_ms, c0_d2h_ms);
    std::fflush(stderr);
    if (!c0_ok) {
        return false;
    }
    return true;
#else
    if (!copyHalideOutputToHost(impl->dst_buf)) {
        return false;
    }
    return true;
#endif
}

void demosaic_warp_rectilinear_halide_cancel(DemosaicWarpHalideHandle* handle) {
    if (!handle) {
        return;
    }
    auto* impl = static_cast<DemosaicWarpHalideAsyncImpl*>(handle->impl);
    delete impl;
    delete handle;
}

halide_buffer_t* demosaic_warp_halide_get_device_buffer(DemosaicWarpHalideHandle* handle) {
    if (!handle || !handle->impl) {
        return nullptr;
    }
    auto* impl = static_cast<DemosaicWarpHalideAsyncImpl*>(handle->impl);
    return impl->dst_buf.raw_buffer();
}
