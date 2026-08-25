#include "libraw_gpu_input_adapter.h"

#include <cmath>
#include <cstdio>
#include <cstring>

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

    *out_input = RawGpuInput{};
    *out_develop = RawDevelopParams{};

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
        for (int c = 0; c < 4; ++c) {
            const uint32_t chan = v.black_channel ? v.black_channel[c] : 0u;
            out_input->component_black[c] =
                static_cast<float>(v.black_scalar) + static_cast<float>(chan);
        }
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
    const float* mul = nullptr;
    if (v.cam_mul && v.cam_mul[0] > 0.0f && v.cam_mul[1] > 0.0f) mul = v.cam_mul;
    else if (v.pre_mul && v.pre_mul[0] > 0.0f && v.pre_mul[1] > 0.0f) mul = v.pre_mul;

    for (int c = 0; c < 4; ++c) out_input->as_shot_neutral[c] = 1.0f;
    if (mul) {
        for (int c = 0; c < 4; ++c) {
            if (mul[c] > 0.0f && std::isfinite(mul[c])) {
                out_input->as_shot_neutral[c] = mul[1] / mul[c];
            }
        }
    } else if (reason_out && reason_cap) {
        // Identity is a legal outcome, not a guess at a matrix; record why.
        std::snprintf(reason_out, reason_cap,
                      "no usable cam_mul/pre_mul; white balance left at identity");
    }

    // --- colour matrix -----------------------------------------------------
    // Observed layout at the pinned revision (libraw/libraw_types.h, struct
    // libraw_colordata_t): `float cam_xyz[4][3]` - 4 camera channels x 3 XYZ
    // columns, i.e. camera-from-XYZ. The column count is therefore fixed at 3
    // and does NOT follow idata.colors; the plan's `in_cols = min(colors, 4)`
    // would index a fourth column that does not exist, so in_cols is clamped to
    // 3 here (documented substitution). Rows beyond min(colors,3) stay zero.
    //
    // DIRECTION (ruled in docs/logs/2026-08-25/r5-camera-to-pcs-ruling.md):
    // cam_xyz is camera-FROM-XYZ, the contract field is camera_to_pcs, and the
    // Task 8 consumer (src/raw_render_params_builder.cpp:92-98) left-multiplies
    // it by ProPhoto::MatrixFromPCS(), which only type-checks for camera->XYZ.
    // So the 3x3 is INVERTED here. Transcribing without inversion, as the plan
    // says, would render the generic route in a scrambled colour space.
    RawColorTransform& xf = out_input->camera_to_pcs;
    bool any_nonzero = false;
    if (v.cam_xyz) {
        for (int i = 0; i < 12; ++i) {
            if (v.cam_xyz[i] != 0.0f) { any_nonzero = true; break; }
        }
    }
    xf.valid = 0;   // never substitute an identity (spec section 4.1.9)
    if (any_nonzero && v.colors >= 3) {
        float cam_from_xyz[9];
        for (int i = 0; i < 9; ++i) cam_from_xyz[i] = v.cam_xyz[i];
        float xyz_from_cam[9];
        if (raw_invert_3x3(cam_from_xyz, xyz_from_cam)) {
            xf.valid = 1;
            xf.out_rows = 3;
            xf.in_cols = 3;
            for (int i = 0; i < 9; ++i) xf.m[i] = xyz_from_cam[i];
        } else if (reason_out && reason_cap && reason_out[0] == '\0') {
            std::snprintf(reason_out, reason_cap,
                          "cam_xyz is not invertible; no camera matrix emitted");
        }
    }

    out_input->decoder_backend = ctx.diagnostics().unpack_backend;

    // --- develop defaults --------------------------------------------------
    out_develop->exposure_ev = 0.0f;
    out_develop->tone_curve_strength = 1.0f;
    out_develop->output_space = kRawOutputColorSpaceSrgb;
    out_develop->max_output_long_edge = 0;

    // Validation is not optional (spec section 6.2 step 8).
    return raw_validate_gpu_input(out_input, reason_out, reason_cap);
}
