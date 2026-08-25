/**
 * ---
 * file_summary: "R2 sized-decode gate: output extent, memory, and crop-vs-scale proof"
 * functions:
 *   - name: "boxDownscaleU16"
 *     description: "Same-ordering CPU reference: box-average the u16 Stage3 source cell"
 *   - name: "renderReference"
 *     description: "Render a box-downscaled source through the production Stage4 AOT"
 *   - name: "runSizedCase"
 *     description: "One maxDim case: production sized decode vs same-ordering reference"
 *   - name: "main"
 *     description: "Gate maxDim 200/1024/2560 on extent, memory and PSNR"
 * ---
 *
 * test_sized_decode.cpp — R2 sized decode acceptance gate (AC5 / AC5-D / AC6).
 *
 * WHAT THIS GATES, AND WHY IT IS NOT JUST A SIZE CHECK
 * ----------------------------------------------------
 * The Stage4 device path crops its source to the DESTINATION extent
 * (dng_render_halide.cpp, non-split branch). If the sized path were wired by
 * simply feeding a small out_w/out_h, the kernel would emit the top-left corner
 * of the frame at exactly the requested dimensions — a plausible-looking image
 * that is a CROP, not a downscale. Every dimension assertion would still pass.
 *
 * So the load-bearing check here is the PSNR against a same-ordering CPU
 * reference: box-average the u16 Stage3 source into output-resolution cells,
 * then run the PRODUCTION dng_render_stage4 kernel over that. That is exactly
 * what the pre-average kernel claims to compute, so a correct implementation
 * scores very high and a crop scores in the single digits.
 *
 * The decode under test is the real production entry
 * (dng_pipeline_decode_to_rgb_sized), so the device-resident Stage3 handoff,
 * the crop-origin normalisation and the scaled dispatch are all exercised as
 * shipped — not re-implemented here.
 *
 * `RenderParams` / `buildRenderParams` have external linkage but are defined
 * only inside dng_render_halide.cpp. Including that TU is how this harness gets
 * production-identical render parameters without touching a production source.
 * This target therefore must NOT also compile dng_render_halide.cpp separately
 * (see CMakeLists.txt).
 *
 * Usage:
 *   test_sized_decode <dng_path> [--max-dims 200,1024,2560] [--threshold 55.0]
 *
 * Exit codes:
 *   0  all cases passed
 *   1  a case failed, or setup failed
 */

#include "../src/dng_render_halide.cpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_image.h>
#include <dng_render.h>

#include "dng_pipeline.h"
#include "dng_render_stage4.h"

using Halide::Runtime::Buffer;

namespace {

// AC5-D gate. Same value as the shipped kernel gate (test_stage4_scaled.cpp):
// this asks "is the sized path computing the box filter it claims", not "does
// it match the full-resolution render" (that comparison is the waived AC7).
constexpr double kPsnrThreshold = 55.0;

// A crop of the right size scores far below this; a correct downscale scores
// far above it. Recorded so a future reader knows the gate has real separation
// rather than being tuned to whatever the implementation happened to produce.
constexpr double kCropWouldScoreBelow = 20.0;

struct SizedResult {
    uint32_t width = 0;
    uint32_t height = 0;
    size_t rgba_bytes = 0;
    double wall_ms = 0.0;
    std::vector<uint8_t> rgb;  // deep copy, alpha stripped
    bool ok = false;
};

// Run the production sized decode and take an owned RGB copy.
SizedResult decodeSized(const char *path, int32_t maxDim) {
    SizedResult out;
    DngPipelineV2Result result;
    const auto t0 = std::chrono::steady_clock::now();
    const bool success =
        dng_pipeline_decode_to_rgb_sized(path, maxDim, result);
    out.wall_ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0)
                      .count();

    const uint8_t *src = result.rgba_ptr;
    size_t srcBytes = result.rgba_size;
    bool isRgba = true;
    if (!src && result.rgb_ptr) {
        src = result.rgb_ptr;
        srcBytes = result.rgb_size;
        isRgba = false;
    }
    if (!success || result.error_code != 0 || !src || result.width == 0 ||
        result.height == 0) {
        printf("  decode FAILED: error_code=%d\n", result.error_code);
        return out;
    }

    out.width = result.width;
    out.height = result.height;
    out.rgba_bytes = srcBytes;
    const size_t px = static_cast<size_t>(result.width) * result.height;
    out.rgb.resize(px * 3);
    for (size_t i = 0; i < px; ++i) {
        out.rgb[i * 3 + 0] = src[i * (isRgba ? 4 : 3) + 0];
        out.rgb[i * 3 + 1] = src[i * (isRgba ? 4 : 3) + 1];
        out.rgb[i * 3 + 2] = src[i * (isRgba ? 4 : 3) + 2];
    }
    if (result.rgba_ptr) {
        dng_rgba_output_release(result.rgba_ptr);
    } else if (result.rgb_ptr) {
        dng_rgb_output_release(result.rgb_ptr);
    }
    out.ok = true;
    return out;
}

// Same cell convention as the kernel: output pixel x covers source columns
// [x*sw/ow, (x+1)*sw/ow), an exact integer-ratio box that tiles the source.
// `mutate` swaps the average for point sampling, which must break the gate —
// the red proof that this reference is actually load-bearing.
Buffer<uint16_t> boxDownscaleU16(const Buffer<uint16_t> &src, int ow, int oh,
                                 bool mutate) {
    const int sw = src.dim(0).extent();
    const int sh = src.dim(1).extent();
    Buffer<uint16_t> out = Buffer<uint16_t>::make_interleaved(ow, oh, 3);
    for (int y = 0; y < oh; ++y) {
        const int y0 = static_cast<int>((static_cast<int64_t>(y) * sh) / oh);
        const int y1 = static_cast<int>((static_cast<int64_t>(y + 1) * sh) / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = static_cast<int>((static_cast<int64_t>(x) * sw) / ow);
            const int x1 = static_cast<int>((static_cast<int64_t>(x + 1) * sw) / ow);
            for (int c = 0; c < 3; ++c) {
                if (mutate) {
                    out(x, y, c) = src(x0, y0, c);
                    continue;
                }
                uint64_t acc = 0;
                uint64_t n = 0;
                for (int sy = y0; sy < std::max(y1, y0 + 1); ++sy) {
                    for (int sx = x0; sx < std::max(x1, x0 + 1); ++sx) {
                        acc += src(sx, sy, c);
                        ++n;
                    }
                }
                out(x, y, c) = static_cast<uint16_t>(n ? (acc / n) : 0);
            }
        }
    }
    return out;
}

double rgbPsnr(const uint8_t *a, const uint8_t *b, size_t n, int *maxAbsOut) {
    double mse = 0.0;
    int maxAbs = 0;
    for (size_t i = 0; i < n; ++i) {
        const int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
        maxAbs = std::max(maxAbs, std::abs(d));
        mse += static_cast<double>(d) * d;
    }
    if (maxAbsOut) *maxAbsOut = maxAbs;
    if (mse == 0.0) return 999.0;
    mse /= static_cast<double>(n);
    return 10.0 * log10(255.0 * 255.0 / mse);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <dng_path> [--max-dims 200,1024,2560] "
                "[--threshold 55.0] [--mutate-reference]\n",
                argv[0]);
        return 1;
    }
    const char *dngPath = argv[1];
    std::vector<int> maxDims;
    double threshold = kPsnrThreshold;
    bool mutateReference = false;
    bool measureColdStart = false;
    int ac8MaxDim = 1024;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--max-dims") && i + 1 < argc) {
            std::string s(argv[++i]);
            size_t pos = 0;
            while (pos <= s.size()) {
                const size_t comma = s.find(',', pos);
                const std::string tok =
                    s.substr(pos, comma == std::string::npos ? std::string::npos
                                                             : comma - pos);
                if (!tok.empty()) maxDims.push_back(atoi(tok.c_str()));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        } else if (!strcmp(argv[i], "--threshold") && i + 1 < argc) {
            threshold = atof(argv[++i]);
        } else if (!strcmp(argv[i], "--mutate-reference")) {
            mutateReference = true;
        } else if (!strcmp(argv[i], "--cold-start")) {
            measureColdStart = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') ac8MaxDim = atoi(argv[++i]);
        }
    }
    if (maxDims.empty()) maxDims = {200, 1024, 2560};

    printf("=== test_sized_decode ===\n");
    printf("dng=%s threshold=%.1f dB%s\n", dngPath, threshold,
           mutateReference ? "  [MUTATED REFERENCE: gate MUST go red]" : "");

    // AC8: measure the sized-path cold-start tax. There is no macOS Stage4
    // prewarm to extend — prewarm_stage4_impl's body is #if defined(__ANDROID__)
    // and a no-op elsewhere, so the production full-resolution kernel is not
    // prewarmed on macOS either. Rather than invent a macOS prewarm path (which
    // would change full-resolution warmup behaviour), the tax is measured.
    // MUST be the first work in the process, or the measurement is meaningless.
    if (measureColdStart) {
        printf("\n[AC8] cold-start tax, maxDim=%d, same process\n", ac8MaxDim);
        SizedResult cold = decodeSized(dngPath, ac8MaxDim);
        SizedResult warm = decodeSized(dngPath, ac8MaxDim);
        if (!cold.ok || !warm.ok) {
            printf("  FAIL: AC8 decode failed\n");
            return 1;
        }
        printf("  first  sized decode: %.1f ms  (%ux%u)\n", cold.wall_ms,
               cold.width, cold.height);
        printf("  second sized decode: %.1f ms  (%ux%u)\n", warm.wall_ms,
               warm.width, warm.height);
        printf("  cold-start tax: %.1f ms\n", cold.wall_ms - warm.wall_ms);
        return 0;
    }

    // ---- Stage1-3 once, for the reference source -------------------------
    dng_host host;
    AutoPtr<dng_negative> negative;
    try {
        dng_file_stream stream(dngPath);
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        negative.Reset(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        negative->ReadStage1Image(host, stream, info);
        negative->BuildStage2Image(host);
    } catch (const dng_exception &e) {
        printf("FAIL: DNG SDK exception during parse/stage2: %d\n", e.ErrorCode());
        return 1;
    }

    std::vector<uint16_t> stage3Workspace;
    if (!dng_pipeline_run_stage3(host, *negative, true, nullptr,
                                    &stage3Workspace)) {
        printf("FAIL: dng_pipeline_run_stage3 failed\n");
        return 1;
    }
    const dng_image *s3 = negative->Stage3Image();
    if (!s3 || s3->Planes() < 3) {
        printf("FAIL: no usable Stage3Image\n");
        return 1;
    }

    // The production device path renders DefaultCropArea, so the reference must
    // read exactly that region — not the whole Stage3 image.
    const dng_rect crop = negative->DefaultCropArea();
    const int sw = crop.W();
    const int sh = crop.H();
    printf("Stage3 crop area: %dx%d (offset %d,%d)\n", sw, sh,
           static_cast<int>(crop.l), static_cast<int>(crop.t));

    Buffer<uint16_t> src = Buffer<uint16_t>::make_interleaved(sw, sh, 3);
    {
        dng_const_tile_buffer tile(*s3, crop);
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
                for (int c = 0; c < 3; ++c)
                    src(x, y, c) =
                        *tile.ConstPixel_uint16(crop.t + y, crop.l + x, c);
    }
    {   // A constant source would make every PSNR below meaningless.
        uint16_t lo = 65535, hi = 0;
        for (int y = 0; y < sh; y += 37)
            for (int x = 0; x < sw; x += 37)
                for (int c = 0; c < 3; ++c) {
                    lo = std::min(lo, src(x, y, c));
                    hi = std::max(hi, src(x, y, c));
                }
        printf("src u16 range over sampled grid: [%u, %u]\n", lo, hi);
        if (lo == hi) {
            printf("FAIL: Stage3 source is constant -- the read is wrong, "
                   "not the pipeline\n");
            return 1;
        }
    }

    // ---- production render parameters ------------------------------------
    dng_render refRenderer(host, *negative);
    const PipelineConfig config = PipelineConfig::loadFromEnv();
    RenderParams params;
    if (!buildRenderParams(host, *negative, refRenderer, config, params)) {
        printf("FAIL: buildRenderParams failed\n");
        return 1;
    }

    // ---- full-resolution baseline for the AC6 memory comparison ----------
    SizedResult full = decodeSized(dngPath, 0);
    if (!full.ok) {
        printf("FAIL: full-resolution decode failed\n");
        return 1;
    }
    printf("full-res decode: %ux%u  rgba_bytes=%zu (%.1f MB)\n\n", full.width,
           full.height, full.rgba_bytes,
           full.rgba_bytes / (1024.0 * 1024.0));

    bool allPass = true;
    for (int maxDim : maxDims) {
        printf("[maxDim=%d]\n", maxDim);

        // Expected extent straight from the production size computation, so the
        // gate cannot drift from the SDK's own aspect-ratio rounding.
        dng_render sizedRenderer(host, *negative);
        sizedRenderer.SetMaximumSize(static_cast<uint32>(maxDim));
        sizedRenderer.SetFinalPixelType(ttByte);
        sizedRenderer.SetFinalSpace(dng_space_sRGB::Get());
        uint32_t expW = 0, expH = 0;
        dng_render_stage4_output_size(*negative, sizedRenderer, expW, expH);

        SizedResult got = decodeSized(dngPath, maxDim);
        if (!got.ok) {
            printf("  FAIL: sized decode failed\n\n");
            allPass = false;
            continue;
        }
        printf("  output %ux%u  expected %ux%u  rgba_bytes=%zu (%.2f MB)\n",
               got.width, got.height, expW, expH, got.rgba_bytes,
               got.rgba_bytes / (1024.0 * 1024.0));

        // A request at or above the sensor long edge must CLAMP to full
        // resolution, never upscale. That is a property worth covering
        // permanently, so it is a first-class expectation here rather than a
        // failure: without this branch the harness reports FAIL on correct
        // behaviour, and whoever runs it next reads that as a real defect.
        const uint32_t fullLong = std::max(full.width, full.height);
        const bool clamped = static_cast<uint32_t>(maxDim) >= fullLong;
        const uint32_t wantLong =
            clamped ? fullLong : static_cast<uint32_t>(maxDim);
        if (clamped) {
            printf("  [clamped] maxDim %d >= sensor long edge %u -> expect full "
                   "resolution, no upscale\n",
                   maxDim, fullLong);
        }

        // AC5: long edge within 1 px of what the request resolves to.
        const uint32_t gotLong = std::max(got.width, got.height);
        const uint32_t expLong = std::max(expW, expH);
        bool casePass = true;
        if (std::abs(static_cast<int64_t>(gotLong) -
                     static_cast<int64_t>(expLong)) > 1) {
            printf("  FAIL AC5: long edge %u, expected %u (+/-1)\n", gotLong,
                   expLong);
            casePass = false;
        }
        if (std::abs(static_cast<int64_t>(gotLong) -
                     static_cast<int64_t>(wantLong)) > 1) {
            printf("  FAIL AC5: long edge %u does not honour maxDim %d "
                   "(expected %u, +/-1)\n",
                   gotLong, maxDim, wantLong);
            casePass = false;
        }
        if (clamped && (got.width != full.width || got.height != full.height)) {
            printf("  FAIL AC5: clamped request returned %ux%u, expected the "
                   "full-resolution %ux%u -- an upscale or resize was attempted\n",
                   got.width, got.height, full.width, full.height);
            casePass = false;
        }

        // AC6: output buffer shrinks in proportion to the pixel count. A
        // clamped request is full resolution by definition, so "smaller than
        // full-res" does not apply to it — it must be exactly equal instead.
        const double pixelRatio =
            (static_cast<double>(got.width) * got.height) /
            (static_cast<double>(full.width) * full.height);
        const double byteRatio = static_cast<double>(full.rgba_bytes) > 0.0
                                     ? static_cast<double>(got.rgba_bytes) /
                                           static_cast<double>(full.rgba_bytes)
                                     : 0.0;
        printf("  AC6 pixel_ratio=%.5f byte_ratio=%.5f (%.1fx smaller)\n",
               pixelRatio, byteRatio,
               byteRatio > 0.0 ? 1.0 / byteRatio : 0.0);
        if (clamped) {
            if (got.rgba_bytes != full.rgba_bytes) {
                printf("  FAIL AC6: clamped request allocated %zu bytes, "
                       "expected the full-resolution %zu\n",
                       got.rgba_bytes, full.rgba_bytes);
                casePass = false;
            }
        } else if (got.rgba_bytes >= full.rgba_bytes) {
            printf("  FAIL AC6: sized buffer is not smaller than full-res\n");
            casePass = false;
        }

        // AC5-D: same-ordering reference, the crop-vs-scale discriminator.
        Buffer<uint16_t> refSrc =
            boxDownscaleU16(src, static_cast<int>(got.width),
                            static_cast<int>(got.height), mutateReference);
        refSrc.set_host_dirty();
        Buffer<uint8_t> refDst = Buffer<uint8_t>::make_interleaved(
            static_cast<int>(got.width), static_cast<int>(got.height), 4);
        // Reuse the production Stage4 wrappers from the included TU so the
        // reference runs the exact colour math the kernel under test runs.
        const int rw = static_cast<int>(got.width);
        const int rh = static_cast<int>(got.height);
        if (!runRenderStage4HalideAot(refSrc.data(), rw, rh, /*src_p=*/3,
                                      /*src_row_step=*/rw * 3,
                                      /*src_col_step=*/3,
                                      /*src_plane_step=*/1, 1.0f / 65535.0f, rw,
                                      rh, params, refDst.data(),
                                      /*fuse_rgba=*/true)) {
            printf("  FAIL: reference render failed\n\n");
            allPass = false;
            continue;
        }

        const size_t px = static_cast<size_t>(got.width) * got.height;
        std::vector<uint8_t> refRgb(px * 3);
        for (size_t i = 0; i < px; ++i) {
            refRgb[i * 3 + 0] = refDst.data()[i * 4 + 0];
            refRgb[i * 3 + 1] = refDst.data()[i * 4 + 1];
            refRgb[i * 3 + 2] = refDst.data()[i * 4 + 2];
        }
        int maxAbs = 0;
        const double psnr =
            rgbPsnr(got.rgb.data(), refRgb.data(), px * 3, &maxAbs);
        printf("  AC5-D PSNR(sized device route vs same-ordering CPU ref) = "
               "%.2f dB  maxAbs=%d\n",
               psnr, maxAbs);
        if (psnr < threshold) {
            printf("  FAIL AC5-D: %.2f dB < %.2f dB threshold%s\n", psnr,
                   threshold,
                   psnr < kCropWouldScoreBelow
                       ? "  (this low = the output is probably a CROP)"
                       : "");
            casePass = false;
        }

        printf("  %s\n\n", casePass ? "[PASS]" : "[FAIL]");
        if (!casePass) allPass = false;
    }

    printf("=== Summary ===\n");
    if (mutateReference) {
        // Red proof: with a mutated reference the gate must NOT be green.
        printf("MUTATION CHECK: %s\n",
               allPass ? "BAD -- gate stayed green against a point-sampled "
                         "reference, so it does not test the box filter"
                       : "GOOD -- gate went red as required");
        return allPass ? 1 : 0;
    }
    printf("OVERALL=%s\n", allPass ? "PASS" : "FAIL");
    return allPass ? 0 : 1;
}
