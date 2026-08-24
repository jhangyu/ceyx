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
    switch (flip) {
        case 0: return kRawOrientationTopLeft;      // EXIF 1
        case 3: return kRawOrientationBottomRight;  // EXIF 3
        case 5: return kRawOrientationLeftTop;      // EXIF 5
        case 6: return kRawOrientationRightTop;     // EXIF 6
        default: return kRawOrientationUnknown;
    }
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
        for (int i = 0; i < 36; ++i) {
            // xtrans_abs is a signed char array; a negative entry would wrap to
            // a huge unsigned value, which raw_color_key_from_libraw rejects as
            // out of range (>= 4) rather than silently indexing cdesc.
            cfa_pattern_[i] = raw_color_key_from_libraw(
                static_cast<uint32_t>(static_cast<unsigned char>(v.xtrans_pattern[i])),
                v.colors, v.cdesc);
        }
        layout.sample_model = kRawSampleModelCfa;
        layout.components_per_pixel = 1;
        layout.cfa_repeat_width = 6;
        layout.cfa_repeat_height = 6;
        layout.cfa_pattern = cfa_pattern_;
        layout.cfa_pattern_count = 36;
    } else if (v.filters != 0) {
        for (int row = 0; row < 2; ++row) {
            for (int col = 0; col < 2; ++col) {
                cfa_pattern_[row * 2 + col] = raw_color_key_from_libraw(
                    bayerKeyIndex(v.filters, row, col), v.colors, v.cdesc);
            }
        }
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
        layout.components_per_pixel = v.colors ? v.colors : 1;
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
    const uint32_t bw = v.black_repeat_width;
    const uint32_t bh = v.black_repeat_height;
    if (v.black_pattern && bw > 0 && bh > 0) {
        if (static_cast<size_t>(bw) * bh > kRawMaxCfaPatternCount) {
            if (reason_out && reason_cap) {
                std::snprintf(reason_out, reason_cap,
                              "black level repeat %ux%u exceeds the 8x8 ceiling", bw, bh);
            }
            return kRawErrLayoutUnsupported;
        }
        out_input->black.repeat_width = bw;
        out_input->black.repeat_height = bh;
        for (uint32_t i = 0; i < bw * bh; ++i) {
            // LibRaw stores per-site deltas on top of the scalar black.
            out_input->black.values[i] =
                static_cast<float>(v.black_scalar) + static_cast<float>(v.black_pattern[i]);
        }
    } else {
        out_input->black.repeat_width = 1;
        out_input->black.repeat_height = 1;
        out_input->black.values[0] = static_cast<float>(v.black_scalar);
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
    // Semantic note for Task 8 (parking-lot, not decided here): the values are
    // camera-from-XYZ as LibRaw stores them, transcribed without inversion, per
    // the plan's explicit mapping rule.
    RawColorTransform& xf = out_input->camera_to_pcs;
    bool any_nonzero = false;
    if (v.cam_xyz) {
        for (int i = 0; i < 12; ++i) {
            if (v.cam_xyz[i] != 0.0f) { any_nonzero = true; break; }
        }
    }
    if (any_nonzero) {
        xf.valid = 1;
        xf.out_rows = 3;
        xf.in_cols = 3;
        const uint32_t rows = v.colors < 3 ? v.colors : 3;
        for (uint32_t r = 0; r < rows; ++r) {
            for (uint32_t c = 0; c < 3; ++c) {
                xf.m[r * 3 + c] = v.cam_xyz[r * 3 + c];
            }
        }
    } else {
        xf.valid = 0;   // never substitute an identity (spec section 4.1.9)
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
