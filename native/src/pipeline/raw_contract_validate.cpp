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

// a+b with an explicit overflow guard; mirrors mulChecked() above. The
// per-plane "needed" byte count sums two already-bounded multiplication
// results plus the sample size — each multiplicand is individually safe
// (bounded by the row/col mulChecked() calls above), but the *sum* of two
// near-INT64_MAX products can still overflow, and that addition was
// previously unguarded (UBSan-proven, parking-lot blocker: docs/logs/2026-08-25/round-2-handoff.md).
bool addChecked(int64_t a, int64_t b, int64_t* out) {
    if (a < 0 || b < 0) return false;
    if (a > INT64_MAX - b) return false;
    *out = a + b;
    return true;
}

// Canonical Fujifilm X-Trans 6x6 CFA arrangement, 0=Red 1=Green 2=Blue.
// This is the standard tile used by LibRaw/dcraw (xtrans_abs) and matches
// the fixture already exercised by test_raw_layout_contract.cpp's
// kXTransLetters at shift (0,0). Counts (8 red / 20 green / 8 blue) alone
// are necessary but not sufficient to identify a valid X-Trans mosaic: a
// scrambled pattern can reproduce the same counts while violating the
// actual sensor arrangement. The spec (section 3.3) does not transcribe the
// exact arrangement rule, so this validator checks structural equality
// against the canonical tile's periodic family instead of re-deriving a
// row/column parity rule from scratch.
const int kCanonicalXTrans[6][6] = {
    {1, 1, 0, 1, 1, 2},
    {1, 1, 2, 1, 1, 0},
    {2, 0, 1, 0, 2, 1},
    {1, 1, 2, 1, 1, 0},
    {1, 1, 0, 1, 1, 2},
    {0, 2, 1, 2, 0, 1},
};

int xtransColorIndex(RawColorKey k) {
    switch (k) {
        case kRawColorKeyRed: return 0;
        case kRawColorKeyGreen: case kRawColorKeyFujiGreen: return 1;
        case kRawColorKeyBlue: return 2;
        default: return -1;
    }
}

// A real X-Trans sensor pattern is periodic: the 6x6 tile repeats across
// the whole sensor, so whichever 6x6 window a decoder/adapter reports is
// just some toroidal (row, col) phase shift of the same infinite mosaic.
// We accept any of the 36 phase shifts of the canonical tile above and
// reject everything else (including patterns with the correct 8/20/8
// per-color counts but a scrambled interior, which is exactly the
// parking-lot defect this closes).
bool isCanonicalXTransArrangement(const RawColorKey* pattern) {
    for (int shift_r = 0; shift_r < 6; ++shift_r) {
        for (int shift_c = 0; shift_c < 6; ++shift_c) {
            bool match = true;
            for (int r = 0; r < 6 && match; ++r) {
                for (int c = 0; c < 6; ++c) {
                    const int idx = xtransColorIndex(pattern[r * 6 + c]);
                    const int want = kCanonicalXTrans[(r + shift_r) % 6][(c + shift_c) % 6];
                    if (idx != want) { match = false; break; }
                }
            }
            if (match) return true;
        }
    }
    return false;
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

    // kRawColorKeyFujiGreen is LibRaw's FOURTH-COLOUR slot, and what that fourth
    // colour physically is depends on the tile size (round-5 finding S4):
    //   - in a 6x6 X-Trans tile it is a second green, so counting it as green is
    //     correct and is what xtransColorIndex() above already does;
    //   - in a 2x2 it is Emerald. LibRaw sets cdesc "RGBE" for the Sony DSC-F828
    //     (third_party/libraw/src/metadata/identify.cpp:2971) and the adapter maps
    //     'E' to this key (libraw_gpu_input_adapter.cpp:38). Counting it as green
    //     there scores reds=1/greens=2/blues=1, classifies a four-colour sensor as
    //     Bayer2x2, and demosaics Emerald AS Green - a silent mis-colour, which is
    //     newly reachable now that Bayer2x2 is wired to a real kernel.
    // So it is green only for the 6x6 family; anywhere else it is an "other",
    // which routes to other_cfa and then to a loud kRawErrLayoutUnsupported.
    const bool fuji_green_is_green = (rw == 6 && rh == 6);
    int reds = 0, greens = 0, blues = 0, others = 0;
    for (size_t i = 0; i < layout->cfa_pattern_count; ++i) {
        switch (layout->cfa_pattern[i]) {
            case kRawColorKeyRed: ++reds; break;
            case kRawColorKeyGreen: ++greens; break;
            case kRawColorKeyFujiGreen:
                if (fuji_green_is_green) ++greens; else ++others;
                break;
            case kRawColorKeyBlue: ++blues; break;
            default: ++others; break;
        }
    }

    if (rw == 2 && rh == 2 && others == 0 &&
        reds == 1 && blues == 1 && greens == 2) {
        return kRawLayoutClassBayer2x2;
    }
    if (rw == 6 && rh == 6 && others == 0 &&
        greens == 20 && reds == 8 && blues == 8 &&
        isCanonicalXTransArrangement(layout->cfa_pattern)) {
        return kRawLayoutClassXTrans6x6;
    }
    return kRawLayoutClassOtherCfa;
}

int raw_layout_class_is_production(RawLayoutClass cls) {
    // Phase 17: Bayer 2x2 and X-Trans 6x6. Phase 19 adds linear RGB (Foveon
    // X3F), which needs no demosaic and rides the shared Stage4 core after a
    // normalize-only pass. Every other class stays an explicit refusal.
    return (cls == kRawLayoutClassBayer2x2 ||
            cls == kRawLayoutClassXTrans6x6 ||
            cls == kRawLayoutClassLinearRgb) ? 1 : 0;
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

        // ponytail: the two multiplications below are individually bounded
        // by the mulChecked() calls above (height-1 < height, width-1 <
        // width, same non-negative multiplicands), so they cannot overflow
        // on their own. The bug was the *addition*: two products each close
        // to INT64_MAX can still overflow when summed, which was previously
        // unguarded (UBSan-proven; parking-lot blocker, round-2-handoff.md).
        // Every step here is explicitly checked, in spec section 9 priority
        // order (overflow before the metadata/coverage check below).
        int64_t rows_extent = 0, cols_extent = 0, needed = 0;
        if (!mulChecked(static_cast<int64_t>(v.height - 1), v.row_stride_bytes, &rows_extent) ||
            !mulChecked(static_cast<int64_t>(v.width - 1), v.pixel_stride_bytes, &cols_extent) ||
            !addChecked(rows_extent, cols_extent, &needed) ||
            !addChecked(needed, static_cast<int64_t>(sample), &needed)) {
            return failWith(reason_out, reason_cap, kRawErrSizeOverflow,
                            "plane byte-size computation overflows");
        }
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
        for (int c = 0; c < 4; ++c) {
            if (input->component_black[c] != 0.0f) {
                return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                                "component_black is set on a CFA layout, where "
                                "the spatial black tile is the only black term");
            }
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

    for (int c = 0; c < 4; ++c) {
        const float cb = input->component_black[c];
        if (!std::isfinite(cb) || cb < 0.0f) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "component_black is negative or not finite");
        }
        if (cb >= input->white_level[c]) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "component_black is not below white level");
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

    // Shape rules are PER CLASS. Widening the mosaic rule into a single
    // permissive test would have silently let a 3-component buffer reach the
    // Bayer kernel, which reads one sample per pixel.
    if (cls == kRawLayoutClassLinearRgb) {
        if (input->plane_count != 1 || input->layout.plane_count != 1 ||
            input->layout.components_per_pixel != 3 ||
            input->layout.sample_type != kRawSampleTypeU16 ||
            input->layout.memory_layout != kRawMemoryLayoutInterleaved) {
            return failWith(reason_out, reason_cap, kRawErrLayoutUnsupported,
                            "linear RGB accepts a single interleaved "
                            "3-component U16 plane only");
        }
        if (input->layout.cfa_pattern != NULL ||
            input->layout.cfa_pattern_count != 0) {
            return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                            "linear RGB must carry no CFA pattern");
        }
        if (input->planes[0].pixel_stride_bytes != 6) {
            return failWith(reason_out, reason_cap, kRawErrLayoutUnsupported,
                            "linear RGB pixel_stride_bytes must be 6 "
                            "(3 interleaved U16 components)");
        }
        // P19 A1 (r7 F1): the linear-RGB branch subtracts black exclusively
        // from component_black and treats the spatial tile as a 1x1 zero by
        // construction. Enforce that invariant instead of asserting it: a
        // cpp==3 file carrying a real spatial black tile (e.g. a
        // BlackLevelRepeatDim 2x2 linear DNG) would otherwise be normalised
        // with its spatial black silently un-subtracted yet return kRawSuccess.
        // Symmetric to the CFA rule above (which rejects component_black on a
        // mosaic, where the spatial tile is the only black term).
        {
            const uint32_t bw = input->black.repeat_width
                                    ? input->black.repeat_width : 1;
            const uint32_t bh = input->black.repeat_height
                                    ? input->black.repeat_height : 1;
            if (bw != 1 || bh != 1 || input->black.values[0] != 0.0f) {
                return failWith(reason_out, reason_cap, kRawErrMetadataInvalid,
                                "linear RGB carries a non-zero spatial black "
                                "tile; black must live in component_black only");
            }
        }
        return kRawSuccess;
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
