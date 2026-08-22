// test_stage4_scaled.cpp — standalone gate for the box-filter-downscaling
// Stage4 AOT (`dng_render_stage4_scaled`).
//
// Context: docs/logs/2026-08-23/targetwidth-sized-decode-handover.md §4 row 6,
// §5.2. The production `dng_render_stage4` kernel must stay byte-identical
// (pinned gate SHAs, Gotcha #99), so the sized path lives in a separate AOT.
// This test gates that separate AOT on its own, with no DNG sample and no
// dng_sdk dependency.
//
// Method
// ------
// The scaled kernel box-averages the Stage3 source down to the requested
// output size and *then* runs the render math. So the semantically equivalent
// reference is: box-average the same source on the CPU, then run the existing,
// already-gated `dng_render_stage4` kernel on that smaller source. The two
// results must agree to within u16 re-quantisation of the averaged source.
//
// The CPU box downscale is written from scratch here (plain nested loops over
// the exact integer-ratio cell) and shares no code with the Halide generator,
// so a bug in the generator's cell geometry cannot hide.
//
// Falsifiability (red -> green)
// -----------------------------
// Run with `--mutate-reference` to replace the CPU box downscale with
// nearest-neighbour point sampling. Everything else is unchanged. The gate
// MUST fail in that mode; if it passes, the test is not measuring what it
// claims to. See the README block printed at the end of a run.
//
// Usage:
//   test_stage4_scaled [--mutate-reference] [--psnr-threshold <db>]

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "HalideBuffer.h"

#include "dng_render_stage4.h"
#include "dng_render_stage4_scaled.h"

using Halide::Runtime::Buffer;

namespace {

constexpr int kTableSize = 4098;  // matches the generator's table_interp domain
constexpr int kHueDiv = 2;
constexpr int kSatDiv = 2;
constexpr int kValDiv = 2;

struct RenderParams {
    Buffer<float> exp_ramp{kTableSize};
    Buffer<float> tone_curve{kTableSize};
    Buffer<float> encode_gamma{kTableSize};
    Buffer<float> camera_white{3};
    Buffer<float> camera_to_rgb{3, 3};   // [col, row]
    Buffer<float> rgb_to_final{3, 3};    // [col, row]
    Buffer<float> huesat_table{kHueDiv * kSatDiv * kValDiv, 3};
    Buffer<float> huesat_encode{kTableSize};
    Buffer<float> huesat_decode{kTableSize};
    Buffer<float> look_table{kHueDiv * kSatDiv * kValDiv, 3};
    Buffer<float> look_encode{kTableSize};
    Buffer<float> look_decode{kTableSize};
};

// table_interp() maps v in [0,1] onto index v * (extent - 2), so an identity
// LUT is table(i) = i / (extent - 2).
void fill_identity_table(Buffer<float> &t) {
    const float denom = static_cast<float>(t.dim(0).extent() - 2);
    for (int i = 0; i < t.dim(0).extent(); ++i) {
        t(i) = static_cast<float>(i) / denom;
    }
}

void fill_gamma_table(Buffer<float> &t, float gamma) {
    const float denom = static_cast<float>(t.dim(0).extent() - 2);
    for (int i = 0; i < t.dim(0).extent(); ++i) {
        float v = static_cast<float>(i) / denom;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        t(i) = std::pow(v, gamma);
    }
}

RenderParams make_render_params() {
    RenderParams p;

    fill_identity_table(p.exp_ramp);
    fill_identity_table(p.tone_curve);
    // A real encode curve, so the 8-bit output actually spreads across the
    // range instead of clustering near black.
    fill_gamma_table(p.encode_gamma, 1.0f / 2.2f);

    for (int i = 0; i < 3; ++i) p.camera_white(i) = 1.0f;

    // Deliberately non-identity, so a channel mix-up in the new kernel is
    // visible. Stored [col, row] to match the generator's indexing.
    const float cam[3][3] = {  // [row][col]
        {1.10f, -0.10f, 0.00f},
        {-0.20f, 1.30f, -0.10f},
        {0.00f, -0.15f, 1.15f},
    };
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            p.camera_to_rgb(col, row) = cam[row][col];
            p.rgb_to_final(col, row) = (row == col) ? 1.0f : 0.0f;
        }
    }

    // HueSat / Look are disabled (has_table = 0) but must still be
    // dimensionally valid: the generator indexes them unconditionally and
    // clamps, because select() is not short-circuit in Halide.
    for (int c = 0; c < 3; ++c) {
        for (int e = 0; e < p.huesat_table.dim(0).extent(); ++e) {
            const float identity = (c == 0) ? 0.0f : 1.0f;  // hueShift, satScale, valScale
            p.huesat_table(e, c) = identity;
            p.look_table(e, c) = identity;
        }
    }
    fill_identity_table(p.huesat_encode);
    fill_identity_table(p.huesat_decode);
    fill_identity_table(p.look_encode);
    fill_identity_table(p.look_decode);

    return p;
}

// Stage3-shaped source: interleaved u16 RGB (dim0 stride 3, dim2 stride 1),
// which is what the macOS Stage4 kernel declares.
Buffer<uint16_t> make_source(int w, int h) {
    Buffer<uint16_t> src = Buffer<uint16_t>::make_interleaved(w, h, 3);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // Low-frequency ramp + high-frequency checker. The checker is the
            // part that only a real box filter averages away; point sampling
            // keeps it, which is what --mutate-reference exploits.
            const float fx = static_cast<float>(x) / static_cast<float>(w - 1);
            const float fy = static_cast<float>(y) / static_cast<float>(h - 1);
            const float checker = (((x >> 1) ^ (y >> 1)) & 1) ? 0.12f : -0.12f;

            const float r = fx * 0.8f + 0.1f + checker;
            const float g = fy * 0.8f + 0.1f - checker;
            const float b = (1.0f - fx * fy) * 0.7f + 0.15f + checker * 0.5f;

            auto q = [](float v) -> uint16_t {
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                return static_cast<uint16_t>(v * 65535.0f + 0.5f);
            };
            src(x, y, 0) = q(r);
            src(x, y, 1) = q(g);
            src(x, y, 2) = q(b);
        }
    }
    return src;
}

// Independent CPU reference downscale, written directly against the intended
// contract: output pixel x covers source columns [x*W/ow, (x+1)*W/ow).
// `mutate` swaps the box average for nearest-neighbour point sampling, which
// must break the gate.
Buffer<uint16_t> cpu_downscale(const Buffer<uint16_t> &src, int ow, int oh, bool mutate) {
    const int w = src.dim(0).extent();
    const int h = src.dim(1).extent();
    Buffer<uint16_t> out = Buffer<uint16_t>::make_interleaved(ow, oh, 3);

    for (int y = 0; y < oh; ++y) {
        const int y0 = static_cast<int>((static_cast<int64_t>(y) * h) / oh);
        int y1 = static_cast<int>((static_cast<int64_t>(y + 1) * h) / oh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ow; ++x) {
            const int x0 = static_cast<int>((static_cast<int64_t>(x) * w) / ow);
            int x1 = static_cast<int>((static_cast<int64_t>(x + 1) * w) / ow);
            if (x1 <= x0) x1 = x0 + 1;

            for (int c = 0; c < 3; ++c) {
                if (mutate) {
                    out(x, y, c) = src(x0, y0, c);
                    continue;
                }
                double acc = 0.0;
                for (int sy = y0; sy < y1; ++sy) {
                    for (int sx = x0; sx < x1; ++sx) {
                        acc += static_cast<double>(src(sx, sy, c));
                    }
                }
                const double n = static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0);
                double avg = acc / n;
                if (avg < 0.0) avg = 0.0;
                if (avg > 65535.0) avg = 65535.0;
                out(x, y, c) = static_cast<uint16_t>(avg + 0.5);
            }
        }
    }
    return out;
}

int call_stage4(Buffer<uint16_t> &src, RenderParams &p, Buffer<uint8_t> &dst) {
    return dng_render_stage4(src, 1.0f / 65535.0f,
                             p.exp_ramp, p.tone_curve, p.encode_gamma,
                             p.camera_white, p.camera_to_rgb, p.rgb_to_final,
                             p.huesat_table, p.huesat_encode, p.huesat_decode,
                             kHueDiv, kSatDiv, kValDiv, /*has_table=*/0, /*has_encoding=*/0,
                             p.look_table, p.look_encode, p.look_decode,
                             kHueDiv, kSatDiv, kValDiv, /*has_table=*/0, /*has_encoding=*/0,
                             dst);
}

int call_stage4_scaled(Buffer<uint16_t> &src, int ow, int oh, RenderParams &p,
                       Buffer<uint8_t> &dst) {
    return dng_render_stage4_scaled(src, 1.0f / 65535.0f, ow, oh,
                                    p.exp_ramp, p.tone_curve, p.encode_gamma,
                                    p.camera_white, p.camera_to_rgb, p.rgb_to_final,
                                    p.huesat_table, p.huesat_encode, p.huesat_decode,
                                    kHueDiv, kSatDiv, kValDiv, /*has_table=*/0, /*has_encoding=*/0,
                                    p.look_table, p.look_encode, p.look_decode,
                                    kHueDiv, kSatDiv, kValDiv, /*has_table=*/0, /*has_encoding=*/0,
                                    dst);
}

// PSNR over the RGB channels only (alpha is a constant 255 in both).
double rgb_psnr(const Buffer<uint8_t> &a, const Buffer<uint8_t> &b, int *max_abs_out) {
    const int w = a.dim(0).extent();
    const int h = a.dim(1).extent();
    double sse = 0.0;
    int max_abs = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) {
                const int d = static_cast<int>(a(x, y, c)) - static_cast<int>(b(x, y, c));
                sse += static_cast<double>(d) * static_cast<double>(d);
                const int ad = d < 0 ? -d : d;
                if (ad > max_abs) max_abs = ad;
            }
        }
    }
    *max_abs_out = max_abs;
    const double n = static_cast<double>(w) * static_cast<double>(h) * 3.0;
    if (sse == 0.0) return 999.0;
    const double mse = sse / n;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

struct Case {
    const char *name;
    int src_w, src_h;
    int out_w, out_h;
};

bool run_case(const Case &tc, RenderParams &p, double threshold, bool mutate) {
    printf("\n--- case %s: %dx%d -> %dx%d ---\n",
           tc.name, tc.src_w, tc.src_h, tc.out_w, tc.out_h);

    Buffer<uint16_t> src = make_source(tc.src_w, tc.src_h);

    Buffer<uint8_t> got = Buffer<uint8_t>::make_interleaved(tc.out_w, tc.out_h, 4);
    int rc = call_stage4_scaled(src, tc.out_w, tc.out_h, p, got);
    if (rc != 0) {
        printf("  FAIL: dng_render_stage4_scaled returned %d\n", rc);
        return false;
    }
    got.copy_to_host();

    Buffer<uint16_t> small_src = cpu_downscale(src, tc.out_w, tc.out_h, mutate);
    Buffer<uint8_t> ref = Buffer<uint8_t>::make_interleaved(tc.out_w, tc.out_h, 4);
    rc = call_stage4(small_src, p, ref);
    if (rc != 0) {
        printf("  FAIL: dng_render_stage4 (reference) returned %d\n", rc);
        return false;
    }
    ref.copy_to_host();

    // Contract check: the kernel must fill the whole requested output and set
    // alpha = 255 everywhere (same RGBA8 semantics as dng_render_stage4).
    for (int y = 0; y < tc.out_h; ++y) {
        for (int x = 0; x < tc.out_w; ++x) {
            if (got(x, y, 3) != 255) {
                printf("  FAIL: alpha != 255 at (%d,%d): %d\n", x, y, got(x, y, 3));
                return false;
            }
        }
    }

    int max_abs = 0;
    const double psnr = rgb_psnr(got, ref, &max_abs);
    const bool pass = psnr >= threshold;
    printf("  PSNR = %.2f dB (threshold %.2f), maxAbs = %d -> %s\n",
           psnr, threshold, max_abs, pass ? "PASS" : "FAIL");
    return pass;
}

}  // namespace

int main(int argc, char **argv) {
    bool mutate = false;
    double threshold = 40.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--mutate-reference") {
            mutate = true;
        } else if (arg == "--psnr-threshold" && i + 1 < argc) {
            threshold = atof(argv[++i]);
        } else {
            printf("usage: %s [--mutate-reference] [--psnr-threshold <db>]\n", argv[0]);
            return 2;
        }
    }

    printf("=== test_stage4_scaled ===\n");
    printf("reference mode: %s\n",
           mutate ? "MUTATED (nearest-neighbour) -- gate is EXPECTED TO FAIL"
                  : "box downscale (CPU, independent)");

    RenderParams params = make_render_params();

    const Case cases[] = {
        // Exact integer ratio (4:1) -- every cell is 4x4.
        {"integer-ratio", 1024, 768, 256, 192},
        // Non-integer ratio -- neighbouring cells differ in size by 1 px,
        // which is what the RDom masking has to get right.
        {"fractional-ratio", 1024, 768, 333, 250},
        // Extreme downscale, the sidebar-thumbnail case from the handover.
        {"thumbnail", 1024, 768, 200, 150},
        // Degenerate 1:1 -- must behave as a plain passthrough render.
        {"identity", 256, 192, 256, 192},
    };

    bool all_pass = true;
    for (const Case &tc : cases) {
        if (!run_case(tc, params, threshold, mutate)) all_pass = false;
    }

    if (mutate) {
        // In mutated mode a PASS means the gate is blind, so invert the verdict.
        printf("\n=== MUTATION CHECK: %s ===\n",
               all_pass ? "BAD -- gate passed against a wrong reference"
                        : "GOOD -- gate rejected the wrong reference");
        return all_pass ? 1 : 0;
    }

    printf("\n=== %s ===\n", all_pass ? "ALL CASES PASS" : "FAILURE");
    return all_pass ? 0 : 1;
}
