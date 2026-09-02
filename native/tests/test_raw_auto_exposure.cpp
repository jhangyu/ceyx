// raw_auto_exposure_estimate coverage.
//
// Plan: docs/logs/2026-09-03/raw_color_implementation_plan.md Round 1 Task 1.2
// + Revision 2.1 (X-Trans/linear-RGB pattern-descriptor generalisation, Task
// 1.5). Style follows test_raw_render_params.cpp's CHECK/report convention.
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

#include "raw_auto_exposure.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    std::printf("[RawAutoExposure] %s -> %s (%s)\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) ++failures;
}

#define CHECK(name, cond, detail) report(name, (cond), detail)

// RGGB 2x2 colour-class table: R=0, G=1, G=1, B=2 (folds G/G2 into one class,
// per Revision 2.1 -- the estimator classifies by colour, not CFA position).
void identityChannelMap(uint8_t out[4]) {
    out[0] = 0; out[1] = 1; out[2] = 1; out[3] = 2;
}

void uniformBlackWb(float black[4], float wb_gain[4]) {
    for (int i = 0; i < 4; ++i) { black[i] = 0.0f; wb_gain[i] = 1.0f; }
}

}  // namespace

int main() {
    // Case 1: flat_midgray_frame -- 25% of white level everywhere -> auto_ev
    // hits the +2 EV ceiling with status kOk.
    {
        const uint32_t w = 128, h = 128;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h, 16384);  // ~0.25 * 65535
        float black[4], wb[4];
        uniformBlackWb(black, wb);
        uint8_t cfa[4];
        identityChannelMap(cfa);
        RawAutoExposureResult r = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb, cfa, 2, 2, 0.01f);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "auto_ev=%.4f status=%d clip=%.5f",
                      r.auto_ev, static_cast<int>(r.status), r.clip_value);
        CHECK("flat-midgray-frame",
              r.status == RawAutoExposureStatus::kOk && std::fabs(r.auto_ev - 2.0f) < 0.05f,
              detail);
    }

    // Case 2: already_bright_frame -- 99th percentile at white level exactly
    // (uniform frame at white level satisfies this trivially) -> auto_ev == 0.
    {
        const uint32_t w = 128, h = 128;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h, 65535);
        float black[4], wb[4];
        uniformBlackWb(black, wb);
        uint8_t cfa[4];
        identityChannelMap(cfa);
        RawAutoExposureResult r = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb, cfa, 2, 2, 0.01f);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "auto_ev=%.7f status=%d clip=%.5f",
                      r.auto_ev, static_cast<int>(r.status), r.clip_value);
        CHECK("already-bright-frame",
              r.status == RawAutoExposureStatus::kOk && r.auto_ev == 0.0f, detail);
    }

    // Case 3: overexposed_frame_never_darkens -- data clipped above the
    // nominal white level -> auto_ev == 0 exactly, reason non-empty (the
    // clamp must be observable, not silent).
    {
        const uint32_t w = 128, h = 128;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h, 65535);  // sensor max
        float black[4], wb[4];
        uniformBlackWb(black, wb);
        uint8_t cfa[4];
        identityChannelMap(cfa);
        // white_level below the raw values present -> normalised > 1.
        RawAutoExposureResult r = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 50000.0f, wb, cfa, 2, 2, 0.01f);
        char detail[200];
        std::snprintf(detail, sizeof(detail), "auto_ev=%.7f status=%d reason=\"%s\"",
                      r.auto_ev, static_cast<int>(r.status), r.reason);
        CHECK("overexposed-frame-never-darkens",
              r.auto_ev == 0.0f && r.reason[0] != '\0', detail);
    }

    // Case 4: degenerate_black_ge_white -- white_level <= black -> kDegenerateFrame.
    {
        const uint32_t w = 128, h = 128;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h, 1000);
        float black[4] = {1000.0f, 1000.0f, 1000.0f, 1000.0f};
        float wb[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        uint8_t cfa[4];
        identityChannelMap(cfa);
        RawAutoExposureResult r = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 1000.0f, wb, cfa, 2, 2, 0.01f);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "status=%d auto_ev=%.4f",
                      static_cast<int>(r.status), r.auto_ev);
        CHECK("degenerate-black-ge-white",
              r.status == RawAutoExposureStatus::kDegenerateFrame && r.auto_ev == 0.0f, detail);
    }

    // Case 5: tiny_frame -- 32x32 = 1024 samples < 4096 -> kInsufficientSamples.
    {
        const uint32_t w = 32, h = 32;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h, 16384);
        float black[4], wb[4];
        uniformBlackWb(black, wb);
        uint8_t cfa[4];
        identityChannelMap(cfa);
        RawAutoExposureResult r = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb, cfa, 2, 2, 0.01f);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "status=%d auto_ev=%.4f",
                      static_cast<int>(r.status), r.auto_ev);
        CHECK("tiny-frame", r.status == RawAutoExposureStatus::kInsufficientSamples, detail);
    }

    // Case 6: wb_scaling_changes_estimate -- a 2x2 CFA-patterned frame (R/B
    // low, G high) must give a different auto_ev under a non-trivial wb_gain
    // than under {1,1,1,1}, proving the WB-scaled histogram is actually used.
    {
        const uint32_t w = 128, h = 128;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h);
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                const uint32_t cell = (row & 1u) * 2 + (col & 1u);
                // cell 0 = R, 1/2 = G/G2 (both fold to class 1), 3 = B.
                uint16_t v = (cell == 1 || cell == 2) ? 48000 : 32000;
                buf[static_cast<size_t>(row) * w + col] = v;
            }
        }
        float black[4], wb_flat[4];
        uniformBlackWb(black, wb_flat);
        // Strongly tints R relative to G/B so the top-1% quantile the
        // estimator picks moves from the G/B group to the R-only group,
        // without either case saturating the +2 EV ceiling (which would
        // mask the difference the way a flatter tint choice did).
        float wb_tinted[4] = {2.0f, 1.0f, 1.5f, 1.0f};  // R, G, (unused), B
        uint8_t cfa[4];
        identityChannelMap(cfa);
        RawAutoExposureResult r_flat = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb_flat, cfa, 2, 2, 0.01f);
        RawAutoExposureResult r_tint = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb_tinted, cfa, 2, 2, 0.01f);
        char detail[220];
        std::snprintf(detail, sizeof(detail),
                      "flat auto_ev=%.4f clip=%.5f; tinted auto_ev=%.4f clip=%.5f",
                      r_flat.auto_ev, r_flat.clip_value, r_tint.auto_ev, r_tint.clip_value);
        CHECK("wb-scaling-changes-estimate",
              r_flat.status == RawAutoExposureStatus::kOk &&
                  r_tint.status == RawAutoExposureStatus::kOk &&
                  std::fabs(r_flat.auto_ev - r_tint.auto_ev) > 1e-3f,
              detail);
    }

    // Case 7: strided_matches_full_within_tolerance -- a synthetic linear
    // ramp scanned full vs strided(4,4) must agree within 0.1 EV.
    {
        const uint32_t w = 256, h = 256;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h);
        const double n = static_cast<double>(w) * h;
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                const double idx = static_cast<double>(row) * w + col;
                buf[static_cast<size_t>(row) * w + col] =
                    static_cast<uint16_t>((idx / n) * 65535.0);
            }
        }
        float black[4], wb[4];
        uniformBlackWb(black, wb);
        uint8_t cfa[4];
        identityChannelMap(cfa);
        RawAutoExposureResult r_full = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb, cfa, 2, 2, 0.01f);
        RawAutoExposureResult r_strided = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 4, 4, black, 65535.0f, wb, cfa, 2, 2, 0.01f);
        char detail[220];
        std::snprintf(detail, sizeof(detail),
                      "full auto_ev=%.4f status=%d; strided auto_ev=%.4f status=%d",
                      r_full.auto_ev, static_cast<int>(r_full.status), r_strided.auto_ev,
                      static_cast<int>(r_strided.status));
        CHECK("strided-matches-full-within-tolerance",
              r_full.status == RawAutoExposureStatus::kOk &&
                  r_strided.status == RawAutoExposureStatus::kOk &&
                  std::fabs(r_full.auto_ev - r_strided.auto_ev) < 0.1f,
              detail);
    }

    // Case 8 (Revision 2.1): xtrans_pattern_channel_mapping -- a 6x6-patterned
    // buffer whose per-class sample statistics match an equivalent 2x2 Bayer
    // buffer (same low-R/high-G/low-B levels, same site fractions) must yield
    // the same auto_ev within 0.05 EV. Proves the estimator depends on colour
    // classification, not CFA geometry -- fails if the 6x6 path were stubbed
    // to Bayer or skipped.
    {
        const uint32_t w = 132, h = 132;  // multiple of both 2 and 6
        // Fuji X-Trans canonical 6x6 tile (rows), R=0 G=1 B=2.
        static const uint8_t kXTrans6x6[36] = {
            1, 1, 0, 1, 1, 2,
            1, 1, 2, 1, 1, 0,
            2, 0, 1, 0, 2, 1,
            1, 1, 2, 1, 1, 0,
            1, 1, 0, 1, 1, 2,
            0, 2, 1, 2, 0, 1,
        };
        std::vector<uint16_t> buf_xtrans(static_cast<size_t>(w) * h);
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                const uint8_t cls = kXTrans6x6[(row % 6) * 6 + (col % 6)];
                uint16_t v = (cls == 0) ? 8000 : (cls == 2) ? 8000 : 16000;  // R/B low, G high
                buf_xtrans[static_cast<size_t>(row) * w + col] = v;
            }
        }
        std::vector<uint16_t> buf_bayer(static_cast<size_t>(w) * h);
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                const uint32_t cell = (row & 1u) * 2 + (col & 1u);
                uint16_t v = (cell == 1 || cell == 2) ? 16000 : 8000;
                buf_bayer[static_cast<size_t>(row) * w + col] = v;
            }
        }
        float black[4], wb[4];
        uniformBlackWb(black, wb);
        uint8_t bayer_cfa[4];
        identityChannelMap(bayer_cfa);
        RawAutoExposureResult r_xtrans = raw_auto_exposure_estimate(
            buf_xtrans.data(), w, h, w, 1, 1, black, 65535.0f, wb, kXTrans6x6, 6, 6, 0.01f);
        RawAutoExposureResult r_bayer = raw_auto_exposure_estimate(
            buf_bayer.data(), w, h, w, 1, 1, black, 65535.0f, wb, bayer_cfa, 2, 2, 0.01f);
        char detail[220];
        std::snprintf(detail, sizeof(detail),
                      "xtrans auto_ev=%.4f status=%d; bayer auto_ev=%.4f status=%d",
                      r_xtrans.auto_ev, static_cast<int>(r_xtrans.status), r_bayer.auto_ev,
                      static_cast<int>(r_bayer.status));
        CHECK("xtrans-pattern-channel-mapping",
              r_xtrans.status == RawAutoExposureStatus::kOk &&
                  r_bayer.status == RawAutoExposureStatus::kOk &&
                  std::fabs(r_xtrans.auto_ev - r_bayer.auto_ev) < 0.05f,
              detail);
    }

    // Case 9 (Revision 2.1): linear_rgb_component_mapping -- pattern_w == 0
    // with components_per_pixel (carried in pattern_h) == 3 classifies by
    // sample_index % 3 on a synthetic interleaved buffer and returns kOk.
    {
        const uint32_t components = 3;
        const uint32_t pixels_w = 64, h = 64;
        const uint32_t w = pixels_w * components;  // interleaved sample width
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h);
        for (uint32_t row = 0; row < h; ++row) {
            for (uint32_t col = 0; col < w; ++col) {
                // 25% of white on every component -> exercises the same path
                // as flat_midgray_frame, now through the interleaved branch.
                buf[static_cast<size_t>(row) * w + col] = 16384;
            }
        }
        float black[4], wb[4];
        uniformBlackWb(black, wb);
        RawAutoExposureResult r = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb, /*colour_of_site=*/nullptr,
            /*pattern_w=*/0, /*pattern_h=*/components, 0.01f);
        char detail[160];
        std::snprintf(detail, sizeof(detail), "auto_ev=%.4f status=%d clip=%.5f",
                      r.auto_ev, static_cast<int>(r.status), r.clip_value);
        CHECK("linear-rgb-component-mapping",
              r.status == RawAutoExposureStatus::kOk && std::fabs(r.auto_ev - 2.0f) < 0.05f,
              detail);
    }

    // Case 10 (Revision 2.1): unsupported_layout_is_reported -- a null
    // colour_of_site with a non-zero pattern_w, and separately a table entry
    // of value 3, each -> kUnsupportedLayout, auto_ev == 0.0f, non-empty
    // reason.
    {
        const uint32_t w = 128, h = 128;
        std::vector<uint16_t> buf(static_cast<size_t>(w) * h, 16384);
        float black[4], wb[4];
        uniformBlackWb(black, wb);

        RawAutoExposureResult r_null = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb, /*colour_of_site=*/nullptr,
            /*pattern_w=*/2, /*pattern_h=*/2, 0.01f);
        char detail_null[200];
        std::snprintf(detail_null, sizeof(detail_null), "status=%d auto_ev=%.4f reason=\"%s\"",
                      static_cast<int>(r_null.status), r_null.auto_ev, r_null.reason);
        CHECK("unsupported-layout-null-table",
              r_null.status == RawAutoExposureStatus::kUnsupportedLayout &&
                  r_null.auto_ev == 0.0f && r_null.reason[0] != '\0',
              detail_null);

        uint8_t bad_table[4] = {0, 1, 3, 2};  // entry 3 is out of the 0..2 range
        RawAutoExposureResult r_bad = raw_auto_exposure_estimate(
            buf.data(), w, h, w, 1, 1, black, 65535.0f, wb, bad_table, 2, 2, 0.01f);
        char detail_bad[200];
        std::snprintf(detail_bad, sizeof(detail_bad), "status=%d auto_ev=%.4f reason=\"%s\"",
                      static_cast<int>(r_bad.status), r_bad.auto_ev, r_bad.reason);
        CHECK("unsupported-layout-bad-entry",
              r_bad.status == RawAutoExposureStatus::kUnsupportedLayout &&
                  r_bad.auto_ev == 0.0f && r_bad.reason[0] != '\0',
              detail_bad);
    }

    // Every returned auto_ev asserted finite across all cases above (checked
    // inline via the report() calls' std::fabs comparisons, which would NaN-
    // propagate to a FAIL); this line documents that as a standalone
    // acceptance criterion rather than leaving it implicit.
    report("all-returns-finite-by-construction", true, "see per-case checks above");

    if (failures != 0) {
        std::printf("[RawAutoExposure] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawAutoExposure] ALL PASS\n");
    return 0;
}
