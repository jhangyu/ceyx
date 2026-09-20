/* =========================================================================
 * mem8 v3 T12 milestone 4 — Y1..Y9, the yuv420 output arm's correctness suite.
 *
 * WHAT THIS FILE IS FOR, stated plainly because the surrounding evidence is
 * easy to over-read: everything committed before this file proved the yuv420
 * path is REACHABLE and CORRECTLY SHAPED — the kernel is dispatched, the
 * planes sit at the frozen offsets, the call returns 0. None of it looked at a
 * single pixel. Gotcha #93, F-T6-1 and F-T8-1 are three recorded cases in this
 * campaign where a GPU build compiled, ran, returned success and produced
 * wrong pixels. Y1..Y9 are the correctness story.
 *
 * EVERY CASE PRINTS TWO LABELS — the ROUTE it exercised and the ARM/BACKEND it
 * ran on. A verdict without both is not evidence; it is the ambiguity that
 * lets a single-route, single-arm pass be read as coverage (plan T12.3).
 *
 * LINKAGE: against the SHARED dng_decoder_native, never against the pipeline
 * sources compiled in. Compiling the pipeline into a test links none of the
 * shipping code, so a green from that shape says nothing about the artifact —
 * and "the symbol is not in the shipped binary" is precisely the failure this
 * campaign has already paid for (tests.cmake:1741-1749, 2026-09-06 lesson).
 *
 * CWD: run from the REPO ROOT. Sample paths are relative and a wrong cwd
 * reports "decode failed" for every file, which reads exactly like a
 * regression you just caused (t12-baton-3 §6).
 *
 * The pre-registered bounds and the red-first mutation list live in
 * native/tests/tmp/t12c-00-prereg.txt, written before any number existed.
 * ========================================================================= */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "ceyx_decode_into.h"
#include "ceyx_encode_api.h"
#include "ceyx_yuv420_oracle.h"
#include "raw_ffi_api.h"
/* T12.6 / Y7-B0: raw_fused_bayer_render_count(), the pipeline's own
 * fused-dispatch counter. Taken from the real header rather than re-declared
 * locally, so a signature change breaks the build instead of silently linking
 * to a different symbol. The header is plain-C-contract only (no Halide). */
#include "raw_gpu_pipeline.h"

/* Y4a talks to libjpeg-turbo DIRECTLY rather than through the dylib's still
 * surface. That is not a shortcut, it is the only available route and it is
 * also the stronger one:
 *   - ceyx_still_decode_rgba has NO libjpeg arm. still_ffi_api.cpp:137-144
 *     returns 0 from ceyx_still_decode_supports(kCeyxFormatJpeg) ON PURPOSE
 *     and a JPEG path returns kCeyxStillErrOpenFailed (-502), so that surface
 *     cannot serve as the oracle.
 *   - jpeg_read_raw_data hands back libjpeg's OWN downsampled Y/Cb/Cr planes.
 *     Comparing planes to planes tests the forward transform, the rounding
 *     convention and the 2x2 box bias head-on, instead of inferring them
 *     through an RGB round trip that also drags in an upsampler we
 *     deliberately do not share.
 */
#include <jpeglib.h>
#include <setjmp.h>

namespace {

/* ---------------------------------------------------------------------- */
/* Harness                                                                 */
/* ---------------------------------------------------------------------- */

int g_pass = 0;
int g_fail = 0;
int g_na = 0;

/* A case's verdict line. `route` and `arm` are MANDATORY — see the header. */
void verdict(const char *ycase, const char *route, const char *arm, bool ok,
             const char *detail) {
    printf("[%s] route=%s arm=%s -> %s | %s\n", ycase, route, arm,
           ok ? "PASS" : "FAIL", detail);
    if (ok) {
        ++g_pass;
    } else {
        ++g_fail;
    }
}

void verdict_na(const char *ycase, const char *route, const char *arm,
                const char *reason) {
    /* An N/A is recorded WITH ITS REASON and never omitted (plan T12.4 note
     * 1: an empty cell is a FAIL). */
    printf("[%s] route=%s arm=%s -> N/A | %s\n", ycase, route, arm, reason);
    ++g_na;
}

/* FNV-1a 64. Used only to compare two byte arrays produced in the SAME run,
 * and to publish a value the on-device run can be compared against. */
uint64_t fnv1a64(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* The one place this file is allowed to name the host arm. On a split-kernel
 * build the same binary reports the Vulkan arm, so the on-device artifact
 * self-describes rather than relying on the reader to remember which machine
 * produced it. */
const char *host_arm() {
#if defined(DNG_STAGE4_SPLIT_KERNEL)
    return "B-two-stage-VULKAN-split";
#else
    return "B-two-stage-METAL-nonsplit";
#endif
}

struct Rgba {
    std::vector<uint8_t> bytes;
    int32_t w = 0;
    int32_t h = 0;
};

/* Decode a file as RGBA8 through the FORMAT-AGNOSTIC entry (the default path
 * every existing caller uses). */
bool decode_rgba8_default(const char *path, Rgba *out, int32_t *err) {
    int32_t w = 0, h = 0;
    if (ceyx_probe_output_size(path, 0, &w, &h) != 0) return false;
    out->w = w;
    out->h = h;
    out->bytes.assign(static_cast<size_t>(w) * h * 4, 0);
    DngResult *r =
        ceyx_decode_into_buffer(path, 0, out->bytes.data(), out->bytes.size());
    const bool ok = (r != nullptr && r->error_code == 0);
    if (err) *err = r ? r->error_code : -999;
    if (r) dng_free_result(r);
    return ok;
}

/* Decode a file as RGBA8 through the FORMAT-TAKING entry with an explicit
 * rgba8 request. */
bool decode_rgba8_format(const char *path, Rgba *out,
                         CeyxYuv420PlaneDescriptor *planes, int32_t *err) {
    int32_t w = 0, h = 0;
    int64_t bytes = 0;
    if (ceyx_probe_output_size_format(path, 0, kCeyxOutputFormatRgba8, &w, &h,
                                      &bytes) != 0)
        return false;
    out->w = w;
    out->h = h;
    out->bytes.assign(static_cast<size_t>(bytes), 0);
    DngResult *r = ceyx_decode_into_buffer_format(path, 0, out->bytes.data(),
                                                  out->bytes.size(),
                                                  kCeyxOutputFormatRgba8,
                                                  planes);
    const bool ok = (r != nullptr && r->error_code == 0);
    if (err) *err = r ? r->error_code : -999;
    if (r) dng_free_result(r);
    return ok;
}

struct Yuv {
    std::vector<uint8_t> bytes;
    CeyxYuv420PlaneDescriptor planes{};
    int32_t w = 0;
    int32_t h = 0;
};

bool decode_yuv420(const char *path, Yuv *out, int32_t *err) {
    int32_t w = 0, h = 0;
    int64_t bytes = 0;
    if (ceyx_probe_output_size_format(path, 0, kCeyxOutputFormatYuv420, &w, &h,
                                      &bytes) != 0)
        return false;
    out->w = w;
    out->h = h;
    out->bytes.assign(static_cast<size_t>(bytes), 0);
    memset(&out->planes, 0, sizeof(out->planes));
    out->planes.struct_size = static_cast<uint32_t>(sizeof(out->planes));
    DngResult *r = ceyx_decode_into_buffer_format(
        path, 0, out->bytes.data(), out->bytes.size(), kCeyxOutputFormatYuv420,
        &out->planes);
    const bool ok = (r != nullptr && r->error_code == 0);
    if (err) *err = r ? r->error_code : -999;
    if (r) dng_free_result(r);
    return ok;
}

/* Delta statistics between two same-extent RGBA8 images, alpha ignored. */
struct DeltaStats {
    double mean[3] = {0, 0, 0};
    int32_t max_abs = 0;
    double fraction_within_16 = 0.0;
};

DeltaStats compare_rgb(const std::vector<uint8_t> &a,
                       const std::vector<uint8_t> &b, size_t pixels) {
    DeltaStats s;
    double acc[3] = {0, 0, 0};
    size_t within = 0;
    for (size_t i = 0; i < pixels; ++i) {
        int32_t worst = 0;
        for (int c = 0; c < 3; ++c) {
            const int32_t d = static_cast<int32_t>(a[i * 4 + c]) -
                              static_cast<int32_t>(b[i * 4 + c]);
            const int32_t ad = d < 0 ? -d : d;
            acc[c] += ad;
            if (ad > worst) worst = ad;
            if (ad > s.max_abs) s.max_abs = ad;
        }
        if (worst <= 16) ++within;
    }
    for (int c = 0; c < 3; ++c) s.mean[c] = pixels ? acc[c] / pixels : 0.0;
    s.fraction_within_16 = pixels ? static_cast<double>(within) / pixels : 0.0;
    return s;
}

/* Build tightly-packed yuv420 planes from RGBA8 using the ORACLE's forward
 * direction — the same symbols the kernels build their Halide Exprs from. Used
 * only by Y4a, whose independent authority is libjpeg, not this function. */
void oracle_forward_yuv420(const uint8_t *rgba, int32_t w, int32_t h,
                           std::vector<uint8_t> *out) {
    namespace o = ceyx::yuv420;
    const int32_t cw = o::chroma_extent(w);
    const int32_t ch = o::chroma_extent(h);
    out->assign(static_cast<size_t>(w) * h + 2u * cw * ch, 0);
    uint8_t *y_plane = out->data();
    uint8_t *cb_plane = y_plane + static_cast<size_t>(w) * h;
    uint8_t *cr_plane = cb_plane + static_cast<size_t>(cw) * ch;

    std::vector<uint8_t> cb_full(static_cast<size_t>(w) * h);
    std::vector<uint8_t> cr_full(static_cast<size_t>(w) * h);
    for (int32_t yy = 0; yy < h; ++yy) {
        for (int32_t xx = 0; xx < w; ++xx) {
            const size_t i = static_cast<size_t>(yy) * w + xx;
            const int32_t r = rgba[i * 4 + 0];
            const int32_t g = rgba[i * 4 + 1];
            const int32_t b = rgba[i * 4 + 2];
            y_plane[i] = o::luma_from_rgb(r, g, b);
            /* libjpeg converts EVERY pixel to Cb/Cr and downsamples the
             * CHROMA; it does not average RGB and convert once. The two differ
             * by rounding, and "only by rounding" is the size of the bound
             * this feeds (t12-baton-1 §5 note 3). */
            cb_full[i] = o::cb_from_rgb(r, g, b);
            cr_full[i] = o::cr_from_rgb(r, g, b);
        }
    }
    for (int32_t cy = 0; cy < ch; ++cy) {
        for (int32_t cx = 0; cx < cw; ++cx) {
            const int32_t x0 = cx * 2;
            const int32_t y0 = cy * 2;
            const int32_t x1 = (x0 + 1 < w) ? x0 + 1 : x0;
            const int32_t y1 = (y0 + 1 < h) ? y0 + 1 : y0;
            const size_t tl = static_cast<size_t>(y0) * w + x0;
            const size_t tr = static_cast<size_t>(y0) * w + x1;
            const size_t bl = static_cast<size_t>(y1) * w + x0;
            const size_t br = static_cast<size_t>(y1) * w + x1;
            const size_t ci = static_cast<size_t>(cy) * cw + cx;
            cb_plane[ci] = o::box_average_2x2(cb_full[tl], cb_full[tr],
                                              cb_full[bl], cb_full[br], cx);
            cr_plane[ci] = o::box_average_2x2(cr_full[tl], cr_full[tr],
                                              cr_full[bl], cr_full[br], cx);
        }
    }
}

/* ====================================================================== */
/* Y1 — DEFAULT-PATH INVARIANCE                                           */
/*                                                                        */
/* The additive change must not have leaked into the rgba8 path. Two       */
/* independent assertions, because each is blind to what the other sees:   */
/*   (a) the format-agnostic entry and an explicit rgba8 request produce    */
/*       BYTE-IDENTICAL output — byte-identity is achievable here precisely */
/*       because nothing about the RGBA arm changes;                        */
/*   (b) the descriptor is ZEROED for an rgba8 request (there are no        */
/*       planes) while struct_size survives — a descriptor that came back   */
/*       populated would mean a yuv420 layout was published for a 4 B/px    */
/*       buffer;                                                            */
/*   (c) the rgba8 output hash equals a GOLDEN LITERAL.                     */
/*                                                                          */
/* (c) WAS ADDED AFTER MUTATION M5 EXPOSED ITS ABSENCE, and the history is  */
/* kept here because it is the more useful half of the story. M5 perturbed  */
/* a Stage-4 colour constant (255.0f -> 254.0f, T20's own probe value) and  */
/* Y1 STAYED GREEN — while every rgba8 hash moved. The reason: (a) compares */
/* the two ENTRIES against EACH OTHER, and under M5 both changed together,  */
/* so the identity held. Y1 printed the hash and asserted nothing about it. */
/* It was the "assertion that cannot fail" shape, in the case whose whole   */
/* job is detecting a default-path regression — and it was found by the     */
/* mutation designed to prove the assertion could fail. That is what        */
/* red-first is for.                                                        */
/*                                                                          */
/* WHY THE LITERALS ARE LEGITIMATE RATHER THAN CIRCULAR: raw_sample.arw's   */
/* 0409d0d2c5116a9a independently reproduces the Metal reference already    */
/* recorded in t12-baton-1, arrived at before this suite existed. The other */
/* two are this suite's first green, frozen deliberately.                   */
/*                                                                          */
/* SCOPED TO THE NON-SPLIT (METAL) ARM. A different backend may legitimately*/
/* differ in the last bit, so on a split build the same numbers are REPORTED*/
/* as the cross-backend comparison (on-device obligation D3) rather than    */
/* asserted locally. Asserting them there would convert a backend           */
/* difference into a test failure with no way to tell the two apart.        */
/* ====================================================================== */
struct GoldenRgba8 {
    const char *path;
    uint64_t fnv1a64;
};
const GoldenRgba8 kGoldenRgba8[] = {
    {"image_samples/raw_sample.arw", 0x0409d0d2c5116a9aULL},
    {"image_samples/raw_corpus/fuji_xt3.raf", 0xb4d193d3414c5896ULL},
    {"image_samples/raw_corpus/sigma_sd_quattro_h_23.x3f",
     0xdff593ab758aaeb0ULL},
};

void run_y1(const std::vector<std::pair<std::string, std::string>> &corpus) {
    for (const auto &entry : corpus) {
        const char *path = entry.first.c_str();
        const char *route = entry.second.c_str();
        Rgba via_default, via_format;
        CeyxYuv420PlaneDescriptor planes{};
        memset(&planes, 0, sizeof(planes));
        planes.struct_size = static_cast<uint32_t>(sizeof(planes));
        int32_t e1 = 0, e2 = 0;
        char detail[1024];

        if (!decode_rgba8_default(path, &via_default, &e1)) {
            snprintf(detail, sizeof(detail),
                     "%s: default rgba8 decode failed err=%d", path, e1);
            verdict("Y1", route, host_arm(), false, detail);
            continue;
        }
        if (!decode_rgba8_format(path, &via_format, &planes, &e2)) {
            snprintf(detail, sizeof(detail),
                     "%s: format-entry rgba8 decode failed err=%d", path, e2);
            verdict("Y1", route, host_arm(), false, detail);
            continue;
        }

        const bool same_extent = (via_default.w == via_format.w &&
                                  via_default.h == via_format.h);
        const bool identical =
            same_extent && via_default.bytes.size() == via_format.bytes.size() &&
            memcmp(via_default.bytes.data(), via_format.bytes.data(),
                   via_default.bytes.size()) == 0;

        bool descriptor_zeroed =
            planes.struct_size == sizeof(CeyxYuv420PlaneDescriptor);
        for (int i = 0; i < 3; ++i) {
            descriptor_zeroed = descriptor_zeroed &&
                                planes.plane_base[i] == nullptr &&
                                planes.plane_width[i] == 0 &&
                                planes.plane_height[i] == 0 &&
                                planes.plane_row_stride[i] == 0;
        }

        const uint64_t hash =
            fnv1a64(via_default.bytes.data(), via_default.bytes.size());

        /* (c) the golden literal. */
        uint64_t golden = 0;
        for (const GoldenRgba8 &g : kGoldenRgba8) {
            if (strcmp(g.path, path) == 0) golden = g.fnv1a64;
        }
        const bool have_golden = (golden != 0);
        const bool hash_matches = have_golden && hash == golden;
#if defined(DNG_STAGE4_SPLIT_KERNEL)
        /* Split/Vulkan build: REPORT the comparison, do not assert it — a
         * genuine backend difference must stay distinguishable from a
         * regression. This line IS the cross-backend evidence (D3). */
        const bool golden_gates = false;
#else
        const bool golden_gates = true;
#endif

        snprintf(detail, sizeof(detail),
                 "%s %dx%d rgba8_fnv1a64=%016" PRIx64
                 " GOLDEN=%016" PRIx64 " match=%d (%s) identical=%d "
                 "descriptor_zeroed_for_rgba8=%d",
                 path, via_default.w, via_default.h, hash, golden,
                 hash_matches ? 1 : 0,
                 golden_gates ? "ASSERTED on the Metal arm"
                              : "REPORTED only — split arm, cross-backend "
                                "evidence for D3, not a local assertion",
                 identical ? 1 : 0, descriptor_zeroed ? 1 : 0);
        verdict("Y1", route, host_arm(),
                identical && descriptor_zeroed && have_golden &&
                    (!golden_gates || hash_matches),
                detail);
    }
}

/* ====================================================================== */
/* Y2 — DESTINATION BYTE COUNT                                            */
/*                                                                        */
/* Expected values are LITERALS. Recomputing them with the same expression */
/* the code under test uses is tautological and would pass on a broken      */
/* implementation (plan T12.3 Y9's rule, which applies equally here).      */
/* The odd-extent rows are the ones mutation M1 turns red; the even rows   */
/* are the control that must STAY green, because that contrast is what      */
/* shows the odd assertions are capable of failing at all.                  */
/* ====================================================================== */
void run_y2() {
    struct Row {
        int32_t w, h;
        int64_t expect_yuv;  /* literal, hand-computed */
        int64_t expect_rgba; /* literal, hand-computed */
        const char *note;
    };
    /* Hand-computed, shown so a reader can check them without running:
     *   6024x4024: 24240576 + 2*(3012*2012 = 6060144)  = 36360864
     *   6000x4000: 24000000 + 2*(3000*2000 = 6000000)  = 36000000
     *   6246x4170: 26045820 + 2*(3123*2085 = 6511455)  = 39068730
     *   7x5  ODD : 35       + 2*(4*3 = 12)             = 59
     *   9x9  ODD : 81       + 2*(5*5 = 25)             = 131
     *   1x1  ODD : 1        + 2*(1*1 = 1)              = 3
     *   6025x4024 ODD W: 24244600 + 2*(3013*2012=6062156) = 36368912
     *   6024x4025 ODD H: 24246600 + 2*(3012*2013=6063156) = 36372912
     */
    const Row rows[] = {
        {6024, 4024, 36360864, 96962304, "even (raw_sample.arw)"},
        {6000, 4000, 36000000, 96000000, "even (dng samples)"},
        {6246, 4170, 39068730, 104183280, "even (fuji_xt3.raf)"},
        {7, 5, 59, 140, "ODD w and ODD h"},
        {9, 9, 131, 324, "ODD w and ODD h"},
        {1, 1, 3, 4, "ODD minimum"},
        {6025, 4024, 36368912, 96978400, "ODD w, even h"},
        {6024, 4025, 36372912, 96986400, "even w, ODD h"},
    };
    for (const Row &r : rows) {
        const int64_t got_yuv =
            ceyx_output_format_byte_count(kCeyxOutputFormatYuv420, r.w, r.h);
        const int64_t got_rgba =
            ceyx_output_format_byte_count(kCeyxOutputFormatRgba8, r.w, r.h);
        char detail[320];
        snprintf(detail, sizeof(detail),
                 "%dx%d %s yuv=%" PRId64 " (expect %" PRId64 ") rgba=%" PRId64
                 " (expect %" PRId64 ")",
                 r.w, r.h, r.note, got_yuv, r.expect_yuv, got_rgba,
                 r.expect_rgba);
        verdict("Y2", "arithmetic", "format-agnostic",
                got_yuv == r.expect_yuv && got_rgba == r.expect_rgba, detail);
    }

    /* Rejection cases: an unknown format and a non-positive extent must
     * return -1, never a plausible number a caller would allocate against. */
    const bool rejects =
        ceyx_output_format_byte_count(2, 16, 16) == -1 &&
        ceyx_output_format_byte_count(-1, 16, 16) == -1 &&
        ceyx_output_format_byte_count(kCeyxOutputFormatYuv420, 0, 16) == -1 &&
        ceyx_output_format_byte_count(kCeyxOutputFormatYuv420, 16, -3) == -1;
    verdict("Y2", "arithmetic", "format-agnostic", rejects,
            "unknown format and non-positive extents all return -1");

    /* And the probe agrees with the contract function for a real file. */
    int32_t w = 0, h = 0;
    int64_t probe_bytes = 0;
    const int32_t rc = ceyx_probe_output_size_format(
        "image_samples/raw_sample.arw", 0, kCeyxOutputFormatYuv420, &w, &h,
        &probe_bytes);
    char detail[256];
    snprintf(detail, sizeof(detail),
             "probe rc=%d %dx%d bytes=%" PRId64 " (expect 6024x4024 36360864)",
             rc, w, h, probe_bytes);
    verdict("Y2", "bayer-arw", host_arm(),
            rc == 0 && w == 6024 && h == 4024 && probe_bytes == 36360864,
            detail);
}

/* ====================================================================== */
/* Y3 — ODD DIMENSIONS                                                    */
/*                                                                        */
/* SCOPE HONESTY, and it must not be softened in any report built on this: */
/* no in-repo sample decodes to an odd extent (t12c-00-dims.txt: all 11     */
/* samples are even x even) and a scaled yuv420 request is REFUSED          */
/* (dng_render_halide.cpp:1549), so there is NO route to an odd-extent      */
/* decode. The odd-extent behaviour of ceyxFillYuv420PlaneDescriptor is     */
/* therefore UNREACHABLE through the public API and is NOT tested here.     */
/*                                                                         */
/* What IS reachable, and is what this case covers, is the exported         */
/* converter — which computes the same plane offsets at caller-chosen       */
/* extents and is otherwise UNEXERCISED by anything in the tree. A naive    */
/* w/2 inside it reads the wrong plane bytes and, on the last chroma row or */
/* column, reads PAST the source entirely. Guard bytes on both sides of the */
/* destination detect the write side; the source is sized to the literal    */
/* contract byte count so an over-read is an ASAN-visible overrun rather    */
/* than a silent wrong pixel.                                              */
/* ====================================================================== */
void run_y3() {
    struct Case {
        int32_t w, h;
        int64_t expect_contract_bytes; /* LITERAL, hand-computed */
    };
    /* Hand-computed, shown so a reader can check them without running:
     *   7x5   : 35 + 2*(4*3 = 12) = 59
     *   9x9   : 81 + 2*(5*5 = 25) = 131
     *   1x1   : 1  + 2*(1*1 = 1)  = 3
     *   15x1  : 15 + 2*(8*1 = 8)  = 31
     *   1x15  : 15 + 2*(1*8 = 8)  = 31
     *   8x8   : 64 + 2*(4*4 = 16) = 96   <- the EVEN control: mutation M1
     *                                       must leave this row green while
     *                                       every ODD row above goes red.
     */
    const Case cases[] = {
        {7, 5, 59}, {9, 9, 131}, {1, 1, 3},
        {15, 1, 31}, {1, 15, 31}, {8, 8, 96},
    };

    for (const Case &c : cases) {
        /* cw/ch lay out this test's OWN synthetic source and its OWN
         * independent expectation. They are written here as an explicit
         * ceiling rather than taken from the code under test, so the two
         * addressings are genuinely separate implementations. The size the
         * converter is held to is the LITERAL in the table above. */
        const int32_t cw = (c.w + 1) / 2;
        const int32_t ch = (c.h + 1) / 2;
        const int64_t contract_bytes = c.expect_contract_bytes;
        const int64_t reported_bytes =
            ceyx_output_format_byte_count(kCeyxOutputFormatYuv420, c.w, c.h);

        std::vector<uint8_t> src(static_cast<size_t>(c.w) * c.h + 2u * cw * ch);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff);
        }

        /* Destination with guard bytes on both sides. */
        const size_t guard = 64;
        const size_t dst_pixels = static_cast<size_t>(c.w) * c.h;
        std::vector<uint8_t> arena(guard + dst_pixels * 4 + guard, 0xA5);
        uint8_t *dst = arena.data() + guard;
        memset(dst, 0, dst_pixels * 4);

        const int32_t rc = ceyx_yuv420_to_rgba8(src.data(), src.size(), dst,
                                                dst_pixels * 4, c.w, c.h);

        bool guards_intact = true;
        for (size_t i = 0; i < guard; ++i) {
            if (arena[i] != 0xA5) guards_intact = false;
            if (arena[guard + dst_pixels * 4 + i] != 0xA5) guards_intact = false;
        }

        /* Independent expectation: recompute every pixel here with LITERAL
         * plane offsets derived from this case's fixed dimensions. This is
         * deliberately a second implementation of the addressing — if the
         * converter used w/2 where this uses ceil, they disagree, and they
         * disagree ONLY at odd extents. */
        const uint8_t *y_plane = src.data();
        const uint8_t *cb_plane = y_plane + dst_pixels;
        const uint8_t *cr_plane = cb_plane + static_cast<size_t>(cw) * ch;
        bool pixels_match = true;
        int32_t first_bad_x = -1, first_bad_y = -1;
        for (int32_t yy = 0; yy < c.h && pixels_match; ++yy) {
            for (int32_t xx = 0; xx < c.w; ++xx) {
                const size_t ci =
                    static_cast<size_t>(yy / 2) * cw + (xx / 2);
                uint8_t r = 0, g = 0, b = 0;
                ceyx::yuv420::rgb_from_ycbcr(
                    y_plane[static_cast<size_t>(yy) * c.w + xx], cb_plane[ci],
                    cr_plane[ci], &r, &g, &b);
                const size_t di = (static_cast<size_t>(yy) * c.w + xx) * 4;
                if (dst[di + 0] != r || dst[di + 1] != g || dst[di + 2] != b ||
                    dst[di + 3] != 255) {
                    pixels_match = false;
                    first_bad_x = xx;
                    first_bad_y = yy;
                    break;
                }
            }
        }

        /* Under-sized source must be refused before any read. One byte short
         * of the CONTRACT size is the boundary that a naive w/2 sizing would
         * ACCEPT: at 7x5 the contract needs 59 bytes and a truncating w/2
         * computes 35 + 2*(3*2) = 47, so 58 bytes would sail through and the
         * converter would then read past the end of its own source. At an
         * EVEN extent the two agree and this rejects either way — which is
         * why the 8x8 control row below is in the table.
         *
         * The expected CODE is non-zero, not specifically -301: a source
         * shortfall is deliberately a -1 argument error rather than
         * kCeyxErrDstTooSmall, because that code names a DESTINATION that
         * cannot hold the result and reporting it here would send the caller
         * off to grow the wrong buffer (raw_ffi_api.cpp:254-256). The
         * destination shortfall below IS -301. */
        const int32_t rc_short = ceyx_yuv420_to_rgba8(
            src.data(), static_cast<size_t>(contract_bytes) - 1, dst,
            dst_pixels * 4, c.w, c.h);
        const int32_t rc_dst_short = ceyx_yuv420_to_rgba8(
            src.data(), src.size(), dst, dst_pixels * 4 - 1, c.w, c.h);

        const bool ok = rc == 0 && guards_intact && pixels_match &&
                        reported_bytes == contract_bytes && rc_short != 0 &&
                        rc_dst_short == kCeyxErrDstTooSmall;
        char detail[448];
        snprintf(detail, sizeof(detail),
                 "%dx%d (%s) rc=%d guards=%d pixels=%d first_bad=(%d,%d) "
                 "bytes=%" PRId64 " (LITERAL expect %" PRId64
                 ") short_src=%d short_dst=%d",
                 c.w, c.h,
                 ((c.w % 2) || (c.h % 2)) ? "ODD" : "even", rc,
                 guards_intact ? 1 : 0, pixels_match ? 1 : 0, first_bad_x,
                 first_bad_y, reported_bytes, contract_bytes, rc_short,
                 rc_dst_short);
        verdict("Y3", "converter-synthetic", "host-ceyx_yuv420_to_rgba8", ok,
                detail);
    }

    verdict_na("Y3-descriptor", "n/a", host_arm(),
               "ODD-EXTENT DESCRIPTOR UNREACHABLE: every in-repo sample is "
               "even x even (t12c-00-dims.txt) and scaled yuv420 is refused "
               "(dng_render_halide.cpp:1549), so no odd-extent decode exists. "
               "The descriptor shares ceyx::yuv420::chroma_extent with the two "
               "cases above; that is a shared-implementation argument, WEAKER "
               "than a direct test, and is not to be reported as coverage.");
}

/* ====================================================================== */
/* Y4a — FIDELITY AGAINST REAL libjpeg, PLANE FOR PLANE (non-circular)    */
/*                                                                        */
/* Asserting our conversion against the oracle it was transcribed from    */
/* CANNOT FAIL — that is the shape this campaign keeps catching. The      */
/* independent authority has to be libjpeg itself.                        */
/*                                                                        */
/* ROUTE, and why it is not the obvious one: ceyx_still_decode_rgba has   */
/* NO libjpeg arm (still_ffi_api.cpp:137-144 returns 0 from               */
/* ceyx_still_decode_supports(kCeyxFormatJpeg) on purpose, and a JPEG     */
/* path answers kCeyxStillErrOpenFailed). So this case compresses with    */
/* libjpeg-turbo directly and reads the result back with                  */
/* jpeg_read_raw_data, which hands over libjpeg's OWN downsampled Y, Cb   */
/* and Cr planes.                                                        */
/*                                                                        */
/* Plane-for-plane is the STRONGER comparison, not a consolation:         */
/*   - it tests the forward matrix, BOTH rounding terms (luma adds        */
/*     ONE_HALF, chroma adds CBCR_OFFSET + ONE_HALF - 1) and the 2x2 box  */
/*     bias DIRECTLY, instead of inferring them through an RGB round trip;*/
/*   - it never involves an upsampler, so it cannot be confounded by      */
/*     libjpeg's fancy triangular upsample, which our inverse             */
/*     deliberately does not share.                                       */
/*                                                                        */
/* CONTENT IS FLAT SINGLE COLOURS at quality 100: a constant block's DCT  */
/* is its DC term alone, so the round trip is exact and the coefficients  */
/* and rounding convention are the only things the comparison can see.    */
/* The saturated primaries and the two range extremes are included        */
/* because a dropped `- 1` in the chroma rounding shows up at the top of  */
/* the range and nowhere else.                                            */
/*                                                                        */
/* The second half of the case then feeds LIBJPEG'S OWN PLANES through    */
/* the exported ceyx_yuv420_to_rgba8 and checks the colour comes back —   */
/* which is what exercises the INVERSE against data we did not produce.   */
/* ====================================================================== */
struct JpegErrTrap {
    struct jpeg_error_mgr base;
    jmp_buf escape;
};

void jpeg_trap_error(j_common_ptr cinfo) {
    JpegErrTrap *t = reinterpret_cast<JpegErrTrap *>(cinfo->err);
    longjmp(t->escape, 1);
}

/* Compress `rgb` (packed RGB, 3 B/px) at quality `q` with 4:2:0 subsampling,
 * then decompress it in RAW mode and hand back libjpeg's downsampled planes.
 * Returns false if libjpeg raised at any point. */
bool libjpeg_roundtrip_planes(const uint8_t *rgb, int32_t w, int32_t h,
                              int q, std::vector<uint8_t> *out_y,
                              std::vector<uint8_t> *out_cb,
                              std::vector<uint8_t> *out_cr, int32_t *out_cw,
                              int32_t *out_ch, const char **why) {
    uint8_t *buf = nullptr;
    unsigned long buf_len = 0;

    {
        struct jpeg_compress_struct cinfo;
        JpegErrTrap jerr;
        cinfo.err = jpeg_std_error(&jerr.base);
        jerr.base.error_exit = jpeg_trap_error;
        if (setjmp(jerr.escape)) {
            jpeg_destroy_compress(&cinfo);
            if (buf) free(buf);
            *why = "libjpeg raised during compress";
            return false;
        }
        jpeg_create_compress(&cinfo);
        jpeg_mem_dest(&cinfo, &buf, &buf_len);
        cinfo.image_width = static_cast<JDIMENSION>(w);
        cinfo.image_height = static_cast<JDIMENSION>(h);
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;
        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, q, TRUE);
        /* 4:2:0 explicitly rather than by default, so the case states the
         * sampling it is testing instead of inheriting it. */
        cinfo.comp_info[0].h_samp_factor = 2;
        cinfo.comp_info[0].v_samp_factor = 2;
        cinfo.comp_info[1].h_samp_factor = 1;
        cinfo.comp_info[1].v_samp_factor = 1;
        cinfo.comp_info[2].h_samp_factor = 1;
        cinfo.comp_info[2].v_samp_factor = 1;
        jpeg_start_compress(&cinfo, TRUE);
        while (cinfo.next_scanline < cinfo.image_height) {
            JSAMPROW row = const_cast<JSAMPROW>(
                rgb + static_cast<size_t>(cinfo.next_scanline) * w * 3);
            jpeg_write_scanlines(&cinfo, &row, 1);
        }
        jpeg_finish_compress(&cinfo);
        jpeg_destroy_compress(&cinfo);
    }

    {
        struct jpeg_decompress_struct dinfo;
        JpegErrTrap jerr;
        dinfo.err = jpeg_std_error(&jerr.base);
        jerr.base.error_exit = jpeg_trap_error;
        if (setjmp(jerr.escape)) {
            jpeg_destroy_decompress(&dinfo);
            free(buf);
            *why = "libjpeg raised during decompress";
            return false;
        }
        jpeg_create_decompress(&dinfo);
        jpeg_mem_src(&dinfo, buf, buf_len);
        jpeg_read_header(&dinfo, TRUE);
        /* RAW mode: no colour conversion, no upsampling. The planes come out
         * exactly as libjpeg stored them. */
        dinfo.raw_data_out = TRUE;
        dinfo.out_color_space = JCS_YCbCr;
        jpeg_start_decompress(&dinfo);

        const int32_t cw = (w + 1) / 2;
        const int32_t ch = (h + 1) / 2;
        /* libjpeg writes in MCU-row units and pads its internal planes up to a
         * multiple of the sampling factor times DCTSIZE, so the buffers must be
         * allocated to the PADDED height or jpeg_read_raw_data writes past
         * them. The padded rows are then discarded. */
        const int32_t y_stride =
            static_cast<int32_t>(dinfo.comp_info[0].width_in_blocks) * DCTSIZE;
        const int32_t c_stride =
            static_cast<int32_t>(dinfo.comp_info[1].width_in_blocks) * DCTSIZE;
        const int mcu_rows = dinfo.max_v_samp_factor * DCTSIZE;
        const int32_t y_alloc_h =
            ((h + mcu_rows - 1) / mcu_rows) * mcu_rows;
        const int32_t c_alloc_h = y_alloc_h / 2;

        std::vector<uint8_t> y_buf(static_cast<size_t>(y_stride) * y_alloc_h);
        std::vector<uint8_t> cb_buf(static_cast<size_t>(c_stride) * c_alloc_h);
        std::vector<uint8_t> cr_buf(static_cast<size_t>(c_stride) * c_alloc_h);

        std::vector<JSAMPROW> y_rows(mcu_rows), cb_rows(mcu_rows / 2),
            cr_rows(mcu_rows / 2);
        while (dinfo.output_scanline < dinfo.output_height) {
            const int32_t sl = static_cast<int32_t>(dinfo.output_scanline);
            for (int i = 0; i < mcu_rows; ++i) {
                y_rows[i] = y_buf.data() +
                            static_cast<size_t>(sl + i) * y_stride;
            }
            for (int i = 0; i < mcu_rows / 2; ++i) {
                cb_rows[i] = cb_buf.data() +
                             static_cast<size_t>(sl / 2 + i) * c_stride;
                cr_rows[i] = cr_buf.data() +
                             static_cast<size_t>(sl / 2 + i) * c_stride;
            }
            JSAMPARRAY planes[3] = {y_rows.data(), cb_rows.data(),
                                    cr_rows.data()};
            jpeg_read_raw_data(&dinfo, planes, mcu_rows);
        }
        jpeg_finish_decompress(&dinfo);
        jpeg_destroy_decompress(&dinfo);
        free(buf);

        /* Repack from libjpeg's padded strides into tight planes. */
        out_y->assign(static_cast<size_t>(w) * h, 0);
        out_cb->assign(static_cast<size_t>(cw) * ch, 0);
        out_cr->assign(static_cast<size_t>(cw) * ch, 0);
        for (int32_t yy = 0; yy < h; ++yy) {
            memcpy(out_y->data() + static_cast<size_t>(yy) * w,
                   y_buf.data() + static_cast<size_t>(yy) * y_stride, w);
        }
        for (int32_t yy = 0; yy < ch; ++yy) {
            memcpy(out_cb->data() + static_cast<size_t>(yy) * cw,
                   cb_buf.data() + static_cast<size_t>(yy) * c_stride, cw);
            memcpy(out_cr->data() + static_cast<size_t>(yy) * cw,
                   cr_buf.data() + static_cast<size_t>(yy) * c_stride, cw);
        }
        *out_cw = cw;
        *out_ch = ch;
    }
    *why = "";
    return true;
}

void run_y4a() {
    struct Colour {
        uint8_t r, g, b;
        const char *name;
    };
    const Colour colours[] = {
        {0, 0, 0, "black"},            {255, 255, 255, "white"},
        {255, 0, 0, "red"},            {0, 255, 0, "green"},
        {0, 0, 255, "blue"},           {255, 255, 0, "yellow"},
        {0, 255, 255, "cyan"},         {255, 0, 255, "magenta"},
        {128, 128, 128, "grey128"},    {1, 1, 1, "near-black"},
        {254, 254, 254, "near-white"}, {200, 30, 90, "crimson"},
        {17, 200, 140, "teal"},        {90, 90, 200, "periwinkle"},
    };
    const int32_t w = 16, h = 16;
    const int32_t cw = 8, ch = 8; /* LITERAL for 16x16 4:2:0 */

    int32_t worst_plane = 0;      /* max |delta| over Y, Cb, Cr */
    int32_t worst_rgb = 0;        /* max |delta| after the inverse */
    const char *worst_colour = "none";
    int examined = 0;
    bool all_ok = true;
    const int total = static_cast<int>(sizeof(colours) / sizeof(colours[0]));

    for (const Colour &c : colours) {
        std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            rgb[i * 3 + 0] = c.r;
            rgb[i * 3 + 1] = c.g;
            rgb[i * 3 + 2] = c.b;
        }

        std::vector<uint8_t> jy, jcb, jcr;
        int32_t got_cw = 0, got_ch = 0;
        const char *why = "";
        if (!libjpeg_roundtrip_planes(rgb.data(), w, h, 100, &jy, &jcb, &jcr,
                                      &got_cw, &got_ch, &why)) {
            printf("  [Y4a-diag] %s: %s\n", c.name, why);
            all_ok = false;
            continue;
        }
        if (got_cw != cw || got_ch != ch) {
            printf("  [Y4a-diag] %s: libjpeg chroma extent %dx%d, expected "
                   "%dx%d\n",
                   c.name, got_cw, got_ch, cw, ch);
            all_ok = false;
            continue;
        }

        /* OUR forward, from the oracle the kernels build their Exprs from. */
        std::vector<uint8_t> yuv;
        std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            rgba[i * 4 + 0] = c.r;
            rgba[i * 4 + 1] = c.g;
            rgba[i * 4 + 2] = c.b;
            rgba[i * 4 + 3] = 255;
        }
        oracle_forward_yuv420(rgba.data(), w, h, &yuv);

        const uint8_t *oy = yuv.data();
        const uint8_t *ocb = oy + static_cast<size_t>(w) * h;
        const uint8_t *ocr = ocb + static_cast<size_t>(cw) * ch;

        int32_t local = 0;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const int32_t d = abs(static_cast<int32_t>(oy[i]) -
                                  static_cast<int32_t>(jy[i]));
            if (d > local) local = d;
        }
        for (size_t i = 0; i < static_cast<size_t>(cw) * ch; ++i) {
            const int32_t dcb = abs(static_cast<int32_t>(ocb[i]) -
                                    static_cast<int32_t>(jcb[i]));
            const int32_t dcr = abs(static_cast<int32_t>(ocr[i]) -
                                    static_cast<int32_t>(jcr[i]));
            if (dcb > local) local = dcb;
            if (dcr > local) local = dcr;
        }

        /* The INVERSE, run over LIBJPEG'S OWN planes — data this test did not
         * produce — and checked back against the colour we started from. */
        std::vector<uint8_t> their_packed;
        their_packed.reserve(static_cast<size_t>(w) * h + 2u * cw * ch);
        their_packed.insert(their_packed.end(), jy.begin(), jy.end());
        their_packed.insert(their_packed.end(), jcb.begin(), jcb.end());
        their_packed.insert(their_packed.end(), jcr.begin(), jcr.end());
        std::vector<uint8_t> restored(static_cast<size_t>(w) * h * 4, 0);
        const int32_t rc =
            ceyx_yuv420_to_rgba8(their_packed.data(), their_packed.size(),
                                 restored.data(), restored.size(), w, h);
        if (rc != 0) {
            printf("  [Y4a-diag] %s: converter rc=%d\n", c.name, rc);
            all_ok = false;
            continue;
        }
        int32_t local_rgb = 0;
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const int32_t dr = abs(static_cast<int32_t>(restored[i * 4 + 0]) -
                                   static_cast<int32_t>(c.r));
            const int32_t dg = abs(static_cast<int32_t>(restored[i * 4 + 1]) -
                                   static_cast<int32_t>(c.g));
            const int32_t db = abs(static_cast<int32_t>(restored[i * 4 + 2]) -
                                   static_cast<int32_t>(c.b));
            if (dr > local_rgb) local_rgb = dr;
            if (dg > local_rgb) local_rgb = dg;
            if (db > local_rgb) local_rgb = db;
            if (restored[i * 4 + 3] != 255) local_rgb = 999;
        }

        ++examined;
        if (local > worst_plane) {
            worst_plane = local;
            worst_colour = c.name;
        }
        if (local_rgb > worst_rgb) worst_rgb = local_rgb;
    }

    /* PRE-REGISTERED BOUND (t12c-00-prereg.txt §1): max per-channel |delta|
     * <= 8. Applied to BOTH halves — the plane comparison against libjpeg's
     * own output, and the inverse run over libjpeg's planes. A transcription
     * error in any single coefficient moves a saturated channel by >= 25, so
     * 8 separates the two while tolerating the q100 DCT round trip. */
    const int32_t kBound = 8;
    char detail[416];
    snprintf(detail, sizeof(detail),
             "colours=%d/%d PLANE max|delta| vs libjpeg's own Y/Cb/Cr = %d "
             "(PRE-REGISTERED BOUND <=%d) worst_colour=%s | INVERSE over "
             "libjpeg's planes max|delta| vs source colour = %d (<=%d)",
             examined, total, worst_plane, kBound, worst_colour, worst_rgb,
             kBound);
    verdict("Y4a", "synthetic-flat", "libjpeg-plane-oracle",
            all_ok && examined == total && worst_plane <= kBound &&
                worst_rgb <= kBound,
            detail);
}

/* ====================================================================== */
/* Y4b — PER-ARM FIDELITY ON A REAL FRAME                                 */
/*                                                                        */
/* AMENDED 2026-09-20 BY LEAD RULING, and the amendment's own history is  */
/* kept here rather than tidied away, because "the bound was changed after */
/* the number arrived" is exactly the move this campaign exists to catch   */
/* and the only defence is that the change is visible and strictly         */
/* stronger.                                                               */
/*                                                                        */
/* ORIGINALLY: three pre-registered statistical bounds on the round trip   */
/* (mean <= 3.0 per channel, >= 99.0% of pixels within +/-16, max <= 128). */
/* On sigma_sd_quattro_h_23.x3f those measured mean=(1.4929, 2.2640,       */
/* 8.1951), within16=0.85533, max=160 — a clear breach.                    */
/*                                                                         */
/* WHY THE BOUND WAS THE WRONG INSTRUMENT, established mechanically and    */
/* not by argument: the HOST-FORWARD CONTROL below runs the same reference */
/* through the oracle's forward transform on the host, with no GPU         */
/* involved, and reproduces the kernel's figures TO FOUR DECIMAL PLACES,   */
/* with the planes byte-identical (diff_bytes = 0). So the mean bound was  */
/* measuring 4:2:0's information loss on full-colour-per-photosite Foveon  */
/* content — not kernel correctness. The instrument mismeasured the        */
/* subject.                                                                */
/*                                                                         */
/* AMENDED ASSERTION: kernel planes == host-forward-control planes, byte   */
/* for byte. Strictly stronger than the bounds it replaces — it is         */
/* content-independent, cannot be passed by easy content, and it asserts   */
/* the actual subject (the kernel reproduces the ideal 4:2:0 result)       */
/* rather than a proxy for it. The three statistics remain REPORTED as     */
/* diagnostics on every route.                                             */
/*                                                                         */
/* THE ORIGINAL RED IS NOT ERASED. Every route also reports its verdict    */
/* against the bound AS ORIGINALLY WRITTEN, so x3f keeps printing          */
/* FAIL-AS-WRITTEN forever. A superseded assertion stays on the record     */
/* beside the one that replaced it.                                        */
/* ====================================================================== */
void run_y4b(const std::vector<std::pair<std::string, std::string>> &corpus) {
    /* The ORIGINAL pre-registered bounds (t12c-00-prereg.txt §1). Retained
     * and still evaluated — superseded as the ASSERTION, not deleted. */
    const double kMeanBound = 3.0;
    const double kWithin16Bound = 0.990;
    const int32_t kMaxBound = 128;

    for (const auto &entry : corpus) {
        const char *path = entry.first.c_str();
        const char *route = entry.second.c_str();
        Rgba reference;
        Yuv yuv;
        int32_t e1 = 0, e2 = 0;
        char detail[1024];

        if (!decode_rgba8_default(path, &reference, &e1)) {
            snprintf(detail, sizeof(detail), "%s: rgba8 decode failed err=%d",
                     path, e1);
            verdict("Y4b", route, host_arm(), false, detail);
            continue;
        }
        if (!decode_yuv420(path, &yuv, &e2)) {
            snprintf(detail, sizeof(detail), "%s: yuv420 decode failed err=%d",
                     path, e2);
            verdict("Y4b", route, host_arm(), false, detail);
            continue;
        }

        std::vector<uint8_t> restored(
            static_cast<size_t>(reference.w) * reference.h * 4, 0);
        const int32_t rc =
            ceyx_yuv420_to_rgba8(yuv.bytes.data(), yuv.bytes.size(),
                                 restored.data(), restored.size(), yuv.w, yuv.h);
        if (rc != 0) {
            snprintf(detail, sizeof(detail), "%s: converter rc=%d", path, rc);
            verdict("Y4b", route, host_arm(), false, detail);
            continue;
        }

        const DeltaStats s = compare_rgb(
            restored, reference.bytes,
            static_cast<size_t>(reference.w) * reference.h);
        /* HOST-FORWARD CONTROL, and it is what makes a Y4b failure
         * interpretable rather than merely alarming.
         *
         * The bound above can be crossed by two completely different
         * situations: a defect in the KERNEL's forward transform, and content
         * whose chroma detail is simply too fine for 4:2:0 to carry. One
         * number cannot tell them apart, and choosing between them by
         * intuition is how a real defect gets explained away as "that file is
         * just noisy" — or, worse, how a pre-registered bound gets quietly
         * relaxed until it passes.
         *
         * So the same rgba8 reference is pushed through the ORACLE's forward
         * transform on the host and back through the same converter. That is
         * the ideal 4:2:0 loss for THIS EXACT CONTENT, computed without the
         * GPU. Read the two together:
         *   kernel ~= host -> the format's own loss; the kernel agrees with
         *                     the oracle and the excess is the content.
         *   kernel >> host -> the kernel's forward transform diverges, and
         *                     the gap is the size of the defect.
         * Reported on EVERY route, pass or fail, so the comparison is on the
         * record rather than reconstructed after a surprise. */
        std::vector<uint8_t> host_yuv;
        oracle_forward_yuv420(reference.bytes.data(), reference.w, reference.h,
                              &host_yuv);
        std::vector<uint8_t> host_restored(
            static_cast<size_t>(reference.w) * reference.h * 4, 0);
        DeltaStats hs;
        bool host_ok = false;
        if (ceyx_yuv420_to_rgba8(host_yuv.data(), host_yuv.size(),
                                 host_restored.data(), host_restored.size(),
                                 reference.w, reference.h) == 0) {
            hs = compare_rgb(host_restored, reference.bytes,
                             static_cast<size_t>(reference.w) * reference.h);
            host_ok = true;
        }

        /* KERNEL-vs-ORACLE PLANE IDENTITY. Strictly stronger than any bound
         * on the statistics above, and — unlike a bound — completely
         * independent of what the content happens to be: it asks whether the
         * GPU kernel's forward transform produced the SAME BYTES the oracle
         * produces for the same input, rather than whether the round trip
         * came out close enough. A bound can be passed by content that is
         * easy and failed by content that is hard; this cannot. */
        const bool planes_identical =
            host_yuv.size() == yuv.bytes.size() &&
            memcmp(host_yuv.data(), yuv.bytes.data(), yuv.bytes.size()) == 0;
        size_t first_plane_diff = 0;
        size_t plane_diff_count = 0;
        if (!planes_identical && host_yuv.size() == yuv.bytes.size()) {
            for (size_t i = 0; i < yuv.bytes.size(); ++i) {
                if (host_yuv[i] != yuv.bytes[i]) {
                    if (plane_diff_count == 0) first_plane_diff = i;
                    ++plane_diff_count;
                }
            }
        }

        /* The ORIGINAL pre-registered bound, still evaluated so its verdict
         * stays on the record. NOT the assertion any more — see the amendment
         * note in this case's header. */
        const bool passes_original_bound =
            s.mean[0] <= kMeanBound && s.mean[1] <= kMeanBound &&
            s.mean[2] <= kMeanBound &&
            s.fraction_within_16 >= kWithin16Bound && s.max_abs <= kMaxBound;

        /* THE AMENDED ASSERTION. */
        const bool ok = host_ok && planes_identical;

        snprintf(detail, sizeof(detail),
                 "%s %dx%d ASSERTION kernel_planes==host_forward_control=%d "
                 "(diff_bytes=%zu first@%zu) | diagnostics: KERNEL "
                 "mean=(%.4f,%.4f,%.4f) within16=%.5f max=%d | CONTROL "
                 "mean=(%.4f,%.4f,%.4f) within16=%.5f max=%d ok=%d | "
                 "ORIGINAL PRE-REGISTERED BOUND (mean<=%.1f within16>=%.3f "
                 "max<=%d) -> %s | yuv_fnv1a64=%016" PRIx64,
                 path, yuv.w, yuv.h, planes_identical ? 1 : 0,
                 plane_diff_count, first_plane_diff, s.mean[0], s.mean[1],
                 s.mean[2], s.fraction_within_16, s.max_abs, hs.mean[0],
                 hs.mean[1], hs.mean[2], hs.fraction_within_16, hs.max_abs,
                 host_ok ? 1 : 0, kMeanBound, kWithin16Bound, kMaxBound,
                 passes_original_bound ? "pass-as-written"
                                       : "FAIL-AS-WRITTEN (superseded: the "
                                         "mean bound measured 4:2:0's "
                                         "information loss on "
                                         "full-colour-per-photosite content, "
                                         "not kernel correctness — instrument "
                                         "mismeasured subject; the "
                                         "host-forward control proved kernel "
                                         "== ideal to four decimals with "
                                         "diff_bytes=0)",
                 fnv1a64(yuv.bytes.data(), yuv.bytes.size()));
        verdict("Y4b", route, host_arm(), ok, detail);
    }
}

/* ====================================================================== */
/* Y5 — NO FULL-FRAME RGBA SCRATCH ON THE yuv420 PATH                     */
/*                                                                        */
/* READING A ZERO HONESTLY: an unwired counter's 0 is indistinguishable    */
/* from a real one. So the rgba8 POSITIVE CONTROL runs first in the same   */
/* process and must report exactly w*h*4. If it reports 0, this case FAILS */
/* — the counter is unwired and the yuv420 zero is unverifiable. It does   */
/* NOT pass quietly.                                                       */
/*                                                                        */
/* The per-decode reading is thread-local and reset at entry; the          */
/* high-water is process-wide and monotonic. They answer different          */
/* questions and neither substitutes for the other, so both are asserted.  */
/* ====================================================================== */
void run_y5(const char *path, const char *route) {
    uint64_t last = 0, high = 0;
    char detail[448];

    Rgba reference;
    int32_t e1 = 0;
    if (!decode_rgba8_default(path, &reference, &e1)) {
        snprintf(detail, sizeof(detail), "%s: rgba8 control decode failed err=%d",
                 path, e1);
        verdict("Y5", route, host_arm(), false, detail);
        return;
    }
    ceyx_debug_stage4_rgba_scratch_counters(&last, &high);
    const uint64_t expect_full =
        static_cast<uint64_t>(reference.w) * reference.h * 4u;
    const bool control_moved = (last == expect_full);
    const uint64_t high_after_rgba = high;

    Yuv yuv;
    int32_t e2 = 0;
    if (!decode_yuv420(path, &yuv, &e2)) {
        snprintf(detail, sizeof(detail), "%s: yuv420 decode failed err=%d", path,
                 e2);
        verdict("Y5", route, host_arm(), false, detail);
        return;
    }
    ceyx_debug_stage4_rgba_scratch_counters(&last, &high);
    const bool yuv_zero = (last == 0);
    const bool high_retained = (high == high_after_rgba);

    snprintf(detail, sizeof(detail),
             "%s POSITIVE CONTROL rgba8 last=%llu (expect %llu, moved=%d) | "
             "yuv420 last=%llu (expect 0) high_water=%llu (retained=%d). "
             "The zero is quoted ONLY beside the control.",
             path, (unsigned long long)expect_full,
             (unsigned long long)expect_full, control_moved ? 1 : 0,
             (unsigned long long)last, (unsigned long long)high,
             high_retained ? 1 : 0);
    verdict("Y5", route, host_arm(),
            control_moved && yuv_zero && high_retained, detail);
}

/* ====================================================================== */
/* Y6 — THE DNG ROUTE SERVES yuv420, AND ITS rgba8 PATH IS UNDISTURBED    */
/*                                                                        */
/* AMENDED by T12.7 (user no-divergence ruling 2026-09-20). (b) used to    */
/* assert the T12.5 CARVE-OUT: that a yuv420 request on a DNG file was     */
/* REFUSED with kCeyxErrFormatUnsupportedInBuild. The ruling retired that  */
/* carve-out — the DNG route is the main path for this library's largest   */
/* consumer — so the same case now asserts the opposite fact. Two          */
/* assertions:                                                             */
/*   (a) its rgba8 decode still succeeds and both entries agree byte for    */
/*       byte — the shared-seam regression, the highest blast radius here;  */
/*   (b) a yuv420 request on a DNG file SUCCEEDS, and the descriptor it     */
/*       returns describes the frozen plane layout exactly: bases at        */
/*       0 / w*h / w*h+cw*ch, extents w x h and ceil(w/2) x ceil(h/2), row  */
/*       stride == plane width. The overrun the refusal protected against   */
/*       is now prevented by every destination on this route being sized    */
/*       through ceyx_output_format_byte_count, which is what makes         */
/*       serving it safe; asserting the EXTENTS (not merely err==0) is what */
/*       makes that claim checkable here.                                   */
/* ====================================================================== */
void run_y6() {
    const char *dngs[] = {"image_samples/lossless_dng_sample.dng",
                          "image_samples/lossy_dng_sample.dng"};
    for (const char *path : dngs) {
        Rgba via_default, via_format;
        int32_t e1 = 0, e2 = 0;
        char detail[768];
        const bool d1 = decode_rgba8_default(path, &via_default, &e1);
        const bool d2 = decode_rgba8_format(path, &via_format, nullptr, &e2);
        const bool identical =
            d1 && d2 && via_default.bytes.size() == via_format.bytes.size() &&
            memcmp(via_default.bytes.data(), via_format.bytes.data(),
                   via_default.bytes.size()) == 0;

        /* T12.7: the yuv420 DNG decode must SUCCEED, through the ordinary
         * helper every other case uses (so this asserts the shipped entry,
         * not a bespoke call), and its descriptor must spell the frozen
         * layout. Extents are recomputed here from the oracle rather than
         * copied from the descriptor — a descriptor compared against itself
         * would pass whatever it said. */
        Yuv yuv;
        int32_t e3 = 0;
        const bool yuv_ok = decode_yuv420(path, &yuv, &e3);
        namespace o = ceyx::yuv420;
        const int32_t cw = yuv_ok ? o::chroma_extent(yuv.w) : 0;
        const int32_t ch = yuv_ok ? o::chroma_extent(yuv.h) : 0;
        const size_t luma = static_cast<size_t>(yuv.w) * yuv.h;
        const size_t chroma = static_cast<size_t>(cw) * ch;
        const uint8_t *base = yuv_ok ? yuv.bytes.data() : nullptr;
        const bool size_ok =
            yuv_ok && yuv.bytes.size() == luma + 2u * chroma &&
            yuv.w == via_default.w && yuv.h == via_default.h;
        const bool planes_ok =
            yuv_ok && yuv.planes.plane_base[0] == base &&
            yuv.planes.plane_base[1] == base + luma &&
            yuv.planes.plane_base[2] == base + luma + chroma &&
            yuv.planes.plane_width[0] == yuv.w &&
            yuv.planes.plane_height[0] == yuv.h &&
            yuv.planes.plane_width[1] == cw && yuv.planes.plane_width[2] == cw &&
            yuv.planes.plane_height[1] == ch &&
            yuv.planes.plane_height[2] == ch &&
            yuv.planes.plane_row_stride[0] == yuv.planes.plane_width[0] &&
            yuv.planes.plane_row_stride[1] == yuv.planes.plane_width[1] &&
            yuv.planes.plane_row_stride[2] == yuv.planes.plane_width[2];

        snprintf(detail, sizeof(detail),
                 "%s rgba8_ok=%d/%d identical=%d rgba8_fnv1a64=%016" PRIx64
                 " | yuv420 decode ok=%d err=%d %dx%d chroma=%dx%d "
                 "bytes=%zu (expect %zu) size_ok=%d planes_ok=%d "
                 "yuv_fnv1a64=%016" PRIx64,
                 path, d1 ? 1 : 0, d2 ? 1 : 0, identical ? 1 : 0,
                 d1 ? fnv1a64(via_default.bytes.data(), via_default.bytes.size())
                    : 0,
                 yuv_ok ? 1 : 0, e3, yuv.w, yuv.h, cw, ch, yuv.bytes.size(),
                 luma + 2u * chroma, size_ok ? 1 : 0, planes_ok ? 1 : 0,
                 yuv_ok ? fnv1a64(yuv.bytes.data(), yuv.bytes.size()) : 0);
        verdict("Y6", "dng", host_arm(),
                identical && yuv_ok && size_ok && planes_ok, detail);
    }

    /* A SCALED yuv420 request must also refuse rather than silently crop.
     * Recorded here because it is the other structural refusal on this path
     * and its absence would be invisible to every other case. */
    int32_t w = 0, h = 0;
    int64_t bytes = 0;
    const char *arw = "image_samples/raw_sample.arw";
    const int32_t prc = ceyx_probe_output_size_format(
        arw, 2048, kCeyxOutputFormatYuv420, &w, &h, &bytes);
    char detail[320];
    if (prc != 0) {
        snprintf(detail, sizeof(detail), "scaled probe rc=%d", prc);
        verdict("Y6", "bayer-arw-scaled", host_arm(), false, detail);
    } else {
        std::vector<uint8_t> dst(static_cast<size_t>(bytes), 0);
        DngResult *r = ceyx_decode_into_buffer_format(
            arw, 2048, dst.data(), dst.size(), kCeyxOutputFormatYuv420,
            nullptr);
        const int32_t code = r ? r->error_code : -999;
        if (r) dng_free_result(r);
        snprintf(detail, sizeof(detail),
                 "scaled yuv420 request %dx%d -> err=%d (must be NON-ZERO: a "
                 "scaled yuv420 archive exists on neither family, so this "
                 "refuses on every backend)",
                 w, h, code);
        verdict("Y6", "bayer-arw-scaled", host_arm(), code != 0, detail);
    }
}

/* ====================================================================== */
/* Y10 — yuv420 FIDELITY ON A REAL CORPUS DNG (T12.7)                     */
/*                                                                        */
/* Y4b already measures round-trip fidelity, but only on generic-RAW       */
/* files. The consumer that motivated T12.7 opens DNGs almost exclusively, */
/* so the route this task newly serves needs its own fidelity number       */
/* rather than inheriting one measured on a different route's kernels.     */
/*                                                                        */
/* Same method and same reading discipline as Y4b: decode rgba8, decode    */
/* yuv420, convert back with ceyx_yuv420_to_rgba8 (T13), and report the    */
/* kernel's delta BESIDE the host-forward control — the ideal 4:2:0 loss   */
/* for this exact content, computed without the GPU. One number alone      */
/* cannot separate "the kernel's transform diverges" from "this content's  */
/* chroma detail is finer than 4:2:0 can carry"; the pair can.             */
/*                                                                        */
/* AMENDED ASSERTION (lead ruling, T12.7). The statistical bound below was */
/* pre-registered, measured, and FAILED on within16/max while the host     */
/* control — the theoretical best any correct implementation can reach on  */
/* this content — failed it IDENTICALLY, to four decimals and on the same  */
/* worst case. An instrument that a provably-ideal implementation also     */
/* fails is measuring the subject (4:2:0's information loss on real camera */
/* content), not the kernel. The assertion is therefore the kernel's       */
/* EQUALITY WITH THAT CONTROL, byte for byte across all three planes,      */
/* which is strictly STRONGER as a kernel-correctness claim than any       */
/* statistical bound: it admits no error at all rather than a budget of    */
/* it. The original bound is retained and PRINTED on every run as a        */
/* diagnostic, never asserted — superseded, not deleted.                   */
/*                                                                        */
/* The pattern is anchored, not re-argued: Y4b above was amended the same  */
/* way for the same reason (its linearrgb-x3f route fails the same style   */
/* of bound today and is accepted as content loss), and this round's       */
/* independent review of the identical control-equality amendment on T13's */
/* C5 returned LEGITIMATE-TIGHTENING                                       */
/* (Halcyon docs/logs/2026-09-20/t13-c5-amendment-review.md).              */
/*                                                                        */
/* The sample lives outside this repo (the consumer's corpus) and is not   */
/* vendored, so its absence is an explicit N/A with its reason, never a    */
/* silent skip. CEYX_T127_DNG overrides the path.                          */
/* ====================================================================== */
void run_y10() {
    const char *env = getenv("CEYX_T127_DNG");
    const char *path =
        env && env[0] ? env
            : "../Halcyon/local_data/photo_samples/DNG/IMG_20251112_092839.dng";
    char detail[1024];

    FILE *probe = fopen(path, "rb");
    if (!probe) {
        snprintf(detail, sizeof(detail),
                 "sample not present at %s (set CEYX_T127_DNG): fidelity on a "
                 "real corpus DNG NOT MEASURED in this run", path);
        verdict_na("Y10", "dng-corpus", host_arm(), detail);
        return;
    }
    fclose(probe);

    /* Pre-registered in native/tests/tmp/t127/prereg.txt, RUN 3, before any
     * of these numbers existed. */
    const double kMeanBound = 6.0;
    const double kWithin16Bound = 0.98;
    const int32_t kMaxBound = 96;

    Rgba reference;
    Yuv yuv;
    int32_t e1 = 0, e2 = 0;
    if (!decode_rgba8_default(path, &reference, &e1)) {
        snprintf(detail, sizeof(detail), "%s: rgba8 decode failed err=%d", path,
                 e1);
        verdict("Y10", "dng-corpus", host_arm(), false, detail);
        return;
    }
    if (!decode_yuv420(path, &yuv, &e2)) {
        snprintf(detail, sizeof(detail), "%s: yuv420 decode failed err=%d", path,
                 e2);
        verdict("Y10", "dng-corpus", host_arm(), false, detail);
        return;
    }
    /* F4: a fidelity number over two different extents is meaningless. */
    if (yuv.w != reference.w || yuv.h != reference.h) {
        snprintf(detail, sizeof(detail),
                 "%s: extent mismatch rgba8=%dx%d yuv420=%dx%d", path,
                 reference.w, reference.h, yuv.w, yuv.h);
        verdict("Y10", "dng-corpus", host_arm(), false, detail);
        return;
    }

    const size_t pixels = static_cast<size_t>(reference.w) * reference.h;
    std::vector<uint8_t> restored(pixels * 4, 0);
    const int32_t rc =
        ceyx_yuv420_to_rgba8(yuv.bytes.data(), yuv.bytes.size(),
                             restored.data(), restored.size(), yuv.w, yuv.h);
    if (rc != 0) {
        snprintf(detail, sizeof(detail), "%s: converter rc=%d", path, rc);
        verdict("Y10", "dng-corpus", host_arm(), false, detail);
        return;
    }
    const DeltaStats s = compare_rgb(restored, reference.bytes, pixels);

    std::vector<uint8_t> host_yuv;
    oracle_forward_yuv420(reference.bytes.data(), reference.w, reference.h,
                          &host_yuv);
    std::vector<uint8_t> host_restored(pixels * 4, 0);
    DeltaStats hs;
    bool host_ok = false;
    if (ceyx_yuv420_to_rgba8(host_yuv.data(), host_yuv.size(),
                             host_restored.data(), host_restored.size(),
                             reference.w, reference.h) == 0) {
        hs = compare_rgb(host_restored, reference.bytes, pixels);
        host_ok = true;
    }

    /* THE ASSERTION: the kernel's own planes, byte for byte against the
     * host-forward control's. Compared on the PLANES, not on the restored
     * RGBA — the round trip through the converter could mask a forward-
     * transform error that the planes would expose. */
    size_t plane_diff_count = 0;
    size_t first_plane_diff = 0;
    bool planes_identical = host_ok && host_yuv.size() == yuv.bytes.size();
    if (planes_identical) {
        for (size_t i = 0; i < host_yuv.size(); ++i) {
            if (host_yuv[i] != yuv.bytes[i]) {
                if (plane_diff_count == 0) first_plane_diff = i;
                ++plane_diff_count;
            }
        }
        planes_identical = (plane_diff_count == 0);
    }

    /* The superseded bound, still evaluated and still printed. */
    const bool passes_original_bound =
        s.mean[0] <= kMeanBound && s.mean[1] <= kMeanBound &&
        s.mean[2] <= kMeanBound &&
        s.fraction_within_16 >= kWithin16Bound && s.max_abs <= kMaxBound;

    const bool ok = host_ok && planes_identical;

    snprintf(detail, sizeof(detail),
             "%s %dx%d ASSERTION kernel_planes==host_forward_control=%d "
             "(diff_bytes=%zu first@%zu) | diagnostics: KERNEL "
             "mean=(%.4f,%.4f,%.4f) within16=%.5f max=%d | CONTROL "
             "mean=(%.4f,%.4f,%.4f) within16=%.5f max=%d ok=%d | ORIGINAL "
             "PRE-REGISTERED BOUND (mean<=%.1f within16>=%.3f max<=%d) -> %s | "
             "yuv_fnv1a64=%016" PRIx64,
             path, reference.w, reference.h, planes_identical ? 1 : 0,
             plane_diff_count, first_plane_diff, s.mean[0], s.mean[1],
             s.mean[2], s.fraction_within_16, s.max_abs, hs.mean[0], hs.mean[1],
             hs.mean[2], hs.fraction_within_16, hs.max_abs, host_ok ? 1 : 0,
             kMeanBound, kWithin16Bound, kMaxBound,
             passes_original_bound
                 ? "pass-as-written"
                 : "FAIL-AS-WRITTEN (superseded: a provably-ideal host control "
                   "fails this bound identically on this content, so the bound "
                   "measures 4:2:0's loss, not the kernel)",
             fnv1a64(yuv.bytes.data(), yuv.bytes.size()));
    verdict("Y10", "dng-corpus", host_arm(), ok, detail);
}

/* ====================================================================== */
/* Y11 — THE HOST-SOURCE STAGE-4 FALLBACK, yuv420 (T12.7)                 */
/*                                                                        */
/* WHY THIS CASE EXISTS, recorded so it is not deleted as redundant: the   */
/* DNG route has TWO Stage-4 entries. The pipeline tries the device-handoff */
/* runner first and falls back to the HOST-SOURCE runner when that is not  */
/* applicable or fails. Every other case here exercises only the first.    */
/* T12.7 added the yuv420 arm to BOTH, and a mutation planted in the       */
/* host-source arm left the whole suite green — proving that arm was       */
/* written but never executed. Unexercised fallback code first runs in     */
/* production, on a real photo, the day a device handoff fails; that is    */
/* the worst possible place to discover it.                                */
/*                                                                        */
/* The route is selected the way production selects it — by the pipeline's */
/* own env-loaded route config, re-read on every decode — not by a test    */
/* hook, so this drives the real fallback rather than a simulation of it.  */
/* Assertion is Y10's: three-plane byte equality with the host-forward     */
/* control. The env is restored on every exit path; a leaked flag would    */
/* silently re-route every later case in this process.                     */
/* ====================================================================== */
void run_y11() {
    const char *path = "image_samples/lossless_dng_sample.dng";
    char detail[768];

    /* RAII: the three route flags are cleared on EVERY exit path below,
     * including the early returns. */
    struct RouteOverride {
        RouteOverride() {
            setenv("DNG_FUSED_DEMOSAIC_WARP", "0", 1);
            setenv("DNG_STAGE3_STAGE4_DEVICE_HANDOFF", "0", 1);
            setenv("DNG_STAGE2_STAGE4_DEVICE_HANDOFF", "0", 1);
        }
        ~RouteOverride() {
            unsetenv("DNG_FUSED_DEMOSAIC_WARP");
            unsetenv("DNG_STAGE3_STAGE4_DEVICE_HANDOFF");
            unsetenv("DNG_STAGE2_STAGE4_DEVICE_HANDOFF");
        }
    } route_override;

    Rgba reference;
    Yuv yuv;
    int32_t e1 = 0, e2 = 0;
    if (!decode_rgba8_default(path, &reference, &e1)) {
        snprintf(detail, sizeof(detail),
                 "%s: rgba8 decode failed err=%d on the host-source route", path,
                 e1);
        verdict("Y11", "dng-hostsrc-fallback", host_arm(), false, detail);
        return;
    }
    if (!decode_yuv420(path, &yuv, &e2)) {
        snprintf(detail, sizeof(detail),
                 "%s: yuv420 decode failed err=%d on the host-source route",
                 path, e2);
        verdict("Y11", "dng-hostsrc-fallback", host_arm(), false, detail);
        return;
    }
    if (yuv.w != reference.w || yuv.h != reference.h) {
        snprintf(detail, sizeof(detail),
                 "%s: extent mismatch rgba8=%dx%d yuv420=%dx%d", path,
                 reference.w, reference.h, yuv.w, yuv.h);
        verdict("Y11", "dng-hostsrc-fallback", host_arm(), false, detail);
        return;
    }

    const size_t pixels = static_cast<size_t>(reference.w) * reference.h;
    std::vector<uint8_t> host_yuv;
    oracle_forward_yuv420(reference.bytes.data(), reference.w, reference.h,
                          &host_yuv);

    size_t plane_diff_count = 0;
    size_t first_plane_diff = 0;
    bool planes_identical = (host_yuv.size() == yuv.bytes.size());
    if (planes_identical) {
        for (size_t i = 0; i < host_yuv.size(); ++i) {
            if (host_yuv[i] != yuv.bytes[i]) {
                if (plane_diff_count == 0) first_plane_diff = i;
                ++plane_diff_count;
            }
        }
        planes_identical = (plane_diff_count == 0);
    }

    snprintf(detail, sizeof(detail),
             "%s %dx%d HOST-SOURCE Stage-4 route (device handoff + fusion "
             "disabled via the pipeline's own route config) ASSERTION "
             "kernel_planes==host_forward_control=%d (diff_bytes=%zu "
             "first@%zu) sizes=%zu/%zu yuv_fnv1a64=%016" PRIx64,
             path, reference.w, reference.h, planes_identical ? 1 : 0,
             plane_diff_count, first_plane_diff, yuv.bytes.size(),
             host_yuv.size(), fnv1a64(yuv.bytes.data(), yuv.bytes.size()));
    verdict("Y11", "dng-hostsrc-fallback", host_arm(), planes_identical,
            detail);
}

/* ====================================================================== */
/* Y7 — CROSS-ARM AGREEMENT (T12.6: arm A has landed, this is now REAL)   */
/*                                                                        */
/* Arm A is the fused Bayer demosaic+render kernel writing yuv420 planes;  */
/* arm B is raw_bayer_demosaic -> Stage-4 -> yuv420 planes. Both are       */
/* driven here on the SAME file, in the SAME process, on the SAME backend, */
/* so a disagreement can only come from the kernels.                       */
/*                                                                        */
/* THE BOUND IS PRE-REGISTERED, in native/tests/tmp/t126-00-y7-prereg.txt, */
/* written before the new kernel produced a single byte. ONE CLAUSE (B5',  */
/* the differing-byte fraction) was added AFTER a mutation defeated the    */
/* original max-only bound; that amendment, and the fact that it was       */
/* authored with the numbers already in hand, is disclosed in full in the  */
/* prereg file's AMENDMENT A1 section. It is a tightening, never a         */
/* widening. Byte equality is                                             */
/* deliberately NOT the assertion: arm A reads the demosaic inline while   */
/* arm B reads it back out of a materialised uint16 Stage-3 buffer, and    */
/* the fused kernel's rgba8 route already carries a recorded, closed       */
/* 1-LSB cross-kernel residue against the two-stage route. If a number     */
/* here exceeds the bound: STOP AND REPORT. Do not widen the bound and do  */
/* not add a guard — that is the evidenced-exception path and it is a user */
/* ruling, not a team judgement.                                           */
/*                                                                        */
/* B0 IS THE CASE'S OWN ANTI-FALSE-GREEN CHECK. Without it, a build where  */
/* fusion silently never engages runs the SAME two-stage decode twice and  */
/* reports a perfect zero diff — the most convincing possible green over   */
/* the exact defect this case exists to catch. The pipeline's fused        */
/* dispatch counter must move by exactly 1 on arm A and exactly 0 on arm   */
/* B.                                                                      */
/*                                                                        */
/* Arm B is reached through the pipeline's own env-loaded route flag,      */
/* re-read per decode, exactly as Y11 reaches the DNG host-source arm —    */
/* not through a test hook. The env is cleared on every exit path.         */
/*                                                                        */
/* Cross-BACKEND agreement (Metal vs Vulkan) remains a different claim,    */
/* delivered by the on-device artifact running this same binary.           */
/* ====================================================================== */
void run_y7() {
    const char *path = "image_samples/raw_sample.arw";
    char detail[1024];

    /* RAII: the route flag is cleared on EVERY exit path below. A leaked
     * flag would silently de-fuse every later case in this process. */
    struct FusionOff {
        FusionOff() { setenv("DNG_RAW_FUSED_BAYER_RENDER", "0", 1); }
        ~FusionOff() { unsetenv("DNG_RAW_FUSED_BAYER_RENDER"); }
    };

    Yuv fused;
    Yuv two_stage;
    int32_t e_a = 0, e_b = 0;

    const uint64_t c0 = raw_fused_bayer_render_count();
    const bool ok_a = decode_yuv420(path, &fused, &e_a);
    const uint64_t c1 = raw_fused_bayer_render_count();
    bool ok_b = false;
    uint64_t c2 = c1;
    {
        FusionOff off;
        ok_b = decode_yuv420(path, &two_stage, &e_b);
        c2 = raw_fused_bayer_render_count();
    }

    if (!ok_a || !ok_b) {
        snprintf(detail, sizeof(detail),
                 "%s: decode failed armA_ok=%d err=%d armB_ok=%d err=%d", path,
                 ok_a ? 1 : 0, e_a, ok_b ? 1 : 0, e_b);
        verdict("Y7", "bayer-arw", "A-fused-vs-B-two-stage", false, detail);
        return;
    }

    /* B0 — the arm-A decode really fused, and the arm-B decode really did
     * not. */
    const uint64_t fused_delta_a = c1 - c0;
    const uint64_t fused_delta_b = c2 - c1;
    const bool b0 = (fused_delta_a == 1u) && (fused_delta_b == 0u);

    /* B1 — sizes, extents and the full plane descriptor agree exactly. */
    const bool b1 =
        fused.w == two_stage.w && fused.h == two_stage.h &&
        fused.bytes.size() == two_stage.bytes.size() &&
        fused.planes.plane_width[0] == two_stage.planes.plane_width[0] &&
        fused.planes.plane_width[1] == two_stage.planes.plane_width[1] &&
        fused.planes.plane_width[2] == two_stage.planes.plane_width[2] &&
        fused.planes.plane_height[0] == two_stage.planes.plane_height[0] &&
        fused.planes.plane_height[1] == two_stage.planes.plane_height[1] &&
        fused.planes.plane_height[2] == two_stage.planes.plane_height[2] &&
        fused.planes.plane_row_stride[0] ==
            two_stage.planes.plane_row_stride[0] &&
        fused.planes.plane_row_stride[1] ==
            two_stage.planes.plane_row_stride[1] &&
        fused.planes.plane_row_stride[2] ==
            two_stage.planes.plane_row_stride[2];

    /* B2/B3/B4 — per-plane max |delta| <= 1. Plane spans are computed from
     * the LITERAL geometry the descriptor reports, so a descriptor defect
     * cannot fold two planes into one comparison. */
    int32_t max_abs[3] = {0, 0, 0};
    size_t diff_count[3] = {0, 0, 0};
    size_t plane_bytes[3] = {0, 0, 0};
    bool spans_ok = b1;
    if (spans_ok) {
        size_t offset = 0;
        for (int p = 0; p < 3; ++p) {
            plane_bytes[p] =
                static_cast<size_t>(fused.planes.plane_width[p]) *
                static_cast<size_t>(fused.planes.plane_height[p]);
            if (offset + plane_bytes[p] > fused.bytes.size()) {
                spans_ok = false;
                break;
            }
            for (size_t i = 0; i < plane_bytes[p]; ++i) {
                const int32_t d =
                    static_cast<int32_t>(fused.bytes[offset + i]) -
                    static_cast<int32_t>(two_stage.bytes[offset + i]);
                const int32_t a = d < 0 ? -d : d;
                if (a > max_abs[p]) max_abs[p] = a;
                if (a != 0) ++diff_count[p];
            }
            offset += plane_bytes[p];
        }
    }

    const bool b2 = spans_ok && max_abs[0] <= 1;
    const bool b3 = spans_ok && max_abs[1] <= 1;
    const bool b4 = spans_ok && max_abs[2] <= 1;
    /* B5' (prereg AMENDMENT A1, disclosed there in full): the differing-byte
     * FRACTION is a pass condition, not a report. It was a report until
     * mutation M2 was run: a deliberate +1 bias on ONE channel of ONE arm
     * moved 17.5-49.9% of every plane while keeping max|delta| at 1, so the
     * max-only bound was green over a planted cross-arm divergence. 1% is a
     * coarse tripwire sized ~3000x above the only measured honest residue
     * (32/11198070 on Vulkan luma) and ~17x below that mutant. Exceeding it is
     * STOP-AND-REPORT; it must not be raised to accommodate a result. */
    bool b5 = spans_ok;
    double diff_fraction[3] = {0.0, 0.0, 0.0};
    for (int p = 0; p < 3 && spans_ok; ++p) {
        if (plane_bytes[p] == 0) {
            b5 = false;
            break;
        }
        diff_fraction[p] = static_cast<double>(diff_count[p]) /
                           static_cast<double>(plane_bytes[p]);
        if (diff_fraction[p] > 0.01) b5 = false;
    }
    const bool ok = b0 && b1 && b2 && b3 && b4 && b5;

    snprintf(detail, sizeof(detail),
             "%s %dx%d B0 fused_delta=[A:%llu expect 1, B:%llu expect 0]=%d "
             "B1 sizes/descriptor=%d spans=%d "
             "max_abs=[Y:%d Cb:%d Cr:%d] bound=1 "
             "B5' diff_fraction=[Y:%.6f Cb:%.6f Cr:%.6f] bound=0.01 pass=%d "
             "diff_bytes=[Y:%zu/%zu Cb:%zu/%zu Cr:%zu/%zu] "
             "fusedA_fnv1a64=%016" PRIx64 " twostageB_fnv1a64=%016" PRIx64
             " prereg=native/tests/tmp/t126-00-y7-prereg.txt",
             path, fused.w, fused.h,
             static_cast<unsigned long long>(fused_delta_a),
             static_cast<unsigned long long>(fused_delta_b), b0 ? 1 : 0,
             b1 ? 1 : 0, spans_ok ? 1 : 0, max_abs[0], max_abs[1], max_abs[2],
             diff_fraction[0], diff_fraction[1], diff_fraction[2], b5 ? 1 : 0,
             diff_count[0], plane_bytes[0], diff_count[1], plane_bytes[1],
             diff_count[2], plane_bytes[2],
             fnv1a64(fused.bytes.data(), fused.bytes.size()),
             fnv1a64(two_stage.bytes.data(), two_stage.bytes.size()));
    verdict("Y7", "bayer-arw", "A-fused-vs-B-two-stage", ok, detail);
}

/* ====================================================================== */
/* Y8 — ROUTE COVERAGE                                                    */
/*                                                                        */
/* A partial format thread is indistinguishable from a working one on any  */
/* single-route test, so at least one NON-DEFAULT route must decode in     */
/* yuv420 and be validated by Y2's sizing and Y4b's fidelity bound. The    */
/* three Stage-4-class call sites are Bayer, X-Trans and linear-RGB        */
/* (raw_gpu_pipeline.cpp:712 / :978 / :1234); this case reaches the two    */
/* that are not the default decode entry's site.                          */
/* ====================================================================== */
void run_y8() {
    struct R {
        const char *path;
        const char *route;
        int32_t w, h;
        int64_t expect_bytes; /* literal */
    };
    const R rows[] = {
        {"image_samples/raw_corpus/fuji_xt3.raf", "xtrans-raf", 6246, 4170,
         39068730},
        {"image_samples/raw_corpus/sigma_sd_quattro_h_23.x3f", "linearrgb-x3f",
         6208, 4160, 38737920},
    };
    for (const R &r : rows) {
        Yuv yuv;
        int32_t e = 0;
        char detail[448];
        if (!decode_yuv420(r.path, &yuv, &e)) {
            snprintf(detail, sizeof(detail), "%s: yuv420 decode failed err=%d",
                     r.path, e);
            verdict("Y8", r.route, host_arm(), false, detail);
            continue;
        }
        const bool sized =
            yuv.w == r.w && yuv.h == r.h &&
            yuv.bytes.size() == static_cast<size_t>(r.expect_bytes);
        /* The descriptor must also be populated on a non-default route — a
         * thread that reached the kernel but not the descriptor would
         * otherwise look identical to a working one. */
        const bool described =
            yuv.planes.plane_base[0] == yuv.bytes.data() &&
            yuv.planes.plane_width[0] == r.w &&
            yuv.planes.plane_height[0] == r.h;
        snprintf(detail, sizeof(detail),
                 "%s %dx%d bytes=%zu (expect %" PRId64
                 ") sized=%d described=%d yuv_fnv1a64=%016" PRIx64,
                 r.path, yuv.w, yuv.h, yuv.bytes.size(), r.expect_bytes,
                 sized ? 1 : 0, described ? 1 : 0,
                 fnv1a64(yuv.bytes.data(), yuv.bytes.size()));
        verdict("Y8", r.route, host_arm(), sized && described, detail);
    }
}

/* ====================================================================== */
/* Y9 — LAYOUT: TIGHT PACKING IS ASSERTED, NEVER ASSUMED                  */
/*                                                                        */
/* Every expected value below is a LITERAL for the case's fixed            */
/* dimensions. Recomputing them with the same expression the code under    */
/* test used would be tautological and would pass on a broken              */
/* implementation (T12.0.1 obligation 1). Halide packs tightly by default, */
/* and a default is not a guarantee.                                       */
/*                                                                        */
/* Why this matters more than it looks: the frozen byte-count formula is   */
/* correct ONLY under tight packing. A padded row makes the formula        */
/* UNDER-count, and an under-count is a heap overrun, not a miscount.      */
/* ====================================================================== */
void run_y9() {
    /* raw_sample.arw is 6024 x 4024. Hand-computed:
     *   luma bytes   = 6024 * 4024                  = 24240576
     *   chroma w/h   = 3012 / 2012
     *   chroma bytes = 3012 * 2012                  = 6060144
     *   Cb offset    = 24240576
     *   Cr offset    = 24240576 + 6060144           = 30300720
     *   total        = 24240576 + 2 * 6060144       = 36360864
     */
    const int32_t kW = 6024, kH = 4024;
    const int32_t kCw = 3012, kCh = 2012;
    const size_t kCbOffset = 24240576u;
    const size_t kCrOffset = 30300720u;
    const size_t kTotal = 36360864u;

    Yuv yuv;
    int32_t e = 0;
    char detail[1024];
    if (!decode_yuv420("image_samples/raw_sample.arw", &yuv, &e)) {
        snprintf(detail, sizeof(detail), "yuv420 decode failed err=%d", e);
        verdict("Y9", "bayer-arw", host_arm(), false, detail);
        return;
    }

    const uint8_t *base = yuv.bytes.data();
    const bool extents_ok = (yuv.w == kW && yuv.h == kH);
    const bool total_ok = (yuv.bytes.size() == kTotal);
    const bool bases_ok = yuv.planes.plane_base[0] == base &&
                          yuv.planes.plane_base[1] == base + kCbOffset &&
                          yuv.planes.plane_base[2] == base + kCrOffset;
    const bool widths_ok = yuv.planes.plane_width[0] == kW &&
                           yuv.planes.plane_width[1] == kCw &&
                           yuv.planes.plane_width[2] == kCw;
    const bool heights_ok = yuv.planes.plane_height[0] == kH &&
                            yuv.planes.plane_height[1] == kCh &&
                            yuv.planes.plane_height[2] == kCh;
    /* Row stride EQUALS plane width — the contract, not an observation of
     * what Halide happened to produce. */
    const bool strides_ok = yuv.planes.plane_row_stride[0] == kW &&
                            yuv.planes.plane_row_stride[1] == kCw &&
                            yuv.planes.plane_row_stride[2] == kCw;
    const bool struct_size_ok =
        yuv.planes.struct_size == sizeof(CeyxYuv420PlaneDescriptor);

    snprintf(detail, sizeof(detail),
             "%dx%d total=%zu(expect %zu) cb_off=%zu(expect %zu) "
             "cr_off=%zu(expect %zu) w=[%d,%d,%d] h=[%d,%d,%d] "
             "stride=[%d,%d,%d] extents=%d bases=%d strides=%d ss=%d",
             yuv.w, yuv.h, yuv.bytes.size(), kTotal,
             static_cast<size_t>(yuv.planes.plane_base[1] - base), kCbOffset,
             static_cast<size_t>(yuv.planes.plane_base[2] - base), kCrOffset,
             yuv.planes.plane_width[0], yuv.planes.plane_width[1],
             yuv.planes.plane_width[2], yuv.planes.plane_height[0],
             yuv.planes.plane_height[1], yuv.planes.plane_height[2],
             yuv.planes.plane_row_stride[0], yuv.planes.plane_row_stride[1],
             yuv.planes.plane_row_stride[2], extents_ok ? 1 : 0,
             bases_ok ? 1 : 0, strides_ok ? 1 : 0, struct_size_ok ? 1 : 0);
    verdict("Y9", "bayer-arw", host_arm(),
            extents_ok && total_ok && bases_ok && widths_ok && heights_ok &&
                strides_ok && struct_size_ok,
            detail);
}

/* ====================================================================== */
/* Y12 — FUSED-RGB PROVENANCE DISCRIMINATOR                               */
/*                                                                        */
/* PRE-REGISTERED IN FULL, BEFORE THIS CODE EXISTED:                      */
/*   native/tests/tmp/t126/t128-00-y12-prereg.txt                          */
/* Read that file before reading this one. It contains the decision table  */
/* (D1..D5), the anti-false-green obligations (G1..G3) and the explicit    */
/* statement of what Y12 does NOT claim. Nothing below may be reinterpreted */
/* against it after the fact.                                              */
/*                                                                        */
/* THE QUESTION THIS CASE ANSWERS, AND WHY NOTHING ELSE COULD              */
/* ---------------------------------------------------------------------- */
/* The fused yuv420 arm is wrong on Vulkan (Y7: max_abs 81/137/129 on      */
/* 12-14% of every plane) and byte-perfect on Metal. Two explanations      */
/* survive and the existing cases cannot tell them apart:                  */
/*   (i)  the fused demosaic+colour EXPRESSION TREE is miscompiled on this */
/*        driver, in which case the SHIPPING rgba8 fused kernel is wrong   */
/*        too and the problem is much wider than T12.6; or                 */
/*   (ii) the tree is fine evaluated once, and what breaks is the fused    */
/*        yuv420 KERNEL SHAPE -- that same deep tree re-evaluated at five  */
/*        coordinates per output pixel and inlined into three plane        */
/*        kernels.                                                          */
/*                                                                        */
/* Y1 cannot decide it: it compares Vulkan against METAL goldens, and that */
/* yardstick is refuted (t126-baton.md S4 H2 — Y1 mismatches on xtrans and */
/* x3f as well, and neither of those routes fuses, so the mismatch is      */
/* general cross-backend noise, not evidence about fusion).                */
/*                                                                        */
/* Y4b cannot decide it either, and this is the subtle one: on bayer-arw   */
/* Y4b's "host-forward control" is built from decode_rgba8_default (:976), */
/* which on an unscaled Bayer decode IS the fused rgba8 kernel. So on that */
/* route the control's provenance is the very kernel family under          */
/* suspicion. A Y4b red there is real but it is UNATTRIBUTABLE.            */
/*                                                                        */
/* Y12 breaks the circle by decoding the same file FOUR ways in one        */
/* process, driving the pipeline's own route flag (raw_gpu_pipeline.cpp    */
/* :540-543), which since bda80348 gates fusion on SCALE alone and so      */
/* applies to BOTH output formats:                                         */
/*      A_rgba8 = rgba8 , fusion ON      B_rgba8 = rgba8 , fusion OFF      */
/*      A_yuv   = yuv420, fusion ON      B_yuv   = yuv420, fusion OFF      */
/* and reading M_rgb (A_rgba8 vs B_rgba8) against M_B (two-stage yuv420 vs */
/* its own host control). M_rgb asks the tree question with the yuv420     */
/* kernel shape removed; M_B asks the plane-write question with the fused  */
/* tree removed. Together they localise the defect to one of the two.      */
/*                                                                        */
/* WHY THE PASS CONDITION EXCLUDES M_A, deliberately: Y12 is a LOCALISER,  */
/* not a second copy of Y7. It must stay GREEN while the defect it         */
/* localises is still present, otherwise it merely duplicates Y7's red and */
/* stops being readable as an independent signal. M_A is REPORTED.         */
/*                                                                        */
/* THE BOUND ON M_rgb IS 1, NOT 0, and that is not a relaxation: the       */
/* fused-vs-two-stage rgba8 1-LSB cross-kernel residue is a pre-existing   */
/* recorded and closed finding (see Y7's header above, and the same        */
/* statement in the prereg). A tree miscompile of the kind (i) predicts    */
/* would land in the 81-137 range already measured, roughly two orders of  */
/* magnitude clear of this bound, so the two outcomes cannot be confused.  */
/* ====================================================================== */
void run_y12() {
    const char *path = "image_samples/raw_sample.arw";
    char detail[1600];

    /* RAII, same discipline as Y7/Y11: the flag is cleared on EVERY exit
     * path. A leaked flag would silently de-fuse every later case in this
     * process, which would look like a fix. */
    struct FusionOff {
        FusionOff() { setenv("DNG_RAW_FUSED_BAYER_RENDER", "0", 1); }
        ~FusionOff() { unsetenv("DNG_RAW_FUSED_BAYER_RENDER"); }
    };

    Rgba a_rgba, b_rgba;
    Yuv a_yuv, b_yuv;
    int32_t e1 = 0, e2 = 0, e3 = 0, e4 = 0;
    bool ok1, ok2, ok3, ok4;

    /* G1 — each leg's fused-dispatch delta is captured around that leg only,
     * so "fusion never engaged anywhere" cannot masquerade as agreement. */
    const uint64_t c0 = raw_fused_bayer_render_count();
    ok1 = decode_rgba8_default(path, &a_rgba, &e1);
    const uint64_t c1 = raw_fused_bayer_render_count();
    ok3 = decode_yuv420(path, &a_yuv, &e3);
    const uint64_t c2 = raw_fused_bayer_render_count();
    uint64_t c3 = c2, c4 = c2;
    {
        FusionOff off;
        ok2 = decode_rgba8_default(path, &b_rgba, &e2);
        c3 = raw_fused_bayer_render_count();
        ok4 = decode_yuv420(path, &b_yuv, &e4);
        c4 = raw_fused_bayer_render_count();
    }

    if (!ok1 || !ok2 || !ok3 || !ok4) {
        snprintf(detail, sizeof(detail),
                 "%s: decode failed A_rgba8=%d(err=%d) B_rgba8=%d(err=%d) "
                 "A_yuv=%d(err=%d) B_yuv=%d(err=%d)",
                 path, ok1 ? 1 : 0, e1, ok2 ? 1 : 0, e2, ok3 ? 1 : 0, e3,
                 ok4 ? 1 : 0, e4);
        verdict("Y12", "bayer-arw", "fused-rgb-provenance", false, detail);
        return;
    }

    const bool g1 = (c1 - c0 == 1u) && (c2 - c1 == 1u) && (c3 - c2 == 0u) &&
                    (c4 - c3 == 0u);

    /* G2 — extents agree across all four legs, and the two yuv420 legs agree
     * with each other in size, so every comparison below spans real data. */
    const bool g2 = a_rgba.w == b_rgba.w && a_rgba.h == b_rgba.h &&
                    a_yuv.w == a_rgba.w && a_yuv.h == a_rgba.h &&
                    b_yuv.w == a_rgba.w && b_yuv.h == a_rgba.h &&
                    a_rgba.bytes.size() == b_rgba.bytes.size() &&
                    a_yuv.bytes.size() == b_yuv.bytes.size();

    /* ---- M_rgb: the fused rgba8 kernel against the two-stage rgba8 kernel,
     * per COLOUR CHANNEL. Alpha is skipped: it is a kernel-written constant
     * and including it would dilute the diff fraction by a quarter. */
    int32_t rgb_max[3] = {0, 0, 0};
    size_t rgb_diff[3] = {0, 0, 0};
    size_t pixels = 0;
    if (g2) {
        pixels = static_cast<size_t>(a_rgba.w) * a_rgba.h;
        for (size_t i = 0; i < pixels; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                const int32_t d =
                    static_cast<int32_t>(a_rgba.bytes[i * 4 + ch]) -
                    static_cast<int32_t>(b_rgba.bytes[i * 4 + ch]);
                const int32_t ad = d < 0 ? -d : d;
                if (ad > rgb_max[ch]) rgb_max[ch] = ad;
                if (ad != 0) ++rgb_diff[ch];
            }
        }
    }

    /* ---- the three plane comparisons. G3: spans come from the LITERAL
     * descriptor geometry of the arm being measured, never from an assumed
     * layout, so a descriptor defect cannot fold two planes into one
     * comparison. */
    struct PlaneCmp {
        size_t diff_bytes[3] = {0, 0, 0};
        int32_t max_abs[3] = {0, 0, 0};
        bool spans_ok = false;
    };
    auto compare_planes = [](const std::vector<uint8_t> &kernel,
                             const std::vector<uint8_t> &control,
                             const CeyxYuv420PlaneDescriptor &d) {
        PlaneCmp r;
        if (kernel.size() != control.size()) return r;
        size_t offset = 0;
        for (int p = 0; p < 3; ++p) {
            const size_t n = static_cast<size_t>(d.plane_width[p]) *
                             static_cast<size_t>(d.plane_height[p]);
            if (offset + n > kernel.size()) return r;
            for (size_t i = 0; i < n; ++i) {
                const int32_t delta =
                    static_cast<int32_t>(kernel[offset + i]) -
                    static_cast<int32_t>(control[offset + i]);
                const int32_t ad = delta < 0 ? -delta : delta;
                if (ad > r.max_abs[p]) r.max_abs[p] = ad;
                if (ad != 0) ++r.diff_bytes[p];
            }
            offset += n;
        }
        r.spans_ok = true;
        return r;
    };

    std::vector<uint8_t> host_from_a, host_from_b;
    PlaneCmp m_a, m_b, m_x;
    if (g2) {
        oracle_forward_yuv420(a_rgba.bytes.data(), a_rgba.w, a_rgba.h,
                              &host_from_a);
        oracle_forward_yuv420(b_rgba.bytes.data(), b_rgba.w, b_rgba.h,
                              &host_from_b);
        m_a = compare_planes(a_yuv.bytes, host_from_a, a_yuv.planes);
        m_b = compare_planes(b_yuv.bytes, host_from_b, b_yuv.planes);
        m_x = compare_planes(a_yuv.bytes, host_from_b, a_yuv.planes);
    }

    const bool rgb_within_1 =
        g2 && rgb_max[0] <= 1 && rgb_max[1] <= 1 && rgb_max[2] <= 1;
    const bool b_exact = m_b.spans_ok && m_b.diff_bytes[0] == 0 &&
                         m_b.diff_bytes[1] == 0 && m_b.diff_bytes[2] == 0;

    /* The pre-registered verdict, verbatim from the prereg's VERDICT POLICY:
     * G1 && G2 && G3 && (M_rgb max <= 1) && (M_B diff_bytes == 0). M_A is
     * reported and is NOT a pass condition — see this case's header. */
    const bool ok = g1 && g2 && m_a.spans_ok && m_b.spans_ok && m_x.spans_ok &&
                    rgb_within_1 && b_exact;

    snprintf(
        detail, sizeof(detail),
        "%s %dx%d | G1 counter_delta=[A_rgba8:%llu A_yuv:%llu B_rgba8:%llu "
        "B_yuv:%llu expect 1,1,0,0]=%d G2 extents=%d | M_rgb "
        "fused_vs_twostage_rgba8 max_abs=[R:%d G:%d B:%d] bound=1 "
        "diff_fraction=[R:%f G:%f B:%f] | M_B twostage_yuv_vs_hostctl "
        "diff_bytes=[%zu,%zu,%zu] max_abs=[%d,%d,%d] | M_A "
        "fused_yuv_vs_hostctl(fused_rgba8) diff_bytes=[%zu,%zu,%zu] "
        "max_abs=[%d,%d,%d] REPORTED-NOT-ASSERTED | M_X "
        "fused_yuv_vs_hostctl(twostage_rgba8) diff_bytes=[%zu,%zu,%zu] "
        "max_abs=[%d,%d,%d] | fnv A_rgba8=%016" PRIx64 " B_rgba8=%016" PRIx64
        " A_yuv=%016" PRIx64 " B_yuv=%016" PRIx64
        " | prereg=native/tests/tmp/t126/t128-00-y12-prereg.txt",
        path, a_rgba.w, a_rgba.h, (unsigned long long)(c1 - c0),
        (unsigned long long)(c2 - c1), (unsigned long long)(c3 - c2),
        (unsigned long long)(c4 - c3), g1 ? 1 : 0, g2 ? 1 : 0, rgb_max[0],
        rgb_max[1], rgb_max[2],
        pixels ? (double)rgb_diff[0] / (double)pixels : 0.0,
        pixels ? (double)rgb_diff[1] / (double)pixels : 0.0,
        pixels ? (double)rgb_diff[2] / (double)pixels : 0.0,
        m_b.diff_bytes[0], m_b.diff_bytes[1], m_b.diff_bytes[2],
        m_b.max_abs[0], m_b.max_abs[1], m_b.max_abs[2], m_a.diff_bytes[0],
        m_a.diff_bytes[1], m_a.diff_bytes[2], m_a.max_abs[0], m_a.max_abs[1],
        m_a.max_abs[2], m_x.diff_bytes[0], m_x.diff_bytes[1],
        m_x.diff_bytes[2], m_x.max_abs[0], m_x.max_abs[1], m_x.max_abs[2],
        fnv1a64(a_rgba.bytes.data(), a_rgba.bytes.size()),
        fnv1a64(b_rgba.bytes.data(), b_rgba.bytes.size()),
        fnv1a64(a_yuv.bytes.data(), a_yuv.bytes.size()),
        fnv1a64(b_yuv.bytes.data(), b_yuv.bytes.size()));
    verdict("Y12", "bayer-arw", "fused-rgb-provenance", ok, detail);
}

/* ====================================================================== */
/* The piggybacked fused-Bayer-route hash (arm A's RGBA8 route).          */
/*                                                                        */
/* Not a Y case. It exists so the SAME binary that runs on-device also     */
/* publishes the number the Metal run publishes, closing the T5-B2-v debt  */
/* inside T12's own sign-off (t12-baton-1 §6). The fused arm is selected   */
/* automatically for an UNSCALED rgba8 Bayer decode                        */
/* (raw_gpu_pipeline.cpp:532), so this is simply the default rgba8 decode  */
/* of raw_sample.arw with its hash published under its own label.          */
/*                                                                        */
/* IF THE ON-DEVICE HASH DISAGREES WITH THE METAL REFERENCE: STOP AND      */
/* REPORT. Do not add a guard. That is the evidenced-exception path and it */
/* is a user ruling, not a team judgement.                                 */
/* ====================================================================== */
void run_fused_hash() {
    const char *files[] = {"image_samples/raw_sample.arw",
                           "image_samples/raw_corpus/fuji_xt3.raf",
                           "image_samples/raw_corpus/sigma_sd_quattro_h_23.x3f"};
    for (const char *path : files) {
        Rgba img;
        int32_t e = 0;
        if (!decode_rgba8_default(path, &img, &e)) {
            printf("[FUSEDHASH] route=unscaled-rgba8 arm=%s file=%s -> DECODE "
                   "FAILED err=%d\n",
                   host_arm(), path, e);
            ++g_fail;
            continue;
        }
        printf("[FUSEDHASH] route=unscaled-rgba8 arm=%s file=%s %dx%d "
               "fnv1a64=%016" PRIx64 "\n",
               host_arm(), path, img.w, img.h,
               fnv1a64(img.bytes.data(), img.bytes.size()));
    }
}

}  // namespace

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("=== T12 milestone 4 — yuv420 output correctness (Y1..Y9) ===\n");
    printf("ARM/BACKEND for every case below: %s\n", host_arm());
    printf("Pre-registered bounds and mutation list: "
           "native/tests/tmp/t12c-00-prereg.txt\n");
    printf("Run from the REPO ROOT — sample paths are relative.\n\n");

    /* Route labels match the three Stage-4-class call sites in
     * raw_gpu_pipeline.cpp (:712 Bayer, :978 X-Trans, :1234 linear-RGB). */
    const std::vector<std::pair<std::string, std::string>> corpus = {
        {"image_samples/raw_sample.arw", "bayer-arw"},
        {"image_samples/raw_corpus/fuji_xt3.raf", "xtrans-raf"},
        {"image_samples/raw_corpus/sigma_sd_quattro_h_23.x3f",
         "linearrgb-x3f"},
    };

    run_y1(corpus);
    run_y2();
    run_y3();
    run_y4a();
    run_y4b(corpus);
    run_y5("image_samples/raw_sample.arw", "bayer-arw");
    run_y6();
    run_y10();
    run_y11();
    run_y7();
    run_y8();
    run_y9();
    /* Y12 runs LAST among the assertions: it drives the fusion route flag,
     * and placing it after every other case means even a leaked flag (which
     * its own RAII forbids) could not re-route an earlier case. */
    run_y12();
    run_fused_hash();

    printf("\n=== SUMMARY pass=%d fail=%d n/a=%d arm=%s ===\n", g_pass, g_fail,
           g_na, host_arm());
    printf("VERDICT=%s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
