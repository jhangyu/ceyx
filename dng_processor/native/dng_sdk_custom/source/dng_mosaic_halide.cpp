/*
---
file_summary: "Stage3 bilinear demosaic bridge; uses Halide AOT Metal by default, with an explicit CPU reference path for disabled AOT."
functions:
  - name: "runDemosaicBilinearAot"
    description: "Wraps the dng_demosaic_bilinear Halide AOT kernel and copies output back to host."
    lines: "87-124"
  - name: "demosaic_pattern_bilinear"
    description: "CPU multithreaded RGGB bilinear demosaic fallback/reference implementation."
    lines: "147-242"
  - name: "demosaic_bilinear_halide_aot"
    description: "C ABI for the Halide AOT demosaic path."
    lines: "244-249"
  - name: "demosaic_bilinear_halide"
    description: "Default demosaic entry; tries AOT unless disabled, then CPU reference if the AOT wrapper returns failure."
    lines: "251-259"
  - name: "demosaic_ahd_halide"
    description: "Historical compatibility entry that currently dispatches to bilinear demosaic."
    lines: "261-266"
  - name: "get_cfa_pattern"
    description: "Returns the fixed RGGB CFA pattern used by current samples."
    lines: "268-273"
---
*/

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include "HalideBuffer.h"
#include "dng_demosaic_bilinear.h"

namespace {

using Halide::Runtime::Buffer;

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

bool demosaicAotDisabled() {
    const char* v = std::getenv("DNG_DEMOSAIC_AOT");
    return v && v[0] == '0';
}

bool demosaicTimingEnabled() {
    const char* v = std::getenv("DNG_DEMOSAIC_HALIDE_TIMING");
    return v && v[0] && v[0] != '0';
}

bool runDemosaicBilinearAot(const uint16_t* input,
                            int width,
                            int height,
                            uint16_t* output) {
    if (!input || !output || width <= 0 || height <= 0) {
        return false;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();
    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(input), width, height);
    Buffer<uint16_t> dst_buf =
        Buffer<uint16_t>::make_interleaved(output, width, height, 3);
    src_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const auto t1 = std::chrono::high_resolution_clock::now();
    const int result = dng_demosaic_bilinear(src_buf.raw_buffer(), dst_buf.raw_buffer());
    const auto t2 = std::chrono::high_resolution_clock::now();
    if (result != 0) {
        return false;
    }
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    const auto t3 = std::chrono::high_resolution_clock::now();

    if (demosaicTimingEnabled()) {
        auto ms = [](const auto& a, const auto& b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count() / 1000.0;
        };
        std::cerr << "[DemosaicHalideTiming] prep=" << ms(t0, t1)
                  << " ms kernel=" << ms(t1, t2)
                  << " ms copy_to_host=" << ms(t2, t3)
                  << " ms\n";
    }

    return true;
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

extern "C" int demosaic_bilinear_halide_aot(const uint16_t* input,
                                             int width,
                                             int height,
                                             uint16_t* output) {
    return runDemosaicBilinearAot(input, width, height, output) ? 1 : 0;
}

extern "C" void demosaic_bilinear_halide(const uint16_t* input,
                                          int width,
                                          int height,
                                          uint16_t* output) {
    if (!demosaicAotDisabled() && demosaic_bilinear_halide_aot(input, width, height, output)) {
        return;
    }
    demosaic_pattern_bilinear(input, width, height, output);
}

extern "C" void demosaic_ahd_halide(const uint16_t* input,
                                     int width,
                                     int height,
                                     uint16_t* output) {
    demosaic_bilinear_halide(input, width, height, output);
}

extern "C" void get_cfa_pattern(int pattern[4]) {
    pattern[0] = 0;
    pattern[1] = 1;
    pattern[2] = 1;
    pattern[3] = 2;
}
