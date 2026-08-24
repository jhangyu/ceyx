// X-Trans 6x6 fused normalize + demosaic: AOT kernel vs same-formula CPU
// reference, plus the coverage property the algorithm depends on.
//
// The coverage case is not decoration: the 5x5 weighted-mean interpolation is
// only total because every one of the 36 phases (shift positions of the 6x6
// tile) has at least one R, G and B site in its 5x5 window. If that were
// false, some pixels would silently fall back to a zero-weight mean.
//
// The tile below is the canonical Fujifilm arrangement that
// src/raw_contract_validate.cpp accepts (kCanonicalXTrans at shift (0,0)) and
// that tests/test_raw_layout_contract.cpp already exercises; the coverage case
// re-derives that equality so this test cannot drift onto a tile the layout
// validator would reject.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "raw_demosaic_reference.h"
#include "raw_pipeline_contract.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    // The extra separator is emitted only for named cases, so the unnamed
    // per-shift case prints exactly "[RawXTransKernel] shift=... -> PASS".
    std::printf("[RawXTransKernel] %s%s%s -> %s\n", name,
                (name && name[0]) ? " " : "", detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

constexpr uint32_t kW = 132;   // multiples of 6 plus a remainder, on purpose
constexpr uint32_t kH = 102;

// Canonical Fujifilm X-Trans 6x6 tile, row-major, matching
// src/raw_contract_validate.cpp:55 kCanonicalXTrans and
// tests/test_raw_layout_contract.cpp:40 kXTransLetters.
const char kXTransLetters[37] = "GGRGGBGGBGGRBRGRBGGGBGGRGGRGGBRBGBRG";

// The same tile as colour indices, used to prove kXTransLetters really is the
// validator's canonical arrangement rather than some other 8/20/8 pattern.
const int kCanonicalXTrans[6][6] = {
    {1, 1, 0, 1, 1, 2},
    {1, 1, 2, 1, 1, 0},
    {2, 0, 1, 0, 2, 1},
    {1, 1, 2, 1, 1, 0},
    {1, 1, 0, 1, 1, 2},
    {0, 2, 1, 2, 0, 1},
};

void makeCfa(int32_t out[36], int shift_x, int shift_y) {
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            const char c = kXTransLetters[((row + shift_y) % 6) * 6 + ((col + shift_x) % 6)];
            out[row * 6 + col] = (c == 'R') ? kRawColorKeyRed
                               : (c == 'B') ? kRawColorKeyBlue
                                            : kRawColorKeyGreen;
        }
    }
}

std::vector<uint16_t> makeNoise(uint32_t w, uint32_t h, int64_t row_stride_bytes) {
    const size_t stride_px = static_cast<size_t>(row_stride_bytes) / 2;
    std::vector<uint16_t> src(stride_px * h, 0);
    uint32_t state = 0x2468ACE1u;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            state = state * 1664525u + 1013904223u;
            src[y * stride_px + x] = static_cast<uint16_t>(700 + (state >> 18) % 14000);
        }
    }
    return src;
}

std::vector<uint16_t> makeConstant(const int32_t cfa[36], uint16_t r, uint16_t g,
                                   uint16_t b) {
    std::vector<uint16_t> src(static_cast<size_t>(kW) * kH, 0);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const int32_t key = cfa[(y % 6) * 6 + (x % 6)];
            src[static_cast<size_t>(y) * kW + x] =
                (key == kRawColorKeyRed) ? r : (key == kRawColorKeyBlue) ? b : g;
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
    const int64_t stride = static_cast<int64_t>(kW) * 2;

    // 1. Coverage: the tile is the validator's canonical arrangement, and every
    //    phase's 5x5 window contains R, G and B.
    {
        int32_t cfa[36];
        makeCfa(cfa, 0, 0);

        bool canonical = true;
        for (int r = 0; r < 6 && canonical; ++r) {
            for (int c = 0; c < 6; ++c) {
                const int idx = (cfa[r * 6 + c] == kRawColorKeyRed) ? 0
                              : (cfa[r * 6 + c] == kRawColorKeyBlue) ? 2 : 1;
                if (idx != kCanonicalXTrans[r][c]) { canonical = false; break; }
            }
        }

        bool all_covered = true;
        int bad_phase = -1;
        for (int phase = 0; phase < 36; ++phase) {
            const int py = phase / 6, px = phase % 6;
            bool has_r = false, has_g = false, has_b = false;
            for (int dy = -2; dy <= 2; ++dy) {
                for (int dx = -2; dx <= 2; ++dx) {
                    const int yy = ((py + dy) % 6 + 6) % 6;
                    const int xx = ((px + dx) % 6 + 6) % 6;
                    const int32_t k = cfa[yy * 6 + xx];
                    if (k == kRawColorKeyRed) has_r = true;
                    else if (k == kRawColorKeyBlue) has_b = true;
                    else has_g = true;
                }
            }
            if (!(has_r && has_g && has_b)) { all_covered = false; bad_phase = phase; break; }
        }

        char detail[128];
        if (!canonical) {
            std::snprintf(detail, sizeof(detail),
                          "tile is not the validator's canonical arrangement");
        } else if (all_covered) {
            std::snprintf(detail, sizeof(detail), "36/36 phases");
        } else {
            std::snprintf(detail, sizeof(detail), "phase %d lacks a colour", bad_phase);
        }
        report("coverage", canonical && all_covered, detail);
    }

    // 2. Kernel vs reference, pattern origin shifted 0..5.
    for (int shift = 0; shift < 6; ++shift) {
        int32_t cfa[36];
        makeCfa(cfa, shift, shift);
        const std::vector<uint16_t> src = makeNoise(kW, kH, stride);

        std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
        raw_xtrans_demosaic_reference(src.data(), kW, kH, stride, cfa,
                                      black_flat, 1, 1, inv_range, ref.data());
        const int ok = raw_xtrans_demosaic_aot(src.data(), kW, kH, stride, cfa,
                                               black_flat, 1, 1, inv_range, got.data());
        const Stats s = compare(ref, got);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "shift=%d psnr=%.2f max_abs=%u",
                      shift, s.psnr, s.max_abs);
        report("", ok == 1 && s.psnr >= 99.0 && s.max_abs <= 1, detail);
    }

    // 3. Constant field reconstructs exactly.
    {
        int32_t cfa[36];
        makeCfa(cfa, 0, 0);
        const uint16_t r = 4096, g = 8192, b = 12288;
        const std::vector<uint16_t> src = makeConstant(cfa, r, g, b);
        const float black_zero[1] = {0.0f};

        std::vector<uint16_t> got(out_elems, 0);
        const int ok = raw_xtrans_demosaic_aot(src.data(), kW, kH, stride, cfa,
                                               black_zero, 1, 1, 1.0f, got.data());
        bool exact = (ok == 1);
        uint32_t bx = 0, by = 0;
        for (uint32_t y = 0; exact && y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const size_t base = (static_cast<size_t>(y) * kW + x) * 3;
                if (got[base] != r || got[base + 1] != g || got[base + 2] != b) {
                    exact = false; bx = x; by = y; break;
                }
            }
        }
        char detail[160];
        if (exact) std::snprintf(detail, sizeof(detail), "exact");
        else std::snprintf(detail, sizeof(detail), "first mismatch at (%u,%u)", bx, by);
        report("constant-field", exact, detail);
    }

    // 4. Borders: finite, in range, and matching the reference.
    {
        int32_t cfa[36];
        makeCfa(cfa, 0, 0);
        const std::vector<uint16_t> src = makeNoise(kW, kH, stride);
        std::vector<uint16_t> ref(out_elems, 0), got(out_elems, 0);
        raw_xtrans_demosaic_reference(src.data(), kW, kH, stride, cfa,
                                      black_flat, 1, 1, inv_range, ref.data());
        const int ok = raw_xtrans_demosaic_aot(src.data(), kW, kH, stride, cfa,
                                               black_flat, 1, 1, inv_range, got.data());
        uint32_t worst = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                if (x >= 3 && y >= 3 && x < kW - 3 && y < kH - 3) continue;
                for (int c = 0; c < 3; ++c) {
                    const size_t i = (static_cast<size_t>(y) * kW + x) * 3 + c;
                    const int d = static_cast<int>(ref[i]) - static_cast<int>(got[i]);
                    const uint32_t mag = static_cast<uint32_t>(d < 0 ? -d : d);
                    if (mag > worst) worst = mag;
                }
            }
        }
        char detail[128];
        std::snprintf(detail, sizeof(detail), "border max_abs=%u", worst);
        report("edge", ok == 1 && worst <= 1, detail);
    }

    // 5. Padded stride must not change a byte.
    {
        int32_t cfa[36];
        makeCfa(cfa, 0, 0);
        const int64_t padded = stride + 64;
        const std::vector<uint16_t> a = makeNoise(kW, kH, stride);
        const std::vector<uint16_t> b = makeNoise(kW, kH, padded);
        std::vector<uint16_t> out_a(out_elems, 0), out_b(out_elems, 0);
        const int ok_a = raw_xtrans_demosaic_aot(a.data(), kW, kH, stride, cfa,
                                                 black_flat, 1, 1, inv_range, out_a.data());
        const int ok_b = raw_xtrans_demosaic_aot(b.data(), kW, kH, padded, cfa,
                                                 black_flat, 1, 1, inv_range, out_b.data());
        report("padded-stride", ok_a == 1 && ok_b == 1 && out_a == out_b,
               "row_stride_bytes = width*2 + 64");
    }

    if (failures != 0) {
        std::printf("[RawXTransKernel] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawXTransKernel] ALL PASS\n");
    return 0;
}
