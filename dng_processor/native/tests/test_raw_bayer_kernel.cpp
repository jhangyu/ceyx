// Fused normalize + Bayer demosaic: AOT kernel vs same-algorithm CPU reference.
//
// Two oracles on purpose. PSNR/max_abs catches codegen drift; the constant-field
// case catches phase transposition, which a whole-image PSNR can hide when the
// image is smooth (spec section 11.2.3, and the 2026-08-16 CFA phase bug).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "raw_demosaic_reference.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    // The extra separator is emitted only for named cases, so the unnamed
    // per-phase case prints exactly "[RawBayerKernel] phase=... -> PASS".
    std::printf("[RawBayerKernel] %s%s%s -> %s\n", name,
                (name && name[0]) ? " " : "", detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

constexpr uint32_t kW = 128;
constexpr uint32_t kH = 96;

struct Phase { const char* name; int32_t red_x; int32_t red_y; };
const Phase kPhases[4] = {{"RGGB", 0, 0}, {"GRBG", 1, 0},
                          {"GBRG", 0, 1}, {"BGGR", 1, 1}};

// Deterministic pseudo-random mosaic: a fixed LCG, so a failure is reproducible
// without shipping a fixture file.
std::vector<uint16_t> makeNoiseMosaic(uint32_t w, uint32_t h, int64_t row_stride_bytes) {
    const size_t stride_px = static_cast<size_t>(row_stride_bytes) / 2;
    std::vector<uint16_t> src(stride_px * h, 0);
    uint32_t state = 0x13572468u;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            state = state * 1664525u + 1013904223u;
            src[y * stride_px + x] = static_cast<uint16_t>(600 + (state >> 18) % 15000);
        }
    }
    return src;
}

std::vector<uint16_t> makeConstantMosaic(const Phase& p, uint16_t r, uint16_t g,
                                         uint16_t b) {
    std::vector<uint16_t> src(static_cast<size_t>(kW) * kH, 0);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const bool red_row = (y % 2) == static_cast<uint32_t>(p.red_y);
            const bool red_col = (x % 2) == static_cast<uint32_t>(p.red_x);
            uint16_t v = g;
            if (red_row && red_col) v = r;
            else if (!red_row && !red_col) v = b;
            src[static_cast<size_t>(y) * kW + x] = v;
        }
    }
    return src;
}

struct Stats { double psnr; uint32_t max_abs; };

Stats compare(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    double sse = 0.0;
    uint32_t max_abs = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        const int diff = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        sse += static_cast<double>(diff) * diff;
        const uint32_t mag = static_cast<uint32_t>(diff < 0 ? -diff : diff);
        if (mag > max_abs) max_abs = mag;
    }
    if (sse == 0.0) return Stats{999.0, 0};
    const double mse = sse / static_cast<double>(a.size());
    return Stats{10.0 * std::log10(65535.0 * 65535.0 / mse), max_abs};
}

}  // namespace

int main() {
    const float black_flat[1] = {512.0f};
    const float inv_range = 65535.0f / (16383.0f - 512.0f);
    const size_t out_elems = static_cast<size_t>(kW) * kH * 3;

    // 1. Kernel vs reference on noise, all four phases.
    for (const Phase& p : kPhases) {
        const int64_t stride = static_cast<int64_t>(kW) * 2;
        const std::vector<uint16_t> src = makeNoiseMosaic(kW, kH, stride);

        std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
        raw_bayer_demosaic_reference(src.data(), kW, kH, stride, p.red_x, p.red_y,
                                     black_flat, 1, 1, inv_range, ref.data());
        const int ok = raw_bayer_demosaic_aot(src.data(), kW, kH, stride,
                                              p.red_x, p.red_y, black_flat, 1, 1,
                                              inv_range, got.data());
        const Stats s = compare(ref, got);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "phase=%s psnr=%.2f max_abs=%u",
                      p.name, s.psnr, s.max_abs);
        report("", ok == 1 && s.psnr >= 99.0 && s.max_abs <= 1, detail);
    }

    // 2. Constant-field oracle: exact reconstruction, every pixel.
    for (const Phase& p : kPhases) {
        // Choose constants that survive normalization exactly.
        const uint16_t r = 4096, g = 8192, b = 12288;
        const std::vector<uint16_t> src = makeConstantMosaic(p, r, g, b);
        const float black_zero[1] = {0.0f};
        const float unity = 1.0f;

        std::vector<uint16_t> got(out_elems, 0);
        const int ok = raw_bayer_demosaic_aot(src.data(), kW, kH,
                                              static_cast<int64_t>(kW) * 2,
                                              p.red_x, p.red_y, black_zero, 1, 1,
                                              unity, got.data());
        bool exact = (ok == 1);
        uint32_t bad_x = 0, bad_y = 0;
        for (uint32_t y = 0; exact && y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const size_t base = (static_cast<size_t>(y) * kW + x) * 3;
                if (got[base] != r || got[base + 1] != g || got[base + 2] != b) {
                    exact = false; bad_x = x; bad_y = y; break;
                }
            }
        }
        char detail[160];
        if (exact) {
            std::snprintf(detail, sizeof(detail), "phase=%s exact", p.name);
        } else {
            std::snprintf(detail, sizeof(detail),
                          "phase=%s first mismatch at (%u,%u)", p.name, bad_x, bad_y);
        }
        report("constant-field", exact, detail);
    }

    // 3. Padded stride must not change a single output byte.
    {
        const int64_t tight = static_cast<int64_t>(kW) * 2;
        const int64_t padded = tight + 64;
        const std::vector<uint16_t> a = makeNoiseMosaic(kW, kH, tight);
        std::vector<uint16_t> b = makeNoiseMosaic(kW, kH, padded);

        std::vector<uint16_t> out_a(out_elems, 0), out_b(out_elems, 0);
        const int ok_a = raw_bayer_demosaic_aot(a.data(), kW, kH, tight, 0, 0,
                                                black_flat, 1, 1, inv_range, out_a.data());
        const int ok_b = raw_bayer_demosaic_aot(b.data(), kW, kH, padded, 0, 0,
                                                black_flat, 1, 1, inv_range, out_b.data());
        report("padded-stride", ok_a == 1 && ok_b == 1 && out_a == out_b,
               "row_stride_bytes = width*2 + 64");
    }

    // 4. Per-site black tile.
    {
        const int64_t stride = static_cast<int64_t>(kW) * 2;
        const std::vector<uint16_t> src = makeNoiseMosaic(kW, kH, stride);
        const float black_tile[4] = {100.0f, 200.0f, 200.0f, 300.0f};

        std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
        raw_bayer_demosaic_reference(src.data(), kW, kH, stride, 0, 0,
                                     black_tile, 2, 2, inv_range, ref.data());
        const int ok = raw_bayer_demosaic_aot(src.data(), kW, kH, stride, 0, 0,
                                              black_tile, 2, 2, inv_range, got.data());
        const Stats s = compare(ref, got);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "psnr=%.2f max_abs=%u", s.psnr, s.max_abs);
        report("black-tile-2x2", ok == 1 && s.psnr >= 99.0 && s.max_abs <= 1, detail);
    }

    if (failures != 0) {
        std::printf("[RawBayerKernel] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawBayerKernel] ALL PASS\n");
    return 0;
}
