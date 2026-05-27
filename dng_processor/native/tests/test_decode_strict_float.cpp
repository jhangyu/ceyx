// Phase 8.1.6 Stage E A1 — Strict-float warp eval binary.
//
// Mirrors the Stage1+Stage2+(bilinear demosaic)+(standalone WarpRectilinear)
// portion of test_decode.cpp, BUT swaps the standalone warp AOT call from
// the production rectilinear_warp to the strict_float variant
// (rectilinear_warp_strict_float). Stage 4 is intentionally omitted — the
// gates for A1 evaluation are at Stage 3.
//
// Coordinate-chain helpers (buildRuntimeParams, buildTileClippingGrid,
// getSrcPixelPosition, etc.) are copied verbatim from
// dng_warp_halide.cpp's anonymous namespace. dng_warp_halide.cpp itself is
// hard-banned for modification (Phase 8.1.6 Stage E §7), so no production
// source is touched here.
//
// Outputs (per CLI flags):
//   * full-frame Stage3 raw dump (for compare_psnr.py)
//   * SDK reference Stage3 raw dump
//   * ROI 256x256 per-plane R/G/B max diff vs SDK
//   * stage3 timing mean across N runs (warp AOT only)
//   * JSON results file (if --json <path>)
//
// Build:  python3 dng_processor/native/scripts/build_native_watchdog.py \
//             --target test_decode_strict_float
// Run:    ./dng_processor/native/build/test_decode_strict_float \
//             image_samples/lossless_dng_sample.dng [--out-dir <dir>]
//             [--repeat N] [--json <path>]
//
// See: docs/logs/2026-05-09/phase8_1_6_stageE/a1_strict_float_design.md

#include "stage_contract_checks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_lens_correction.h>
#include <dng_mosaic_halide.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_rect.h>
#include <dng_warp_halide.h>

#include "HalideBuffer.h"

#include "rectilinear_warp_strict_float.h"

using std::cerr;
using std::cout;
using std::string;
using std::vector;
using Halide::Runtime::Buffer;
using std::chrono::high_resolution_clock;
using std::chrono::microseconds;
using std::chrono::duration_cast;

namespace {

// ===========================================================================
// COPIED FROM dng_processor/native/dng_sdk_custom/source/dng_warp_halide.cpp
// (anonymous namespace). Strict_float variant must mirror production setup
// bit-exact for runtime params + tile bounds; only the AOT entry differs.
// ===========================================================================

constexpr int kResampleSubsampleBits2D = 5;
constexpr int kResampleSubsampleCount2D = 1 << kResampleSubsampleBits2D;

struct WarpRuntimeParamsLocal {
    double center_x = 0.0;
    double center_y = 0.0;
    double norm_radius = 1.0;
    double inv_norm_radius = 1.0;
    double pixel_scale_v = 1.0;
    double pixel_scale_v_inv = 1.0;
};

WarpRuntimeParamsLocal buildRuntimeParamsLocal(int width, int height,
                                               const WarpRectilinearParams& params) {
    WarpRuntimeParamsLocal rt;
    const dng_rect bounds(0, 0, height, width);
    rt.center_x = Lerp_real64(static_cast<double>(bounds.l),
                              static_cast<double>(bounds.r),
                              params.center_h64);
    rt.center_y = Lerp_real64(static_cast<double>(bounds.t),
                              static_cast<double>(bounds.b),
                              params.center_v64);
    rt.pixel_scale_v = 1.0 / params.pixel_aspect_ratio64;
    rt.pixel_scale_v_inv = params.pixel_aspect_ratio64;

    dng_rect square_bounds(bounds);
    square_bounds.b = square_bounds.t +
                      Round_int32(rt.pixel_scale_v * static_cast<double>(square_bounds.H()));

    const dng_point_real64 square_center(
        Lerp_real64(static_cast<double>(square_bounds.t),
                    static_cast<double>(square_bounds.b),
                    params.center_v64),
        Lerp_real64(static_cast<double>(square_bounds.l),
                    static_cast<double>(square_bounds.r),
                    params.center_h64));

    rt.norm_radius = MaxDistancePointToRect(square_center, square_bounds);
    rt.inv_norm_radius = 1.0 / rt.norm_radius;
    return rt;
}

inline int warpPlaneIndexLocal(int channel, const WarpRectilinearParams& params) {
    if (params.planes == 0 || params.planes == 1) return 0;
    if (channel >= 0 && channel < static_cast<int>(params.planes)) return channel;
    return 0;
}

void getSrcPixelPositionLocal(double dst_x, double dst_y, int plane,
                              const WarpRuntimeParamsLocal& rt,
                              const WarpRectilinearParams& params,
                              double& src_x, double& src_y) {
    const double* rad = params.rad_params64[plane];
    const double* tan = params.tan_params64[plane];
    const double diff_x = dst_x - rt.center_x;
    const double diff_y = dst_y - rt.center_y;
    const double diff_norm_x = diff_x * rt.inv_norm_radius;
    const double diff_norm_y = diff_y * rt.inv_norm_radius;
    const double diff_scaled_x = diff_norm_x;
    const double diff_scaled_y = diff_norm_y * rt.pixel_scale_v;
    const double rr = std::min(diff_scaled_x * diff_scaled_x +
                               diff_scaled_y * diff_scaled_y, 1.0);
    const double ratio = rad[0] + rr * (rad[1] + rr * (rad[2] + rr * rad[3]));
    const double tan_v = tan[0] * (rr + 2.0 * diff_scaled_y * diff_scaled_y) +
                         2.0 * tan[1] * diff_scaled_x * diff_scaled_y;
    const double tan_h = tan[1] * (rr + 2.0 * diff_scaled_x * diff_scaled_x) +
                         2.0 * tan[0] * diff_scaled_x * diff_scaled_y;
    if (params.is_tan_nop_all) {
        src_x = rt.center_x + (dst_x - rt.center_x) * ratio;
        src_y = rt.center_y + (dst_y - rt.center_y) * ratio;
        return;
    }
    if (params.is_rad_nop_all) {
        src_x = dst_x + rt.norm_radius * tan_h;
        src_y = dst_y + rt.norm_radius * tan_v * rt.pixel_scale_v_inv;
        return;
    }
    src_x = rt.center_x + rt.norm_radius * (diff_norm_x * ratio + tan_h);
    src_y = rt.center_y + rt.norm_radius * (diff_norm_y * ratio + tan_v * rt.pixel_scale_v_inv);
}

int computeSdkTileExtentLocal(int extent) {
    if (extent <= 0) return 1;
    constexpr int kMaxTileExtent = 256;
    const int count = (extent + kMaxTileExtent - 1) / kMaxTileExtent;
    return std::max(1, (extent + count - 1) / count);
}

struct TileClippingGridLocal {
    int tile_width = 1;
    int tile_height = 1;
    int tiles_x = 1;
    int tiles_y = 1;
    vector<int32_t> bounds;
    int32_t get(int kind, int tx, int ty) const {
        return bounds[static_cast<size_t>(kind) +
                      4u * (static_cast<size_t>(tx) +
                            static_cast<size_t>(tiles_x) * ty)];
    }
};

TileClippingGridLocal buildTileClippingGridLocal(int width, int height, int dst_planes,
                                                 const WarpRuntimeParamsLocal& rt,
                                                 const WarpRectilinearParams& params) {
    TileClippingGridLocal grid;
    grid.tile_width  = computeSdkTileExtentLocal(width);
    grid.tile_height = computeSdkTileExtentLocal(height);
    grid.tiles_x = std::max(1, (width  + grid.tile_width  - 1) / grid.tile_width);
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
                const int p = warpPlaneIndexLocal(plane, params);
                for (int c = l; c < r; ++c) {
                    double sx = 0.0, sy = 0.0;
                    getSrcPixelPositionLocal(c, t, p, rt, params, sx, sy);
                    yMin = std::min(yMin, static_cast<int32_t>(std::floor(sy)));
                    getSrcPixelPositionLocal(c, b - 1, p, rt, params, sx, sy);
                    yMax = std::max(yMax, static_cast<int32_t>(std::ceil(sy)));
                }
                for (int rr = t; rr < b; ++rr) {
                    double sx = 0.0, sy = 0.0;
                    getSrcPixelPositionLocal(l, rr, p, rt, params, sx, sy);
                    xMin = std::min(xMin, static_cast<int32_t>(std::floor(sx)));
                    getSrcPixelPositionLocal(r - 1, rr, p, rt, params, sx, sy);
                    xMax = std::max(xMax, static_cast<int32_t>(std::ceil(sx)));
                }
            }

            const int32_t hMin = xMin - kKernelRadius;
            const int32_t vMin = yMin - kKernelRadius;
            const int32_t hMax = xMax + kKernelRadius - kBicubicWidth;
            const int32_t vMax = yMax + kKernelRadius - kBicubicWidth;

            const size_t base = 4u * (static_cast<size_t>(tx) +
                                      static_cast<size_t>(grid.tiles_x) * ty);
            grid.bounds[base + 0] = hMin;
            grid.bounds[base + 1] = hMax;
            grid.bounds[base + 2] = vMin;
            grid.bounds[base + 3] = vMax;
        }
    }
    return grid;
}

// ---------------------------------------------------------------------------
// Strict-float warp AOT call (mirrors runWarpHalideAot in dng_warp_halide.cpp
// minus the precompute branch, which we never enable here).
// ---------------------------------------------------------------------------

bool runWarpStrictFloatAot(const uint16_t* src_interleaved_rgb,
                           int width, int height, int planes,
                           const WarpRuntimeParamsLocal& rt,
                           const WarpRectilinearParams& params,
                           const TileClippingGridLocal& tile_grid,
                           uint16_t* dst_interleaved_rgb,
                           double* out_kernel_ms,
                           double* out_copy_ms) {
    if (!src_interleaved_rgb || !dst_interleaved_rgb || width <= 0 || height <= 0) {
        return false;
    }

    Buffer<uint16_t> src_buf =
        Buffer<uint16_t>::make_interleaved(const_cast<uint16_t*>(src_interleaved_rgb),
                                           width, height, planes);
    Buffer<uint16_t> dst_buf =
        Buffer<uint16_t>::make_interleaved(dst_interleaved_rgb, width, height, planes);

    const int plane_count = static_cast<int>(std::max<uint32_t>(1, params.planes));
    vector<float> rad_storage(static_cast<size_t>(4 * plane_count));
    vector<float> tan_storage(static_cast<size_t>(2 * plane_count));
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
                                    4, tile_grid.tiles_x, tile_grid.tiles_y);

    // Strict_float AOT does not exercise the precompute branch (we pass
    // precompute_coords=false implicitly via the generator default), but ABI
    // still requires the 4 placeholder buffers. Reuse a single zero buffer.
    vector<int32_t> zero_storage(static_cast<size_t>(width) * height * 3, 0);
    Buffer<int32_t> base_x_buf(zero_storage.data(), width, height, 3);
    Buffer<int32_t> base_y_buf = base_x_buf;
    Buffer<int32_t> frac_x_buf = base_x_buf;
    Buffer<int32_t> frac_y_buf = base_x_buf;

    src_buf.set_host_dirty();
    rad_buf.set_host_dirty();
    tan_buf.set_host_dirty();
    tile_bounds_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const auto t0 = high_resolution_clock::now();
    const int result = rectilinear_warp_strict_float(
        src_buf.raw_buffer(), rad_buf.raw_buffer(), tan_buf.raw_buffer(),
        tile_bounds_buf.raw_buffer(),
        static_cast<int32_t>(params.planes),
        static_cast<float>(rt.center_x),
        static_cast<float>(rt.center_y),
        static_cast<float>(rt.norm_radius),
        static_cast<float>(rt.inv_norm_radius),
        static_cast<float>(rt.pixel_scale_v),
        static_cast<float>(rt.pixel_scale_v_inv),
        params.is_rad_nop_all ? 1 : 0,
        params.is_tan_nop_all ? 1 : 0,
        static_cast<int32_t>(tile_grid.tile_width),
        static_cast<int32_t>(tile_grid.tile_height),
        base_x_buf.raw_buffer(), base_y_buf.raw_buffer(),
        frac_x_buf.raw_buffer(), frac_y_buf.raw_buffer(),
        dst_buf.raw_buffer());
    if (result != 0) {
        cerr << "ERROR: rectilinear_warp_strict_float returned " << result << "\n";
        return false;
    }
    const auto t1 = high_resolution_clock::now();
    if (dst_buf.device_sync() != 0) {
        cerr << "ERROR: device_sync failed\n";
        return false;
    }
    if (dst_buf.copy_to_host() != 0) {
        cerr << "ERROR: copy_to_host failed\n";
        return false;
    }
    const auto t2 = high_resolution_clock::now();

    if (out_kernel_ms) {
        *out_kernel_ms = duration_cast<microseconds>(t1 - t0).count() / 1000.0;
    }
    if (out_copy_ms) {
        *out_copy_ms = duration_cast<microseconds>(t2 - t1).count() / 1000.0;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void extractImageData(dng_image* image, vector<uint16_t>& data,
                      uint32_t& width, uint32_t& height, uint32_t& planes) {
    width  = image->Width();
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
}

AutoPtr<dng_image> makeImageFromInterleaved(dng_host& host, uint32_t width,
                                            uint32_t height, uint32_t planes,
                                            const vector<uint16_t>& data) {
    dng_point size(static_cast<int32>(height), static_cast<int32>(width));
    AutoPtr<dng_image> image(host.Make_dng_image(dng_rect(size), planes, ttShort));
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
    return AutoPtr<dng_image>(image.Release());
}

const dng_opcode_WarpRectilinear* findWarpRectilinear(const dng_negative& negative) {
    const dng_opcode_list& list = negative.OpcodeList3();
    for (uint32_t i = 0; i < list.Count(); ++i) {
        const dng_opcode& opcode = list.Entry(i);
        if (opcode.OpcodeID() == dngOpcode_WarpRectilinear) {
            return &static_cast<const dng_opcode_WarpRectilinear&>(opcode);
        }
    }
    return nullptr;
}

bool saveRawFile(const string& path, const void* data, size_t bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(data), bytes);
    return f.good();
}

double computePsnr16(const uint16_t* a, const uint16_t* b, size_t n) {
    if (n == 0) return 0.0;
    double mse = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        mse += d * d;
    }
    mse /= static_cast<double>(n);
    if (mse <= 0.0) return std::numeric_limits<double>::infinity();
    return 10.0 * std::log10((65535.0 * 65535.0) / mse);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0]
             << " <dng_path> [--out-dir DIR] [--repeat N] [--json PATH]\n";
        return 1;
    }
    const string dngPath = argv[1];
    string outDir = "docs/logs/2026-05-09/phase8_1_6_stageE/a1_strict_float_eval";
    int repeat = 5;
    string jsonPath;

    for (int i = 2; i < argc; ++i) {
        const string a = argv[i];
        if (a == "--out-dir" && i + 1 < argc) outDir = argv[++i];
        else if (a == "--repeat" && i + 1 < argc) repeat = std::max(1, std::atoi(argv[++i]));
        else if (a == "--json" && i + 1 < argc) jsonPath = argv[++i];
        else {
            cerr << "WARN: unknown arg " << a << "\n";
        }
    }

    try {
        dng_host host;
        dng_file_stream stream(dngPath.c_str());

        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        if (!info.IsValidDNG()) {
            cerr << "ERROR: Not a valid DNG\n";
            return 1;
        }
        const dng_ifd& rawIFD = *info.fIFD[info.fMainIndex];
        if (StageContract::detectDecodePath(rawIFD.fPhotometricInterpretation)
            != StageContract::DecodePath::CFA_BAYER) {
            cerr << "ERROR: expected CFA Bayer input\n";
            return 1;
        }

        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        negative->ReadStage1Image(host, stream, info);
        negative->BuildStage2Image(host);

        dng_image* stage2 = const_cast<dng_image*>(negative->Stage2Image());
        uint32_t s2w = 0, s2h = 0, s2p = 0;
        vector<uint16_t> stage2Data;
        extractImageData(stage2, stage2Data, s2w, s2h, s2p);

        // Pre-Opcode3: bilinear demosaic to RGB interleaved.
        vector<uint16_t> preOpcode3(static_cast<size_t>(s2w) * s2h * 3);
        demosaic_bilinear_compat(stage2Data.data(),
                                 static_cast<int>(s2w),
                                 static_cast<int>(s2h),
                                 preOpcode3.data());

        const dng_opcode_WarpRectilinear* warpOpcode = findWarpRectilinear(*negative);
        if (!warpOpcode) {
            cerr << "ERROR: OpcodeList3 missing WarpRectilinear\n";
            return 1;
        }

        WarpRectilinearParams params;
        if (!extractWarpRectilinearParams(*warpOpcode,
                                          negative->PixelAspectRatio(),
                                          params)) {
            cerr << "ERROR: extractWarpRectilinearParams failed\n";
            return 1;
        }

        // SDK reference path (uses original dng_opcode Apply directly).
        AutoPtr<dng_image> sdkImage = makeImageFromInterleaved(host, s2w, s2h, 3, preOpcode3);
        const_cast<dng_opcode_WarpRectilinear*>(warpOpcode)
            ->Apply(host, *negative, sdkImage);
        uint32_t rW = 0, rH = 0, rP = 0;
        vector<uint16_t> sdkData;
        extractImageData(sdkImage.Get(), sdkData, rW, rH, rP);

        // Strict-float AOT path (timing across N runs).
        const WarpRuntimeParamsLocal rt =
            buildRuntimeParamsLocal(static_cast<int>(s2w), static_cast<int>(s2h), params);
        const TileClippingGridLocal grid =
            buildTileClippingGridLocal(static_cast<int>(s2w),
                                       static_cast<int>(s2h), 3, rt, params);

        vector<uint16_t> sfData(preOpcode3.size());
        vector<double> kernel_ms_runs, copy_ms_runs;
        for (int run = 0; run < repeat; ++run) {
            double k_ms = 0.0, c_ms = 0.0;
            std::fill(sfData.begin(), sfData.end(), uint16_t{0});
            const bool ok = runWarpStrictFloatAot(preOpcode3.data(),
                                                   static_cast<int>(s2w),
                                                   static_cast<int>(s2h), 3,
                                                   rt, params, grid,
                                                   sfData.data(),
                                                   &k_ms, &c_ms);
            if (!ok) {
                cerr << "ERROR: strict_float warp run " << run << " failed\n";
                return 1;
            }
            kernel_ms_runs.push_back(k_ms);
            copy_ms_runs.push_back(c_ms);
            cerr << "[Run " << run << "] kernel=" << std::fixed
                 << std::setprecision(3) << k_ms << " ms copy=" << c_ms
                 << " ms\n";
        }

        auto mean = [](const vector<double>& v) {
            double s = 0.0; for (double x : v) s += x;
            return v.empty() ? 0.0 : s / static_cast<double>(v.size());
        };
        const double kernel_ms_mean = mean(kernel_ms_runs);
        const double copy_ms_mean = mean(copy_ms_runs);
        const double stage3_warp_ms_mean = kernel_ms_mean + copy_ms_mean;

        // Full-frame Stage3 PSNR (strict_float vs SDK).
        const size_t pixCount = static_cast<size_t>(s2w) * s2h * 3;
        const double psnr_full = computePsnr16(sfData.data(), sdkData.data(), pixCount);

        // ROI 256x256 bottom-right per-plane max diff.
        const int roi_w = 256, roi_h = 256;
        const int x0 = std::max(0, static_cast<int>(s2w) - roi_w);
        const int y0 = std::max(0, static_cast<int>(s2h) - roi_h);
        const int x1 = std::min(static_cast<int>(s2w), x0 + roi_w);
        const int y1 = std::min(static_cast<int>(s2h), y0 + roi_h);

        int roi_max[3] = {0,0,0};
        long long roi_sum[3] = {0,0,0};
        const long long roi_count = static_cast<long long>(x1 - x0) * (y1 - y0);
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const size_t idx = (static_cast<size_t>(y) * s2w + x) * 3 + c;
                    const int d = std::abs(static_cast<int>(sfData[idx]) -
                                           static_cast<int>(sdkData[idx]));
                    if (d > roi_max[c]) roi_max[c] = d;
                    roi_sum[c] += d;
                }
            }
        }

        // Save raw dumps.
        // Use mkdir -p style hint: rely on caller to create outDir. Continue
        // even if save fails (warn only).
        const string sfRawPath = outDir + "/strict_float_stage3_" +
            std::to_string(s2w) + "x" + std::to_string(s2h) + "_3p.raw";
        const string sdkRawPath = outDir + "/sdk_reference_stage3_" +
            std::to_string(s2w) + "x" + std::to_string(s2h) + "_3p.raw";
        if (!saveRawFile(sfRawPath, sfData.data(), pixCount * sizeof(uint16_t))) {
            cerr << "WARN: failed to save " << sfRawPath << "\n";
        }
        if (!saveRawFile(sdkRawPath, sdkData.data(), pixCount * sizeof(uint16_t))) {
            cerr << "WARN: failed to save " << sdkRawPath << "\n";
        }

        // Console summary.
        const char* planeNames[3] = {"R","G","B"};
        cout << std::fixed << std::setprecision(4);
        cout << "===== Phase 8.1.6 Stage E A1 strict_float eval =====\n";
        cout << "Image:        " << dngPath << "\n";
        cout << "Stage3 size:  " << s2w << "x" << s2h << " x 3 planes\n";
        cout << "Repeat runs:  " << repeat << "\n";
        cout << "PSNR full:    " << psnr_full << " dB\n";
        cout << "ROI " << (x1-x0) << "x" << (y1-y0)
             << " (x=[" << x0 << "," << x1 << ") y=[" << y0 << "," << y1 << ")):\n";
        for (int c = 0; c < 3; ++c) {
            cout << "  " << planeNames[c] << ": max=" << roi_max[c]
                 << " mean=" << (static_cast<double>(roi_sum[c]) / roi_count) << "\n";
        }
        cout << "Stage3 warp timing (mean over " << repeat << " runs):\n";
        cout << "  kernel:    " << std::setprecision(3) << kernel_ms_mean << " ms\n";
        cout << "  copy_back: " << copy_ms_mean << " ms\n";
        cout << "  total:     " << stage3_warp_ms_mean << " ms\n";
        cout << "Raw outputs:\n";
        cout << "  " << sfRawPath << "\n";
        cout << "  " << sdkRawPath << "\n";

        // JSON dump.
        if (!jsonPath.empty()) {
            std::ofstream j(jsonPath);
            if (!j) {
                cerr << "WARN: failed to open " << jsonPath << "\n";
            } else {
                j << std::fixed << std::setprecision(6);
                j << "{\n";
                j << "  \"image\": \"" << dngPath << "\",\n";
                j << "  \"width\": " << s2w << ",\n";
                j << "  \"height\": " << s2h << ",\n";
                j << "  \"repeat\": " << repeat << ",\n";
                j << "  \"psnr_full_db\": " << psnr_full << ",\n";
                j << "  \"roi\": {\n";
                j << "    \"x0\": " << x0 << ", \"y0\": " << y0
                  << ", \"x1\": " << x1 << ", \"y1\": " << y1 << ",\n";
                for (int c = 0; c < 3; ++c) {
                    j << "    \"" << planeNames[c] << "_max\": " << roi_max[c]
                      << ", \"" << planeNames[c] << "_mean\": "
                      << (static_cast<double>(roi_sum[c]) / roi_count);
                    j << (c == 2 ? "\n" : ",\n");
                }
                j << "  },\n";
                j << "  \"timing_ms\": {\n";
                j << "    \"kernel_mean\": " << kernel_ms_mean << ",\n";
                j << "    \"copy_mean\": " << copy_ms_mean << ",\n";
                j << "    \"stage3_warp_mean\": " << stage3_warp_ms_mean << ",\n";
                j << "    \"runs\": [";
                for (size_t i = 0; i < kernel_ms_runs.size(); ++i) {
                    j << "{\"kernel\":" << kernel_ms_runs[i]
                      << ",\"copy\":" << copy_ms_runs[i] << "}";
                    if (i + 1 < kernel_ms_runs.size()) j << ",";
                }
                j << "]\n";
                j << "  },\n";
                j << "  \"raw_outputs\": {\n";
                j << "    \"strict_float\": \"" << sfRawPath << "\",\n";
                j << "    \"sdk_reference\": \"" << sdkRawPath << "\"\n";
                j << "  }\n";
                j << "}\n";
                cout << "JSON:         " << jsonPath << "\n";
            }
        }
        return 0;
    } catch (const dng_exception& e) {
        cerr << "DNG Exception: " << e.ErrorCode() << "\n";
    } catch (const std::exception& e) {
        cerr << "Exception: " << e.what() << "\n";
    }
    return 1;
}
