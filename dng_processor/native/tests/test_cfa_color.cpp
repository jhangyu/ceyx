/**
 * test_cfa_color.cpp — end-to-end pixel color-correctness gate for a DNG.
 *
 * Why this exists (2026-08-16): a BGGR sensor's DNG decoded through the
 * production FFI entry came out with R and B swapped (daylight sky rendered
 * orange). Nothing failed: correct size, correct timings, kDngSuccess, and no
 * test in the repo ever looked at a pixel's color. This tool is the missing
 * observable — it decodes a real file and asserts a channel relation over a
 * region whose true color is known.
 *
 * Usage:
 *   test_cfa_color <dng_path> [--expect-blue-sky] [--min-b-minus-r N]
 *
 * --expect-blue-sky (default) samples the top band of the image, which for
 * the intended fixture is open sky, and requires mean(B) - mean(R) >= N
 * (default 50). Reference numbers from the diagnosis of the vivo BGGR sample
 * (IMG_20251112_092839.dng): correct decode gives R~0.9 G~135.4 B~236.2;
 * the RGGB-hard-coded decode gives the exact mirror R~236.2 G~135.4 B~0.9.
 * A sign flip of ~235 makes any threshold in the tens unambiguous while
 * staying far away from tone/exposure noise.
 *
 * Output contract: a "[CFA COLOR] ... PASS|FAIL" line; exit 0 only on PASS.
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dng_ffi_api.h"

namespace {

struct ChannelMeans {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
    long long samples = 0;
};

// Mean R/G/B over the top `band_fraction` of rows (full width). The fixture's
// sky occupies the top of the frame; alpha is ignored.
ChannelMeans meanTopBand(const uint8_t* rgba, int width, int height,
                         double band_fraction) {
    ChannelMeans m;
    int bandRows = static_cast<int>(height * band_fraction);
    if (bandRows < 1) bandRows = 1;
    if (bandRows > height) bandRows = height;

    double sumR = 0.0, sumG = 0.0, sumB = 0.0;
    long long n = 0;
    for (int y = 0; y < bandRows; ++y) {
        const uint8_t* row = rgba + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            sumR += row[x * 4 + 0];
            sumG += row[x * 4 + 1];
            sumB += row[x * 4 + 2];
            ++n;
        }
    }
    if (n > 0) {
        m.r = sumR / static_cast<double>(n);
        m.g = sumG / static_cast<double>(n);
        m.b = sumB / static_cast<double>(n);
        m.samples = n;
    }
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <dng_path> [--min-b-minus-r N] "
                     "[--band-fraction F]\n",
                     argv[0]);
        return 2;
    }

    const char* dngPath = argv[1];
    double minBminusR = 50.0;
    double bandFraction = 0.10;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--min-b-minus-r") == 0 && i + 1 < argc) {
            minBminusR = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--band-fraction") == 0 && i + 1 < argc) {
            bandFraction = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--expect-blue-sky") == 0) {
            // Default behaviour; accepted for call-site readability.
        } else {
            std::fprintf(stderr, "[CFA COLOR] FAIL: unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    DngResult* result = dng_decode_and_process(dngPath);
    if (!result) {
        std::printf("[CFA COLOR] FAIL: decode returned null result for %s\n", dngPath);
        return 1;
    }
    if (result->error_code != 0 || !result->rgba_data ||
        result->width <= 0 || result->height <= 0) {
        std::printf("[CFA COLOR] FAIL: decode error_code=%d w=%d h=%d rgba=%p\n",
                    result->error_code, result->width, result->height,
                    static_cast<void*>(result->rgba_data));
        dng_free_result(result);
        return 1;
    }

    const ChannelMeans m = meanTopBand(result->rgba_data, result->width,
                                       result->height, bandFraction);
    const double delta = m.b - m.r;
    const bool pass = delta >= minBminusR;

    std::printf("[CFA COLOR] file=%s size=%dx%d band=%.2f samples=%lld "
                "meanR=%.2f meanG=%.2f meanB=%.2f B-R=%.2f min=%.2f [%s]\n",
                dngPath, result->width, result->height, bandFraction,
                m.samples, m.r, m.g, m.b, delta, minBminusR,
                pass ? "PASS" : "FAIL");

    dng_free_result(result);
    return pass ? 0 : 1;
}
