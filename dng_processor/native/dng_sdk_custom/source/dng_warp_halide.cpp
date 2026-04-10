#include "dng_warp_halide.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

#include "HalideBuffer.h"
#include "rectilinear_warp.h"

#include <dng_host.h>
#include <dng_lens_correction.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_rect.h>

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

WarpRuntimeParams buildRuntimeParams(int width, int height, const WarpRectilinearParams& params) {
    WarpRuntimeParams runtime;
    runtime.pixel_scale_v = 1.0 / static_cast<double>(params.pixel_aspect_ratio);
    runtime.pixel_scale_v_inv = params.pixel_aspect_ratio;
    runtime.center_x = static_cast<double>(params.center_h) * static_cast<double>(width);
    runtime.center_y = static_cast<double>(params.center_v) * static_cast<double>(height);

    const double square_height = std::floor(runtime.pixel_scale_v * static_cast<double>(height) + 0.5);
    const double square_center_x = static_cast<double>(params.center_h) * static_cast<double>(width);
    const double square_center_y = static_cast<double>(params.center_v) * square_height;
    const double max_dx = std::max(square_center_x, static_cast<double>(width) - square_center_x);
    const double max_dy = std::max(square_center_y, square_height - square_center_y);
    runtime.norm_radius = std::sqrt(max_dx * max_dx + max_dy * max_dy);
    runtime.inv_norm_radius = 1.0 / runtime.norm_radius;
    return runtime;
}

void warpRectilinearCpu(const uint16_t* src,
                        int width,
                        int height,
                        int planes,
                        const WarpRectilinearParams& params,
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
            const double diff_x = static_cast<double>(x) - runtime.center_x;
            const double diff_y = static_cast<double>(y) - runtime.center_y;
            const double diff_norm_x = diff_x * runtime.inv_norm_radius;
            const double diff_norm_y = diff_y * runtime.inv_norm_radius;
            const double diff_scaled_x = diff_norm_x;
            const double diff_scaled_y = diff_norm_y * runtime.pixel_scale_v;
            const double rr = std::min(diff_scaled_x * diff_scaled_x + diff_scaled_y * diff_scaled_y, 1.0);

            for (int c = 0; c < planes; ++c) {
                const int plane = std::min<int>(params.planes == 1 ? 0 : c, params.planes - 1);
                const float* rad = params.rad_params[plane];
                const float* tan = params.tan_params[plane];
                const double ratio = static_cast<double>(rad[0]) +
                                     rr * (static_cast<double>(rad[1]) +
                                           rr * (static_cast<double>(rad[2]) +
                                                 rr * static_cast<double>(rad[3])));

                const double tan_v =
                    static_cast<double>(tan[0]) * (rr + 2.0 * diff_scaled_y * diff_scaled_y) +
                    2.0 * static_cast<double>(tan[1]) * diff_scaled_x * diff_scaled_y;
                const double tan_h =
                    static_cast<double>(tan[1]) * (rr + 2.0 * diff_scaled_x * diff_scaled_x) +
                    2.0 * static_cast<double>(tan[0]) * diff_scaled_x * diff_scaled_y;

                const double src_x = runtime.center_x +
                                    runtime.norm_radius * (diff_norm_x * ratio + tan_h);
                const double src_y = runtime.center_y +
                                    runtime.norm_radius *
                                        (diff_norm_y * ratio + tan_v * runtime.pixel_scale_v_inv);

                const double src_x_floor = std::floor(src_x);
                const double src_y_floor = std::floor(src_y);

                int base_x = static_cast<int>(src_x_floor) - 1;
                int base_y = static_cast<int>(src_y_floor) - 1;
                int frac_x_idx = quantizeSubsampleIndex(src_x - src_x_floor);
                int frac_y_idx = quantizeSubsampleIndex(src_y - src_y_floor);

                const int min_base_x = -1;
                const int min_base_y = -1;
                const int max_base_x = width - 2;
                const int max_base_y = height - 2;

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

bool runWarpHalideAot(const uint16_t* src_interleaved_rgb,
                      int width,
                      int height,
                      int planes,
                      const WarpRectilinearParams& params,
                      uint16_t* dst_interleaved_rgb) {
    if (!src_interleaved_rgb || !dst_interleaved_rgb || width <= 0 || height <= 0 || planes <= 0) {
        return false;
    }

    const bool timing_enabled = []() {
        const char* v = std::getenv("DNG_WARP_HALIDE_TIMING");
        return v && v[0] && v[0] != '0';
    }();
    const auto t0 = std::chrono::high_resolution_clock::now();

    // Fast path: RGB interleaved input/output (common Stage3 path).
    Buffer<uint16_t> src_buf =
        Buffer<uint16_t>::make_interleaved(const_cast<uint16_t*>(src_interleaved_rgb), width, height, planes);
    Buffer<uint16_t> dst_buf =
        Buffer<uint16_t>::make_interleaved(dst_interleaved_rgb, width, height, planes);

    const int plane_count = static_cast<int>(std::max<uint32_t>(1, params.planes));
    thread_local std::vector<float> rad_storage;
    thread_local std::vector<float> tan_storage;
    rad_storage.resize(static_cast<size_t>(4 * plane_count));
    tan_storage.resize(static_cast<size_t>(2 * plane_count));

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

    // Be explicit for GPU backends: inputs are host-authored, output host is stale
    // until we copy back after kernel execution.
    src_buf.set_host_dirty();
    rad_buf.set_host_dirty();
    tan_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const auto t1 = std::chrono::high_resolution_clock::now();
    const int result = rectilinear_warp(src_buf.raw_buffer(),
                                        rad_buf.raw_buffer(),
                                        tan_buf.raw_buffer(),
                                        static_cast<int32_t>(params.planes),
                                        params.center_h,
                                        params.center_v,
                                        params.pixel_aspect_ratio,
                                        dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    const auto t2 = std::chrono::high_resolution_clock::now();
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    const auto t3 = std::chrono::high_resolution_clock::now();

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

}  // namespace

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
                                  float pixelAspectRatio,
                                  WarpRectilinearParams& params) {
    const dng_warp_params_rectilinear& warp = opcode.WarpParams();
    if (warp.fPlanes == 0 || warp.fPlanes > 4) {
        return false;
    }

    params = WarpRectilinearParams{};
    params.planes = warp.fPlanes;
    params.center_h = static_cast<float>(warp.fCenter.h);
    params.center_v = static_cast<float>(warp.fCenter.v);
    params.pixel_aspect_ratio = pixelAspectRatio;

    for (uint32_t plane = 0; plane < warp.fPlanes; ++plane) {
        for (int i = 0; i < 4; ++i) {
            params.rad_params[plane][i] = static_cast<float>(warp.fRadParams[plane][i]);
        }
        for (int i = 0; i < 2; ++i) {
            params.tan_params[plane][i] = static_cast<float>(warp.fTanParams[plane][i]);
        }
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

    if (mode == WarpRectilinearMode::HALIDE_METAL || mode == WarpRectilinearMode::AUTO) {
        if (runWarpHalideAot(src_interleaved_rgb, width, height, planes, params, dst_interleaved_rgb)) {
            return true;
        }
        if (mode == WarpRectilinearMode::HALIDE_METAL) {
            return false;
        }
    }

    warpRectilinearCpu(src_interleaved_rgb, width, height, planes, params, dst_interleaved_rgb);
    return true;
}

bool apply_warp_rectilinear_to_image(dng_host& host,
                                     dng_negative& negative,
                                     const dng_opcode_WarpRectilinear& opcode,
                                     AutoPtr<dng_image>& image,
                                     WarpRectilinearMode mode) {
    (void) host;
    if (mode == WarpRectilinearMode::SDK) {
        return false;
    }

    const bool timing_enabled = []() {
        const char* v = std::getenv("DNG_WARP_HALIDE_TIMING");
        return v && v[0] && v[0] != '0';
    }();
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
                                      static_cast<float>(negative.PixelAspectRatio()),
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
