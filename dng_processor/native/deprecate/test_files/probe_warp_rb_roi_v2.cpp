// Phase 8.1.6 Stage E B2-V2 — V2 probe binary.
//
// Mirrors probe_warp_rb_roi.cpp but instead of dumping host-double "ref"
// values into the halide_* CSV columns, this v2 binary calls two AOT debug
// kernels that expose the *true* per-pixel intermediates (src_x/src_y/
// base_x/base_y/frac_x_idx/frac_y_idx/clipped_x/clipped_y) computed inside
// the Halide pipeline:
//   * dng_warp_debug_cpu (target = host)            -> cpu_halide_*
//   * dng_warp_debug_metal (target = host-metal-..) -> metal_halide_*
//
// CSV schema follows v2_probe_design.md § 4 (34 columns). PGM heatmaps
// follow v1 naming with `_v2` suffix and add a cpu-vs-metal trio.
//
// Coexists with v1: this binary writes `roi_probe_samples_v2.csv` and
// `roi_heatmap_*_v2.pgm`. v1 outputs are untouched.

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

#include "HalideBuffer.h"

// AOT C entry points emitted by add_halide_library (B3-A wiring).
#include "dng_warp_debug_cpu.h"
#if defined(__APPLE__)
#include "dng_warp_debug_metal.h"
#include "HalideRuntimeMetal.h"
#endif

using std::cerr;
using std::cout;
using std::string;
using std::vector;
using Halide::Runtime::Buffer;

namespace {

// ---------------------------------------------------------------------------
// Host-double reference coord chain — copied verbatim from v1 probe so the
// CSV `ref_*` columns remain comparable across v1/v2 runs.
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
// Image utilities (mirror v1 probe).
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

// ---------------------------------------------------------------------------
// Debug AOT invocation helpers.
//
// Buffer layouts must match the generator:
//   * src/dst : interleaved RGB (stride x = 3, c-extent = 3, c-stride = 1)
//   * dbg_*   : planar (W, H, 3) — Halide default planar layout
// ---------------------------------------------------------------------------

struct DebugBuffers {
    vector<uint16_t> dst;
    vector<float>    src_x;
    vector<float>    src_y;
    vector<int32_t>  base_x;
    vector<int32_t>  base_y;
    vector<int32_t>  frac_x_idx;
    vector<int32_t>  frac_y_idx;
    vector<uint8_t>  clipped_x;
    vector<uint8_t>  clipped_y;

    void resize(size_t pixels) {
        // dst is interleaved (3 planes packed); dbg are planar (W*H per plane).
        dst.assign(pixels * 3, 0);
        src_x.assign(pixels * 3, 0.0f);
        src_y.assign(pixels * 3, 0.0f);
        base_x.assign(pixels * 3, 0);
        base_y.assign(pixels * 3, 0);
        frac_x_idx.assign(pixels * 3, 0);
        frac_y_idx.assign(pixels * 3, 0);
        clipped_x.assign(pixels * 3, 0);
        clipped_y.assign(pixels * 3, 0);
    }
};

// Build interleaved Buffer<uint16_t> wrapping caller storage.
template <typename T>
Buffer<T> makeInterleaved(T* data, int width, int height, int planes) {
    return Buffer<T>::make_interleaved(data, width, height, planes);
}

// Build planar Buffer<T> (W, H, 3).
template <typename T>
Buffer<T> makePlanar3(T* data, int width, int height) {
    return Buffer<T>(data, width, height, 3);
}

bool runDebugCpu(const uint16_t* src_rgb,
                 int width, int height,
                 const WarpRectilinearParams& params,
                 const WarpRuntimeParamsLocal& rt,
                 const TileClippingGridLocal& grid,
                 DebugBuffers& out) {
    out.resize(static_cast<size_t>(width) * height);

    Buffer<uint16_t> src_buf = makeInterleaved(const_cast<uint16_t*>(src_rgb), width, height, 3);
    Buffer<uint16_t> dst_buf = makeInterleaved(out.dst.data(), width, height, 3);

    const int plane_count = static_cast<int>(std::max<uint32_t>(1, params.planes));
    vector<float> rad_storage(static_cast<size_t>(4 * plane_count));
    vector<float> tan_storage(static_cast<size_t>(2 * plane_count));
    for (int p = 0; p < plane_count; ++p) {
        for (int i = 0; i < 4; ++i) rad_storage[p * 4 + i] = params.rad_params[p][i];
        for (int i = 0; i < 2; ++i) tan_storage[p * 2 + i] = params.tan_params[p][i];
    }
    Buffer<float> rad_buf(rad_storage.data(), 4, plane_count);
    Buffer<float> tan_buf(tan_storage.data(), 2, plane_count);
    Buffer<int32_t> tile_bounds_buf(const_cast<int32_t*>(grid.bounds.data()),
                                    4, grid.tiles_x, grid.tiles_y);

    Buffer<float>   dbg_sx_buf  = makePlanar3<float>(out.src_x.data(),      width, height);
    Buffer<float>   dbg_sy_buf  = makePlanar3<float>(out.src_y.data(),      width, height);
    Buffer<int32_t> dbg_bx_buf  = makePlanar3<int32_t>(out.base_x.data(),   width, height);
    Buffer<int32_t> dbg_by_buf  = makePlanar3<int32_t>(out.base_y.data(),   width, height);
    Buffer<int32_t> dbg_fx_buf  = makePlanar3<int32_t>(out.frac_x_idx.data(), width, height);
    Buffer<int32_t> dbg_fy_buf  = makePlanar3<int32_t>(out.frac_y_idx.data(), width, height);
    Buffer<uint8_t> dbg_cx_buf  = makePlanar3<uint8_t>(out.clipped_x.data(), width, height);
    Buffer<uint8_t> dbg_cy_buf  = makePlanar3<uint8_t>(out.clipped_y.data(), width, height);

    int r = dng_warp_debug_cpu(src_buf, rad_buf, tan_buf, tile_bounds_buf,
                               static_cast<int32_t>(params.planes),
                               static_cast<float>(rt.center_x),
                               static_cast<float>(rt.center_y),
                               static_cast<float>(rt.norm_radius),
                               static_cast<float>(rt.inv_norm_radius),
                               static_cast<float>(rt.pixel_scale_v),
                               static_cast<float>(rt.pixel_scale_v_inv),
                               params.is_rad_nop_all ? 1 : 0,
                               params.is_tan_nop_all ? 1 : 0,
                               static_cast<int32_t>(grid.tile_width),
                               static_cast<int32_t>(grid.tile_height),
                               dst_buf,
                               dbg_sx_buf, dbg_sy_buf,
                               dbg_bx_buf, dbg_by_buf,
                               dbg_fx_buf, dbg_fy_buf,
                               dbg_cx_buf, dbg_cy_buf);
    if (r != 0) {
        cerr << "ERROR: dng_warp_debug_cpu returned " << r << "\n";
        return false;
    }
    return true;
}

#if defined(__APPLE__)
// Metal path: dst + 8 dbg buffers must each be device_malloc'd before launch
// and copy_to_host'd after. Helpers wrap that.
template <typename T>
bool prepDeviceOutput(Buffer<T>& buf, const halide_device_interface_t* iface) {
    buf.set_host_dirty(false);
    int r = buf.device_malloc(iface);
    if (r != 0) {
        cerr << "ERROR: device_malloc failed (" << r << ")\n";
        return false;
    }
    return true;
}

template <typename T>
bool readbackDevice(Buffer<T>& buf) {
    int r = buf.copy_to_host();
    if (r != 0) {
        cerr << "ERROR: copy_to_host failed (" << r << ")\n";
        return false;
    }
    return true;
}

bool runDebugMetal(const uint16_t* src_rgb,
                   int width, int height,
                   const WarpRectilinearParams& params,
                   const WarpRuntimeParamsLocal& rt,
                   const TileClippingGridLocal& grid,
                   DebugBuffers& out) {
    out.resize(static_cast<size_t>(width) * height);

    Buffer<uint16_t> src_buf = makeInterleaved(const_cast<uint16_t*>(src_rgb), width, height, 3);
    Buffer<uint16_t> dst_buf = makeInterleaved(out.dst.data(), width, height, 3);

    const int plane_count = static_cast<int>(std::max<uint32_t>(1, params.planes));
    vector<float> rad_storage(static_cast<size_t>(4 * plane_count));
    vector<float> tan_storage(static_cast<size_t>(2 * plane_count));
    for (int p = 0; p < plane_count; ++p) {
        for (int i = 0; i < 4; ++i) rad_storage[p * 4 + i] = params.rad_params[p][i];
        for (int i = 0; i < 2; ++i) tan_storage[p * 2 + i] = params.tan_params[p][i];
    }
    Buffer<float> rad_buf(rad_storage.data(), 4, plane_count);
    Buffer<float> tan_buf(tan_storage.data(), 2, plane_count);
    Buffer<int32_t> tile_bounds_buf(const_cast<int32_t*>(grid.bounds.data()),
                                    4, grid.tiles_x, grid.tiles_y);

    Buffer<float>   dbg_sx_buf  = makePlanar3<float>(out.src_x.data(),      width, height);
    Buffer<float>   dbg_sy_buf  = makePlanar3<float>(out.src_y.data(),      width, height);
    Buffer<int32_t> dbg_bx_buf  = makePlanar3<int32_t>(out.base_x.data(),   width, height);
    Buffer<int32_t> dbg_by_buf  = makePlanar3<int32_t>(out.base_y.data(),   width, height);
    Buffer<int32_t> dbg_fx_buf  = makePlanar3<int32_t>(out.frac_x_idx.data(), width, height);
    Buffer<int32_t> dbg_fy_buf  = makePlanar3<int32_t>(out.frac_y_idx.data(), width, height);
    Buffer<uint8_t> dbg_cx_buf  = makePlanar3<uint8_t>(out.clipped_x.data(), width, height);
    Buffer<uint8_t> dbg_cy_buf  = makePlanar3<uint8_t>(out.clipped_y.data(), width, height);

    src_buf.set_host_dirty();
    rad_buf.set_host_dirty();
    tan_buf.set_host_dirty();
    tile_bounds_buf.set_host_dirty();

    const halide_device_interface_t* metal_iface = halide_metal_device_interface();
    if (!metal_iface) {
        cerr << "ERROR: halide_metal_device_interface() returned null\n";
        return false;
    }
    if (!prepDeviceOutput(dst_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_sx_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_sy_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_bx_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_by_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_fx_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_fy_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_cx_buf, metal_iface)) return false;
    if (!prepDeviceOutput(dbg_cy_buf, metal_iface)) return false;

    int r = dng_warp_debug_metal(src_buf, rad_buf, tan_buf, tile_bounds_buf,
                                 static_cast<int32_t>(params.planes),
                                 static_cast<float>(rt.center_x),
                                 static_cast<float>(rt.center_y),
                                 static_cast<float>(rt.norm_radius),
                                 static_cast<float>(rt.inv_norm_radius),
                                 static_cast<float>(rt.pixel_scale_v),
                                 static_cast<float>(rt.pixel_scale_v_inv),
                                 params.is_rad_nop_all ? 1 : 0,
                                 params.is_tan_nop_all ? 1 : 0,
                                 static_cast<int32_t>(grid.tile_width),
                                 static_cast<int32_t>(grid.tile_height),
                                 dst_buf,
                                 dbg_sx_buf, dbg_sy_buf,
                                 dbg_bx_buf, dbg_by_buf,
                                 dbg_fx_buf, dbg_fy_buf,
                                 dbg_cx_buf, dbg_cy_buf);
    if (r != 0) {
        cerr << "ERROR: dng_warp_debug_metal returned " << r << "\n";
        return false;
    }

    if (!readbackDevice(dst_buf)) return false;
    if (!readbackDevice(dbg_sx_buf)) return false;
    if (!readbackDevice(dbg_sy_buf)) return false;
    if (!readbackDevice(dbg_bx_buf)) return false;
    if (!readbackDevice(dbg_by_buf)) return false;
    if (!readbackDevice(dbg_fx_buf)) return false;
    if (!readbackDevice(dbg_fy_buf)) return false;
    if (!readbackDevice(dbg_cx_buf)) return false;
    if (!readbackDevice(dbg_cy_buf)) return false;
    return true;
}
#endif  // __APPLE__

// Index helpers.
//   interleaved RGB (dst): (y * W + x) * 3 + c
//   planar (dbg)         : c * (W * H) + y * W + x
inline size_t interleavedIdx(int x, int y, int c, int W) {
    return (static_cast<size_t>(y) * W + x) * 3 + c;
}
inline size_t planarIdx(int x, int y, int c, int W, int H) {
    return static_cast<size_t>(c) * W * H + static_cast<size_t>(y) * W + x;
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

        // --- SDK reference ---
        AutoPtr<dng_image> sdkImage = makeImageFromInterleaved(host, s2w, s2h, 3, preOpcode3);
        const_cast<dng_opcode_WarpRectilinear*>(warpOpcode)
            ->Apply(host, *negative, sdkImage);
        uint32_t rW = 0, rH = 0, rP = 0;
        vector<uint16_t> sdkData;
        extractImageData(sdkImage.Get(), sdkData, rW, rH, rP);

        // --- Halide CPU debug AOT (real per-pixel intermediates) ---
        const WarpRuntimeParamsLocal rt =
            buildRuntimeParamsLocal(static_cast<int>(s2w),
                                    static_cast<int>(s2h), params);
        const TileClippingGridLocal grid =
            buildTileClippingGridLocal(static_cast<int>(s2w),
                                       static_cast<int>(s2h), 3, rt, params);

        DebugBuffers cpuDbg;
        const auto t_cpu0 = std::chrono::high_resolution_clock::now();
        if (!runDebugCpu(preOpcode3.data(),
                         static_cast<int>(s2w), static_cast<int>(s2h),
                         params, rt, grid, cpuDbg)) {
            return 1;
        }
        const auto t_cpu1 = std::chrono::high_resolution_clock::now();
        std::fprintf(stderr, "[DebugCPU] elapsed=%.1f ms\n",
                     std::chrono::duration_cast<std::chrono::microseconds>(t_cpu1 - t_cpu0).count() / 1000.0);

        // --- Halide Metal debug AOT (Apple only) ---
        bool haveMetal = false;
        DebugBuffers metalDbg;
#if defined(__APPLE__)
        const auto t_m0 = std::chrono::high_resolution_clock::now();
        haveMetal = runDebugMetal(preOpcode3.data(),
                                  static_cast<int>(s2w), static_cast<int>(s2h),
                                  params, rt, grid, metalDbg);
        const auto t_m1 = std::chrono::high_resolution_clock::now();
        if (haveMetal) {
            std::fprintf(stderr, "[DebugMetal] elapsed=%.1f ms\n",
                         std::chrono::duration_cast<std::chrono::microseconds>(t_m1 - t_m0).count() / 1000.0);
        } else {
            cerr << "WARN: Metal debug AOT failed; metal_halide_* cols will be 0\n";
            metalDbg.resize(static_cast<size_t>(s2w) * s2h);  // zero-fill
        }
#else
        metalDbg.resize(static_cast<size_t>(s2w) * s2h);
#endif

        // --- ROI: bottom-right 256x256 ---
        const int roi_w = 256, roi_h = 256;
        const int x0 = std::max(0, static_cast<int>(s2w) - roi_w);
        const int y0 = std::max(0, static_cast<int>(s2h) - roi_h);
        const int x1 = std::min(static_cast<int>(s2w), x0 + roi_w);
        const int y1 = std::min(static_cast<int>(s2h), y0 + roi_h);

        // --- CSV (v2 schema, 34 cols) ---
        const string csvPath = outDir + "/roi_probe_samples_v2.csv";
        std::ofstream csv(csvPath);
        if (!csv) {
            cerr << "ERROR: cannot open " << csvPath << " for write\n";
            return 1;
        }
        csv << "# v2 ROI: x=[" << x0 << "," << x1 << ") y=[" << y0 << "," << y1
            << ") W=" << s2w << " H=" << s2h
            << " metal_available=" << (haveMetal ? 1 : 0) << "\n";
        csv << "x,y,plane,"
               "ref_src_x,ref_src_y,ref_base_x,ref_base_y,"
               "ref_frac_x_idx,ref_frac_y_idx,ref_clipped_x,ref_clipped_y,"
               "cpu_halide_src_x,cpu_halide_src_y,cpu_halide_base_x,cpu_halide_base_y,"
               "cpu_halide_frac_x_idx,cpu_halide_frac_y_idx,cpu_halide_clipped_x,cpu_halide_clipped_y,"
               "metal_halide_src_x,metal_halide_src_y,metal_halide_base_x,metal_halide_base_y,"
               "metal_halide_frac_x_idx,metal_halide_frac_y_idx,metal_halide_clipped_x,metal_halide_clipped_y,"
               "sdk_value,cpu_value,metal_value,"
               "diff_cpu_vs_sdk,diff_metal_vs_sdk,"
               "diff_metal_vs_cpu_src_x,diff_metal_vs_cpu_src_y\n";
        csv << std::setprecision(9);

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

                    // Host-double reference.
                    double sx = 0.0, sy = 0.0;
                    getSrcPixelPositionLocal(static_cast<double>(x),
                                             static_cast<double>(y),
                                             plane, rt, params, sx, sy);
                    const double sxf = std::floor(sx);
                    const double syf = std::floor(sy);
                    const int ref_base_x = static_cast<int>(sxf) - 1;
                    const int ref_base_y = static_cast<int>(syf) - 1;
                    const int ref_fx_idx = quantizeSubsampleIndex(sx - sxf);
                    const int ref_fy_idx = quantizeSubsampleIndex(sy - syf);
                    const int ref_clip_x = (ref_base_x < min_bx || ref_base_x > max_bx) ? 1 : 0;
                    const int ref_clip_y = (ref_base_y < min_by || ref_base_y > max_by) ? 1 : 0;

                    const size_t pix_inter = interleavedIdx(x, y, c, static_cast<int>(s2w));
                    const size_t pix_planar =
                        planarIdx(x, y, c, static_cast<int>(s2w), static_cast<int>(s2h));

                    const float cpu_sx = cpuDbg.src_x[pix_planar];
                    const float cpu_sy = cpuDbg.src_y[pix_planar];
                    const int32_t cpu_bx = cpuDbg.base_x[pix_planar];
                    const int32_t cpu_by = cpuDbg.base_y[pix_planar];
                    const int32_t cpu_fx = cpuDbg.frac_x_idx[pix_planar];
                    const int32_t cpu_fy = cpuDbg.frac_y_idx[pix_planar];
                    const int     cpu_cx = cpuDbg.clipped_x[pix_planar];
                    const int     cpu_cy = cpuDbg.clipped_y[pix_planar];

                    const float met_sx = haveMetal ? metalDbg.src_x[pix_planar] : 0.0f;
                    const float met_sy = haveMetal ? metalDbg.src_y[pix_planar] : 0.0f;
                    const int32_t met_bx = haveMetal ? metalDbg.base_x[pix_planar] : 0;
                    const int32_t met_by = haveMetal ? metalDbg.base_y[pix_planar] : 0;
                    const int32_t met_fx = haveMetal ? metalDbg.frac_x_idx[pix_planar] : 0;
                    const int32_t met_fy = haveMetal ? metalDbg.frac_y_idx[pix_planar] : 0;
                    const int     met_cx = haveMetal ? metalDbg.clipped_x[pix_planar] : 0;
                    const int     met_cy = haveMetal ? metalDbg.clipped_y[pix_planar] : 0;

                    const uint16_t sdk_v = sdkData[pix_inter];
                    const uint16_t cpu_v = cpuDbg.dst[pix_inter];
                    const uint16_t met_v = haveMetal ? metalDbg.dst[pix_inter]
                                                     : static_cast<uint16_t>(0);

                    const float dmc_sx = haveMetal ? (met_sx - cpu_sx) : 0.0f;
                    const float dmc_sy = haveMetal ? (met_sy - cpu_sy) : 0.0f;

                    csv << x << "," << y << "," << planeNames[c] << ","
                        << sx << "," << sy << ","
                        << ref_base_x << "," << ref_base_y << ","
                        << ref_fx_idx << "," << ref_fy_idx << ","
                        << ref_clip_x << "," << ref_clip_y << ","
                        << cpu_sx << "," << cpu_sy << ","
                        << cpu_bx << "," << cpu_by << ","
                        << cpu_fx << "," << cpu_fy << ","
                        << cpu_cx << "," << cpu_cy << ","
                        << met_sx << "," << met_sy << ","
                        << met_bx << "," << met_by << ","
                        << met_fx << "," << met_fy << ","
                        << met_cx << "," << met_cy << ","
                        << sdk_v << "," << cpu_v << "," << met_v << ","
                        << (static_cast<int>(cpu_v) - static_cast<int>(sdk_v)) << ","
                        << (static_cast<int>(met_v) - static_cast<int>(sdk_v)) << ","
                        << dmc_sx << "," << dmc_sy
                        << "\n";
                }
            }
        }
        csv.close();
        std::fprintf(stderr, "[CSV] %s rows=%d\n", csvPath.c_str(),
                     (x1 - x0) * (y1 - y0) * 3);

        // --- Heatmaps (ROI-only PGM, _v2 suffix) ---
        for (int c = 0; c < 3; ++c) {
            std::ostringstream p;
            p << outDir << "/roi_heatmap_cpu_vs_sdk_" << planeNames[c] << "_v2.pgm";
            writeRoiHeatmapPgm(sdkData, cpuDbg.dst, static_cast<int>(s2w),
                               static_cast<int>(s2h), c,
                               x0, y0, x1, y1, p.str());
        }
        if (haveMetal) {
            for (int c = 0; c < 3; ++c) {
                std::ostringstream p;
                p << outDir << "/roi_heatmap_metal_vs_sdk_" << planeNames[c] << "_v2.pgm";
                writeRoiHeatmapPgm(sdkData, metalDbg.dst, static_cast<int>(s2w),
                                   static_cast<int>(s2h), c,
                                   x0, y0, x1, y1, p.str());
            }
            // Nice-to-have (design § 3.4): cpu vs metal trio.
            for (int c = 0; c < 3; ++c) {
                std::ostringstream p;
                p << outDir << "/roi_heatmap_cpu_vs_metal_" << planeNames[c] << "_v2.pgm";
                writeRoiHeatmapPgm(cpuDbg.dst, metalDbg.dst, static_cast<int>(s2w),
                                   static_cast<int>(s2h), c,
                                   x0, y0, x1, y1, p.str());
            }
        }

        // --- Summary stats ---
        cout << std::fixed << std::setprecision(4);
        cout << "v2 ROI: x=[" << x0 << "," << x1 << ") y=[" << y0 << "," << y1
             << ") size=" << (x1 - x0) << "x" << (y1 - y0)
             << " metal=" << (haveMetal ? "yes" : "no") << "\n";
        for (int c = 0; c < 3; ++c) {
            long long sumCpu = 0, sumMetal = 0;
            int maxCpu = 0, maxMetal = 0;
            const long long count = static_cast<long long>(x1 - x0) * (y1 - y0);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const size_t idx = interleavedIdx(x, y, c, static_cast<int>(s2w));
                    const int dCpu = std::abs(static_cast<int>(cpuDbg.dst[idx]) -
                                              static_cast<int>(sdkData[idx]));
                    const int dMet = haveMetal
                        ? std::abs(static_cast<int>(metalDbg.dst[idx]) -
                                   static_cast<int>(sdkData[idx]))
                        : 0;
                    sumCpu += dCpu;   if (dCpu > maxCpu) maxCpu = dCpu;
                    sumMetal += dMet; if (dMet > maxMetal) maxMetal = dMet;
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
