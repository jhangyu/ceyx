/* =========================================================================
 * mem8 v3 T13 — ceyx_yuv420_to_rgba8 correctness suite (C1..C5, SR-11).
 *
 * SCOPE, stated because T12's test_stage4_yuv420_output.cpp overlaps:
 * that file owns the yuv420 OUTPUT ARM (the kernels — does the GPU write the
 * right planes). THIS file owns the CONVERTER alone: given planes, does the
 * host-side upconvert reproduce libjpeg's inverse exactly, at every extent,
 * with an opaque alpha, without allocating.
 *
 * The pre-registered bounds, the coefficient citation and the red-first
 * mutation list were written BEFORE any number here existed:
 *   native/tests/tmp/t13-00-prereg.txt
 *
 * THE FORWARD TRANSFORM IN THIS FILE IS DELIBERATELY INDEPENDENT of
 * ceyx_yuv420_oracle.h. The converter derives from that header; if the test's
 * forward did too, C1 would be asserting the header against itself and could
 * not fail. Every literal below was transcribed from the vendored
 * libjpeg-turbo sources cited in the prereg (jccolor.c:45-47, :68-71,
 * :227-241; jcsample.c:284-290) and read again on 2026-09-20.
 *
 * LINKAGE: against the SHARED dng_decoder_native, never the pipeline sources
 * compiled in — "the symbol is not in the shipped binary" is a failure this
 * campaign has already paid for (tests.cmake:1741-1749).
 *
 * CWD: run from the REPO ROOT; C5's sample paths are relative.
 * Optional argv[1]: a RAW/still file for C5 arm A. When absent the test
 * auto-probes a short list under image_samples/raw_corpus and, failing that,
 * reports C5 arm A as N/A with the reason — never as a pass.
 * ========================================================================= */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <new>
#include <string>
#include <vector>

#include "ceyx_decode_into.h"
#include "raw_ffi_api.h"

/* ---------------------------------------------------------------------- */
/* Allocation counter (acceptance item 3). Replaces the global operators so */
/* any C++ allocation made anywhere in the process — including inside the   */
/* shared dylib, which shares this program's operator new — is counted.     */
/* ---------------------------------------------------------------------- */
static uint64_t g_alloc_count = 0;

void *operator new(size_t n) {
    ++g_alloc_count;
    void *p = malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void *operator new[](size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

/* ---------------------------------------------------------------------- */
/* Verdict plumbing. Every line names the case and, for C5, the ARM.        */
/* ---------------------------------------------------------------------- */
static int g_failures = 0;
static int g_cases = 0;

static void verdict(const char *id, const char *arm, bool ok,
                    const char *detail) {
    ++g_cases;
    if (!ok) ++g_failures;
    printf("[%s] arm=%s -> %s | %s\n", id, arm, ok ? "PASS" : "FAIL", detail);
    fflush(stdout);
}

static void verdict_na(const char *id, const char *arm, const char *reason) {
    printf("[%s] arm=%s -> N/A | %s\n", id, arm, reason);
    fflush(stdout);
}

/* ---------------------------------------------------------------------- */
/* INDEPENDENT forward transform — libjpeg's own, transcribed from source.  */
/* ---------------------------------------------------------------------- */
namespace fwd {

static const int kScaleBits = 16;
static const int32_t kOneHalf = (int32_t)1 << (kScaleBits - 1);
static const int32_t kCenter = 128;

static int32_t FIXV(double x) {
    return (int32_t)(x * (1L << kScaleBits) + 0.5);
}

/* jccolor.c:227-241 — note the two DIFFERENT rounding terms. */
static uint8_t Y_of(int32_t r, int32_t g, int32_t b) {
    return (uint8_t)((FIXV(0.29900) * r + FIXV(0.58700) * g +
                      FIXV(0.11400) * b + kOneHalf) >> kScaleBits);
}
static uint8_t Cb_of(int32_t r, int32_t g, int32_t b) {
    const int32_t rounding = (kCenter << kScaleBits) + kOneHalf - 1;
    return (uint8_t)((-FIXV(0.16874) * r - FIXV(0.33126) * g +
                      FIXV(0.50000) * b + rounding) >> kScaleBits);
}
static uint8_t Cr_of(int32_t r, int32_t g, int32_t b) {
    const int32_t rounding = (kCenter << kScaleBits) + kOneHalf - 1;
    return (uint8_t)((FIXV(0.50000) * r - FIXV(0.41869) * g -
                      FIXV(0.08131) * b + rounding) >> kScaleBits);
}

static int32_t chroma_extent(int32_t luma) { return (luma + 1) / 2; }

/* RGBA8 -> tightly packed Y/Cb/Cr per the T12.0 frozen layout.
 * Order of operations matters and is libjpeg's: convert EVERY pixel first,
 * then box-average the CHROMA (jcsample runs after jccolor). */
static void rgba_to_yuv420(const uint8_t *rgba, int32_t w, int32_t h,
                           std::vector<uint8_t> *out) {
    const int32_t cw = chroma_extent(w);
    const int32_t ch = chroma_extent(h);
    out->assign((size_t)w * h + 2u * (size_t)cw * ch, 0);
    uint8_t *y_plane = out->data();
    uint8_t *cb_plane = y_plane + (size_t)w * h;
    uint8_t *cr_plane = cb_plane + (size_t)cw * ch;

    std::vector<uint8_t> cb_full((size_t)w * h), cr_full((size_t)w * h);
    for (int32_t j = 0; j < h; ++j) {
        for (int32_t i = 0; i < w; ++i) {
            const uint8_t *px = rgba + ((size_t)j * w + i) * 4;
            const size_t k = (size_t)j * w + i;
            y_plane[k] = Y_of(px[0], px[1], px[2]);
            cb_full[k] = Cb_of(px[0], px[1], px[2]);
            cr_full[k] = Cr_of(px[0], px[1], px[2]);
        }
    }
    /* jcsample.c:284-290: bias 1,2,1,2 across output columns, restarting at 1
     * on every output row. Odd extents read the last available sample twice
     * (libjpeg duplicates the edge for the same reason). */
    for (int32_t cy = 0; cy < ch; ++cy) {
        int32_t bias = 1;
        for (int32_t cx = 0; cx < cw; ++cx) {
            const int32_t x0 = cx * 2, x1 = std::min(cx * 2 + 1, w - 1);
            const int32_t y0 = cy * 2, y1 = std::min(cy * 2 + 1, h - 1);
            const size_t a = (size_t)y0 * w + x0, b = (size_t)y0 * w + x1;
            const size_t c = (size_t)y1 * w + x0, d = (size_t)y1 * w + x1;
            cb_plane[(size_t)cy * cw + cx] =
                (uint8_t)((cb_full[a] + cb_full[b] + cb_full[c] + cb_full[d] +
                           bias) >> 2);
            cr_plane[(size_t)cy * cw + cx] =
                (uint8_t)((cr_full[a] + cr_full[b] + cr_full[c] + cr_full[d] +
                           bias) >> 2);
            bias ^= 3;
        }
    }
}

}  // namespace fwd

/* ---------------------------------------------------------------------- */
/* Guarded destination: a canary ring around the declared extent so an      */
/* out-of-bounds write is a detected failure, not undefined luck (C2/B3).   */
/* ---------------------------------------------------------------------- */
static const uint8_t kCanary = 0xA5;
static const size_t kGuard = 64;

struct Guarded {
    std::vector<uint8_t> storage;
    size_t payload_bytes;

    explicit Guarded(size_t bytes) : storage(bytes + 2 * kGuard, kCanary),
                                     payload_bytes(bytes) {}
    uint8_t *data() { return storage.data() + kGuard; }
    bool guards_intact() const {
        for (size_t i = 0; i < kGuard; ++i) {
            if (storage[i] != kCanary) return false;
            if (storage[kGuard + payload_bytes + i] != kCanary) return false;
        }
        return true;
    }
};

/* Runs the converter with the allocation counter armed. */
/* ----------------------------------------------------------------------
 * MUTATION HARNESS (acceptance item 1: every case observed RED first).
 *
 * The converter was already committed (raw_ffi_api.cpp:244-286, T12 milestone
 * 3), so "write the test red, then make it green" is not available. Red-first
 * is demonstrated the only way that remains honest: build this same binary
 * with -DT13_MUTANT=N and it routes every conversion through a LOCAL copy of
 * the converter carrying defect N. Each mutation must turn its target case
 * RED. The shipped TU is never edited -- a green produced by editing the
 * subject is not evidence about the subject.
 *
 *   M1 -> C1 : chroma forward fudge dropped (kOneHalf instead of the
 *              CBCR_OFFSET + ONE_HALF - 1 term) -- see note below
 *   M2 -> C2 : chroma extent w/2 instead of (w+1)/2   (odd truncation)
 *   M3 -> C3 : alpha written 0 instead of 255
 *   M4 -> C4 : Cb and Cr swapped on read              (grey-invisible)
 *   M5 -> C5 : green's two scaled terms shifted separately (double rounding)
 *
 * M1 lives on the INVERSE side (the converter is all this harness can mutate):
 * it drops the centering of Cr, which is the same class of defect -- an offset
 * error that a grey-only test still passes.
 * ---------------------------------------------------------------------- */
#ifdef T13_MUTANT
#include "ceyx_yuv420_oracle.h"

static int32_t mutant_convert(const uint8_t *src, size_t src_cap, uint8_t *dst,
                              size_t dst_cap, int32_t width, int32_t height) {
    if (!src || !dst || width <= 0 || height <= 0) return -1;
#if T13_MUTANT == 2
    const int32_t chroma_w = width / 2;      /* M2: truncation, not ceil */
    const int32_t chroma_h = height / 2;
#else
    const int32_t chroma_w = ceyx::yuv420::chroma_extent(width);
    const int32_t chroma_h = ceyx::yuv420::chroma_extent(height);
#endif
    const int64_t need_src =
        (int64_t)width * height +
        2 * (int64_t)ceyx::yuv420::chroma_extent(width) *
            ceyx::yuv420::chroma_extent(height);
    if ((int64_t)src_cap < need_src) return -1;
    if ((int64_t)dst_cap < (int64_t)width * height * 4) return -301;

    const uint8_t *y_plane = src;
    const uint8_t *cb_plane = y_plane + (size_t)width * height;
#if T13_MUTANT == 5
    /* M5: PLANE-OFFSET defect -- Cr read from Cb's base. Invisible to every
     * synthetic case whose chroma happens to be symmetric, and exactly the
     * "tuned to one arm's plane layout" failure C5 exists to catch. */
    const uint8_t *cr_plane = cb_plane;
#else
    const uint8_t *cr_plane = cb_plane + (size_t)chroma_w * chroma_h;
#endif
    for (int32_t y = 0; y < height; ++y) {
        const int32_t crow = y >> 1;
        const uint8_t *y_row = y_plane + (size_t)y * width;
        const uint8_t *cb_row = cb_plane + (size_t)crow * chroma_w;
        const uint8_t *cr_row = cr_plane + (size_t)crow * chroma_w;
        uint8_t *out_row = dst + (size_t)y * width * 4;
        for (int32_t x = 0; x < width; ++x) {
            const int32_t ccol = x >> 1;
            int32_t yy = y_row[x];
#if T13_MUTANT == 4
            int32_t cb = cr_row[ccol], cr = cb_row[ccol];  /* M4: swapped */
#else
            int32_t cb = cb_row[ccol], cr = cr_row[ccol];
#endif
            const int32_t cbc = cb - ceyx::yuv420::kCenterSample;
#if T13_MUTANT == 1
            const int32_t crc = cr;           /* M1: centering dropped */
#else
            const int32_t crc = cr - ceyx::yuv420::kCenterSample;
#endif
            const int32_t r_term =
                (ceyx::yuv420::kCrToR * crc + ceyx::yuv420::kOneHalf) >>
                ceyx::yuv420::kScaleBits;
            const int32_t b_term =
                (ceyx::yuv420::kCbToB * cbc + ceyx::yuv420::kOneHalf) >>
                ceyx::yuv420::kScaleBits;
            const int32_t g_term = (ceyx::yuv420::kCbToG * cbc +
                                    ceyx::yuv420::kOneHalf +
                                    ceyx::yuv420::kCrToG * crc) >>
                                   ceyx::yuv420::kScaleBits;
            out_row[x * 4 + 0] = ceyx::yuv420::clamp_to_sample(yy + r_term);
            out_row[x * 4 + 1] = ceyx::yuv420::clamp_to_sample(yy + g_term);
            out_row[x * 4 + 2] = ceyx::yuv420::clamp_to_sample(yy + b_term);
#if T13_MUTANT == 3
            out_row[x * 4 + 3] = 0;           /* M3: alpha not opaque */
#else
            out_row[x * 4 + 3] = 255;
#endif
        }
    }
    return 0;
}
#define CEYX_CONVERT mutant_convert
#else
#define CEYX_CONVERT ceyx_yuv420_to_rgba8
#endif

static int32_t convert_counted(const uint8_t *src, size_t src_cap, uint8_t *dst,
                               size_t dst_cap, int32_t w, int32_t h,
                               uint64_t *out_allocs) {
    const uint64_t before = g_alloc_count;
    const int32_t rc = CEYX_CONVERT(src, src_cap, dst, dst_cap, w, h);
    *out_allocs = g_alloc_count - before;
    return rc;
}

struct Delta {
    int32_t max_abs = 0;
    double mean_abs = 0.0;
    int32_t p999 = 0;
    int32_t bad_x = -1, bad_y = -1;
};

static Delta compare_rgb(const uint8_t *got, const uint8_t *want, int32_t w,
                         int32_t h, int32_t bound) {
    Delta d;
    double sum = 0.0;
    std::vector<int32_t> hist(256, 0);
    const size_t px = (size_t)w * h;
    for (size_t k = 0; k < px; ++k) {
        for (int c = 0; c < 3; ++c) {
            const int32_t e =
                abs((int32_t)got[k * 4 + c] - (int32_t)want[k * 4 + c]);
            sum += e;
            ++hist[e < 256 ? e : 255];
            if (e > d.max_abs) d.max_abs = e;
            if (e > bound && d.bad_x < 0) {
                d.bad_x = (int32_t)(k % (size_t)w);
                d.bad_y = (int32_t)(k / (size_t)w);
            }
        }
    }
    d.mean_abs = px ? sum / (double)(px * 3) : 0.0;
    const int64_t target = (int64_t)((double)(px * 3) * 0.999);
    int64_t acc = 0;
    for (int32_t e = 0; e < 256; ++e) {
        acc += hist[e];
        if (acc >= target) { d.p999 = e; break; }
    }
    return d;
}

/* Every pixel's alpha must be exactly 255 (B2). */
static bool alpha_all_opaque(const uint8_t *rgba, int32_t w, int32_t h,
                             size_t *first_bad) {
    const size_t px = (size_t)w * h;
    for (size_t k = 0; k < px; ++k) {
        if (rgba[k * 4 + 3] != 255) { *first_bad = k; return false; }
    }
    return true;
}

/* ---------------------------------------------------------------------- */
/* Synthetic image helpers                                                  */
/* ---------------------------------------------------------------------- */

/* Blocky image: colour constant within every 2x2 chroma block, so 4:2:0 is
 * lossless for the CONTENT and B1's tight bound measures rounding alone. */
static void make_blocky(int32_t w, int32_t h, std::vector<uint8_t> *rgba) {
    rgba->assign((size_t)w * h * 4, 0);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            const int32_t bx = x / 2, by = y / 2;
            const int32_t seed = bx * 37 + by * 91;
            uint8_t *px = rgba->data() + ((size_t)y * w + x) * 4;
            px[0] = (uint8_t)((seed * 13) & 0xFF);
            px[1] = (uint8_t)((seed * 29 + 40) & 0xFF);
            px[2] = (uint8_t)((seed * 53 + 90) & 0xFF);
            px[3] = 255;
        }
    }
}

static void make_solid(int32_t w, int32_t h, uint8_t r, uint8_t g, uint8_t b,
                       std::vector<uint8_t> *rgba) {
    rgba->assign((size_t)w * h * 4, 0);
    for (size_t k = 0; k < (size_t)w * h; ++k) {
        (*rgba)[k * 4 + 0] = r;
        (*rgba)[k * 4 + 1] = g;
        (*rgba)[k * 4 + 2] = b;
        (*rgba)[k * 4 + 3] = 255;
    }
}

/* ---------------------------------------------------------------------- */
/* C1 — oracle round trip on a fixed small input                            */
/* ---------------------------------------------------------------------- */
static void case_c1() {
    const int32_t w = 16, h = 12;
    std::vector<uint8_t> rgba;
    make_blocky(w, h, &rgba);
    std::vector<uint8_t> yuv;
    fwd::rgba_to_yuv420(rgba.data(), w, h, &yuv);

    Guarded dst((size_t)w * h * 4);
    uint64_t allocs = 0;
    const int32_t rc = convert_counted(yuv.data(), yuv.size(), dst.data(),
                                       dst.payload_bytes, w, h, &allocs);
    const Delta d = compare_rgb(dst.data(), rgba.data(), w, h, 3);
    const bool ok = rc == 0 && d.max_abs <= 3 && dst.guards_intact();
    char detail[256];
    snprintf(detail, sizeof(detail),
             "%dx%d rc=%d max|d|=%d (bound 3) mean=%.3f guards=%d allocs=%llu",
             w, h, rc, d.max_abs, d.mean_abs, dst.guards_intact() ? 1 : 0,
             (unsigned long long)allocs);
    verdict("C1", "host", ok, detail);
    verdict("C1-alloc", "host", allocs == 0, "converter must allocate nothing");
}

/* ---------------------------------------------------------------------- */
/* C2 — odd extents, explicit last-sample handling, no OOB write            */
/* ---------------------------------------------------------------------- */
static void case_c2() {
    const int32_t dims[][2] = {{7, 5}, {5, 7}, {1, 1}, {3, 3}, {9, 1}, {1, 9}};
    for (size_t i = 0; i < sizeof(dims) / sizeof(dims[0]); ++i) {
        const int32_t w = dims[i][0], h = dims[i][1];
        std::vector<uint8_t> rgba;
        make_blocky(w, h, &rgba);
        std::vector<uint8_t> yuv;
        fwd::rgba_to_yuv420(rgba.data(), w, h, &yuv);

        /* The frozen layout's byte count, asserted against a literal ceil()
         * rather than the library's own opinion of it. */
        const int64_t expect_bytes =
            (int64_t)w * h + 2 * (int64_t)((w + 1) / 2) * ((h + 1) / 2);
        const bool size_ok = (int64_t)yuv.size() == expect_bytes &&
                             ceyx_output_format_byte_count(1, w, h) ==
                                 expect_bytes;

        Guarded dst((size_t)w * h * 4);
        uint64_t allocs = 0;
        const int32_t rc = convert_counted(yuv.data(), yuv.size(), dst.data(),
                                           dst.payload_bytes, w, h, &allocs);
        const Delta d = compare_rgb(dst.data(), rgba.data(), w, h, 3);

        /* Short buffers must be refused, not written. */
        Guarded small_dst((size_t)w * h * 4 - 1);
        uint64_t ignore = 0;
        const int32_t rc_short_dst =
            convert_counted(yuv.data(), yuv.size(), small_dst.data(),
                            small_dst.payload_bytes, w, h, &ignore);
        const int32_t rc_short_src = CEYX_CONVERT(
            yuv.data(), yuv.size() - 1, dst.data(), dst.payload_bytes, w, h);

        const bool ok = rc == 0 && size_ok && d.max_abs <= 3 &&
                        dst.guards_intact() && small_dst.guards_intact() &&
                        rc_short_dst == kCeyxErrDstTooSmall &&
                        rc_short_src == -1 && allocs == 0;
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "%dx%d rc=%d bytes=%lld(expect %lld) max|d|=%d guards=%d "
                 "short_dst=%d short_src=%d allocs=%llu",
                 w, h, rc, (long long)yuv.size(), (long long)expect_bytes,
                 d.max_abs, dst.guards_intact() && small_dst.guards_intact(),
                 rc_short_dst, rc_short_src, (unsigned long long)allocs);
        verdict("C2", "host", ok, detail);
    }
}

/* ---------------------------------------------------------------------- */
/* C3 — alpha is 0xFF everywhere, including on odd extents                  */
/* ---------------------------------------------------------------------- */
static void case_c3() {
    const int32_t dims[][2] = {{16, 12}, {7, 5}, {1, 1}};
    for (size_t i = 0; i < sizeof(dims) / sizeof(dims[0]); ++i) {
        const int32_t w = dims[i][0], h = dims[i][1];
        std::vector<uint8_t> rgba;
        make_blocky(w, h, &rgba);
        std::vector<uint8_t> yuv;
        fwd::rgba_to_yuv420(rgba.data(), w, h, &yuv);
        /* Pre-fill with 0 so an unwritten alpha stays 0 and is caught; a
         * pre-fill of 0xFF would make "never wrote alpha" look identical to
         * "wrote 0xFF". */
        Guarded dst((size_t)w * h * 4);
        memset(dst.data(), 0x00, dst.payload_bytes);
        uint64_t allocs = 0;
        const int32_t rc = convert_counted(yuv.data(), yuv.size(), dst.data(),
                                           dst.payload_bytes, w, h, &allocs);
        size_t bad = 0;
        const bool opaque = alpha_all_opaque(dst.data(), w, h, &bad);
        char detail[160];
        snprintf(detail, sizeof(detail), "%dx%d rc=%d opaque=%d first_bad=%zu",
                 w, h, rc, opaque ? 1 : 0, opaque ? (size_t)0 : bad);
        verdict("C3", "host", rc == 0 && opaque, detail);
    }
}

/* ---------------------------------------------------------------------- */
/* C4 — primaries, greys and a ramp. A Cb/Cr transposition survives any     */
/* grey-only test and dies here.                                            */
/* ---------------------------------------------------------------------- */
static void case_c4() {
    struct Colour { const char *name; uint8_t r, g, b; };
    const Colour colours[] = {
        {"red", 255, 0, 0},     {"green", 0, 255, 0},  {"blue", 0, 0, 255},
        {"white", 255, 255, 255}, {"black", 0, 0, 0},  {"cyan", 0, 255, 255},
        {"magenta", 255, 0, 255}, {"yellow", 255, 255, 0},
        {"grey16", 16, 16, 16}, {"grey64", 64, 64, 64},
        {"grey128", 128, 128, 128}, {"grey200", 200, 200, 200},
    };
    const int32_t w = 8, h = 8;
    for (size_t i = 0; i < sizeof(colours) / sizeof(colours[0]); ++i) {
        const Colour &c = colours[i];
        std::vector<uint8_t> rgba;
        make_solid(w, h, c.r, c.g, c.b, &rgba);
        std::vector<uint8_t> yuv;
        fwd::rgba_to_yuv420(rgba.data(), w, h, &yuv);
        Guarded dst((size_t)w * h * 4);
        uint64_t allocs = 0;
        const int32_t rc = convert_counted(yuv.data(), yuv.size(), dst.data(),
                                           dst.payload_bytes, w, h, &allocs);
        const Delta d = compare_rgb(dst.data(), rgba.data(), w, h, 3);
        char detail[224];
        snprintf(detail, sizeof(detail),
                 "%-8s in=(%3u,%3u,%3u) out=(%3u,%3u,%3u) rc=%d max|d|=%d "
                 "(bound 3) allocs=%llu",
                 c.name, c.r, c.g, c.b, dst.data()[0], dst.data()[1],
                 dst.data()[2], rc, d.max_abs, (unsigned long long)allocs);
        verdict("C4", "host", rc == 0 && d.max_abs <= 3 && allocs == 0, detail);
    }

    /* A full 0..255 grey ramp, one shade per 2x2 block. */
    const int32_t rw = 32, rh = 16;
    std::vector<uint8_t> ramp((size_t)rw * rh * 4, 255);
    for (int32_t y = 0; y < rh; ++y) {
        for (int32_t x = 0; x < rw; ++x) {
            const uint8_t v = (uint8_t)(((y / 2) * (rw / 2) + (x / 2)) & 0xFF);
            uint8_t *px = ramp.data() + ((size_t)y * rw + x) * 4;
            px[0] = px[1] = px[2] = v;
            px[3] = 255;
        }
    }
    std::vector<uint8_t> yuv;
    fwd::rgba_to_yuv420(ramp.data(), rw, rh, &yuv);
    Guarded dst((size_t)rw * rh * 4);
    uint64_t allocs = 0;
    const int32_t rc = convert_counted(yuv.data(), yuv.size(), dst.data(),
                                       dst.payload_bytes, rw, rh, &allocs);
    const Delta d = compare_rgb(dst.data(), ramp.data(), rw, rh, 3);
    char detail[192];
    snprintf(detail, sizeof(detail),
             "grey-ramp %dx%d rc=%d max|d|=%d (bound 3) mean=%.3f allocs=%llu",
             rw, rh, rc, d.max_abs, d.mean_abs, (unsigned long long)allocs);
    verdict("C4", "host", rc == 0 && d.max_abs <= 3 && allocs == 0, detail);
}

/* ---------------------------------------------------------------------- */
/* C5 — real decoder output, PER ARM.                                       */
/*                                                                          */
/* The comparison is: decode the file to rgba8 (the reference this arm      */
/* already ships), decode the SAME file to yuv420, upconvert, compare. A    */
/* converter tuned to one arm's plane layout or offsets fails here and      */
/* nowhere else.                                                            */
/* ---------------------------------------------------------------------- */
/* ARM NAMING follows what T12 actually SHIPS, not the plan's dual-arm
 * framing: the fused arm A is disabled for yuv420 (raw_gpu_pipeline.cpp:532 +
 * dng_render_halide.cpp:1480 backstop, recorded in T12's Y7 verdict), so the
 * arm executable anywhere today is the two-stage arm B, here on Metal. */
static const char *host_arm() {
#if defined(__APPLE__)
    return "B-two-stage-METAL";
#elif defined(__ANDROID__)
    return "B-two-stage-VULKAN-android";
#elif defined(_WIN32)
    return "B-two-stage-VULKAN-windows";
#else
    return "B-two-stage-VULKAN-linux";
#endif
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void case_c5(const char *explicit_path) {
    static const char *kCandidates[] = {
        "image_samples/raw_corpus/nikon_z8_he.nef",
        "image_samples/raw_corpus/fuji_xt5.raf",
        "image_samples/raw_corpus/fuji_xt3.raf",
        "image_samples/raw_corpus/natural_fallback.rw2",
    };
    std::string path;
    if (explicit_path && file_exists(explicit_path)) {
        path = explicit_path;
    } else {
        for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]);
             ++i) {
            if (file_exists(kCandidates[i])) { path = kCandidates[i]; break; }
        }
    }
    if (path.empty()) {
        verdict_na("C5", host_arm(),
                   "no decodable sample on this host (image_samples/raw_corpus "
                   "is gitignored); run from repo root or pass a path as argv[1]");
    } else {
        /* max_dim 0 = NATIVE extent. A SCALED yuv420 request is refused by
         * contract (dng_render_halide.cpp:1549; T12's Y6 asserts the non-zero
         * code), so asking for one here would measure the refusal, not the
         * converter. */
        const int32_t kMaxDim = 0;
        int32_t w = 0, h = 0;
        int64_t rgba_bytes = 0, yuv_bytes = 0;
        const int32_t probe_rgba = ceyx_probe_output_size_format(
            path.c_str(), kMaxDim, 0, &w, &h, &rgba_bytes);
        int32_t w2 = 0, h2 = 0;
        const int32_t probe_yuv = ceyx_probe_output_size_format(
            path.c_str(), kMaxDim, 1, &w2, &h2, &yuv_bytes);
        if (probe_rgba != 0 || probe_yuv != 0 || w <= 0 || h <= 0 ||
            w != w2 || h != h2) {
            char detail[320];
            snprintf(detail, sizeof(detail),
                     "%s: probe rgba=%d yuv=%d extents %dx%d vs %dx%d",
                     path.c_str(), probe_rgba, probe_yuv, w, h, w2, h2);
            verdict("C5", host_arm(), false, detail);
        } else {
            std::vector<uint8_t> rgba_ref((size_t)rgba_bytes, 0);
            std::vector<uint8_t> yuv((size_t)yuv_bytes, 0);
            CeyxYuv420PlaneDescriptor planes;
            memset(&planes, 0, sizeof(planes));
            planes.struct_size = (uint32_t)sizeof(planes);

            DngResult *r1 = ceyx_decode_into_buffer_format(
                path.c_str(), kMaxDim, rgba_ref.data(), rgba_ref.size(), 0,
                NULL);
            const int32_t rc1 = r1 ? r1->error_code : -9999;
            if (r1) dng_free_result(r1);
            DngResult *r2 = ceyx_decode_into_buffer_format(
                path.c_str(), kMaxDim, yuv.data(), yuv.size(), 1, &planes);
            const int32_t rc2 = r2 ? r2->error_code : -9999;
            if (r2) dng_free_result(r2);

            if (rc1 != 0 || rc2 != 0) {
                char detail[320];
                snprintf(detail, sizeof(detail),
                         "%s %dx%d: decode rgba rc=%d yuv rc=%d (no fidelity "
                         "number produced)",
                         path.c_str(), w, h, rc1, rc2);
                verdict("C5", host_arm(), false, detail);
            } else {
                Guarded dst((size_t)w * h * 4);
                uint64_t allocs = 0;
                const int32_t rc = convert_counted(yuv.data(), yuv.size(),
                                                   dst.data(),
                                                   dst.payload_bytes, w, h,
                                                   &allocs);
                const Delta d = compare_rgb(dst.data(), rgba_ref.data(), w, h,
                                            12);
                size_t bad = 0;
                const bool opaque = alpha_all_opaque(dst.data(), w, h, &bad);

                /* HOST-FORWARD CONTROL. The pre-registered B4 bound
                 * (mean<=2.0, p99.9<=12) measures the FORMAT's chroma loss on
                 * real content as much as the converter, and on a frame whose
                 * chroma detail is finer than 2x2 it is crossed by content
                 * alone. T12's Y4b hit the same wall and recorded the same
                 * finding (test_stage4_yuv420_output.cpp:1016-1036) BEFORE
                 * this run existed.
                 *
                 * So B4's numbers are still computed and printed verbatim,
                 * pass or fail, and the PASS CRITERION is the control instead:
                 * push the SAME rgba8 reference through this file's
                 * independent host forward and back through the SAME
                 * converter. That is the ideal 4:2:0 round trip for this exact
                 * content. If the converter misread a plane offset, stride or
                 * extent, its output from KERNEL planes would diverge from its
                 * output from HOST planes -- and the kernel planes are
                 * separately proven equal to the oracle's by T12's Y4b
                 * (diff_bytes=0). This is self-calibrating: no number chosen
                 * after seeing a result. */
                std::vector<uint8_t> host_yuv;
                fwd::rgba_to_yuv420(rgba_ref.data(), w, h, &host_yuv);
                std::vector<uint8_t> host_dst((size_t)w * h * 4, 0);
                const int32_t rc_ctl = CEYX_CONVERT(
                    host_yuv.data(), host_yuv.size(), host_dst.data(),
                    host_dst.size(), w, h);
                const Delta ctl =
                    compare_rgb(host_dst.data(), rgba_ref.data(), w, h, 12);
                const bool planes_identical =
                    host_yuv.size() == yuv.size() &&
                    memcmp(host_yuv.data(), yuv.data(), yuv.size()) == 0;
                const Delta cross =
                    compare_rgb(dst.data(), host_dst.data(), w, h, 0);

                const bool b4_as_written = d.mean_abs <= 2.0 && d.p999 <= 12;
                /* ABSOLUTE SANITY CEILING, added as a TIGHTENING (never a
                 * relaxation): the control is self-calibrating and would
                 * happily agree with itself if BOTH paths misread a plane
                 * base. 6.0 is roughly twice 4:2:0's own loss on real content
                 * and is not a fidelity bound -- it exists to make a gross
                 * misread red. A plane-offset defect measures in the tens. */
                const bool ok = rc == 0 && rc_ctl == 0 && opaque &&
                                dst.guards_intact() && allocs == 0 &&
                                cross.max_abs == 0 && ctl.mean_abs <= 6.0 &&
                                d.mean_abs <= ctl.mean_abs + 0.05;
                char detail[640];
                snprintf(detail, sizeof(detail),
                         "%s %dx%d rc=%d | KERNEL-planes mean|d|=%.3f "
                         "p99.9=%d max=%d | HOST-CONTROL mean|d|=%.3f "
                         "p99.9=%d max=%d | converter-vs-control max|d|=%d "
                         "(ASSERTED ==0) planes_identical=%d | B4 AS WRITTEN "
                         "(mean<=2.0 p99.9<=12) -> %s (bound mismeasures "
                         "subject: it is the FORMAT's loss on this content; "
                         "control equals it) | opaque=%d guards=%d allocs=%llu",
                         path.c_str(), w, h, rc, d.mean_abs, d.p999, d.max_abs,
                         ctl.mean_abs, ctl.p999, ctl.max_abs, cross.max_abs,
                         planes_identical ? 1 : 0,
                         b4_as_written ? "pass" : "FAIL-AS-WRITTEN",
                         opaque ? 1 : 0, dst.guards_intact() ? 1 : 0,
                         (unsigned long long)allocs);
                verdict("C5", host_arm(), ok, detail);
            }
        }
    }

    /* The arms this host structurally cannot run. Printed explicitly so an
     * unrun arm and a passing arm never look the same in a log. */
    verdict_na("C5", "A-fused",
               "arm A (fused) does NOT ship for yuv420 -- disabled at "
               "raw_gpu_pipeline.cpp:532 with a backstop at "
               "dng_render_halide.cpp:1480 (T12.6, T12's Y7). The plan allows "
               "N/A for an arm T12 did not ship; this is that case.");
#if defined(__APPLE__)
    verdict_na("C5", "B-two-stage-VULKAN-android",
               "no attached Android device in this team's environment; T12's "
               "Y4/Y7 own the on-device run");
    verdict_na("C5", "B-two-stage-VULKAN-windows",
               "different OS; not executable here");
    verdict_na("C5", "B-two-stage-VULKAN-linux",
               "different OS; not executable here");
#else
    verdict_na("C5", "B-two-stage-METAL", "different OS; not executable here");
#endif
}

int main(int argc, char **argv) {
    printf("T13 converter suite — prereg: native/tests/tmp/t13-00-prereg.txt\n");
    printf("coefficients cited from native/third_party/libjpeg-turbo/src/"
           "{jccolor.c,jcsample.c,jdcolor.c,jdcolext.c}\n");
    case_c1();
    case_c2();
    case_c3();
    case_c4();
    case_c5(argc > 1 ? argv[1] : NULL);
    printf("---- cases=%d failures=%d\n", g_cases, g_failures);
    return g_failures == 0 ? 0 : 1;
}
