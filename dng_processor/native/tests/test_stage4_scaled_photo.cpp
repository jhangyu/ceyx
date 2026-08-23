// test_stage4_scaled_photo.cpp — real-photograph AC7 measurement for the two
// sized-Stage4 kernel variants, plus viewable image output.
//
// WHY THIS EXISTS
// ---------------
// The frozen contract AC7 defines the reference as a box downscale of the
// full-resolution 8-bit output. Two kernels are in the tree:
//
//   Variant A  dng_render_stage4_scaled_preavg — averages the u16 Stage3
//              source BEFORE the colour math. Colour pipeline then runs at
//              output resolution, which is where the handover §3.1 ~192 ms
//              Stage4 saving comes from. Diverges from AC7's reference because
//              the tone curve and encode gamma are non-linear.
//   Variant B  dng_render_stage4_scaled — averages AFTER the colour math.
//              Matches AC7's reference by construction, but the colour
//              pipeline still runs at sensor resolution, so no time saving.
//
// On synthetic sources Variant A measured 27.4 dB against AC7 on a
// high-contrast image and 60-67 dB on a smooth one — i.e. its AC7 failure is
// content-dependent. This harness puts a real photograph between those two
// bounds and writes images the decision-maker can actually look at.
//
// WHAT IS CONTROLLED
// ------------------
// All three images come from the SAME real Stage3 buffer and the SAME
// production render parameters (`buildRenderParams`, the exact function the
// production path uses). The full-resolution reference render uses the
// production `dng_render_stage4` AOT. So the ONLY difference between the two
// variants and the reference is where the box filter sits. Nothing else varies.
//
// THIS IS NOT THE PRODUCTION PATH. Neither scaled kernel is wired into
// dng_pipeline_v2 — that is R2 work. This is a standalone harness that drives
// the AOT kernels directly. Do not read its output as shipped-pipeline output.
//
// Usage:
//   test_stage4_scaled_photo <dng_path> [--max-dim N] [--out-dir DIR]

// `RenderParams` and `buildRenderParams` have external linkage but are defined
// only inside this translation unit, which is off-limits for editing this
// round. Including it is how the harness gets production-identical render
// parameters without modifying a single production source file. This target
// therefore must NOT also compile dng_render_halide.cpp separately.
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
#include <dng_pixel_buffer.h>
#include <dng_render.h>

#include "dng_pipeline_v2.h"
#include "dng_render_stage4.h"
#include "dng_render_stage4_scaled.h"
#include "dng_render_stage4_scaled_preavg.h"

using Halide::Runtime::Buffer;

namespace {

struct Rgba {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // interleaved RGBA
    uint8_t at(int x, int y, int c) const {
        return px[(static_cast<size_t>(y) * w + x) * 4 + c];
    }
};

// ---------------------------------------------------------------------------
// Halide buffer wrappers over the production RenderParams
// ---------------------------------------------------------------------------

struct HalideParams {
    Buffer<float> exp_ramp, tone_curve, encode_gamma;
    Buffer<float> camera_white, camera_to_rgb, rgb_to_final;
    Buffer<float> huesat_table, huesat_encode, huesat_decode;
    Buffer<float> look_table, look_encode, look_decode;
    const RenderParams *p = nullptr;
};

Buffer<float> wrap1(std::vector<float> &v) {
    return Buffer<float>(v.data(), static_cast<int>(v.size()));
}

// [entry, component] with entry fastest, matching the flat [comp*n + entry]
// layout buildRenderParams writes.
Buffer<float> wrapTable(std::vector<float> &v, int entries) {
    if (entries <= 0 || v.empty()) {
        static std::vector<float> dummy(3, 0.0f);
        return Buffer<float>(dummy.data(), 1, 3);
    }
    return Buffer<float>(v.data(), entries, 3);
}

HalideParams makeHalideParams(RenderParams &p) {
    HalideParams h;
    h.p = &p;
    h.exp_ramp = wrap1(p.exp_ramp);
    h.tone_curve = wrap1(p.tone_curve);
    h.encode_gamma = wrap1(p.encode_gamma);
    h.camera_white = Buffer<float>(p.camera_white, 3);
    // float[9] is row-major [row*3+col]; dim0 (fastest) is therefore col,
    // which is the [col, row] indexing the generator expects.
    h.camera_to_rgb = Buffer<float>(p.camera_to_rgb, 3, 3);
    h.rgb_to_final = Buffer<float>(p.rgb_to_final, 3, 3);

    const int hs_entries = p.huesat_hue_div * p.huesat_sat_div * p.huesat_val_div;
    const int lk_entries = p.look_hue_div * p.look_sat_div * p.look_val_div;
    h.huesat_table = wrapTable(p.huesat_table, hs_entries);
    h.look_table = wrapTable(p.look_table, lk_entries);
    h.huesat_encode = wrap1(p.huesat_encode);
    h.huesat_decode = wrap1(p.huesat_decode);
    h.look_encode = wrap1(p.look_encode);
    h.look_decode = wrap1(p.look_decode);

    // REQUIRED. A Halide::Runtime::Buffer wrapping caller-owned host memory
    // starts with host_dirty == false, so a GPU pipeline assumes the device
    // copy is current and uploads nothing — every table reads as zero and the
    // render comes out black. Production does exactly this at
    // dng_render_halide.cpp:1040-1044; omitting it was silent, not an error.
    h.exp_ramp.set_host_dirty();
    h.tone_curve.set_host_dirty();
    h.encode_gamma.set_host_dirty();
    h.camera_white.set_host_dirty();
    h.camera_to_rgb.set_host_dirty();
    h.rgb_to_final.set_host_dirty();
    h.huesat_table.set_host_dirty();
    h.huesat_encode.set_host_dirty();
    h.huesat_decode.set_host_dirty();
    h.look_table.set_host_dirty();
    h.look_encode.set_host_dirty();
    h.look_decode.set_host_dirty();
    return h;
}

int runFullRes(Buffer<uint16_t> &src, HalideParams &h, Buffer<uint8_t> &dst) {
    const RenderParams &p = *h.p;
    return dng_render_stage4(src, 1.0f / 65535.0f,
                             h.exp_ramp, h.tone_curve, h.encode_gamma,
                             h.camera_white, h.camera_to_rgb, h.rgb_to_final,
                             h.huesat_table, h.huesat_encode, h.huesat_decode,
                             p.huesat_hue_div, p.huesat_sat_div, p.huesat_val_div,
                             p.huesat_has_table, p.huesat_has_encoding,
                             h.look_table, h.look_encode, h.look_decode,
                             p.look_hue_div, p.look_sat_div, p.look_val_div,
                             p.look_has_table, p.look_has_encoding,
                             dst);
}

int runVariantA(Buffer<uint16_t> &src, int ow, int oh, HalideParams &h, Buffer<uint8_t> &dst) {
    const RenderParams &p = *h.p;
    return dng_render_stage4_scaled_preavg(src, 1.0f / 65535.0f, ow, oh,
                             h.exp_ramp, h.tone_curve, h.encode_gamma,
                             h.camera_white, h.camera_to_rgb, h.rgb_to_final,
                             h.huesat_table, h.huesat_encode, h.huesat_decode,
                             p.huesat_hue_div, p.huesat_sat_div, p.huesat_val_div,
                             p.huesat_has_table, p.huesat_has_encoding,
                             h.look_table, h.look_encode, h.look_decode,
                             p.look_hue_div, p.look_sat_div, p.look_val_div,
                             p.look_has_table, p.look_has_encoding,
                             dst);
}

int runVariantB(Buffer<uint16_t> &src, int ow, int oh, HalideParams &h, Buffer<uint8_t> &dst) {
    const RenderParams &p = *h.p;
    return dng_render_stage4_scaled(src, 1.0f / 65535.0f, ow, oh,
                             h.exp_ramp, h.tone_curve, h.encode_gamma,
                             h.camera_white, h.camera_to_rgb, h.rgb_to_final,
                             h.huesat_table, h.huesat_encode, h.huesat_decode,
                             p.huesat_hue_div, p.huesat_sat_div, p.huesat_val_div,
                             p.huesat_has_table, p.huesat_has_encoding,
                             h.look_table, h.look_encode, h.look_decode,
                             p.look_hue_div, p.look_sat_div, p.look_val_div,
                             p.look_has_table, p.look_has_encoding,
                             dst);
}

// ---------------------------------------------------------------------------
// AC7 reference: box downscale of the full-res 8-bit output (PIL-equivalent)
// ---------------------------------------------------------------------------

Rgba boxDownscale8(const Rgba &src, int ow, int oh) {
    Rgba out;
    out.w = ow;
    out.h = oh;
    out.px.assign(static_cast<size_t>(ow) * oh * 4, 255);
    for (int y = 0; y < oh; ++y) {
        const int y0 = static_cast<int>((static_cast<int64_t>(y) * src.h) / oh);
        int y1 = static_cast<int>((static_cast<int64_t>(y + 1) * src.h) / oh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < ow; ++x) {
            const int x0 = static_cast<int>((static_cast<int64_t>(x) * src.w) / ow);
            int x1 = static_cast<int>((static_cast<int64_t>(x + 1) * src.w) / ow);
            if (x1 <= x0) x1 = x0 + 1;
            for (int c = 0; c < 3; ++c) {
                double acc = 0.0;
                for (int sy = y0; sy < y1; ++sy)
                    for (int sx = x0; sx < x1; ++sx)
                        acc += src.at(sx, sy, c);
                const double n = static_cast<double>(x1 - x0) * (y1 - y0);
                double v = acc / n;
                if (v < 0.0) v = 0.0;
                if (v > 255.0) v = 255.0;
                out.px[(static_cast<size_t>(y) * ow + x) * 4 + c] =
                    static_cast<uint8_t>(v + 0.5);
            }
        }
    }
    return out;
}

double psnrRgb(const Rgba &a, const Rgba &b, int *max_abs_out) {
    double sse = 0.0;
    int max_abs = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            for (int c = 0; c < 3; ++c) {
                const int d = static_cast<int>(a.at(x, y, c)) - static_cast<int>(b.at(x, y, c));
                sse += static_cast<double>(d) * d;
                max_abs = std::max(max_abs, std::abs(d));
            }
    *max_abs_out = max_abs;
    if (sse == 0.0) return 999.0;
    const double mse = sse / (static_cast<double>(a.w) * a.h * 3.0);
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

Rgba fromBuffer(Buffer<uint8_t> &b) {
    Rgba r;
    r.w = b.dim(0).extent();
    r.h = b.dim(1).extent();
    r.px.assign(static_cast<size_t>(r.w) * r.h * 4, 255);
    for (int y = 0; y < r.h; ++y)
        for (int x = 0; x < r.w; ++x)
            for (int c = 0; c < 4; ++c)
                r.px[(static_cast<size_t>(y) * r.w + x) * 4 + c] = b(x, y, c);
    return r;
}

bool writePpm(const std::string &path, const Rgba &img) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", img.w, img.h);
    std::vector<uint8_t> row(static_cast<size_t>(img.w) * 3);
    for (int y = 0; y < img.h; ++y) {
        for (int x = 0; x < img.w; ++x)
            for (int c = 0; c < 3; ++c) row[x * 3 + c] = img.at(x, y, c);
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

Rgba crop(const Rgba &src, int x0, int y0, int w, int h) {
    Rgba out;
    out.w = w;
    out.h = h;
    out.px.assign(static_cast<size_t>(w) * h * 4, 255);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c)
                out.px[(static_cast<size_t>(y) * w + x) * 4 + c] = src.at(x0 + x, y0 + y, c);
    return out;
}

// Amplified |A - ref|, so a difference invisible at 1x is visible on screen.
Rgba absDiff(const Rgba &a, const Rgba &b, int gain) {
    Rgba out;
    out.w = a.w;
    out.h = a.h;
    out.px.assign(static_cast<size_t>(a.w) * a.h * 4, 255);
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            for (int c = 0; c < 3; ++c) {
                int d = std::abs(static_cast<int>(a.at(x, y, c)) - static_cast<int>(b.at(x, y, c))) * gain;
                out.px[(static_cast<size_t>(y) * a.w + x) * 4 + c] =
                    static_cast<uint8_t>(std::min(d, 255));
            }
    return out;
}

// Mechanical crop selection: mean per-channel luma variance over a sliding
// window. Reported, not hand-picked, so it cannot be chosen to flatter either
// variant. Returns the highest-variance (most detailed) and lowest-variance
// (smoothest) window origins.
void pickCrops(const Rgba &img, int win, int step,
               int *hx, int *hy, int *lx, int *ly) {
    double best = -1.0, worst = 1e30;
    *hx = *hy = *lx = *ly = 0;
    for (int y = 0; y + win <= img.h; y += step) {
        for (int x = 0; x + win <= img.w; x += step) {
            double sum = 0.0, sum2 = 0.0;
            for (int yy = y; yy < y + win; yy += 2)
                for (int xx = x; xx < x + win; xx += 2) {
                    const double l = 0.299 * img.at(xx, yy, 0) +
                                     0.587 * img.at(xx, yy, 1) +
                                     0.114 * img.at(xx, yy, 2);
                    sum += l;
                    sum2 += l * l;
                }
            const double n = static_cast<double>((win / 2)) * (win / 2);
            const double var = sum2 / n - (sum / n) * (sum / n);
            if (var > best) { best = var; *hx = x; *hy = y; }
            if (var < worst) { worst = var; *lx = x; *ly = y; }
        }
    }
    printf("  crop selection: highest-variance window at (%d,%d) var=%.1f; "
           "lowest-variance at (%d,%d) var=%.1f\n", *hx, *hy, best, *lx, *ly, worst);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: %s <dng_path> [--max-dim N] [--out-dir DIR]\n", argv[0]);
        return 2;
    }
    const char *dngPath = argv[1];
    int maxDim = 1024;
    std::string outDir = "tmp/verify/visual";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--max-dim" && i + 1 < argc) maxDim = atoi(argv[++i]);
        else if (a == "--out-dir" && i + 1 < argc) outDir = argv[++i];
    }

    printf("=== test_stage4_scaled_photo ===\n");
    printf("NOT the production path: this harness drives the AOT kernels directly.\n");
    printf("dng=%s maxDim=%d outDir=%s\n", dngPath, maxDim, outDir.c_str());

    // ---- real DNG through Stage1-3 -------------------------------------
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
    if (!dng_pipeline_v2_run_stage3(host, *negative, true, nullptr, &stage3Workspace)) {
        printf("FAIL: dng_pipeline_v2_run_stage3 failed\n");
        return 1;
    }

    const dng_image *s3 = negative->Stage3Image();
    if (!s3) {
        printf("FAIL: no Stage3Image\n");
        return 1;
    }
    const dng_rect bounds = s3->Bounds();
    const int sw = bounds.W();
    const int sh = bounds.H();
    printf("Stage3: %dx%d planes=%u\n", sw, sh, s3->Planes());
    if (s3->Planes() < 3) {
        printf("FAIL: Stage3 has %u planes, need 3\n", s3->Planes());
        return 1;
    }

    // Interleaved u16 RGB, the layout the Stage4 kernels declare.
    Buffer<uint16_t> src = Buffer<uint16_t>::make_interleaved(sw, sh, 3);
    {
        dng_const_tile_buffer tile(*s3, bounds);
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
                for (int c = 0; c < 3; ++c)
                    src(x, y, c) = *tile.ConstPixel_uint16(bounds.t + y, bounds.l + x, c);
    }

    {   // Sanity: a constant source would make every PSNR below meaningless.
        uint16_t lo = 65535, hi = 0;
        for (int y = 0; y < sh; y += 37)
            for (int x = 0; x < sw; x += 37)
                for (int c = 0; c < 3; ++c) {
                    lo = std::min(lo, src(x, y, c));
                    hi = std::max(hi, src(x, y, c));
                }
        printf("src u16 range over sampled grid: [%u, %u]  centre px = (%u,%u,%u)\n",
               lo, hi, src(sw / 2, sh / 2, 0), src(sw / 2, sh / 2, 1), src(sw / 2, sh / 2, 2));
        if (lo == hi) {
            printf("FAIL: Stage3 source is constant -- the read is wrong, not the kernels\n");
            return 1;
        }
    }

    src.set_host_dirty();

    // ---- production render parameters ----------------------------------
    dng_render renderer(host, *negative);
    const PipelineConfig config = PipelineConfig::loadFromEnv();
    RenderParams params;
    if (!buildRenderParams(host, *negative, renderer, config, params)) {
        printf("FAIL: buildRenderParams failed\n");
        return 1;
    }
    HalideParams hp = makeHalideParams(params);
    printf("param sizes: exp=%zu tone=%zu gamma=%zu hs_tbl=%zu hs_enc=%zu lk_tbl=%zu lk_enc=%zu\n",
           params.exp_ramp.size(), params.tone_curve.size(), params.encode_gamma.size(),
           params.huesat_table.size(), params.huesat_encode.size(),
           params.look_table.size(), params.look_encode.size());
    if (!params.encode_gamma.empty())
        printf("gamma[0,1,2048,last]= %.4f %.4f %.4f %.4f\n",
               params.encode_gamma[0], params.encode_gamma[1],
               params.encode_gamma[params.encode_gamma.size() / 2],
               params.encode_gamma.back());
    if (!params.tone_curve.empty())
        printf("tone[0,mid,last]= %.4f %.4f %.4f\n", params.tone_curve[0],
               params.tone_curve[params.tone_curve.size() / 2], params.tone_curve.back());
    printf("white=%.4f,%.4f,%.4f  cam2rgb row0=%.4f,%.4f,%.4f\n",
           params.camera_white[0], params.camera_white[1], params.camera_white[2],
           params.camera_to_rgb[0], params.camera_to_rgb[1], params.camera_to_rgb[2]);
    printf("params: huesat %dx%dx%d has=%d  look %dx%dx%d has=%d\n",
           params.huesat_hue_div, params.huesat_sat_div, params.huesat_val_div,
           params.huesat_has_table,
           params.look_hue_div, params.look_sat_div, params.look_val_div,
           params.look_has_table);

    // ---- full-resolution render (basis of the AC7 reference) -----------
    Buffer<uint8_t> fullBuf = Buffer<uint8_t>::make_interleaved(sw, sh, 4);
    if (runFullRes(src, hp, fullBuf) != 0) {
        printf("FAIL: full-res dng_render_stage4 returned nonzero\n");
        return 1;
    }
    fullBuf.copy_to_host();
    const Rgba full = fromBuffer(fullBuf);
    {
        uint8_t lo = 255, hi = 0;
        for (int y = 0; y < sh; y += 37)
            for (int x = 0; x < sw; x += 37)
                for (int c = 0; c < 3; ++c) {
                    lo = std::min<uint8_t>(lo, full.at(x, y, c));
                    hi = std::max<uint8_t>(hi, full.at(x, y, c));
                }
        printf("full-res render 8-bit range over sampled grid: [%u, %u]\n", lo, hi);
        if (lo == hi) {
            printf("FAIL: full-res render is constant -- reference is degenerate\n");
            return 1;
        }
    }

    // ---- target size ----------------------------------------------------
    int ow, oh;
    if (sw >= sh) { ow = maxDim; oh = static_cast<int>(std::lround(static_cast<double>(sh) * maxDim / sw)); }
    else { oh = maxDim; ow = static_cast<int>(std::lround(static_cast<double>(sw) * maxDim / sh)); }
    printf("target: %dx%d\n", ow, oh);

    const Rgba ref = boxDownscale8(full, ow, oh);

    Buffer<uint8_t> aBuf = Buffer<uint8_t>::make_interleaved(ow, oh, 4);
    Buffer<uint8_t> bBuf = Buffer<uint8_t>::make_interleaved(ow, oh, 4);
    if (runVariantA(src, ow, oh, hp, aBuf) != 0) { printf("FAIL: variant A returned nonzero\n"); return 1; }
    if (runVariantB(src, ow, oh, hp, bBuf) != 0) { printf("FAIL: variant B returned nonzero\n"); return 1; }
    aBuf.copy_to_host();
    bBuf.copy_to_host();
    const Rgba varA = fromBuffer(aBuf);
    const Rgba varB = fromBuffer(bBuf);

    int maxA = 0, maxB = 0;
    const double psnrA = psnrRgb(varA, ref, &maxA);
    const double psnrB = psnrRgb(varB, ref, &maxB);

    printf("[PHOTO-AC7] variant=A-preavg  dng=%s out=%dx%d PSNR=%.2f dB maxAbs=%d\n",
           dngPath, ow, oh, psnrA, maxA);
    printf("[PHOTO-AC7] variant=B-postavg dng=%s out=%dx%d PSNR=%.2f dB maxAbs=%d\n",
           dngPath, ow, oh, psnrB, maxB);

    // ---- viewable output -------------------------------------------------
    const std::string pre = outDir + "/";
    const std::string tag = "_" + std::to_string(maxDim);
    writePpm(pre + "variantA" + tag + ".ppm", varA);
    writePpm(pre + "variantB" + tag + ".ppm", varB);
    writePpm(pre + "reference" + tag + ".ppm", ref);
    writePpm(pre + "diffA_x8" + tag + ".ppm", absDiff(varA, ref, 8));
    writePpm(pre + "diffB_x8" + tag + ".ppm", absDiff(varB, ref, 8));

    const int win = std::min({256, ow, oh});
    int hx, hy, lx, ly;
    pickCrops(ref, win, std::max(1, win / 4), &hx, &hy, &lx, &ly);
    // Same window on all three images, so the crops are directly comparable.
    writePpm(pre + "cropDetail_variantA" + tag + ".ppm", crop(varA, hx, hy, win, win));
    writePpm(pre + "cropDetail_variantB" + tag + ".ppm", crop(varB, hx, hy, win, win));
    writePpm(pre + "cropDetail_reference" + tag + ".ppm", crop(ref, hx, hy, win, win));
    writePpm(pre + "cropSmooth_variantA" + tag + ".ppm", crop(varA, lx, ly, win, win));
    writePpm(pre + "cropSmooth_variantB" + tag + ".ppm", crop(varB, lx, ly, win, win));
    writePpm(pre + "cropSmooth_reference" + tag + ".ppm", crop(ref, lx, ly, win, win));

    // Per-crop PSNR, so the visual comparison has numbers attached.
    {
        int m = 0;
        const Rgba refD = crop(ref, hx, hy, win, win);
        const Rgba refS = crop(ref, lx, ly, win, win);
        printf("[PHOTO-AC7-CROP] region=detail  window=%dx%d@(%d,%d) A=%.2f dB",
               win, win, hx, hy, psnrRgb(crop(varA, hx, hy, win, win), refD, &m));
        printf(" (maxAbs=%d)", m);
        printf(" B=%.2f dB\n", psnrRgb(crop(varB, hx, hy, win, win), refD, &m));
        printf("[PHOTO-AC7-CROP] region=smooth  window=%dx%d@(%d,%d) A=%.2f dB",
               win, win, lx, ly, psnrRgb(crop(varA, lx, ly, win, win), refS, &m));
        printf(" (maxAbs=%d)", m);
        printf(" B=%.2f dB\n", psnrRgb(crop(varB, lx, ly, win, win), refS, &m));
    }

    printf("[PHOTO-AC7] images written to %s\n", outDir.c_str());
    return 0;
}
