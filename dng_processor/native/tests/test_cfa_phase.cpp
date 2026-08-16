/**
 * test_cfa_phase.cpp — CFA phase (Bayer pattern) coverage for all four phases.
 *
 * Why this exists (2026-08-16): the demosaic expression used to hard-code
 * "red is at (even row, even col)" (RGGB). Files declaring BGGR / GRBG / GBRG
 * in their CFAPattern tag decoded with R and B misplaced, silently — no error
 * code, correct buffer size, only the pixel values were wrong, and nothing in
 * the test suite looked at pixel colors.
 *
 * The check: build a synthetic mosaic whose R / G / B sites carry three
 * distinct constant values, for each of the four phases, and demosaic it with
 * that phase. Bilinear interpolation of a constant field is exact (avg2 and
 * avg4 round-trip integers), and the CFA-aware edge repeat keeps boundary
 * samples on same-color sites, so *every* output pixel must come back with
 * exactly (kRed, kGreen, kBlue). If the phase is ignored or mismapped, the R
 * and B (or R and G) channels come back with the wrong constants.
 *
 * Both implementations are covered: the Halide AOT kernel
 * (demosaic_bilinear_halide_aot, which is what production runs) and the CPU
 * reference (demosaic_pattern_bilinear, which is the Stage3 PSNR baseline).
 * get_cfa_pattern's phase -> color-key expansion is checked too.
 *
 * No DNG file is needed. Output contract: one "[CFA PHASE] ... PASS|FAIL"
 * line per case plus a final "[CFA PHASE] ALL PASS" marker; exit 0 only when
 * every case passed.
 */

#include <cstdint>
#include <cstdio>
#include <vector>

#include "dng_cfa_phase.h"
#include "dng_mosaic_halide.h"

namespace {

constexpr uint16_t kRed = 1000;
constexpr uint16_t kGreen = 2000;
constexpr uint16_t kBlue = 3000;

constexpr int kWidth = 8;
constexpr int kHeight = 8;

struct Phase {
    const char* name;
    int red_x;
    int red_y;
};

const Phase kPhases[4] = {
    {"RGGB", 0, 0},
    {"GRBG", 1, 0},
    {"GBRG", 0, 1},
    {"BGGR", 1, 1},
};

// Synthetic CFA plane for the given phase: red where (x%2==red_x &&
// y%2==red_y), blue at the diagonally opposite parity, green elsewhere.
std::vector<uint16_t> makeMosaic(const Phase& phase) {
    std::vector<uint16_t> src(static_cast<size_t>(kWidth) * kHeight, 0);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const bool redRow = (y % 2) == phase.red_y;
            const bool redCol = (x % 2) == phase.red_x;
            uint16_t v = kGreen;
            if (redRow && redCol) {
                v = kRed;
            } else if (!redRow && !redCol) {
                v = kBlue;
            }
            src[static_cast<size_t>(y) * kWidth + x] = v;
        }
    }
    return src;
}

// Every output pixel of a constant-per-channel mosaic must reconstruct the
// exact constants. Reports the first mismatch so a failure is diagnosable.
bool checkOutput(const char* impl, const Phase& phase,
                 const std::vector<uint16_t>& out) {
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const size_t base = (static_cast<size_t>(y) * kWidth + x) * 3;
            const uint16_t r = out[base + 0];
            const uint16_t g = out[base + 1];
            const uint16_t b = out[base + 2];
            if (r != kRed || g != kGreen || b != kBlue) {
                std::printf("[CFA PHASE] %s / %s (red_x=%d red_y=%d) -> FAIL "
                            "at (%d,%d): got R=%u G=%u B=%u, want R=%u G=%u B=%u\n",
                            phase.name, impl, phase.red_x, phase.red_y, x, y,
                            static_cast<unsigned>(r), static_cast<unsigned>(g),
                            static_cast<unsigned>(b),
                            static_cast<unsigned>(kRed),
                            static_cast<unsigned>(kGreen),
                            static_cast<unsigned>(kBlue));
                return false;
            }
        }
    }
    std::printf("[CFA PHASE] %s / %s (red_x=%d red_y=%d) -> PASS\n",
                phase.name, impl, phase.red_x, phase.red_y);
    return true;
}

bool checkPatternExpansion(const Phase& phase) {
    int pattern[4] = {-1, -1, -1, -1};
    get_cfa_pattern(pattern, phase.red_x, phase.red_y);

    int expected[4];
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            int color = 1;  // green
            if (row == phase.red_y && col == phase.red_x) {
                color = 0;  // red
            } else if (row != phase.red_y && col != phase.red_x) {
                color = 2;  // blue
            }
            expected[row * 2 + col] = color;
        }
    }

    for (int i = 0; i < 4; ++i) {
        if (pattern[i] != expected[i]) {
            std::printf("[CFA PHASE] %s / get_cfa_pattern -> FAIL: "
                        "got {%d,%d,%d,%d}, want {%d,%d,%d,%d}\n",
                        phase.name, pattern[0], pattern[1], pattern[2],
                        pattern[3], expected[0], expected[1], expected[2],
                        expected[3]);
            return false;
        }
    }
    std::printf("[CFA PHASE] %s / get_cfa_pattern -> PASS\n", phase.name);
    return true;
}

// Resolver coverage (2026-08-16 review, S1). The rest of this file hands
// literal phases to the kernels, so nothing here exercised
// dng_resolve_cfa_phase() itself — and RGGB/BGGR, the only two phases we have
// real files for, are both transpose-invariant. Transposing red_x/red_y in
// the resolver would therefore stay green everywhere while every GRBG and
// GBRG camera decoded as its transpose. This case pins the row/col roles down
// by calling the resolver (never re-deriving the mapping) on GRBG and GBRG,
// the two phases that are not transpose-invariant.
//
// The synthetic CFAPattern is built from the phase *name* (row-major letters),
// which is independent of red_x/red_y, so this is not a mirror of the
// implementation.
bool checkResolver(const Phase& phase) {
    dng_mosaic_info mosaic;
    mosaic.fCFAPatternSize = dng_point(2, 2);  // (v, h)
    for (int i = 0; i < 4; ++i) {
        uint8 key = colorKeyGreen;
        if (phase.name[i] == 'R') {
            key = colorKeyRed;
        } else if (phase.name[i] == 'B') {
            key = colorKeyBlue;
        }
        mosaic.fCFAPattern[i / 2][i % 2] = key;  // row-major
    }

    int red_x = -1;
    int red_y = -1;
    const char* reason = nullptr;
    const bool ok = dng_resolve_cfa_phase(&mosaic, red_x, red_y, &reason);
    if (!ok || red_x != phase.red_x || red_y != phase.red_y) {
        std::printf("[CFA PHASE] %s / resolver -> FAIL: got ok=%d red_x=%d "
                    "red_y=%d (reason=%s), want ok=1 red_x=%d red_y=%d\n",
                    phase.name, ok ? 1 : 0, red_x, red_y,
                    reason ? reason : "(none)", phase.red_x, phase.red_y);
        return false;
    }
    std::printf("[CFA PHASE] %s / resolver -> PASS (red_x=%d red_y=%d)\n",
                phase.name, red_x, red_y);
    return true;
}

}  // namespace

int main() {
    std::printf("=== CFA phase coverage (%dx%d synthetic, 4 Bayer phases) ===\n",
                kWidth, kHeight);

    bool allPass = true;
    const size_t outElems = static_cast<size_t>(kWidth) * kHeight * 3;

    for (const Phase& phase : kPhases) {
        const std::vector<uint16_t> src = makeMosaic(phase);

        std::vector<uint16_t> cpuOut(outElems, 0);
        demosaic_pattern_bilinear(src.data(), kWidth, kHeight, cpuOut.data(),
                                  phase.red_x, phase.red_y);
        allPass = checkOutput("cpu-reference", phase, cpuOut) && allPass;

        std::vector<uint16_t> aotOut(outElems, 0);
        if (!demosaic_bilinear_halide_aot(src.data(), kWidth, kHeight,
                                          aotOut.data(), phase.red_x,
                                          phase.red_y)) {
            std::printf("[CFA PHASE] %s / halide-aot -> FAIL: kernel returned 0\n",
                        phase.name);
            allPass = false;
        } else {
            allPass = checkOutput("halide-aot", phase, aotOut) && allPass;
        }

        allPass = checkPatternExpansion(phase) && allPass;
        allPass = checkResolver(phase) && allPass;
    }

    if (!allPass) {
        std::printf("[CFA PHASE] FAIL\n");
        return 1;
    }
    std::printf("[CFA PHASE] ALL PASS\n");
    return 0;
}
