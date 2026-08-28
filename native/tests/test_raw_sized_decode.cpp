/**
 * ---
 * file_summary: "LibRaw-path scaled-decode gate: output extent + crop-vs-scale proof"
 * functions:
 *   - name: "boxDownscaleRgba"
 *     description: "Area box-average an RGBA8 image into a smaller RGBA8 image"
 *   - name: "psnrRgb"
 *     description: "PSNR over the RGB channels of two equally-sized RGBA8 images"
 *   - name: "runSizedCase"
 *     description: "Decode one RAW file full + sized, gate extent and crop-vs-scale"
 *   - name: "main"
 *     description: "Gate every RAW file passed on argv; --simulate-crop proves the discriminator"
 * ---
 *
 * test_raw_sized_decode.cpp — scaled decode acceptance gate for the LibRaw path.
 *
 * WHAT THIS GATES, AND WHY IT IS NOT JUST A SIZE CHECK
 * ----------------------------------------------------
 * The raw render branches (raw_gpu_pipeline.cpp) feed the SHARED Stage4 entry.
 * If scaled decode were wired by simply asking for a small out_w/out_h against
 * an unchanged (== src) src_w/src_h, the kernel would emit the top-left CORNER
 * of the frame at the requested dimensions — a plausible-looking image that is a
 * CROP, not a downscale. Every dimension assertion would still pass.
 *
 * So the load-bearing check is a crop-vs-scale discriminator: from the full-res
 * decode we build TWO references at the sized extent — an area box-average
 * (what a real downscale must resemble) and a top-left crop (what the broken
 * wiring produces) — and require the sized decode to match the box-average
 * MARGIN dB better than it matches the crop. This is deliberately a relative
 * test: the production scaled kernel pre-averages the Stage3 buffer before the
 * Stage4 tone curve, whereas the box-average reference here averages the final
 * RGBA, so an absolute PSNR threshold would be brittle. The RELATIVE gap
 * between "looks like a downscale" and "looks like a crop" is large and stable
 * for any natural image, so the margin test is robust while still failing hard
 * on a crop.
 *
 * The decode under test is the real production C ABI (raw_decode_and_process),
 * linked from the shipped dylib, so the device-resident Stage3 handoff, the
 * scaled Stage4 dispatch and the RGBA pool are all exercised as shipped.
 *
 * --simulate-crop feeds the crop reference itself as the "candidate": the
 * discriminator must then REJECT it (nonzero exit). That is the red half of the
 * red->green evidence and needs no separate build — it proves the gate catches
 * the exact failure mode (a crop) that the extent checks alone cannot see.
 *
 * Usage:
 *   test_raw_sized_decode <raw_file> [<raw_file> ...] [--simulate-crop]
 *
 * Exit codes:
 *   0  every file passed (or, with --simulate-crop, the discriminator rejected
 *      the crop as required)
 *   1  a case failed, or setup/decoding failed
 *   2  no usable file was processed
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "raw_ffi_api.h"

namespace {

constexpr double kCropVsScaleMarginDb = 6.0;

struct Rgba {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;  // w*h*4, RGBA
};

// Area box-average src into a dst of (dw x dh). Each dst pixel averages the
// source rectangle it covers; the same-ordering downscale a correct sized
// decode must resemble.
Rgba boxDownscaleRgba(const Rgba& src, int dw, int dh) {
    Rgba dst;
    dst.w = dw;
    dst.h = dh;
    dst.px.assign(static_cast<size_t>(dw) * dh * 4, 0);
    for (int oy = 0; oy < dh; ++oy) {
        const int sy0 = static_cast<int>(static_cast<int64_t>(oy) * src.h / dh);
        int sy1 = static_cast<int>(static_cast<int64_t>(oy + 1) * src.h / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int ox = 0; ox < dw; ++ox) {
            const int sx0 = static_cast<int>(static_cast<int64_t>(ox) * src.w / dw);
            int sx1 = static_cast<int>(static_cast<int64_t>(ox + 1) * src.w / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint64_t acc[3] = {0, 0, 0};
            uint64_t n = 0;
            for (int sy = sy0; sy < sy1 && sy < src.h; ++sy) {
                const uint8_t* row = &src.px[(static_cast<size_t>(sy) * src.w) * 4];
                for (int sx = sx0; sx < sx1 && sx < src.w; ++sx) {
                    const uint8_t* p = &row[static_cast<size_t>(sx) * 4];
                    acc[0] += p[0];
                    acc[1] += p[1];
                    acc[2] += p[2];
                    ++n;
                }
            }
            uint8_t* o = &dst.px[(static_cast<size_t>(oy) * dw + ox) * 4];
            if (n == 0) n = 1;
            o[0] = static_cast<uint8_t>(acc[0] / n);
            o[1] = static_cast<uint8_t>(acc[1] / n);
            o[2] = static_cast<uint8_t>(acc[2] / n);
            o[3] = 255;
        }
    }
    return dst;
}

// Top-left crop of src at (dw x dh): what the broken (unscaled src, small dst)
// wiring produces.
Rgba topLeftCrop(const Rgba& src, int dw, int dh) {
    Rgba dst;
    dst.w = dw;
    dst.h = dh;
    dst.px.assign(static_cast<size_t>(dw) * dh * 4, 0);
    for (int y = 0; y < dh && y < src.h; ++y) {
        const uint8_t* srow = &src.px[(static_cast<size_t>(y) * src.w) * 4];
        uint8_t* drow = &dst.px[(static_cast<size_t>(y) * dw) * 4];
        const int copyw = std::min(dw, src.w);
        std::memcpy(drow, srow, static_cast<size_t>(copyw) * 4);
    }
    return dst;
}

double psnrRgb(const Rgba& a, const Rgba& b) {
    if (a.w != b.w || a.h != b.h) return 0.0;
    double mse = 0.0;
    const size_t n = static_cast<size_t>(a.w) * a.h;
    for (size_t i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const double d = static_cast<double>(a.px[i * 4 + c]) -
                             static_cast<double>(b.px[i * 4 + c]);
            mse += d * d;
        }
    }
    mse /= static_cast<double>(n * 3);
    if (mse <= 1e-9) return 99.0;
    const double psnr = 10.0 * std::log10(255.0 * 255.0 / mse);
    return std::min(psnr, 99.0);
}

bool decodeRaw(const char* path, int max_dim, Rgba* out) {
    DngResult* r = raw_decode_and_process(path, max_dim);
    if (!r) {
        std::fprintf(stderr, "  raw_decode_and_process returned NULL\n");
        return false;
    }
    bool ok = false;
    if (r->error_code == 0 && r->rgba_data && r->width > 0 && r->height > 0) {
        out->w = r->width;
        out->h = r->height;
        out->px.assign(r->rgba_data,
                       r->rgba_data + static_cast<size_t>(r->width) * r->height * 4);
        ok = true;
    } else {
        std::fprintf(stderr, "  decode failed: error_code=%d w=%d h=%d rgba=%p\n",
                     r->error_code, r->width, r->height,
                     static_cast<void*>(r->rgba_data));
    }
    dng_free_result(r);
    return ok;
}

// Returns: 0 pass, 1 fail. simulate_crop feeds the crop reference as the
// candidate to prove the discriminator rejects a crop.
int runSizedCase(const char* path, bool simulate_crop) {
    std::printf("[RawSized] %s%s\n", path, simulate_crop ? "  (--simulate-crop)" : "");

    Rgba full;
    if (!decodeRaw(path, 0, &full)) {
        std::printf("  FAIL: full-res decode failed\n");
        return 1;
    }
    const int long_edge = std::max(full.w, full.h);
    // A clear downscale, large enough that the box-average reference is stable.
    const int target = std::max(64, long_edge / 3);
    if (target >= long_edge) {
        std::printf("  FAIL: image too small to test a downscale (long edge %d)\n",
                    long_edge);
        return 1;
    }

    Rgba candidate;
    int cw, ch;
    if (simulate_crop) {
        // Expected sized extent, computed the same way the pipeline does
        // (scale both edges by target/long_edge, round to nearest, clamp >=1).
        const double s = static_cast<double>(target) / long_edge;
        cw = std::max(1, static_cast<int>(std::llround(full.w * s)));
        ch = std::max(1, static_cast<int>(std::llround(full.h * s)));
        candidate = topLeftCrop(full, cw, ch);  // the broken behaviour
    } else {
        if (!decodeRaw(path, target, &candidate)) {
            std::printf("  FAIL: sized decode failed\n");
            return 1;
        }
        cw = candidate.w;
        ch = candidate.h;
        // Extent gate: long edge matches the requested cap; aspect preserved.
        const int cand_long = std::max(cw, ch);
        if (std::abs(cand_long - target) > 1) {
            std::printf("  FAIL: sized long edge %d != requested %d\n",
                        cand_long, target);
            return 1;
        }
        const double src_ar = static_cast<double>(full.w) / full.h;
        const double dst_ar = static_cast<double>(cw) / ch;
        if (std::fabs(src_ar - dst_ar) > 0.02) {
            std::printf("  FAIL: aspect changed src=%.4f dst=%.4f\n", src_ar, dst_ar);
            return 1;
        }
    }

    const Rgba ref_scale = boxDownscaleRgba(full, cw, ch);
    const Rgba ref_crop = topLeftCrop(full, cw, ch);
    const double psnr_scale = psnrRgb(candidate, ref_scale);
    const double psnr_crop = psnrRgb(candidate, ref_crop);
    std::printf("  extent %dx%d -> %dx%d  psnr(scale)=%.2f dB  psnr(crop)=%.2f dB\n",
                full.w, full.h, cw, ch, psnr_scale, psnr_crop);

    const bool looks_like_scale = psnr_scale >= psnr_crop + kCropVsScaleMarginDb;
    if (simulate_crop) {
        // The candidate IS a crop: the discriminator MUST reject it.
        if (looks_like_scale) {
            std::printf("  FAIL: discriminator accepted a crop (margin too loose)\n");
            return 1;
        }
        std::printf("  OK: discriminator rejected the crop as required\n");
        return 0;
    }
    if (!looks_like_scale) {
        std::printf("  FAIL: sized decode resembles a CROP, not a downscale "
                    "(need scale >= crop + %.1f dB)\n", kCropVsScaleMarginDb);
        return 1;
    }
    std::printf("  PASS\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    bool simulate_crop = false;
    std::vector<const char*> files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--simulate-crop") == 0) {
            simulate_crop = true;
        } else {
            files.push_back(argv[i]);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr,
                     "usage: %s <raw_file> [<raw_file> ...] [--simulate-crop]\n",
                     argv[0]);
        return 2;
    }

    int failures = 0;
    int processed = 0;
    for (const char* f : files) {
        FILE* fp = std::fopen(f, "rb");
        if (!fp) {
            std::printf("[RawSized] SKIP (absent): %s\n", f);
            continue;
        }
        std::fclose(fp);
        ++processed;
        failures += runSizedCase(f, simulate_crop);
    }

    if (processed == 0) {
        std::fprintf(stderr, "[RawSized] no usable file processed\n");
        return 2;
    }
    if (failures) {
        std::printf("[RawSized] FAIL (%d of %d cases)\n", failures, processed);
        return 1;
    }
    std::printf("[RawSized] ALL PASS (%d cases)\n", processed);
    return 0;
}
