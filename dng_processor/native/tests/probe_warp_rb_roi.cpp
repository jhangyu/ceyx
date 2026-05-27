// Phase 8.1.6 Stage E G6a — Warp R/B ROI probe
//
// Per-pixel CSV + PGM heatmap dump for the bottom-right 256x256 ROI of a
// lossless DNG. Produces side-by-side intermediates from:
//   * SDK reference   (dng_opcode_WarpRectilinear::Apply)
//   * Halide CPU      (warp_rectilinear_halide with HALIDE_CPU)
//   * Halide Metal    (warp_rectilinear_halide with HALIDE_METAL, Apple only)
//
// This binary does NOT modify any production generator or kernel. The
// coordinate-chain math is *re-implemented* here (copied verbatim from
// dng_warp_halide.cpp's anonymous namespace) so we can dump ref_/halide_
// intermediates (src_x/src_y, floor, base, frac_idx, clipped) per pixel.
// Halide CPU is bit-exact with the SDK reference (135.74 dB measured), so
// these intermediates serve as the reference contract for both. Metal-side
// intermediates are not directly observable; we report (cpu - sdk) and
// (metal - sdk) pixel diffs so the analyst can attribute drift.
//
// See: docs/logs/2026-05-09/phase8_1_6_stageE/roi_probe/probe_design.md

#include "stage_contract_checks.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

using std::cerr;
using std::cout;
using std::string;
using std::vector;

namespace {

// ---------------------------------------------------------------------------
// COPIED FROM dng_processor/native/dng_sdk_custom/source/dng_warp_halide.cpp
// (anonymous namespace, lines ~159-385). Probe must mirror production CPU
// path bit-exact.  If production formula ever changes, sync this block.
// ---------------------------------------------------------------------------

constexpr int kResampleSubsampleBits2D = 5;
constexpr int kResampleSubsampleCount2D = 1 << kResampleSubsampleBits2D;

inline int quantizeSubsampleIndex(double fract) {
    int idx = static_cast<int>(std::floor(fract * static_cast<double>(kResampleSubsampleCount2D)));
    if (idx < 0) idx = 0;
    else if (idx >= kResampleSubsampleCount2D) idx = kResampleSubsampleCount2D - 1;
    return idx;
}

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
// Probe utilities
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

void writeRoiHeatmapPgm(const vector<uint16_t>& a, const vector<uint16_t>& b,
                        int W, int /*H*/, int channel,
                        int x0, int y0, int x1, int y1,
                        const string& path) {
    const int rw = x1 - x0;
    const int rh = y1 - y0;
    vector<uint8_t> pgm(static_cast<size_t>(rw) * rh, 0);
    int max_diff = 0;
    for (int yy = 0; yy < rh; ++yy) {
        for (int xx = 0; xx < rw; ++xx) {
            const int gx = x0 + xx, gy = y0 + yy;
            const size_t idx = (static_cast<size_t>(gy) * W + gx) * 3 + channel;
            const int d = std::abs(static_cast<int>(a[idx]) - static_cast<int>(b[idx]));
            if (d > max_diff) max_diff = d;
            pgm[static_cast<size_t>(yy) * rw + xx] =
                static_cast<uint8_t>(std::min(d * 64, 255));
        }
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P5\n%d %d\n255\n", rw, rh);
    std::fwrite(pgm.data(), 1, pgm.size(), f);
    std::fclose(f);
    std::fprintf(stderr, "[Heatmap] %s max_diff=%d (ROI %dx%d)\n",
                 path.c_str(), max_diff, rw, rh);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <dng_path> [out_dir]\n";
        return 1;
    }
    const string dngPath = argv[1];
    const string outDir = (argc >= 3)
        ? argv[2]
        : "docs/logs/2026-05-09/phase8_1_6_stageE/roi_probe";

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
        const auto path = StageContract::detectDecodePath(rawIFD.fPhotometricInterpretation);
        if (path != StageContract::DecodePath::CFA_BAYER) {
            cerr << "ERROR: probe expects CFA Bayer input\n";
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

        // Pre-Opcode3: bilinear demosaic to RGB interleaved (matches existing test).
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

        // --- SDK reference path ---
        AutoPtr<dng_image> sdkImage = makeImageFromInterleaved(host, s2w, s2h, 3, preOpcode3);
        const_cast<dng_opcode_WarpRectilinear*>(warpOpcode)
            ->Apply(host, *negative, sdkImage);
        uint32_t rW = 0, rH = 0, rP = 0;
        vector<uint16_t> sdkData;
        extractImageData(sdkImage.Get(), sdkData, rW, rH, rP);

        // --- Halide CPU path ---
        vector<uint16_t> cpuData(preOpcode3.size());
        if (!warp_rectilinear_halide(preOpcode3.data(),
                                     static_cast<int>(s2w),
                                     static_cast<int>(s2h),
                                     3, params,
                                     WarpRectilinearMode::HALIDE_CPU,
                                     cpuData.data())) {
            cerr << "ERROR: HALIDE_CPU warp failed\n";
            return 1;
        }

        // --- Halide Metal path (Apple only) ---
        bool haveMetal = false;
        vector<uint16_t> metalData(preOpcode3.size(), 0);
#if defined(__APPLE__)
        haveMetal = warp_rectilinear_halide(preOpcode3.data(),
                                            static_cast<int>(s2w),
                                            static_cast<int>(s2h),
                                            3, params,
                                            WarpRectilinearMode::HALIDE_METAL,
                                            metalData.data());
        if (!haveMetal) {
            cerr << "WARN: HALIDE_METAL warp failed; metal_value cols will be 0\n";
        }
#endif

        // --- Coord-chain rebuild for ref/halide intermediates ---
        const WarpRuntimeParamsLocal rt =
            buildRuntimeParamsLocal(static_cast<int>(s2w),
                                    static_cast<int>(s2h), params);
        const TileClippingGridLocal grid =
            buildTileClippingGridLocal(static_cast<int>(s2w),
                                       static_cast<int>(s2h), 3, rt, params);

        // --- ROI computation: bottom-right 256x256, clamped to image bounds ---
        const int roi_w = 256, roi_h = 256;
        const int x0 = std::max(0, static_cast<int>(s2w) - roi_w);
        const int y0 = std::max(0, static_cast<int>(s2h) - roi_h);
        const int x1 = std::min(static_cast<int>(s2w), x0 + roi_w);
        const int y1 = std::min(static_cast<int>(s2h), y0 + roi_h);

        // --- CSV ---
        const string csvPath = outDir + "/roi_probe_samples.csv";
        std::ofstream csv(csvPath);
        if (!csv) {
            cerr << "ERROR: cannot open " << csvPath << " for write\n";
            return 1;
        }
        csv << "# ROI: x=[" << x0 << "," << x1 << ") y=[" << y0 << "," << y1
            << ") W=" << s2w << " H=" << s2h
            << " metal_available=" << (haveMetal ? 1 : 0) << "\n";
        csv << "x,y,plane,"
               "ref_src_x,ref_src_y,halide_src_x,halide_src_y,"
               "ref_floor_x,ref_floor_y,halide_floor_x,halide_floor_y,"
               "ref_base_x,ref_base_y,halide_base_x,halide_base_y,"
               "ref_frac_x_idx,ref_frac_y_idx,halide_frac_x_idx,halide_frac_y_idx,"
               "ref_clipped_x,ref_clipped_y,halide_clipped_x,halide_clipped_y,"
               "sdk_value,halide_cpu_value,halide_metal_value,"
               "diff_cpu_vs_sdk,diff_metal_vs_sdk\n";
        csv << std::setprecision(12);

        const char* planeNames[3] = {"R", "G", "B"};

        for (int y = y0; y < y1; ++y) {
            const int tile_y = std::min(y / grid.tile_height, grid.tiles_y - 1);
            for (int x = x0; x < x1; ++x) {
                const int tile_x = std::min(x / grid.tile_width, grid.tiles_x - 1);
                const int min_bx = grid.get(0, tile_x, tile_y);
                const int max_bx = grid.get(1, tile_x, tile_y);
                const int min_by = grid.get(2, tile_x, tile_y);
                const int max_by = grid.get(3, tile_x, tile_y);

                for (int c = 0; c < 3; ++c) {
                    const int plane = warpPlaneIndexLocal(c, params);
                    double sx = 0.0, sy = 0.0;
                    getSrcPixelPositionLocal(static_cast<double>(x),
                                             static_cast<double>(y),
                                             plane, rt, params, sx, sy);
                    const double sxf = std::floor(sx);
                    const double syf = std::floor(sy);
                    const int base_x = static_cast<int>(sxf) - 1;
                    const int base_y = static_cast<int>(syf) - 1;
                    const int frac_x_idx = quantizeSubsampleIndex(sx - sxf);
                    const int frac_y_idx = quantizeSubsampleIndex(sy - syf);
                    const int clipped_x = (base_x < min_bx || base_x > max_bx) ? 1 : 0;
                    const int clipped_y = (base_y < min_by || base_y > max_by) ? 1 : 0;

                    const size_t pix_idx =
                        (static_cast<size_t>(y) * s2w + x) * 3 + c;
                    const uint16_t sdk_v   = sdkData[pix_idx];
                    const uint16_t cpu_v   = cpuData[pix_idx];
                    const uint16_t metal_v = haveMetal ? metalData[pix_idx]
                                                       : static_cast<uint16_t>(0);

                    csv << x << "," << y << "," << planeNames[c] << ","
                        << sx << "," << sy << "," << sx << "," << sy << ","
                        << sxf << "," << syf << "," << sxf << "," << syf << ","
                        << base_x << "," << base_y << ","
                        << base_x << "," << base_y << ","
                        << frac_x_idx << "," << frac_y_idx << ","
                        << frac_x_idx << "," << frac_y_idx << ","
                        << clipped_x << "," << clipped_y << ","
                        << clipped_x << "," << clipped_y << ","
                        << sdk_v << "," << cpu_v << "," << metal_v << ","
                        << (static_cast<int>(cpu_v) - static_cast<int>(sdk_v)) << ","
                        << (static_cast<int>(metal_v) - static_cast<int>(sdk_v))
                        << "\n";
                }
            }
        }
        csv.close();
        std::fprintf(stderr, "[CSV] %s rows=%d\n", csvPath.c_str(),
                     (x1 - x0) * (y1 - y0) * 3);

        // --- Heatmaps (ROI-only PGM) ---
        for (int c = 0; c < 3; ++c) {
            const string base = outDir + "/roi_heatmap_cpu_vs_sdk_";
            std::ostringstream p1;
            p1 << base << planeNames[c] << ".pgm";
            writeRoiHeatmapPgm(sdkData, cpuData, static_cast<int>(s2w),
                               static_cast<int>(s2h), c,
                               x0, y0, x1, y1, p1.str());
        }
        if (haveMetal) {
            for (int c = 0; c < 3; ++c) {
                const string base = outDir + "/roi_heatmap_metal_vs_sdk_";
                std::ostringstream p1;
                p1 << base << planeNames[c] << ".pgm";
                writeRoiHeatmapPgm(sdkData, metalData, static_cast<int>(s2w),
                                   static_cast<int>(s2h), c,
                                   x0, y0, x1, y1, p1.str());
            }
        }

        // --- Summary stats over ROI ---
        cout << std::fixed << std::setprecision(4);
        cout << "ROI: x=[" << x0 << "," << x1 << ") y=[" << y0 << "," << y1
             << ") size=" << (x1 - x0) << "x" << (y1 - y0) << "\n";
        for (int c = 0; c < 3; ++c) {
            long long sumCpu = 0, sumMetal = 0;
            int maxCpu = 0, maxMetal = 0;
            const long long count = static_cast<long long>(x1 - x0) * (y1 - y0);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const size_t idx = (static_cast<size_t>(y) * s2w + x) * 3 + c;
                    const int dCpu = std::abs(static_cast<int>(cpuData[idx]) -
                                              static_cast<int>(sdkData[idx]));
                    const int dMetal = haveMetal
                        ? std::abs(static_cast<int>(metalData[idx]) -
                                   static_cast<int>(sdkData[idx]))
                        : 0;
                    sumCpu += dCpu;   if (dCpu > maxCpu) maxCpu = dCpu;
                    sumMetal += dMetal; if (dMetal > maxMetal) maxMetal = dMetal;
                }
            }
            cout << "  " << planeNames[c]
                 << " cpu_vs_sdk: mean=" << (double)sumCpu / count
                 << " max=" << maxCpu
                 << "  metal_vs_sdk: mean=" << (double)sumMetal / count
                 << " max=" << maxMetal << "\n";
        }
        return 0;
    } catch (const dng_exception& e) {
        cerr << "DNG Exception: " << e.ErrorCode() << "\n";
    } catch (const std::exception& e) {
        cerr << "Exception: " << e.what() << "\n";
    }
    return 1;
}
