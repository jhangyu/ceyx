// Layout classification and RawGpuInput invariant coverage.
//
// Why table-driven: the P1-P3 layouts (mono, linear RGB, YCbCr, quad Bayer,
// Foveon, pixel-shift) ship in Phase 17 as *descriptor + error routing only*.
// The only thing that proves that promise is a case per layout asserting the
// class and that raw_validate_gpu_input returns kRawErrLayoutUnsupported.
#include <cstdio>
#include <cstring>

#include "raw_contract_validate.h"
#include "raw_pipeline_contract.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    std::printf("[RawLayout] %s %s -> %s\n", name, detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

// --- pattern builders -------------------------------------------------------

// Bayer phase -> 2x2 keys, row-major. RGGB=(0,0) GRBG=(1,0) GBRG=(0,1) BGGR=(1,1)
void makeBayer(RawColorKey out[4], int red_x, int red_y) {
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 2; ++col) {
            RawColorKey key = kRawColorKeyGreen;
            if (row == red_y && col == red_x) key = kRawColorKeyRed;
            else if (row != red_y && col != red_x) key = kRawColorKeyBlue;
            out[row * 2 + col] = key;
        }
    }
}

// Canonical Fujifilm X-Trans 6x6: 20 green, 8 red, 8 blue.
// ponytail: plan doc's literal letter string undercounted R/B (6/7 instead
// of 8/8); replaced with the standard Fuji X-Trans tile, verified below.
const char kXTransLetters[37] = "GGRGGBGGBGGRBRGRBGGGBGGRGGRGGBRBGBRG";

void makeXTrans(RawColorKey out[36], int shift_x, int shift_y) {
    for (int row = 0; row < 6; ++row) {
        for (int col = 0; col < 6; ++col) {
            const char c = kXTransLetters[((row + shift_y) % 6) * 6 +
                                          ((col + shift_x) % 6)];
            out[row * 6 + col] = (c == 'R') ? kRawColorKeyRed
                               : (c == 'B') ? kRawColorKeyBlue
                                            : kRawColorKeyGreen;
        }
    }
}

// --- fixture ---------------------------------------------------------------

struct Fixture {
    RawPlaneView plane{};
    RawColorKey pattern[kRawMaxCfaPatternCount]{};
    RawGpuInput input{};
    static constexpr uint32_t kW = 64;
    static constexpr uint32_t kH = 48;
    static constexpr int64_t kRowStride = kW * 2;

    // A valid single-plane U16 Bayer RGGB input; individual cases then break
    // exactly one field, so a failure names one invariant rather than many.
    Fixture() {
        plane.data = storage;
        plane.byte_size = sizeof(storage);
        plane.width = kW;
        plane.height = kH;
        plane.row_stride_bytes = kRowStride;
        plane.pixel_stride_bytes = 2;

        makeBayer(pattern, 0, 0);

        input.planes = &plane;
        input.plane_count = 1;
        input.layout.sample_model = kRawSampleModelCfa;
        input.layout.sample_type = kRawSampleTypeU16;
        input.layout.memory_layout = kRawMemoryLayoutInterleaved;
        input.layout.geometry = kRawGeometryRectilinear;
        input.layout.plane_count = 1;
        input.layout.components_per_pixel = 1;
        input.layout.cfa_repeat_width = 2;
        input.layout.cfa_repeat_height = 2;
        input.layout.cfa_pattern = pattern;
        input.layout.cfa_pattern_count = 4;
        input.active_area = RawRect{0, 0, kW, kH};
        input.default_crop = RawRect{0, 0, kW, kH};
        input.orientation = kRawOrientationTopLeft;
        input.black.repeat_width = 1;
        input.black.repeat_height = 1;
        input.black.values[0] = 512.0f;
        for (int i = 0; i < 4; ++i) {
            input.white_level[i] = 16383.0f;
            input.as_shot_neutral[i] = 1.0f;
        }
        input.camera_to_pcs.valid = 1;
        input.camera_to_pcs.out_rows = 3;
        input.camera_to_pcs.in_cols = 3;
        for (int i = 0; i < 9; ++i) input.camera_to_pcs.m[i] = (i % 4 == 0) ? 1.0f : 0.0f;
        input.decoder_backend = kRawDecoderBackendRawSpeed3;
    }

    uint16_t storage[kW * kH]{};
};

void expectClass(const char* name, const RawLayoutDescriptor& layout,
                 RawLayoutClass want) {
    const RawLayoutClass got = raw_classify_layout(&layout);
    char detail[128];
    std::snprintf(detail, sizeof(detail), "class=%s want=%s",
                  raw_layout_class_name(got), raw_layout_class_name(want));
    report(name, got == want, detail);
}

void expectValidate(const char* name, const RawGpuInput& input,
                    RawErrorCode want) {
    char reason[256] = {0};
    const RawErrorCode got = raw_validate_gpu_input(&input, reason, sizeof(reason));
    char detail[512];
    std::snprintf(detail, sizeof(detail), "error=%s want=%s reason=\"%s\"",
                  raw_error_name(got), raw_error_name(want), reason);
    report(name, got == want, detail);
}

}  // namespace

int main() {
    // ---- classification ----------------------------------------------------
    {
        Fixture f;
        const struct { const char* name; int rx; int ry; } phases[4] = {
            {"bayer_rggb", 0, 0}, {"bayer_grbg", 1, 0},
            {"bayer_gbrg", 0, 1}, {"bayer_bggr", 1, 1}};
        for (const auto& p : phases) {
            makeBayer(f.pattern, p.rx, p.ry);
            expectClass(p.name, f.input.layout, kRawLayoutClassBayer2x2);
            int32_t rx = -1, ry = -1;
            char detail[96];
            const int ok = raw_bayer_phase_from_pattern(&f.input.layout, &rx, &ry);
            std::snprintf(detail, sizeof(detail), "phase=(%d,%d) want=(%d,%d)",
                          rx, ry, p.rx, p.ry);
            report("bayer_phase", ok == 1 && rx == p.rx && ry == p.ry, detail);
        }
    }
    {
        Fixture f;
        makeXTrans(f.pattern, 0, 0);
        f.input.layout.cfa_repeat_width = 6;
        f.input.layout.cfa_repeat_height = 6;
        f.input.layout.cfa_pattern_count = 36;
        expectClass("xtrans_6x6", f.input.layout, kRawLayoutClassXTrans6x6);
    }
    {
        Fixture f;
        f.input.layout.sample_model = kRawSampleModelMonochrome;
        f.input.layout.cfa_pattern = nullptr;
        f.input.layout.cfa_pattern_count = 0;
        expectClass("monochrome", f.input.layout, kRawLayoutClassMonochrome);
        expectValidate("monochrome_unsupported", f.input, kRawErrLayoutUnsupported);
    }
    {
        Fixture f;
        f.input.layout.sample_model = kRawSampleModelLinearRgb;
        f.input.layout.components_per_pixel = 3;
        f.input.layout.cfa_pattern = nullptr;
        f.input.layout.cfa_pattern_count = 0;
        expectClass("linear_rgb", f.input.layout, kRawLayoutClassLinearRgb);
        expectValidate("linear_rgb_unsupported", f.input, kRawErrLayoutUnsupported);
    }
    {
        Fixture f;
        f.input.layout.sample_model = kRawSampleModelLinearYCbCr;
        f.input.layout.components_per_pixel = 3;
        f.input.layout.cfa_pattern = nullptr;
        f.input.layout.cfa_pattern_count = 0;
        expectClass("linear_ycbcr", f.input.layout, kRawLayoutClassLinearYCbCr);
        expectValidate("linear_ycbcr_unsupported", f.input, kRawErrLayoutUnsupported);
    }
    {
        // Quad Bayer: 4x4 of RGGB-like quads. Must NOT be mistaken for Bayer.
        Fixture f;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) {
                const int qr = row / 2, qc = col / 2;
                f.pattern[row * 4 + col] = (qr == 0 && qc == 0) ? kRawColorKeyRed
                                         : (qr == 1 && qc == 1) ? kRawColorKeyBlue
                                                                : kRawColorKeyGreen;
            }
        f.input.layout.cfa_repeat_width = 4;
        f.input.layout.cfa_repeat_height = 4;
        f.input.layout.cfa_pattern_count = 16;
        expectClass("quad_bayer_4x4", f.input.layout, kRawLayoutClassOtherCfa);
        expectValidate("quad_bayer_unsupported", f.input, kRawErrLayoutUnsupported);
    }
    {
        Fixture f;
        f.pattern[0] = kRawColorKeyRed;   f.pattern[1] = kRawColorKeyGreen;
        f.pattern[2] = kRawColorKeyBlue;  f.pattern[3] = kRawColorKeyWhite;
        expectClass("rgbw_2x2", f.input.layout, kRawLayoutClassOtherCfa);
    }
    {
        Fixture f;
        f.pattern[0] = kRawColorKeyCyan;    f.pattern[1] = kRawColorKeyMagenta;
        f.pattern[2] = kRawColorKeyYellow;  f.pattern[3] = kRawColorKeyGreen;
        expectClass("cmy_2x2", f.input.layout, kRawLayoutClassOtherCfa);
    }
    {
        Fixture f;
        f.input.layout.sample_model = kRawSampleModelLayered;
        f.input.layout.cfa_pattern = nullptr;
        f.input.layout.cfa_pattern_count = 0;
        expectClass("layered_foveon", f.input.layout, kRawLayoutClassLayered);
        expectValidate("layered_unsupported", f.input, kRawErrLayoutUnsupported);
    }
    {
        Fixture f;
        f.input.layout.sample_model = kRawSampleModelMultiFrame;
        f.input.layout.cfa_pattern = nullptr;
        f.input.layout.cfa_pattern_count = 0;
        expectClass("multiframe_pixelshift", f.input.layout, kRawLayoutClassMultiFrame);
        expectValidate("multiframe_unsupported", f.input, kRawErrLayoutUnsupported);
    }
    {
        Fixture f;
        f.input.layout.cfa_repeat_width = 9;
        f.input.layout.cfa_repeat_height = 9;
        f.input.layout.cfa_pattern_count = 81;
        expectClass("cfa_9x9_oversize", f.input.layout, kRawLayoutClassUnsupported);
    }
    {
        // An unknown key must never be coerced to Green (spec section 3.3.5).
        Fixture f;
        f.pattern[1] = kRawColorKeyUnknown;
        expectClass("cfa_unknown_key", f.input.layout, kRawLayoutClassUnsupported);
        int32_t rx = 0, ry = 0;
        report("cfa_unknown_key_no_phase",
               raw_bayer_phase_from_pattern(&f.input.layout, &rx, &ry) == 0,
               "phase_rejected");
    }
    {
        Fixture f;
        f.input.layout.sample_model = kRawSampleModelUnknown;
        expectClass("sample_model_unknown", f.input.layout, kRawLayoutClassUnsupported);
    }

    // ---- validation --------------------------------------------------------
    { Fixture f; f.plane.width = 0;
      expectValidate("zero_width", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.plane.row_stride_bytes = -128;
      expectValidate("negative_row_stride", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.plane.pixel_stride_bytes = -2;
      expectValidate("negative_pixel_stride", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.plane.pixel_stride_bytes = 1;
      expectValidate("stride_smaller_than_sample", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.plane.byte_size -= 1;
      expectValidate("byte_size_short_by_one", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.plane.row_stride_bytes = INT64_MAX / 2; f.plane.height = 8;
      expectValidate("size_overflow_row_stride", f.input, kRawErrSizeOverflow); }
    { Fixture f; f.input.default_crop = RawRect{0, 0, Fixture::kW + 1, Fixture::kH};
      expectValidate("crop_outside_extent", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.input.active_area = RawRect{-1, 0, Fixture::kW, Fixture::kH};
      expectValidate("active_area_outside_extent", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.input.layout.cfa_pattern_count = 3;
      expectValidate("cfa_count_mismatch", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.input.orientation = kRawOrientationUnknown;
      expectValidate("orientation_zero", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.input.orientation = static_cast<RawOrientation>(9);
      expectValidate("orientation_nine", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.input.black.values[0] = 20000.0f;
      expectValidate("black_ge_white", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.input.camera_to_pcs.valid = 0;
      expectValidate("matrix_absent", f.input, kRawErrMetadataInvalid); }
    { Fixture f; f.input.layout.plane_count = 3;
      expectValidate("bayer_three_planes", f.input, kRawErrLayoutUnsupported); }
    { Fixture f; f.input.layout.sample_type = kRawSampleTypeF32;
      // ponytail: F32 doubles the required sample size (4 bytes); the shared
      // fixture's default u16 stride/byte_size would trip the stride/coverage
      // metadata rules first (rules 4-5) instead of exercising the intended
      // "wrong sample type" layout-unsupported rule (rule 11). Widen stride
      // and byte_size to keep this case isolated to sample_type only.
      f.plane.pixel_stride_bytes = 4;
      f.plane.row_stride_bytes = static_cast<int64_t>(Fixture::kW) * 4;
      f.plane.byte_size = static_cast<size_t>(f.plane.row_stride_bytes) * f.plane.height;
      expectValidate("bayer_f32_sample", f.input, kRawErrLayoutUnsupported); }
    { Fixture f; makeXTrans(f.pattern, 0, 0);
      f.input.layout.cfa_repeat_width = 6; f.input.layout.cfa_repeat_height = 6;
      f.input.layout.cfa_pattern_count = 36; f.input.layout.components_per_pixel = 2;
      expectValidate("xtrans_two_components", f.input, kRawErrLayoutUnsupported); }

    // ---- the [Contract] line itself ---------------------------------------
    {
        Fixture f;
        char reason[256] = {0};
        const RawErrorCode rc = raw_validate_gpu_input(&f.input, reason, sizeof(reason));
        raw_contract_print("RawInput", &f.input, rc, reason, stdout);
        report("contract_line_valid_input", rc == kRawSuccess, "printed");
    }

    if (failures != 0) {
        std::printf("[RawLayout] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawLayout] ALL PASS\n");
    return 0;
}
