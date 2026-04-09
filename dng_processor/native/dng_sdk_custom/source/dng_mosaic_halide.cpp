/**
 * dng_mosaic_halide.cpp
 *
 * Phase 5.3 - Bilinear demosaic aligned with DNG SDK Stage3 layout.
 *
 * Notes:
 * - Uses RGGB Bayer pattern expected by current sample set.
 * - Uses edge-clamp boundary behavior.
 * - Writes interleaved RGB output: RGBRGB...
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace {

inline int clampCoord(int v, int lo, int hi) {
    return std::min(std::max(v, lo), hi);
}

inline uint16_t sampleClamp(const uint16_t* input, int width, int height, int x, int y) {
    const int cx = clampCoord(x, 0, width - 1);
    const int cy = clampCoord(y, 0, height - 1);
    return input[cy * width + cx];
}

inline uint16_t avg2(uint16_t a, uint16_t b) {
    return static_cast<uint16_t>((static_cast<uint32_t>(a) + static_cast<uint32_t>(b) + 1u) >> 1);
}

inline uint16_t avg4(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    const uint32_t total = static_cast<uint32_t>(a) + static_cast<uint32_t>(b) +
                           static_cast<uint32_t>(c) + static_cast<uint32_t>(d);
    return static_cast<uint16_t>((total + 2u) >> 2);
}

}  // namespace

extern "C" void demosaic_pattern_bilinear(const uint16_t* input,
                                          int width,
                                          int height,
                                          uint16_t* output) {
    if (!input || !output || width <= 0 || height <= 0) {
        return;
    }

    // RGGB layout:
    //   row even: R G R G ...
    //   row odd : G B G B ...
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool evenRow = (y & 1) == 0;
            const bool evenCol = (x & 1) == 0;

            const uint16_t c = sampleClamp(input, width, height, x, y);
            const uint16_t n = sampleClamp(input, width, height, x, y - 1);
            const uint16_t s = sampleClamp(input, width, height, x, y + 1);
            const uint16_t w = sampleClamp(input, width, height, x - 1, y);
            const uint16_t e = sampleClamp(input, width, height, x + 1, y);
            const uint16_t nw = sampleClamp(input, width, height, x - 1, y - 1);
            const uint16_t ne = sampleClamp(input, width, height, x + 1, y - 1);
            const uint16_t sw = sampleClamp(input, width, height, x - 1, y + 1);
            const uint16_t se = sampleClamp(input, width, height, x + 1, y + 1);

            uint16_t r = 0;
            uint16_t g = 0;
            uint16_t b = 0;

            if (evenRow && evenCol) {
                // R site
                r = c;
                g = avg4(n, s, w, e);
                b = avg4(nw, ne, sw, se);
            } else if (!evenRow && !evenCol) {
                // B site
                r = avg4(nw, ne, sw, se);
                g = avg4(n, s, w, e);
                b = c;
            } else if (evenRow && !evenCol) {
                // G site on R row
                r = avg2(w, e);
                g = c;
                b = avg2(n, s);
            } else {
                // G site on B row
                r = avg2(n, s);
                g = c;
                b = avg2(w, e);
            }

            const size_t idx = static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x);
            const size_t base = idx * 3;
            output[base + 0] = r;
            output[base + 1] = g;
            output[base + 2] = b;
        }
    }
}

extern "C" void demosaic_bilinear_halide(const uint16_t* input,
                                          int width,
                                          int height,
                                          uint16_t* output) {
    demosaic_pattern_bilinear(input, width, height, output);
}

extern "C" void demosaic_ahd_halide(const uint16_t* input,
                                     int width,
                                     int height,
                                     uint16_t* output) {
    demosaic_pattern_bilinear(input, width, height, output);
}

extern "C" void get_cfa_pattern(int pattern[4]) {
    pattern[0] = 0;
    pattern[1] = 1;
    pattern[2] = 1;
    pattern[3] = 2;
}
