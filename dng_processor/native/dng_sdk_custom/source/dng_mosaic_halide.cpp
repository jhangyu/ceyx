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
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

inline int clampCoord(int v, int lo, int hi) {
    return std::min(std::max(v, lo), hi);
}

inline int positiveModulo(int v, int m) {
    const int r = v % m;
    return (r < 0) ? (r + m) : r;
}

// Match DNG SDK edge_repeat with repeatV/repeatH = CFA pattern size (2x2 for Bayer).
inline int mapRepeatCoord(int coord, int size, int repeat) {
    if (size <= 0) {
        return 0;
    }
    repeat = clampCoord(repeat, 1, size);

    if (coord < 0) {
        return positiveModulo(coord, repeat);
    }
    if (coord >= size) {
        const int start = size - repeat;
        return start + positiveModulo(coord - start, repeat);
    }
    return coord;
}

inline uint16_t sampleRepeat(const uint16_t* input, int width, int height, int x, int y) {
    constexpr int kCfaRepeatV = 2;
    constexpr int kCfaRepeatH = 2;
    const int cx = mapRepeatCoord(x, width, kCfaRepeatH);
    const int cy = mapRepeatCoord(y, height, kCfaRepeatV);
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

    auto process_rows = [&](int y_begin, int y_end) {
        // RGGB layout:
        //   row even: R G R G ...
        //   row odd : G B G B ...
        for (int y = y_begin; y < y_end; ++y) {
            for (int x = 0; x < width; ++x) {
                const bool evenRow = (y & 1) == 0;
                const bool evenCol = (x & 1) == 0;

                const uint16_t c = sampleRepeat(input, width, height, x, y);
                const uint16_t n = sampleRepeat(input, width, height, x, y - 1);
                const uint16_t s = sampleRepeat(input, width, height, x, y + 1);
                const uint16_t w = sampleRepeat(input, width, height, x - 1, y);
                const uint16_t e = sampleRepeat(input, width, height, x + 1, y);
                const uint16_t nw = sampleRepeat(input, width, height, x - 1, y - 1);
                const uint16_t ne = sampleRepeat(input, width, height, x + 1, y - 1);
                const uint16_t sw = sampleRepeat(input, width, height, x - 1, y + 1);
                const uint16_t se = sampleRepeat(input, width, height, x + 1, y + 1);

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
    };

    unsigned int thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0) {
        thread_count = 4;
    }
    const char* env = std::getenv("DNG_DEMOSAIC_THREADS");
    if (env && env[0] != '\0') {
        const int parsed = std::atoi(env);
        if (parsed > 0) {
            thread_count = static_cast<unsigned int>(parsed);
        }
    }
    if (thread_count > static_cast<unsigned int>(height)) {
        thread_count = static_cast<unsigned int>(height);
    }
    if (thread_count <= 1) {
        process_rows(0, height);
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    int y0 = 0;
    for (unsigned int i = 0; i < thread_count; ++i) {
        const int remain_rows = height - y0;
        const int remain_threads = static_cast<int>(thread_count - i);
        const int span = remain_rows / remain_threads;
        const int y1 = y0 + span;
        workers.emplace_back(process_rows, y0, y1);
        y0 = y1;
    }
    for (auto& t : workers) {
        t.join();
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
