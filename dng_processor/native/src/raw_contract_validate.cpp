#include "raw_contract_validate.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Sets *reason and returns code, so every rule is one line at the call site.
RawErrorCode failWith(char* reason_out, size_t reason_cap, RawErrorCode code,
                      const char* text) {
    if (reason_out && reason_cap > 0) {
        std::snprintf(reason_out, reason_cap, "%s", text);
    }
    return code;
}

size_t sampleSizeBytes(RawSampleType type) {
    return type == kRawSampleTypeF32 ? 4u : 2u;
}

// a*b with an explicit overflow guard; returns false when the product would
// not fit. Everything downstream allocates from these products, so this runs
// before any allocation (spec section 10.1).
bool mulChecked(int64_t a, int64_t b, int64_t* out) {
    if (a < 0 || b < 0) return false;
    if (a != 0 && b > INT64_MAX / a) return false;
    *out = a * b;
    return true;
}

bool rectInside(const RawRect& r, uint32_t w, uint32_t h) {
    if (r.width == 0 || r.height == 0) return false;
    if (r.x < 0 || r.y < 0) return false;
    const int64_t right = static_cast<int64_t>(r.x) + r.width;
    const int64_t bottom = static_cast<int64_t>(r.y) + r.height;
    return right <= static_cast<int64_t>(w) && bottom <= static_cast<int64_t>(h);
}

}  // namespace

extern "C" {

RawLayoutClass raw_classify_layout(const RawLayoutDescriptor* layout) {
    if (!layout) return kRawLayoutClassUnsupported;

    switch (layout->sample_model) {
        case kRawSampleModelUnknown: return kRawLayoutClassUnsupported;
        case kRawSampleModelLayered: return kRawLayoutClassLayered;
        case kRawSampleModelMultiFrame: return kRawLayoutClassMultiFrame;
        case kRawSampleModelMonochrome: return kRawLayoutClassMonochrome;
        case kRawSampleModelLinearRgb: return kRawLayoutClassLinearRgb;
        case kRawSampleModelLinearYCbCr: return kRawLayoutClassLinearYCbCr;
        case kRawSampleModelCfa: break;
    }

    const uint32_t rw = layout->cfa_repeat_width;
    const uint32_t rh = layout->cfa_repeat_height;
    if (!layout->cfa_pattern) return kRawLayoutClassUnsupported;
    if (rw == 0 || rh == 0) return kRawLayoutClassUnsupported;
    if (rw > kRawMaxCfaRepeat || rh > kRawMaxCfaRepeat) {
        return kRawLayoutClassUnsupported;  // spec section 3.3.3
    }
    if (layout->cfa_pattern_count != static_cast<size_t>(rw) * rh) {
        return kRawLayoutClassUnsupported;
    }
    for (size_t i = 0; i < layout->cfa_pattern_count; ++i) {
        if (layout->cfa_pattern[i] == kRawColorKeyUnknown) {
            return kRawLayoutClassUnsupported;  // never coerce to Green
        }
    }

    int reds = 0, greens = 0, blues = 0, others = 0;
    for (size_t i = 0; i < layout->cfa_pattern_count; ++i) {
        switch (layout->cfa_pattern[i]) {
            case kRawColorKeyRed: ++reds; break;
            case kRawColorKeyGreen: case kRawColorKeyFujiGreen: ++greens; break;
            case kRawColorKeyBlue: ++blues; break;
            default: ++others; break;
        }
    }

    if (rw == 2 && rh == 2 && others == 0 &&
        reds == 1 && blues == 1 && greens == 2) {
        return kRawLayoutClassBayer2x2;
    }
    if (rw == 6 && rh == 6 && others == 0 &&
        greens == 20 && reds == 8 && blues == 8) {
        return kRawLayoutClassXTrans6x6;
    }
    return kRawLayoutClassOtherCfa;
}

int raw_layout_class_is_production(RawLayoutClass cls) {
    return (cls == kRawLayoutClassBayer2x2 || cls == kRawLayoutClassXTrans6x6) ? 1 : 0;
}

const char* raw_layout_class_name(RawLayoutClass cls) {
    switch (cls) {
        case kRawLayoutClassBayer2x2: return "bayer2x2";
        case kRawLayoutClassXTrans6x6: return "xtrans6x6";
        case kRawLayoutClassMonochrome: return "monochrome";
        case kRawLayoutClassLinearRgb: return "linear_rgb";
        case kRawLayoutClassLinearYCbCr: return "linear_ycbcr";
        case kRawLayoutClassOtherCfa: return "other_cfa";
        case kRawLayoutClassLayered: return "layered";
        case kRawLayoutClassMultiFrame: return "multi_frame";
        default: return "unsupported";
    }
}

int raw_bayer_phase_from_pattern(const RawLayoutDescriptor* layout,
                                 int32_t* out_red_x, int32_t* out_red_y) {
    if (!layout || !out_red_x || !out_red_y) return 0;
    if (raw_classify_layout(layout) != kRawLayoutClassBayer2x2) return 0;
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            if (layout->cfa_pattern[row * 2 + col] == kRawColorKeyRed) {
                *out_red_x = col;
                *out_red_y = row;
                return 1;
            }
        }
    }
    return 0;
}

RawErrorCode raw_validate_gpu_input(const RawGpuInput* input,
                                    char* reason_out, size_t reason_cap) {
    if (reason_out && reason_cap > 0) reason_out[0] = '\0';

    if (!input) return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                                "input is null");
    if (!input->planes || input->plane_count == 0) {
        return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                        "no planes");
    }

    const size_t sample = sampleSizeBytes(input->layout.sample_type);

    for (size_t p = 0; p < input->plane_count; ++p) {
        const RawPlaneView& v = input->planes[p];
        char msg[192];

        if (v.width == 0 || v.height == 0) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "plane width/height is zero");
        }
        if (!v.data) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "plane data pointer is null");
        }

        // ponytail: stride-sign must be checked before the overflow
        // multiplication below, not after — mulChecked() treats any negative
        // operand as "would not fit" (it can't safely take INT64_MAX/a for a
        // negative a), so a negative stride would otherwise be misreported as
        // kRawErrSizeOverflow instead of the explicit negative-stride rule.
        if (v.row_stride_bytes <= 0 || v.pixel_stride_bytes <= 0) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "stride must be positive (negative stride is not "
                            "accepted in Phase 17)");
        }

        int64_t rows_bytes = 0, cols_bytes = 0, pixels = 0;
        if (!mulChecked(v.row_stride_bytes, static_cast<int64_t>(v.height), &rows_bytes) ||
            !mulChecked(v.pixel_stride_bytes, static_cast<int64_t>(v.width), &cols_bytes) ||
            !mulChecked(static_cast<int64_t>(v.width), static_cast<int64_t>(v.height), &pixels)) {
            return failWith(reason_out, reason_cap, kRawErrSizeOverflow,
                            "plane size arithmetic overflows");
        }

        if (v.pixel_stride_bytes < static_cast<int64_t>(sample)) {
            std::snprintf(msg, sizeof(msg),
                          "pixel_stride_bytes %lld is smaller than the %zu-byte sample",
                          static_cast<long long>(v.pixel_stride_bytes), sample);
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid, msg);
        }

        const int64_t needed =
            static_cast<int64_t>(v.height - 1) * v.row_stride_bytes +
            static_cast<int64_t>(v.width - 1) * v.pixel_stride_bytes +
            static_cast<int64_t>(sample);
        if (static_cast<int64_t>(v.byte_size) < needed) {
            std::snprintf(msg, sizeof(msg),
                          "byte_size %zu does not cover the last pixel (needs %lld)",
                          v.byte_size, static_cast<long long>(needed));
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid, msg);
        }
    }

    const RawPlaneView& first = input->planes[0];
    if (!rectInside(input->active_area, first.width, first.height)) {
        return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                        "active_area is not inside the decoded extent");
    }
    if (!rectInside(input->default_crop, first.width, first.height)) {
        return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                        "default_crop is not inside the decoded extent");
    }

    if (input->layout.sample_model == kRawSampleModelCfa) {
        const size_t want = static_cast<size_t>(input->layout.cfa_repeat_width) *
                            input->layout.cfa_repeat_height;
        if (input->layout.cfa_pattern_count != want) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "cfa_pattern_count != repeat_width * repeat_height");
        }
    }

    if (input->orientation < kRawOrientationTopLeft ||
        input->orientation > kRawOrientationLeftBottom) {
        return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                        "orientation is not a known EXIF value");
    }

    const uint32_t bw = input->black.repeat_width ? input->black.repeat_width : 1;
    const uint32_t bh = input->black.repeat_height ? input->black.repeat_height : 1;
    if (static_cast<size_t>(bw) * bh > kRawMaxCfaPatternCount) {
        return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                        "black level repeat exceeds the 8x8 ceiling");
    }
    float black_max = 0.0f;
    for (uint32_t i = 0; i < bw * bh; ++i) {
        const float b = input->black.values[i];
        if (!std::isfinite(b) || b < 0.0f) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "black level is negative or not finite");
        }
        if (b > black_max) black_max = b;
    }
    for (int c = 0; c < 4; ++c) {
        const float w = input->white_level[c];
        if (!std::isfinite(w) || w <= 0.0f) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "white level is non-positive or not finite");
        }
        if (black_max >= w) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "black level is not below white level");
        }
    }

    const RawLayoutClass cls = raw_classify_layout(&input->layout);

    // Matrix presence is a metadata rule and runs before the "unsupported"
    // verdict so a malformed exotic file reports the malformation.
    if (input->camera_to_pcs.valid == 0 && cls != kRawLayoutClassMonochrome) {
        return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                        "camera colour matrix is absent and must not be guessed");
    }

    if (!raw_layout_class_is_production(cls)) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "layout class '%s' has no production kernel in Phase 17",
                      raw_layout_class_name(cls));
        return failWith(reason_out, reason_cap, kRawErrLayoutUnsupported, msg);
    }

    if (input->plane_count != 1 || input->layout.plane_count != 1 ||
        input->layout.components_per_pixel != 1 ||
        input->layout.sample_type != kRawSampleTypeU16) {
        return failWith(reason_out, reason_cap, kRawErrLayoutUnsupported,
                        "Bayer/X-Trans P0 accepts single-plane single-component "
                        "U16 mosaics only");
    }

    return kRawSuccess;
}

void raw_contract_print(const char* stage_name, const RawGpuInput* input,
                        RawErrorCode status, const char* reason, FILE* out) {
    if (!out) return;
    if (!input || !input->planes || input->plane_count == 0) {
        std::fprintf(out, "[Contract] %s -> FAIL (%s: no input)\n",
                     stage_name ? stage_name : "?", raw_error_name(status));
        return;
    }
    const RawPlaneView& v = input->planes[0];
    char cfa[16];
    if (input->layout.cfa_repeat_width && input->layout.cfa_repeat_height) {
        std::snprintf(cfa, sizeof(cfa), "%ux%u", input->layout.cfa_repeat_width,
                      input->layout.cfa_repeat_height);
    } else {
        std::snprintf(cfa, sizeof(cfa), "none");
    }

    std::fprintf(out,
                 "[Contract] %s layout=%s size=%ux%u planes=%zu comps=%u "
                 "sample=%s stride=%lld crop=%d,%d,%ux%u orient=%d cfa=%s "
                 "backend=%s -> %s",
                 stage_name ? stage_name : "?",
                 raw_layout_class_name(raw_classify_layout(&input->layout)),
                 v.width, v.height, input->plane_count,
                 input->layout.components_per_pixel,
                 input->layout.sample_type == kRawSampleTypeF32 ? "f32" : "u16",
                 static_cast<long long>(v.row_stride_bytes),
                 input->default_crop.x, input->default_crop.y,
                 input->default_crop.width, input->default_crop.height,
                 static_cast<int>(input->orientation), cfa,
                 raw_backend_name(input->decoder_backend),
                 status == kRawSuccess ? "PASS" : "FAIL");

    if (status == kRawSuccess) {
        std::fprintf(out, "\n");
    } else {
        std::fprintf(out, " (%s: %s)\n", raw_error_name(status),
                     reason ? reason : "");
    }
}

}  // extern "C"
