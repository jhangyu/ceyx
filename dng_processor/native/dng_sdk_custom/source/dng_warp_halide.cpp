/*
---
file_summary: >
  Stage3 WarpRectilinear 橋接層。負責從 DNG SDK opcode 取出 warp 參數、建立 tile clipping grid，
  dispatch 到 Halide AOT 或明確指定的 CPU reference-compatible 路徑，並在 bit-exact 模式下退回 SDK Apply。

notes:
  - `warp_rectilinear_halide()` 是純 buffer 入口；`apply_warp_rectilinear_to_image()` 是 `dng_image` 入口。
  - Halide kernel 依賴 tile clipping grid 來對齊 SDK resample 邊界與減少無效取樣。
  - `DNG_WARP_BIT_EXACT` 啟用時，直接改走 SDK opcode `Apply()`。

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
  - name: "isWarpBitExactModeEnabled"
    description: "透過 `PipelineConfig` 讀取 debug-only warp bit-exact flag。"
    lines: "201-203"
  - name: "buildRuntimeParams"
    description: "由影像尺寸與 opcode 參數推導 warp 執行期幾何參數。"
    lines: "205-234"
  - name: "computeSdkTileExtent"
    description: "推導接近 SDK 行為的 tile size。"
    lines: "236-243"
  - name: "warpPlaneIndex"
    description: "將輸出 channel 映射到 warp plane index。"
    lines: "245-253"
  - name: "getSrcPixelPosition"
    description: "把目標座標反算回 source 座標，套用 radial/tangential distortion。"
    lines: "255-294"
  - name: "buildTileClippingGrid"
    description: "預估每個 tile 所需的 source sampling bounds。"
    lines: "296-373"
  - name: "warpRectilinearCpu"
    description: "CPU fallback；用 clipping grid + bicubic resample 執行 warp。"
    lines: "375-455"
  - name: "imageToInterleaved"
    description: "把 `dng_image` 讀成 uint16 interleaved buffer。"
    lines: "457-482"
  - name: "writeInterleavedToImage"
    description: "把 interleaved buffer 寫回 `dng_image`。"
    lines: "484-505"
  - name: "copyHalideOutputToHost"
    description: "將 Halide device output 同步回 caller-owned host buffer，並依 centralized timing config 拆分 sync/copy。"
    lines: "507-543"
  - name: "runWarpHalideAot"
    description: "建立 Halide Buffer 與 tile bounds，呼叫 rectilinear_warp AOT kernel。"
    lines: "545-633"
  - name: "runDemosaicWarpHalideAot"
    description: "建立 Bayer input 與 RGB output Halide Buffer，呼叫 fused demosaic+WarpRectilinear AOT kernel。"
    lines: "635-718"
  - name: "demosaic_warp_rectilinear_halide_dispatch / _finish / _cancel"
    description: "Phase 8.2.1 Path D — 拆 fused kernel 為 async dispatch + finish；caller 可在 GPU sync window 內並行做 CPU 工作。Handle 採 PIMPL：file-scope `DemosaicWarpHalideHandle{void*}` 包 anon-ns `DemosaicWarpHalideAsyncImpl`。"
    lines: "結尾段；以 `// --- Phase 8.2.1 Path D` 註解標記"
  - name: "warpRectilinearModeName"
    description: "列舉值轉字串。"
    lines: "722-730"
  - name: "extractWarpRectilinearParams"
    description: "從 SDK opcode 取出 plane/radial/tangential 參數並填入 runtime struct。"
    lines: "732-773"
  - name: "warp_rectilinear_halide"
    description: "buffer 入口；依 mode dispatch Halide Metal / Halide CPU / AUTO 路徑。"
    lines: "775-816"
  - name: "demosaic_warp_rectilinear_halide"
    description: "Bayer fused demosaic+WarpRectilinear buffer 入口；正式 Stage3 fast path。"
    lines: "818-849"
  - name: "apply_warp_rectilinear_to_image"
    description: "Stage3 正式入口；處理 bit-exact SDK fallback、image <-> buffer 轉換與 timing。"
    lines: "851-912"
---
*/
#include "dng_warp_halide.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

#include "HalideBuffer.h"
#include "dng_pipeline_config.h"
#include "dng_demosaic_warp.h"
#include "rectilinear_warp.h"
#include "rectilinear_warp_precomputed.h"
#include <cstdlib>
#include <cstring>

#include <dng_host.h>
#include <dng_lens_correction.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_rect.h>
#include <dng_utils.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using Halide::Runtime::Buffer;

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

bool isWarpBitExactModeEnabled() {
    return PipelineConfig::loadFromEnv().debug.warp_bit_exact;
}

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

bool copyHalideOutputToHost(Buffer<uint16_t>& dst_buf,
                            bool timing_enabled,
                            const char* label,
                            const std::chrono::high_resolution_clock::time_point& copyStart,
                            std::chrono::high_resolution_clock::time_point& copyEnd) {
    const bool split_timing = PipelineConfig::loadFromEnv().timing.halide_readback_split;
    if (!split_timing) {
        if (dst_buf.copy_to_host() != 0) {
            return false;
        }
        copyEnd = std::chrono::high_resolution_clock::now();
        return true;
    }

    const auto syncStart = copyStart;
    auto syncEnd = syncStart;
    if (dst_buf.device_sync() != 0) {
        return false;
    }
    syncEnd = std::chrono::high_resolution_clock::now();
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    copyEnd = std::chrono::high_resolution_clock::now();

    if (timing_enabled) {
        auto ms = [](const auto& a, const auto& b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
        };
        std::cerr << label
                  << " sync_wait=" << ms(syncStart, syncEnd)
                  << " ms host_copy=" << ms(syncEnd, copyEnd)
                  << " ms copy_to_host=" << ms(copyStart, copyEnd)
                  << " ms\n";
    }
    return true;
}

// Phase 8.1.6 D-1: env-var gated host-CPU double-precision precompute.
// Reads DNG_WARP_PRECOMPUTED_COORDS=1 to switch standalone warp Metal path
// to the rectilinear_warp_precomputed AOT variant, feeding base_x/base_y/
// frac_x_idx/frac_y_idx as input buffers (computed on host with double).
bool isPrecomputedCoordsEnabled() {
    const char* v = std::getenv("DNG_WARP_PRECOMPUTED_COORDS");
    if (!v) return false;
    return v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y';
}

// Compute base_x / base_y / frac_x_idx / frac_y_idx for every (x, y, c) in
// double precision on the host. Mirrors the generator's "else" branch line for
// line, but each arithmetic op is double. This eliminates Metal fast-math /
// FMA contraction drift on the chromatic aberration polynomial for R/B planes.
void computePrecomputedCoords(int width,
                              int height,
                              int dst_planes,
                              const WarpRuntimeParams& runtime,
                              const WarpRectilinearParams& params,
                              std::vector<int32_t>& out_base_x,
                              std::vector<int32_t>& out_base_y,
                              std::vector<int32_t>& out_frac_x_idx,
                              std::vector<int32_t>& out_frac_y_idx) {
    const size_t total = static_cast<size_t>(width) * height * 3u;
    out_base_x.assign(total, 0);
    out_base_y.assign(total, 0);
    out_frac_x_idx.assign(total, 0);
    out_frac_y_idx.assign(total, 0);

    // Detect identity per plane (rad=[1,0,0,0] AND tan=[0,0]) — for those we
    // can fill base_x = x-1, base_y = y-1, frac=0 directly without polynomial.
    auto plane_is_identity = [&](int plane) -> bool {
        const double* r = params.rad_params64[plane];
        const double* t = params.tan_params64[plane];
        return r[0] == 1.0 && r[1] == 0.0 && r[2] == 0.0 && r[3] == 0.0 &&
               t[0] == 0.0 && t[1] == 0.0;
    };

    // We always pack 3 channels (matching the AOT input ABI).
    for (int c = 0; c < 3; ++c) {
        const int plane = warpPlaneIndex(c, params);
        const size_t base_off = static_cast<size_t>(c) * width * height;
        if (plane_is_identity(plane)) {
            // src_x = x, src_y = y → base = (x-1, y-1), frac = 0.
            for (int y = 0; y < height; ++y) {
                int32_t* bxr = out_base_x.data() + base_off + static_cast<size_t>(y) * width;
                int32_t* byr = out_base_y.data() + base_off + static_cast<size_t>(y) * width;
                for (int x = 0; x < width; ++x) {
                    bxr[x] = x - 1;
                    byr[x] = y - 1;
                }
                // frac arrays already zero-initialized by assign().
            }
            continue;
        }

        for (int y = 0; y < height; ++y) {
            const size_t row_off = base_off + static_cast<size_t>(y) * width;
            int32_t* bxr = out_base_x.data() + row_off;
            int32_t* byr = out_base_y.data() + row_off;
            int32_t* fxr = out_frac_x_idx.data() + row_off;
            int32_t* fyr = out_frac_y_idx.data() + row_off;
            for (int x = 0; x < width; ++x) {
                double sx = 0.0, sy = 0.0;
                getSrcPixelPosition(static_cast<double>(x),
                                    static_cast<double>(y),
                                    plane,
                                    runtime,
                                    params,
                                    sx, sy);
                const double sxf = std::floor(sx);
                const double syf = std::floor(sy);
                bxr[x] = static_cast<int32_t>(sxf) - 1;
                byr[x] = static_cast<int32_t>(syf) - 1;
                fxr[x] = quantizeSubsampleIndex(sx - sxf);
                fyr[x] = quantizeSubsampleIndex(sy - syf);
            }
        }
    }
    (void)dst_planes;
}

// T4-A: Shared static zero buffer for the baseline (non-precomputed) ABI
// placeholder. Avoids allocating + zero-filling ~73 MB on every call.
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
            munmap(s_zero_buf_ptr, s_zero_buf_bytes);
            s_zero_buf_ptr = nullptr;
            s_zero_buf_bytes = 0;
        }
        void* p = mmap(nullptr, needed_bytes,
                       PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            p = std::calloc(needed_elems, sizeof(int32_t));
        }
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

    const bool timing_enabled = PipelineConfig::loadFromEnv().timing.warp_halide;
    const bool use_precomputed = isPrecomputedCoordsEnabled();
    const auto t0 = std::chrono::high_resolution_clock::now();

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

    // Phase 8.1.6 D-1: optionally precompute coords on host in double.
    std::vector<int32_t> base_x_storage, base_y_storage, frac_x_storage, frac_y_storage;
    Buffer<int32_t> base_x_buf, base_y_buf, frac_x_buf, frac_y_buf;
    auto t_pre0 = std::chrono::high_resolution_clock::now();
    auto t_pre1 = t_pre0;
    if (use_precomputed) {
        t_pre0 = std::chrono::high_resolution_clock::now();
        computePrecomputedCoords(width, height, planes, runtime, params,
                                 base_x_storage, base_y_storage,
                                 frac_x_storage, frac_y_storage);
        t_pre1 = std::chrono::high_resolution_clock::now();
        // Halide Buffer with planar layout (x, y, c): width × height × 3.
        base_x_buf  = Buffer<int32_t>(base_x_storage.data(),  width, height, 3);
        base_y_buf  = Buffer<int32_t>(base_y_storage.data(),  width, height, 3);
        frac_x_buf  = Buffer<int32_t>(frac_x_storage.data(),  width, height, 3);
        frac_y_buf  = Buffer<int32_t>(frac_y_storage.data(),  width, height, 3);
    } else {
        // T4-A: Reuse static mmap lazy-zero buffer; no per-call zero-fill (~73 MB saved).
        const int32_t* zero_ptr = getOrGrowZeroBuf(width, height);
        base_x_buf  = Buffer<int32_t>(const_cast<int32_t*>(zero_ptr), width, height, 3);
        base_y_buf  = base_x_buf;
        frac_x_buf  = base_x_buf;
        frac_y_buf  = base_x_buf;
    }

    // Be explicit for GPU backends: inputs are host-authored, output host is stale
    // until we copy back after kernel execution.
    src_buf.set_host_dirty();
    rad_buf.set_host_dirty();
    tan_buf.set_host_dirty();
    tile_bounds_buf.set_host_dirty();
    if (use_precomputed) {
        base_x_buf.set_host_dirty();
        base_y_buf.set_host_dirty();
        frac_x_buf.set_host_dirty();
        frac_y_buf.set_host_dirty();
    }
    dst_buf.set_host_dirty(false);

    const auto t1 = std::chrono::high_resolution_clock::now();
    int result = 0;
    if (use_precomputed) {
        result = rectilinear_warp_precomputed(src_buf.raw_buffer(),
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
    } else {
        // Baseline path also expects the 4 precompute buffers in ABI (gen
        // emits them regardless because they are listed as Inputs); kernel
        // simply does not read from them when precompute_coords=false.
        result = rectilinear_warp(src_buf.raw_buffer(),
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
    }
    if (result != 0) {
        return false;
    }
    const auto t2 = std::chrono::high_resolution_clock::now();
    if (timing_enabled && use_precomputed) {
        auto ms = [](const auto& a, const auto& b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
        };
        std::cerr << "[WarpHalidePrecompute] host_double_precompute=" << ms(t_pre0, t_pre1)
                  << " ms\n";
    }
    auto copyEnd = t2;
    if (!copyHalideOutputToHost(dst_buf, timing_enabled, "[WarpHalideReadback]",
                                t2, copyEnd)) {
        return false;
    }
    const auto t3 = copyEnd;

    if (timing_enabled) {
        auto ms = [](const auto& a, const auto& b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
        };
        std::cerr << "[WarpHalideTiming] prep=" << ms(t0, t1)
                  << " ms kernel=" << ms(t1, t2)
                  << " ms copy_to_host=" << ms(t2, t3)
                  << " ms\n";
    }

    return true;
}

bool runDemosaicWarpHalideAot(const uint16_t* src_bayer,
                              int width,
                              int height,
                              const WarpRuntimeParams& runtime,
                              const WarpRectilinearParams& params,
                              const TileClippingGrid& tile_grid,
                              uint16_t* dst_interleaved_rgb) {
    if (!src_bayer || !dst_interleaved_rgb || width <= 0 || height <= 0) {
        return false;
    }

    const bool timing_enabled = PipelineConfig::loadFromEnv().timing.demosaic_warp_halide;
    const auto t0 = std::chrono::high_resolution_clock::now();

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

    const auto t1 = std::chrono::high_resolution_clock::now();
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
                                         dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    const auto t2 = std::chrono::high_resolution_clock::now();
    auto copyEnd = t2;
    if (!copyHalideOutputToHost(dst_buf, timing_enabled, "[DemosaicWarpHalideReadback]",
                                t2, copyEnd)) {
        return false;
    }
    const auto t3 = copyEnd;

    if (timing_enabled) {
        auto ms = [](const auto& a, const auto& b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
        };
        std::cerr << "[DemosaicWarpHalideTiming] prep=" << ms(t0, t1)
                  << " ms kernel=" << ms(t1, t2)
                  << " ms copy_to_host=" << ms(t2, t3)
                  << " ms\n";
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
    bool timing_enabled = false;
    std::chrono::high_resolution_clock::time_point t0;
    std::chrono::high_resolution_clock::time_point t1;
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
    if (mode == WarpRectilinearMode::HALIDE_METAL || mode == WarpRectilinearMode::AUTO) {
        if (runWarpHalideAot(src_interleaved_rgb,
                             width,
                             height,
                             planes,
                             runtime,
                             params,
                             tile_grid,
                             dst_interleaved_rgb)) {
            return true;
        }
        if (mode == WarpRectilinearMode::HALIDE_METAL) {
            return false;
        }
    }

    warpRectilinearCpu(src_interleaved_rgb,
                       width,
                       height,
                       planes,
                       params,
                       tile_grid,
                       dst_interleaved_rgb);
    return true;
}

bool demosaic_warp_rectilinear_halide(const uint16_t* src_bayer,
                                      int width,
                                      int height,
                                      const WarpRectilinearParams& params,
                                      WarpRectilinearMode mode,
                                      uint16_t* dst_interleaved_rgb) {
    if (!src_bayer || !dst_interleaved_rgb || width <= 0 || height <= 0) {
        return false;
    }
    if (mode == WarpRectilinearMode::SDK || isWarpBitExactModeEnabled()) {
        return false;
    }

    const WarpRuntimeParams runtime = buildRuntimeParams(width, height, params);
    const TileClippingGrid tile_grid = buildTileClippingGrid(width, height, 3, runtime, params);
    if (mode == WarpRectilinearMode::HALIDE_METAL || mode == WarpRectilinearMode::AUTO) {
        if (runDemosaicWarpHalideAot(src_bayer,
                                     width,
                                     height,
                                     runtime,
                                     params,
                                     tile_grid,
                                     dst_interleaved_rgb)) {
            return true;
        }
        if (mode == WarpRectilinearMode::HALIDE_METAL) {
            return false;
        }
    }

    return false;
}

bool apply_warp_rectilinear_to_image(dng_host& host,
                                     dng_negative& negative,
                                     const dng_opcode_WarpRectilinear& opcode,
                                     AutoPtr<dng_image>& image,
                                     WarpRectilinearMode mode) {
    if (mode == WarpRectilinearMode::SDK) {
        return false;
    }

    if (isWarpBitExactModeEnabled()) {
        const_cast<dng_opcode_WarpRectilinear&>(opcode).Apply(host, negative, image);
        return true;
    }

    const bool timing_enabled = PipelineConfig::loadFromEnv().timing.warp_halide;
    auto ms = [](const auto& a, const auto& b) {
        return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
    };

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t planes = 0;
    std::vector<uint16_t> src_data;
    const auto t0 = std::chrono::high_resolution_clock::now();
    if (!imageToInterleaved(image.Get(), src_data, width, height, planes)) {
        return false;
    }
    const auto t1 = std::chrono::high_resolution_clock::now();

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
    const auto t2 = std::chrono::high_resolution_clock::now();

    if (!writeInterleavedToImage(image.Get(), dst_data, width, height, planes)) {
        return false;
    }
    const auto t3 = std::chrono::high_resolution_clock::now();
    if (timing_enabled) {
        std::cerr << "[WarpHalideImageTiming] get_interleaved=" << ms(t0, t1)
                  << " ms warp_apply=" << ms(t1, t2)
                  << " ms put_image=" << ms(t2, t3)
                  << " ms\n";
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
    uint16_t* dst_interleaved_rgb) {
    if (!src_bayer || !dst_interleaved_rgb || width <= 0 || height <= 0) {
        return nullptr;
    }
    if (mode == WarpRectilinearMode::SDK || isWarpBitExactModeEnabled()) {
        return nullptr;
    }
    if (mode != WarpRectilinearMode::HALIDE_METAL &&
        mode != WarpRectilinearMode::HALIDE_CPU &&
        mode != WarpRectilinearMode::AUTO) {
        return nullptr;
    }

    auto impl = std::make_unique<DemosaicWarpHalideAsyncImpl>();
    impl->timing_enabled =
        PipelineConfig::loadFromEnv().timing.demosaic_warp_halide;
    impl->t0 = std::chrono::high_resolution_clock::now();

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

    impl->t1 = std::chrono::high_resolution_clock::now();
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
        impl->dst_buf.raw_buffer());
    if (result != 0) {
        return nullptr;
    }

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

    const auto t2 = std::chrono::high_resolution_clock::now();
    auto copyEnd = t2;
    if (!copyHalideOutputToHost(impl->dst_buf, impl->timing_enabled,
                                "[DemosaicWarpHalideReadback]", t2, copyEnd)) {
        return false;
    }
    const auto t3 = copyEnd;

    if (impl->timing_enabled) {
        auto ms = [](const auto& a, const auto& b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a)
                       .count() /
                   1000.0;
        };
        std::cerr << "[DemosaicWarpHalideTiming] prep="
                  << ms(impl->t0, impl->t1) << " ms kernel="
                  << ms(impl->t1, t2) << " ms copy_to_host="
                  << ms(t2, t3) << " ms\n";
    }
    return true;
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
