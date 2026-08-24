#include "raw_demosaic_reference.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "HalideBuffer.h"
#include "raw_bayer_demosaic.h"
#include "raw_xtrans_demosaic.h"

namespace {

// Mirrors dng_halide_utils.h::map_repeat_coord with repeat 2: out-of-range
// coordinates wrap onto a same-colour CFA site.
int mapRepeatCoord(int coord, int size) {
    const int repeat = std::min(2, size);
    if (coord < 0) return ((coord % repeat) + repeat) % repeat;
    if (coord >= size) {
        const int start = size - repeat;
        return start + (((coord - start) % repeat) + repeat) % repeat;
    }
    return coord;
}

// Mirrors dng_halide_utils.h::map_repeat_coord_n: the X-Trans generalisation
// of the rule above, with repeat 6 so an out-of-range coordinate lands on a
// site of the same 6x6 phase.
int mapRepeatCoordN(int coord, int size, int repeat_n) {
    const int repeat = std::min(repeat_n, size);
    if (coord < 0) return ((coord % repeat) + repeat) % repeat;
    if (coord >= size) {
        const int start = size - repeat;
        return start + (((coord - start) % repeat) + repeat) % repeat;
    }
    return coord;
}

// Same rounding constants as avg2_u16 / avg4_u16 in dng_halide_utils.h.
uint16_t avg2(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>((static_cast<uint32_t>(a) + b + 1u) >> 1);
}

uint16_t avg4(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    return static_cast<uint16_t>(
        (static_cast<uint32_t>(a) + b + c + d + 2u) >> 2);
}

}  // namespace

void raw_bayer_demosaic_reference(const uint16_t* src, uint32_t width,
                                  uint32_t height, int64_t row_stride_bytes,
                                  int32_t red_x, int32_t red_y,
                                  const float* black_tile,
                                  uint32_t black_repeat_width,
                                  uint32_t black_repeat_height,
                                  float inv_range, uint16_t* dst) {
    const size_t stride_px = static_cast<size_t>(row_stride_bytes) / 2;
    const uint32_t bw = black_repeat_width ? black_repeat_width : 1;
    const uint32_t bh = black_repeat_height ? black_repeat_height : 1;

    auto sample = [&](int sx, int sy) -> uint16_t {
        const int mx = mapRepeatCoord(sx, static_cast<int>(width));
        const int my = mapRepeatCoord(sy, static_cast<int>(height));
        const float level = black_tile[(my % static_cast<int>(bh)) * bw +
                                       (mx % static_cast<int>(bw))];
        const float norm =
            (static_cast<float>(src[static_cast<size_t>(my) * stride_px + mx]) - level) *
            inv_range;
        const float clamped = norm < 0.0f ? 0.0f : (norm > 65535.0f ? 65535.0f : norm);
        return static_cast<uint16_t>(clamped);
    };

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const int ix = static_cast<int>(x), iy = static_cast<int>(y);
            const uint16_t center = sample(ix, iy);
            const uint16_t n  = sample(ix, iy - 1), s  = sample(ix, iy + 1);
            const uint16_t w  = sample(ix - 1, iy), e  = sample(ix + 1, iy);
            const uint16_t nw = sample(ix - 1, iy - 1), ne = sample(ix + 1, iy - 1);
            const uint16_t sw = sample(ix - 1, iy + 1), se = sample(ix + 1, iy + 1);

            const bool red_row = (iy % 2) == red_y;
            const bool red_col = (ix % 2) == red_x;
            const bool red_site = red_row && red_col;
            const bool blue_site = !red_row && !red_col;
            const bool green_on_red_row = red_row && !red_col;

            const uint16_t r = red_site ? center
                             : blue_site ? avg4(nw, ne, sw, se)
                             : green_on_red_row ? avg2(w, e)
                                                : avg2(n, s);
            const uint16_t g = (red_site || blue_site) ? avg4(n, s, w, e) : center;
            const uint16_t b = red_site ? avg4(nw, ne, sw, se)
                             : blue_site ? center
                             : green_on_red_row ? avg2(n, s)
                                                : avg2(w, e);

            const size_t base = (static_cast<size_t>(y) * width + x) * 3;
            dst[base + 0] = r;
            dst[base + 1] = g;
            dst[base + 2] = b;
        }
    }
}

int raw_bayer_demosaic_aot(const uint16_t* src, uint32_t width, uint32_t height,
                           int64_t row_stride_bytes, int32_t red_x, int32_t red_y,
                           const float* black_tile, uint32_t black_repeat_width,
                           uint32_t black_repeat_height, float inv_range,
                           uint16_t* dst) {
    if (!src || !dst || !black_tile || width == 0 || height == 0) return 0;

    const uint32_t bw = black_repeat_width ? black_repeat_width : 1;
    const uint32_t bh = black_repeat_height ? black_repeat_height : 1;

    // Stride-aware wrap: no host repack even when raw_pitch > width*2
    // (spec section 5.2.1).
    halide_dimension_t src_dims[2] = {
        {0, static_cast<int32_t>(width), 1, 0},
        {0, static_cast<int32_t>(height),
         static_cast<int32_t>(row_stride_bytes / 2), 0}};
    Halide::Runtime::Buffer<const uint16_t> src_buf(src, 2, src_dims);

    Halide::Runtime::Buffer<const float> black_buf(black_tile,
                                                   static_cast<int>(bw),
                                                   static_cast<int>(bh));

    halide_dimension_t dst_dims[3] = {
        {0, static_cast<int32_t>(width), 3, 0},
        {0, static_cast<int32_t>(height), static_cast<int32_t>(width) * 3, 0},
        {0, 3, 1, 0}};
    Halide::Runtime::Buffer<uint16_t> dst_buf(dst, 3, dst_dims);

    // GPU targets: the runtime only uploads an input buffer whose host_dirty
    // flag is set, so without these the kernel reads freshly device-malloc'd
    // (uninitialised) memory. Same handshake as runDemosaicBilinearAot in
    // src/dng_mosaic_halide.cpp:103-115.
    src_buf.set_host_dirty();
    black_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int rc = raw_bayer_demosaic(src_buf, red_x, red_y, black_buf,
                                      inv_range, dst_buf);
    if (rc != 0) return 0;
    if (dst_buf.copy_to_host() != 0) return 0;
    return 1;
}

void raw_xtrans_demosaic_reference(const uint16_t* src, uint32_t width,
                                   uint32_t height, int64_t row_stride_bytes,
                                   const int32_t* cfa6x6,
                                   const float* black_tile,
                                   uint32_t black_repeat_width,
                                   uint32_t black_repeat_height,
                                   float inv_range, uint16_t* dst) {
    const size_t stride_px = static_cast<size_t>(row_stride_bytes) / 2;
    const int w = static_cast<int>(width);
    const int h = static_cast<int>(height);
    const uint32_t bw = black_repeat_width ? black_repeat_width : 1;
    const uint32_t bh = black_repeat_height ? black_repeat_height : 1;

    auto norm = [&](int sx, int sy) -> float {
        const int mx = mapRepeatCoordN(sx, w, 6);
        const int my = mapRepeatCoordN(sy, h, 6);
        const float level = black_tile[(my % static_cast<int>(bh)) * bw +
                                       (mx % static_cast<int>(bw))];
        const float v =
            (static_cast<float>(src[static_cast<size_t>(my) * stride_px + mx]) - level) *
            inv_range;
        return v < 0.0f ? 0.0f : (v > 65535.0f ? 65535.0f : v);
    };

    auto key_at = [&](int sx, int sy) -> int32_t {
        const int mx = mapRepeatCoordN(sx, w, 6);
        const int my = mapRepeatCoordN(sy, h, 6);
        return cfa6x6[(my % 6) * 6 + (mx % 6)];
    };

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int32_t own = key_at(x, y);
            const float center = norm(x, y);
            for (int c = 0; c < 3; ++c) {
                float value;
                if (own == c) {
                    value = center;
                } else {
                    float weighted = 0.0f, weight = 0.0f;
                    for (int dy = -2; dy <= 2; ++dy) {
                        for (int dx = -2; dx <= 2; ++dx) {
                            if (key_at(x + dx, y + dy) != c) continue;
                            const float wgt =
                                1.0f / (1.0f + static_cast<float>(dx * dx + dy * dy));
                            weighted += wgt * norm(x + dx, y + dy);
                            weight += wgt;
                        }
                    }
                    value = weighted / (weight > 1e-6f ? weight : 1e-6f);
                }
                const float rounded = std::floor(value + 0.5f);
                const float clamped = rounded < 0.0f ? 0.0f
                                    : (rounded > 65535.0f ? 65535.0f : rounded);
                dst[(static_cast<size_t>(y) * width + x) * 3 + c] =
                    static_cast<uint16_t>(clamped);
            }
        }
    }
}

int raw_xtrans_demosaic_aot(const uint16_t* src, uint32_t width, uint32_t height,
                            int64_t row_stride_bytes, const int32_t* cfa6x6,
                            const float* black_tile, uint32_t black_repeat_width,
                            uint32_t black_repeat_height, float inv_range,
                            uint16_t* dst) {
    if (!src || !dst || !cfa6x6 || !black_tile || width == 0 || height == 0) return 0;

    const uint32_t bw = black_repeat_width ? black_repeat_width : 1;
    const uint32_t bh = black_repeat_height ? black_repeat_height : 1;

    // Stride-aware wrap: no host repack even when raw_pitch > width*2
    // (spec section 5.2.1).
    halide_dimension_t src_dims[2] = {
        {0, static_cast<int32_t>(width), 1, 0},
        {0, static_cast<int32_t>(height),
         static_cast<int32_t>(row_stride_bytes / 2), 0}};
    Halide::Runtime::Buffer<const uint16_t> src_buf(src, 2, src_dims);
    Halide::Runtime::Buffer<const int32_t> cfa_buf(cfa6x6, 6, 6);
    Halide::Runtime::Buffer<const float> black_buf(black_tile,
                                                   static_cast<int>(bw),
                                                   static_cast<int>(bh));

    halide_dimension_t dst_dims[3] = {
        {0, static_cast<int32_t>(width), 3, 0},
        {0, static_cast<int32_t>(height), static_cast<int32_t>(width) * 3, 0},
        {0, 3, 1, 0}};
    Halide::Runtime::Buffer<uint16_t> dst_buf(dst, 3, dst_dims);

    // GPU targets: the runtime only uploads an input buffer whose host_dirty
    // flag is set, so without these the kernel reads freshly device-malloc'd
    // (uninitialised) memory. Same handshake as raw_bayer_demosaic_aot above.
    src_buf.set_host_dirty();
    cfa_buf.set_host_dirty();
    black_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    if (raw_xtrans_demosaic(src_buf, cfa_buf, black_buf, inv_range, dst_buf) != 0) {
        return 0;
    }
    if (dst_buf.copy_to_host() != 0) return 0;
    return 1;
}
