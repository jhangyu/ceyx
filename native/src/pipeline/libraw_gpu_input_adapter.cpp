#include "libraw_gpu_input_adapter.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "raw_auto_exposure.h"
#include "raw_contract_validate.h"

namespace {

// Standard LibRaw packed-Bayer accessor: two bits per site in a 2x8 tile.
uint32_t bayerKeyIndex(uint32_t filters, int row, int col) {
    return (filters >> ((((row << 1) & 14) + (col & 1)) << 1)) & 3;
}

uint32_t lcmU32(uint32_t a, uint32_t b) {
    if (a == 0 || b == 0) return 0;
    uint32_t x = a, y = b;
    while (y != 0) { const uint32_t t = x % y; x = y; y = t; }
    return (a / x) * b;
}

}  // namespace

// Linear sRGB -> XYZ(D50 PCS). Transcribed from the DNG SDK's own sRGB colour
// space (third_party/dng_sdk/source/dng_color_space.cpp:262-264) rather than
// from a published sRGB/Bradford table: the two differ in the 4th decimal, and
// the whole point of the route table below is that both LibRaw and DNG images
// land in the SAME PCS the Stage-4 kernel expects.
//
// It is a literal here ONLY because this translation unit must not depend on
// dng_sdk: libraw_gpu_input_adapter.cpp is also compiled into test_libraw_adapter
// (cmake/tests.cmake:746-752), which links libraw_vendored alone. The
// duplication is therefore gated, not trusted: test_raw_render_params asserts
// this array equals dng_space_sRGB::Get().MatrixToPCS() to float precision, so
// an SDK update that moved the numbers would fail a test rather than silently
// skew every LibRaw render.
// The PCS white: XYtoXYZ(D50_xy_coord()), i.e. exactly what the SDK's
// PCStoXYZ() returns (dng_xy_coord.cpp:82 over the (0.3457, 0.3585) constant at
// dng_xy_coord.h:147-150).
//
// NOT the design document's (0.9642, 1.0, 0.8249): that is a 4-decimal rounding
// of D50 and sits 2.0e-4 away from this value in Z -- twice AC-1.2's own 1e-4
// tolerance, so asserting the invariant against the rounded literal would fail a
// correct implementation. Flagged to the lead 2026-08-28.
extern "C" void raw_pcs_white(float out3[3]) {
    const double x = 0.3457, y = 0.3585;
    out3[0] = static_cast<float>(x / y);
    out3[1] = 1.0f;
    out3[2] = static_cast<float>((1.0 - x - y) / y);
}

extern "C" void raw_srgb_to_pcs_matrix(float out9[9]) {
    // The SDK's own published primaries, then the SDK's own correction for
    // them. dng_color_space::SetMatrixToPCS (dng_color_space.cpp:210-233) does
    // not store the literal: "the matrix values are often rounded, so adjust to
    // get them to convert device white exactly to the PCS" -- it row-scales by
    // PCStoXYZ()[r] / rowsum[r]. Reproducing that scaling here is what makes the
    // rows sum to the PCS white EXACTLY, which is the whole basis of the AC-1.2
    // invariant; using the bare literal leaves a 4e-6 skew.
    static const double kSrgbToPcs[9] = {
        0.4361, 0.3851, 0.1431,
        0.2225, 0.7169, 0.0606,
        0.0139, 0.0971, 0.7141};
    float white[3];
    raw_pcs_white(white);
    for (int r = 0; r < 3; ++r) {
        const double sum = kSrgbToPcs[r * 3 + 0] + kSrgbToPcs[r * 3 + 1] +
                           kSrgbToPcs[r * 3 + 2];
        const double s = static_cast<double>(white[r]) / sum;
        for (int c = 0; c < 3; ++c) {
            out9[r * 3 + c] = static_cast<float>(kSrgbToPcs[r * 3 + c] * s);
        }
    }
}

// Which branch of the section-1.5 route table produced camera_to_pcs.
//
// Declared here rather than in libraw_gpu_input_adapter.h because that header is
// outside this change's file ownership. The two test files that consume it
// (test_libraw_adapter.cpp, test_raw_render_params.cpp) mirror these three
// values verbatim; test_libraw_adapter drives each branch and asserts the
// returned route, so a future renumbering fails a test rather than silently
// mislabelling a route in a diagnostic.
// Plain `int`, not an enum, precisely BECAUSE the declaration has to be
// duplicated in the test files: an enum return type would make the two
// declarations mangle differently and fail at link time in a way that reads like
// a missing symbol rather than a mismatched contract.
constexpr int kRawCameraMatrixRouteNone = 0;
constexpr int kRawCameraMatrixRouteRgbCam = 1;
constexpr int kRawCameraMatrixRouteCamXyz = 2;

int raw_camera_to_pcs_from_libraw(const float* rgb_cam, const float* cam_xyz,
                                  uint32_t raw_color, uint32_t colors,
                                  RawColorTransform* out, char* reason_out,
                                  size_t reason_cap);

// Stage 2 (design section 2.2): the white-balance source-selection sentinel
// chain. Declared here (not in the header, which is outside this change's file
// ownership) and mirrored in test_libraw_adapter.cpp so the guards can be driven
// with synthetic metadata -- no corpus file reaches the pre_mul / identity /
// as_shot_wb_applied branches, so a corpus-only test would leave them
// unexercised while reporting green (the 2026-07-10 allowlist lesson).
void raw_white_balance_from_libraw(const float* cam_mul, const float* pre_mul,
                                   uint32_t as_shot_wb_applied, uint32_t colors,
                                   float out_neutral[4], char* reason_out,
                                   size_t reason_cap);

RawColorKey raw_color_key_from_libraw(uint32_t libraw_index, uint32_t colors,
                                      const char* cdesc) {
    if (!cdesc || libraw_index >= 4) return kRawColorKeyUnknown;
    if (colors == 0) return kRawColorKeyUnknown;
    const char c = cdesc[libraw_index];
    switch (c) {
        case 'R': return kRawColorKeyRed;
        case 'G': return kRawColorKeyGreen;
        case 'B': return kRawColorKeyBlue;
        case 'C': return kRawColorKeyCyan;
        case 'M': return kRawColorKeyMagenta;
        case 'Y': return kRawColorKeyYellow;
        case 'W': return kRawColorKeyWhite;
        case 'E': return kRawColorKeyFujiGreen;   // LibRaw "RGBE" emerald slot
        default:  return kRawColorKeyUnknown;
    }
}

RawOrientation raw_orientation_from_libraw_flip(int32_t flip) {
    // Inverse of tiff.cpp:631's "50132467" EXIF->flip table; see the header for
    // the derivation. All eight dcraw bit-field values are legal, so mapping any
    // of them to Unknown would reject a perfectly ordinary portrait file.
    switch (flip) {
        case 0: return kRawOrientationTopLeft;      // EXIF 1
        case 1: return kRawOrientationTopRight;     // EXIF 2 (mirror horizontal)
        case 2: return kRawOrientationBottomLeft;   // EXIF 4 (mirror vertical)
        case 3: return kRawOrientationBottomRight;  // EXIF 3 (180 deg)
        case 4: return kRawOrientationLeftTop;      // EXIF 5 (transpose)
        case 5: return kRawOrientationLeftBottom;   // EXIF 8 (270 deg CW)
        case 6: return kRawOrientationRightTop;     // EXIF 6 (90 deg CW)
        case 7: return kRawOrientationRightBottom;  // EXIF 7 (anti-transpose)
        default: return kRawOrientationUnknown;
    }
}

uint32_t raw_bayer_channel_index_at_plane(uint32_t filters,
                                          uint32_t left_margin,
                                          uint32_t top_margin,
                                          uint32_t plane_row,
                                          uint32_t plane_col) {
    // colour at plane (r,c) == FC(r - top_margin, c - left_margin); FC is
    // 2-periodic and -x == x (mod 2), so adding the margin is the same shift.
    return bayerKeyIndex(filters, static_cast<int>((plane_row + top_margin) & 1u),
                         static_cast<int>((plane_col + left_margin) & 1u));
}

RawErrorCode raw_bayer_filters_check_2x2(uint32_t filters, char* reason_out,
                                         size_t reason_cap) {
    if (filters == 1) {
        if (reason_out && reason_cap) {
            std::snprintf(reason_out, reason_cap,
                          "filters==1 selects LibRaw's 16x16 CFA table "
                          "(utils_dcraw.cpp:41-42), not a 2x2 mosaic");
        }
        return kRawErrLayoutUnsupported;
    }
    // The word covers rows 0..7; a 2x2 summary is faithful only if every one of
    // those rows equals the row of the same parity.
    for (int row = 2; row < 8; ++row) {
        for (int col = 0; col < 2; ++col) {
            const uint32_t got = bayerKeyIndex(filters, row, col);
            const uint32_t want = bayerKeyIndex(filters, row & 1, col);
            if (got != want) {
                if (reason_out && reason_cap) {
                    std::snprintf(reason_out, reason_cap,
                                  "filters word 0x%08x is not 2-row periodic "
                                  "(row %d col %d is %u, row %d col %d is %u); a "
                                  "2x2 tile would mis-colour half the rows",
                                  filters, row, col, got, row & 1, col, want);
                }
                return kRawErrLayoutUnsupported;
            }
        }
    }
    return kRawSuccess;
}

RawErrorCode raw_black_pattern_from_libraw(uint32_t black_scalar,
                                           const uint32_t* channel_black,
                                           const uint8_t* channel_index,
                                           uint32_t cfa_w, uint32_t cfa_h,
                                           const uint32_t* spatial_black,
                                           uint32_t spatial_w, uint32_t spatial_h,
                                           uint32_t left_margin, uint32_t top_margin,
                                           RawBlackLevelPattern* out,
                                           char* reason_out, size_t reason_cap) {
    if (!out) return kRawErrMetadataInvalid;

    // A channel term that is present but all-zero is deliberately treated as
    // absent: folding it in would only enlarge the emitted tile (lcm) without
    // changing a single value, and every current corpus file is in that case.
    bool has_channel = channel_black && channel_index && cfa_w > 0 && cfa_h > 0;
    if (has_channel) {
        has_channel = channel_black[0] || channel_black[1] ||
                      channel_black[2] || channel_black[3];
    }
    const bool has_spatial = spatial_black && spatial_w > 0 && spatial_h > 0;

    if (has_spatial &&
        static_cast<size_t>(spatial_w) * spatial_h > kRawMaxCfaPatternCount) {
        if (reason_out && reason_cap) {
            std::snprintf(reason_out, reason_cap,
                          "black level repeat %ux%u exceeds the 8x8 ceiling",
                          spatial_w, spatial_h);
        }
        return kRawErrLayoutUnsupported;
    }

    const uint32_t cw = has_channel ? cfa_w : 1u;
    const uint32_t ch = has_channel ? cfa_h : 1u;
    const uint32_t sw = has_spatial ? spatial_w : 1u;
    const uint32_t sh = has_spatial ? spatial_h : 1u;

    const uint32_t tile_w = lcmU32(cw, sw);
    const uint32_t tile_h = lcmU32(ch, sh);
    if (tile_w == 0 || tile_h == 0 ||
        tile_w > kRawMaxCfaRepeat || tile_h > kRawMaxCfaRepeat) {
        if (reason_out && reason_cap) {
            std::snprintf(reason_out, reason_cap,
                          "combined black level repeat %ux%u (cfa %ux%u, spatial "
                          "%ux%u) exceeds the 8x8 ceiling",
                          tile_w, tile_h, cw, ch, sw, sh);
        }
        return kRawErrLayoutUnsupported;
    }

    // Plane-origin offsets for the visible-relative spatial term (see header).
    const uint32_t row_shift = has_spatial ? (top_margin % sh) : 0u;
    const uint32_t col_shift = has_spatial ? (left_margin % sw) : 0u;

    *out = RawBlackLevelPattern{};
    out->repeat_width = tile_w;
    out->repeat_height = tile_h;
    for (uint32_t r = 0; r < tile_h; ++r) {
        for (uint32_t c = 0; c < tile_w; ++c) {
            uint64_t v = black_scalar;
            if (has_channel) {
                // channel_index is already plane-relative (caller's contract).
                const uint32_t idx = channel_index[(r % ch) * cfa_w + (c % cw)];
                if (idx < 4) v += channel_black[idx];
            }
            if (has_spatial) {
                // visible row/col of this plane site, without going negative.
                const uint32_t sr = (r + sh - row_shift) % sh;
                const uint32_t sc = (c + sw - col_shift) % sw;
                v += spatial_black[sr * spatial_w + sc];
            }
            out->values[r * tile_w + c] = static_cast<float>(v);
        }
    }
    return kRawSuccess;
}

void raw_component_black_from_libraw(uint32_t black_scalar,
                                     const uint32_t* channel_black,
                                     float out[4]) {
    for (int c = 0; c < 4; ++c) {
        const uint32_t chan = channel_black ? channel_black[c] : 0u;
        out[c] = static_cast<float>(black_scalar) + static_cast<float>(chan);
    }
}

bool raw_invert_3x3(const float in9[9], float out9[9]) {
    if (!in9 || !out9) return false;
    double m[9];
    for (int i = 0; i < 9; ++i) {
        if (!std::isfinite(in9[i])) return false;
        m[i] = static_cast<double>(in9[i]);
    }
    const double c00 = m[4] * m[8] - m[5] * m[7];
    const double c01 = m[5] * m[6] - m[3] * m[8];
    const double c02 = m[3] * m[7] - m[4] * m[6];
    const double det = m[0] * c00 + m[1] * c01 + m[2] * c02;
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;

    const double inv[9] = {
        c00,
        m[2] * m[7] - m[1] * m[8],
        m[1] * m[5] - m[2] * m[4],
        c01,
        m[0] * m[8] - m[2] * m[6],
        m[2] * m[3] - m[0] * m[5],
        c02,
        m[1] * m[6] - m[0] * m[7],
        m[0] * m[4] - m[1] * m[3],
    };
    for (int i = 0; i < 9; ++i) {
        const double value = inv[i] / det;
        if (!std::isfinite(value)) return false;
        out9[i] = static_cast<float>(value);
    }
    return true;
}

RawErrorCode LibRawGpuInputAdapter::build(const LibRawFrontendContext& ctx,
                                          RawGpuInput* out_input,
                                          RawDevelopParams* out_develop,
                                          char* reason_out, size_t reason_cap) {
    if (reason_out && reason_cap > 0) reason_out[0] = '\0';
    if (!out_input || !out_develop) return kRawErrMetadataInvalid;

    // auto_exposure_mode is the one RawDevelopParams field build() reads as
    // INPUT: every other field here is pure output (reset below), but the
    // caller must be able to request kRawAutoExposureOff before the adapter
    // computes anything. Captured before the reset that follows.
    const int32_t requested_auto_exposure_mode = out_develop->auto_exposure_mode;

    *out_input = RawGpuInput{};
    *out_develop = RawDevelopParams{};
    out_develop->auto_exposure_mode = requested_auto_exposure_mode;

    if (!ctx.is_open()) {
        if (reason_out && reason_cap) {
            std::snprintf(reason_out, reason_cap, "frontend context is not open");
        }
        return kRawErrParseFailed;
    }

    const LibRawRawView& v = ctx.raw_view();

    // --- plane (borrowed, never copied) -----------------------------------
    planes_[0] = v.plane;
    out_input->planes = planes_;
    out_input->plane_count = 1;

    // --- CFA pattern -------------------------------------------------------
    RawLayoutDescriptor& layout = out_input->layout;
    layout.sample_type = kRawSampleTypeU16;
    layout.memory_layout = kRawMemoryLayoutInterleaved;
    layout.geometry = kRawGeometryRectilinear;
    layout.plane_count = 1;

    // LibRaw colour INDEX per site (what FC returns), kept alongside the colour
    // KEY tile because cblack[0..3] is indexed by the index, not by the key.
    uint8_t channel_index[kRawMaxCfaPatternCount] = {0};
    uint32_t channel_w = 0, channel_h = 0;

    if (v.filters == 9) {
        // X-Trans: keep the whole 6x6. Collapsing to filters==9 is the bug
        // spec section 3.3.2 exists to prevent.
        if (!v.xtrans_pattern) {
            if (reason_out && reason_cap) {
                std::snprintf(reason_out, reason_cap,
                              "filters==9 but no 6x6 xtrans pattern");
            }
            return kRawErrMetadataInvalid;
        }
        // No margin shift here, and that is verified rather than assumed:
        // identify.cpp:2548-2551 derives the visible-relative `xtrans` FROM
        // `xtrans_abs` by adding top_margin/left_margin, so `xtrans_abs` (what
        // the frontend hands us) is already the absolute raw-plane tile.
        for (int i = 0; i < 36; ++i) {
            // xtrans_abs is a signed char array; a negative entry would wrap to
            // a huge unsigned value, which raw_color_key_from_libraw rejects as
            // out of range (>= 4) rather than silently indexing cdesc.
            const unsigned char idx =
                static_cast<unsigned char>(v.xtrans_pattern[i]);
            cfa_pattern_[i] =
                raw_color_key_from_libraw(static_cast<uint32_t>(idx), v.colors, v.cdesc);
            channel_index[i] = idx;
        }
        channel_w = 6;
        channel_h = 6;
        layout.sample_model = kRawSampleModelCfa;
        layout.components_per_pixel = 1;
        layout.cfa_repeat_width = 6;
        layout.cfa_repeat_height = 6;
        layout.cfa_pattern = cfa_pattern_;
        layout.cfa_pattern_count = 36;
    } else if (v.filters != 0) {
        // The 2x2 summary below is only faithful if the word really is 2-row
        // periodic; five vendored bodies are not (round-5 finding S3, citations
        // in the header). Reject those by name rather than leaning on the
        // colour-descriptor check that happens to catch them today.
        const RawErrorCode periodic_rc =
            raw_bayer_filters_check_2x2(v.filters, reason_out, reason_cap);
        if (periodic_rc != kRawSuccess) return periodic_rc;

        // PLANE-relative, not visible-relative: LibRaw's filters word is indexed
        // in visible coordinates, so it is shifted by the margin parity here
        // (round-5 origin ruling; derivation and citations in the header).
        for (uint32_t row = 0; row < 2; ++row) {
            for (uint32_t col = 0; col < 2; ++col) {
                const uint32_t idx = raw_bayer_channel_index_at_plane(
                    v.filters, v.visible_left, v.visible_top, row, col);
                cfa_pattern_[row * 2 + col] =
                    raw_color_key_from_libraw(idx, v.colors, v.cdesc);
                channel_index[row * 2 + col] = static_cast<uint8_t>(idx);
            }
        }
        channel_w = 2;
        channel_h = 2;
        layout.sample_model = kRawSampleModelCfa;
        layout.components_per_pixel = 1;
        layout.cfa_repeat_width = 2;
        layout.cfa_repeat_height = 2;
        layout.cfa_pattern = cfa_pattern_;
        layout.cfa_pattern_count = 4;
    } else {
        // filters == 0: not a CFA sensor. components_per_pixel alone never
        // decides this (spec section 3.3.4) - sample_model does.
        layout.sample_model = (v.colors == 1)   ? kRawSampleModelMonochrome
                            : (v.colors >= 3)   ? kRawSampleModelLinearRgb
                                                : kRawSampleModelUnknown;
        // From the BUFFER the frontend actually accepted, not from `colors`
        // (which describes the sensor). For X3F both say 3; for a future
        // 4-component buffer they would not, and the contract must describe
        // the memory the GPU will read.
        layout.components_per_pixel =
            v.components_per_pixel ? v.components_per_pixel : 1;
        layout.cfa_pattern = nullptr;
        layout.cfa_pattern_count = 0;
    }

    // --- rects -------------------------------------------------------------
    out_input->active_area = RawRect{0, 0, v.raw_width, v.raw_height};
    out_input->default_crop = RawRect{static_cast<int32_t>(v.visible_left),
                                      static_cast<int32_t>(v.visible_top),
                                      v.visible_width, v.visible_height};

    out_input->orientation = raw_orientation_from_libraw_flip(v.flip);

    // --- black -------------------------------------------------------------
    // All THREE LibRaw terms, including the per-channel cblack[0..3] that the
    // plan omits (round-4 finding F-R4-02) - see the header for the citation.
    const RawErrorCode black_rc = raw_black_pattern_from_libraw(
        (layout.sample_model == kRawSampleModelLinearRgb) ? 0u : v.black_scalar,
        v.black_channel, channel_index, channel_w, channel_h,
        v.black_pattern, v.black_repeat_width, v.black_repeat_height,
        v.visible_left, v.visible_top, &out_input->black, reason_out, reason_cap);
    if (black_rc != kRawSuccess) return black_rc;

    // P19: per-component black for non-CFA layouts. The spatial tile above was
    // built with a ZERO scalar and no channel term for this branch (see the
    // channel_w/channel_h == 0 arguments), so the whole black level lives here
    // exactly once. Double-subtracting color.black is the failure mode this
    // arrangement exists to make impossible.
    for (int c = 0; c < 4; ++c) out_input->component_black[c] = 0.0f;
    if (layout.sample_model == kRawSampleModelLinearRgb) {
        raw_component_black_from_libraw(v.black_scalar, v.black_channel,
                                        out_input->component_black);
    }

    // --- white -------------------------------------------------------------
    if (v.white_level == 0) {
        if (reason_out && reason_cap) {
            std::snprintf(reason_out, reason_cap, "libraw reported white_level 0");
        }
        return kRawErrMetadataInvalid;
    }
    for (int c = 0; c < 4; ++c) {
        out_input->white_level[c] = static_cast<float>(v.white_level);
    }

    // --- white balance -----------------------------------------------------
    // Full design section 2.2 sentinel chain lives in the free function so the
    // as_shot_wb_applied / pre_mul / identity guards can be driven synthetically
    // (no corpus file reaches them).
    raw_white_balance_from_libraw(v.cam_mul, v.pre_mul, v.as_shot_wb_applied,
                                  v.colors, out_input->as_shot_neutral, reason_out,
                                  reason_cap);

    // --- colour matrix -----------------------------------------------------
    raw_camera_to_pcs_from_libraw(v.rgb_cam, v.cam_xyz, v.raw_color, v.colors,
                                  &out_input->camera_to_pcs, reason_out,
                                  reason_cap);

    out_input->decoder_backend = ctx.diagnostics().unpack_backend;

    // --- automatic exposure baseline (Round 1 Task 1.3;
    // explore_codebase_color_gap.md §6 H1: LibRaw's own CPU render entry point
    // (the forbidden dcraw_process call, spec §13.1) is never invoked here, so
    // LibRaw's own auto-brighten gain never applies -- this ports the
    // histogram estimator (raw_auto_exposure.h) as our own replacement) ------
    // Bayer-only: the estimator's cfa_channel_of is a plain 2x2 map, and a
    // true 2x2-periodic Bayer word is the only layout this adapter can express
    // that way without inventing a mapping no LibRaw metadata supports (the
    // 6x6 X-Trans tile and non-CFA linear-RGB layouts are out of scope here).
    // Left at its zero-initialised 0.0f default in those cases and when the
    // mode is off -- never a fabricated value, spec section 4.1.9.
    out_develop->auto_exposure_ev = 0.0f;
    if (out_develop->auto_exposure_mode == kRawAutoExposureOn &&
        layout.sample_model == kRawSampleModelCfa &&
        layout.cfa_repeat_width == 2 && layout.cfa_repeat_height == 2 &&
        v.plane.data != nullptr && v.plane.pixel_stride_bytes == sizeof(uint16_t)) {
        float black4[4];
        for (int pos = 0; pos < 4; ++pos) {
            const uint32_t row = static_cast<uint32_t>(pos / 2);
            const uint32_t col = static_cast<uint32_t>(pos % 2);
            const uint32_t idx =
                (row % out_input->black.repeat_height) * out_input->black.repeat_width +
                (col % out_input->black.repeat_width);
            black4[pos] = out_input->black.values[idx];
        }
        // wb_gain per the header contract: the gain implied by as_shot_neutral,
        // green-referenced (as_shot_neutral itself is already green-referenced
        // by raw_white_balance_from_libraw, so its reciprocal is too).
        float wb_gain[4];
        for (int c = 0; c < 4; ++c) {
            wb_gain[c] = out_input->as_shot_neutral[c] > 1e-6f
                             ? 1.0f / out_input->as_shot_neutral[c] : 1.0f;
        }
        uint32_t cfa_channel_of[4] = {0, 1, 2, 3};
        // Task 1.1's decision gate (native/scripts/tmp/round1_histogram_spike.md):
        // a full scan costs 12-16% of raw_unpack_ms on the largest corpus file,
        // above the 10% threshold, so stride 4x4 (plan B) is required here.
        const uint32_t row_pitch_samples = static_cast<uint32_t>(
            v.plane.row_stride_bytes / static_cast<int64_t>(sizeof(uint16_t)));
        const RawAutoExposureResult est = raw_auto_exposure_estimate(
            static_cast<const uint16_t*>(v.plane.data), v.raw_width, v.raw_height,
            row_pitch_samples, /*stride_x=*/4, /*stride_y=*/4, black4,
            out_input->white_level[0], wb_gain, cfa_channel_of);
        if (est.status == RawAutoExposureStatus::kOk) {
            out_develop->auto_exposure_ev = est.auto_ev;
        }
    }

    // --- develop defaults --------------------------------------------------
    out_develop->exposure_ev = 0.0f;
    out_develop->tone_curve_strength = 1.0f;
    out_develop->output_space = kRawOutputColorSpaceSrgb;
    out_develop->max_output_long_edge = 0;

    // Validation is not optional (spec section 6.2 step 8).
    return raw_validate_gpu_input(out_input, reason_out, reason_cap);
}

// Free function, not a lambda inside build(), for one reason: the FALLBACK route
// is not reachable from any file in the corpus (every sample LibRaw recognises
// yields a usable rgb_cam), so a corpus-only test would leave half the route
// table unexercised while reporting green. AC-1.2 requires the D50-white
// invariant asserted for BOTH routes, and this signature is what lets the tests
// drive each branch directly with synthetic metadata.
//
// CONTRACT (design Task_raw_color_architecture.md Rev 2 section 1.5):
// camera_to_pcs carries a WHITE-PRESERVING matrix -- it maps *white-balanced*
// camera RGB to XYZ(D50 PCS), i.e. camera_to_pcs * (1,1,1) == PCS white. The
// white balance itself is a right-multiplied diag(g) that the builder folds
// in; it is deliberately NOT baked in here. Because the invariant holds, the
// builder needs no residual row normalisation, and normalizeRowsToNeutral --
// which formed diag(k)*A, a diagonal in the OUTPUT space where von Kries
// requires one in the INPUT space -- is gone.
//
// Format knowledge lives here, so the choice of route is invisible to the
// builder. Two routes, one invariant:
//
//   PRIMARY   rgb_cam usable   camera_to_pcs = M_srgb_to_pcs * rgb_cam3
//   FALLBACK  cam_xyz usable   camera_to_pcs = rowScaleToPcsWhite(Invert(cam_xyz3))
//   NEITHER                    valid = 0 + reason (never an invented matrix)
//
// rgb_cam is preferred because it is LibRaw's OWN colorimetry, is
// white-preserving by construction (utils_dcraw.cpp:296-312 normalises the
// forward matrix BEFORE inverting it, which is not the same matrix as
// normalising the inverse), and is written directly by simple_coeff for
// cameras that have no cam_xyz at all (colordata.cpp:1918-1936, the Foveon
// case).
//
// Returns the route actually taken, so a test can assert WHICH branch ran
// rather than only that the result is well-formed.
int raw_camera_to_pcs_from_libraw(const float* rgb_cam, const float* cam_xyz,
                                  uint32_t raw_color, uint32_t colors,
                                  RawColorTransform* out, char* reason_out,
                                  size_t reason_cap) {
    if (!out) return kRawCameraMatrixRouteNone;
    RawColorTransform& xf = *out;
    xf.valid = 0;   // never substitute an identity (spec section 4.1.9)

    // AC-2.3 (design section 2.3): 4-colour sensors (CMYG / RGBE / Foveon
    // Quattro) have no supported 3x3 camera->PCS path. rgb_cam is genuinely 3x4
    // and cam_xyz's 4th camera row would be dropped, so either route yields a
    // colour-wrong render reporting success. Fail explicitly rather than
    // fall through to the cam_xyz fallback (which colors == 4 otherwise passes).
    if (colors >= 4) {
        if (reason_out && reason_cap && reason_out[0] == '\0') {
            std::snprintf(reason_out, reason_cap,
                          "colors=%u: 4-colour sensors (CMYG/RGBE/Quattro) are "
                          "not supported; no camera matrix emitted", colors);
        }
        return kRawCameraMatrixRouteNone;
    }

    float pcs_white[3];
    raw_pcs_white(pcs_white);

    // --- route 1: LibRaw's rgb_cam (sRGB-from-camera, 3 rows x 4 columns) ---
    // raw_color != 0 means LibRaw matched no camera and left rgb_cam at the
    // identity (identify.cpp:509). The identity check is kept as well: applying
    // an ICC profile also sets raw_color (apply_profile.cpp:69), and an identity
    // here would be exactly the invented matrix spec 4.1.9 forbids.
    //
    // colors == 3 is required, not colors >= 3: for a 4-colour sensor rgb_cam is
    // genuinely 3x4 and dropping its fourth column would be a wrong-colour
    // render reporting success. Such a file falls through to the branches below
    // rather than being silently truncated here; the explicit unsupported branch
    // design section 2.3 calls for is Stage 2's AC-2.3.
    bool rgb_cam_usable = false;
    float srgb_from_cam[9] = {0};
    if (rgb_cam && raw_color == 0 && colors == 3) {
        bool finite = true;
        bool identity = true;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                // rgb_cam is [3][4]: row stride is 4, not 3.
                const float value = rgb_cam[r * 4 + c];
                if (!std::isfinite(value)) finite = false;
                srgb_from_cam[r * 3 + c] = value;
                const float want = (r == c) ? 1.0f : 0.0f;
                if (std::fabs(value - want) > 1e-6f) identity = false;
            }
        }
        rgb_cam_usable = finite && !identity;
    }

    if (rgb_cam_usable) {
        float srgb_to_pcs[9];
        raw_srgb_to_pcs_matrix(srgb_to_pcs);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                double acc = 0.0;
                for (int k = 0; k < 3; ++k) {
                    acc += static_cast<double>(srgb_to_pcs[r * 3 + k]) *
                           static_cast<double>(srgb_from_cam[k * 3 + c]);
                }
                xf.m[r * 3 + c] = static_cast<float>(acc);
            }
        }
        xf.valid = 1;
        xf.out_rows = 3;
        xf.in_cols = 3;
        return kRawCameraMatrixRouteRgbCam;
    }

    // --- route 2 (fallback): Invert(cam_xyz), rescaled onto the PCS white ---
    // cam_xyz is `float cam_xyz[4][3]` -- 4 camera channels x 3 XYZ columns,
    // i.e. camera-FROM-XYZ, so the 3x3 is INVERTED to get camera->XYZ (ruled in
    // docs/logs/2026-08-25/r5-camera-to-pcs-ruling.md). The column count is fixed
    // at 3 and does NOT follow idata.colors.
    //
    // Unlike rgb_cam, Invert(cam_xyz) is NOT white-preserving, so each row is
    // scaled to make it so. This is a left diagonal, which is legitimate ONLY
    // because it runs before the builder's right diagonal and its job is to fix
    // the matrix's own white point, not to white-balance the image -- the same
    // role NormalizeForwardMatrix plays in the SDK. It is an approximation of
    // that function's Bradford adaptation (design section 7, Stage 3).
    bool any_nonzero = false;
    if (cam_xyz) {
        for (int i = 0; i < 12; ++i) {
            if (cam_xyz[i] != 0.0f) { any_nonzero = true; break; }
        }
    }
    if (any_nonzero && colors >= 3) {
        float cam_from_xyz[9];
        for (int i = 0; i < 9; ++i) cam_from_xyz[i] = cam_xyz[i];
        float xyz_from_cam[9];
        if (raw_invert_3x3(cam_from_xyz, xyz_from_cam)) {
            bool scalable = true;
            float scaled[9];
            for (int r = 0; r < 3; ++r) {
                const double sum = static_cast<double>(xyz_from_cam[r * 3 + 0]) +
                                   static_cast<double>(xyz_from_cam[r * 3 + 1]) +
                                   static_cast<double>(xyz_from_cam[r * 3 + 2]);
                if (!(std::fabs(sum) > 1e-8)) { scalable = false; break; }
                const double k = static_cast<double>(pcs_white[r]) / sum;
                for (int c = 0; c < 3; ++c) {
                    scaled[r * 3 + c] = static_cast<float>(
                        static_cast<double>(xyz_from_cam[r * 3 + c]) * k);
                }
            }
            if (scalable) {
                xf.valid = 1;
                xf.out_rows = 3;
                xf.in_cols = 3;
                for (int i = 0; i < 9; ++i) xf.m[i] = scaled[i];
                return kRawCameraMatrixRouteCamXyz;
            }
            if (reason_out && reason_cap && reason_out[0] == '\0') {
                std::snprintf(reason_out, reason_cap,
                              "Invert(cam_xyz) has a zero row sum; it cannot be "
                              "mapped onto the PCS white");
            }
            return kRawCameraMatrixRouteNone;
        }
        if (reason_out && reason_cap && reason_out[0] == '\0') {
            std::snprintf(reason_out, reason_cap,
                          "cam_xyz is not invertible; no camera matrix emitted");
        }
        return kRawCameraMatrixRouteNone;
    }

    if (reason_out && reason_cap && reason_out[0] == '\0') {
        std::snprintf(reason_out, reason_cap,
                      "no usable colour data (raw_color=%u, colors=%u, rgb_cam "
                      "identity or absent, cam_xyz absent); no camera matrix "
                      "emitted",
                      raw_color, colors);
    }
    return kRawCameraMatrixRouteNone;
}

// Design section 2.2 white-balance source-selection sentinel chain. Produces the
// selected 4-channel multiplier vector m, then the DNG-style as_shot_neutral,
// out_neutral[c] = m[1] / m[c] (green is the reference channel, exactly as the
// pre-Stage-2 code used it, so a file with a usable cam_mul renders identically).
//
//   if (as_shot_wb_applied)                             m = {1,1,1,1}
//   else if (cam_mul[0] > 1e-5 && cam_mul[2] > 1e-5)    m = cam_mul
//   else if (pre_mul[0] > 1e-5 && pre_mul[1] > 1e-5)    m = pre_mul  // + reason
//   else                                                m = {1,1,1,1} // + reason
//   if (m[1] == 0) m[1] = 1
//   if (m[3] == 0) m[3] = (colors < 4) ? m[1] : 1
//
// cam_mul[0] < -0.5 is LibRaw's legacy auto-WB marker (a negative sentinel), so
// it fails the `> 1e-5` test and falls through to pre_mul -- the same channels
// [0]/[2] LibRaw's own scale_colors sentinel checks (the pre-Stage-2 code tested
// [0]/[1], which wrongly accepted a cam_mul whose blue channel was unparsed).
void raw_white_balance_from_libraw(const float* cam_mul, const float* pre_mul,
                                   uint32_t as_shot_wb_applied, uint32_t colors,
                                   float out_neutral[4], char* reason_out,
                                   size_t reason_cap) {
    const bool write_reason = reason_out && reason_cap && reason_out[0] == '\0';

    float m[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    const bool cam_mul_usable =
        cam_mul && std::isfinite(cam_mul[0]) && std::isfinite(cam_mul[2]) &&
        cam_mul[0] > 1e-5f && cam_mul[2] > 1e-5f;
    const bool pre_mul_usable =
        pre_mul && std::isfinite(pre_mul[0]) && std::isfinite(pre_mul[1]) &&
        pre_mul[0] > 1e-5f && pre_mul[1] > 1e-5f;

    if (as_shot_wb_applied) {
        // WB already folded upstream (Nikon sRAW / small-raw). Multiplying by
        // cam_mul again double-applies it and the frame goes hard magenta.
        m[0] = m[1] = m[2] = m[3] = 1.0f;
    } else if (cam_mul_usable) {
        for (int c = 0; c < 4; ++c) m[c] = cam_mul[c];
    } else if (pre_mul_usable) {
        for (int c = 0; c < 4; ++c) m[c] = pre_mul[c];
        if (write_reason) {
            std::snprintf(reason_out, reason_cap,
                          "cam_mul unusable (channels [0]/[2] not both > 1e-5); "
                          "fell back to pre_mul daylight white balance");
        }
    } else {
        // Identity is a legal outcome, not a guess at a matrix; record why.
        m[0] = m[1] = m[2] = m[3] = 1.0f;
        if (write_reason) {
            std::snprintf(reason_out, reason_cap,
                          "no usable cam_mul or pre_mul; white balance left at "
                          "identity");
        }
    }

    // Reference (green) channel must be positive; a 4th channel of 0 is by design
    // for a 3-colour cam_mul and takes the green gain rather than a 0 (which would
    // be an infinite as_shot_neutral). colors == 4 keeps 1.0 (that render is
    // rejected by the matrix's unsupported branch anyway).
    if (m[1] == 0.0f) m[1] = 1.0f;
    if (m[3] == 0.0f) m[3] = (colors < 4) ? m[1] : 1.0f;

    for (int c = 0; c < 4; ++c) {
        out_neutral[c] =
            (m[c] > 0.0f && std::isfinite(m[c])) ? (m[1] / m[c]) : 1.0f;
    }
}

