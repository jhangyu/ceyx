// Pins the plain-C contract: enum values, struct sizes, and the name helpers.
// These values are consumed by every later task, so a silent renumbering here
// is a cross-task bug. Output contract: one line per case, final
// "[RawContractABI] ALL PASS"; exit 0 only when every case passed.
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "raw_pipeline_contract.h"

// P19 (round-5 review F-R5-02): "offsets are pinned by this file" was an
// aspirational comment, not an in-repo guard -- grep -c offsetof was 0. These
// static_asserts are the guard: every existing RawGpuInput member offset is
// pinned so a future mid-struct insertion (as opposed to the append-only rule
// contract version 2 relies on) fails to COMPILE, not just to match an
// out-of-band dump. Values derived from the struct's own field order and
// natural (LP64, no packing pragma) alignment, then cross-checked against the
// reviewer's independent clang -fdump-record-layouts-complete dump
// (scripts/tmp/p19/r5_review.md): both agree on every value below.
static_assert(offsetof(RawGpuInput, planes) == 0, "planes offset moved");
static_assert(offsetof(RawGpuInput, plane_count) == 8, "plane_count offset moved");
static_assert(offsetof(RawGpuInput, layout) == 16, "layout offset moved");
static_assert(offsetof(RawGpuInput, active_area) == 64, "active_area offset moved");
static_assert(offsetof(RawGpuInput, default_crop) == 80, "default_crop offset moved");
static_assert(offsetof(RawGpuInput, orientation) == 96, "orientation offset moved");
static_assert(offsetof(RawGpuInput, black) == 100, "black offset moved");
static_assert(offsetof(RawGpuInput, white_level) == 364, "white_level offset moved");
static_assert(offsetof(RawGpuInput, as_shot_neutral) == 380,
             "as_shot_neutral offset moved");
static_assert(offsetof(RawGpuInput, camera_to_pcs) == 396,
             "camera_to_pcs offset moved");
static_assert(offsetof(RawGpuInput, decoder_backend) == 456,
             "decoder_backend offset moved");
// P19: appended at the end, contract version 2 (raw_pipeline_contract.h).
static_assert(offsetof(RawGpuInput, component_black) == 460,
             "component_black offset moved - must stay APPENDED, never inserted");
static_assert(sizeof(RawGpuInput) == 480, "RawGpuInput size changed");

// Round 1 Task 1.3, contract version 3: RawDevelopParams gains
// auto_exposure_mode/auto_exposure_ev, appended at the end. Same guard
// pattern as RawGpuInput above -- offsets pinned so a future mid-struct
// insertion fails to compile.
static_assert(offsetof(RawDevelopParams, exposure_ev) == 0, "exposure_ev offset moved");
static_assert(offsetof(RawDevelopParams, tone_curve_strength) == 4,
             "tone_curve_strength offset moved");
static_assert(offsetof(RawDevelopParams, output_space) == 8, "output_space offset moved");
static_assert(offsetof(RawDevelopParams, max_output_long_edge) == 12,
             "max_output_long_edge offset moved");
static_assert(offsetof(RawDevelopParams, auto_exposure_mode) == 16,
             "auto_exposure_mode offset moved - must stay APPENDED, never inserted");
static_assert(offsetof(RawDevelopParams, auto_exposure_ev) == 20,
             "auto_exposure_ev offset moved - must stay APPENDED, never inserted");
// Round 2 Task 2.3, contract version 4: RawDevelopParams gains shadows,
// appended at the end. Same guard pattern.
static_assert(offsetof(RawDevelopParams, shadows) == 24,
             "shadows offset moved - must stay APPENDED, never inserted");
static_assert(sizeof(RawDevelopParams) == 28, "RawDevelopParams size changed");

namespace {

int failures = 0;

void check(const char* name, bool ok) {
    std::printf("[RawContractABI] %s -> %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

}  // namespace

int main() {
    check("error_values",
          kRawSuccess == 0 && kRawErrNullPath == -201 &&
          kRawErrProbeFailed == -202 && kRawErrParseFailed == -203 &&
          kRawErrUnpackFailed == -204 && kRawErrLayoutUnsupported == -205 &&
          kRawErrMetadataInvalid == -206 && kRawErrGpuUnavailable == -207 &&
          kRawErrKernelFailed == -208 && kRawErrAllocationFailed == -209 &&
          kRawErrSizeOverflow == -210 && kRawErrCancelled == -211);

    check("sample_model_values",
          kRawSampleModelCfa == 0 && kRawSampleModelMonochrome == 1 &&
          kRawSampleModelLinearRgb == 2 && kRawSampleModelLinearYCbCr == 3 &&
          kRawSampleModelLayered == 4 && kRawSampleModelMultiFrame == 5 &&
          kRawSampleModelUnknown == 6);

    check("color_key_values",
          kRawColorKeyRed == 0 && kRawColorKeyGreen == 1 &&
          kRawColorKeyBlue == 2 && kRawColorKeyCyan == 3 &&
          kRawColorKeyMagenta == 4 && kRawColorKeyYellow == 5 &&
          kRawColorKeyWhite == 6 && kRawColorKeyFujiGreen == 7 &&
          kRawColorKeyUnknown == 8);

    // EXIF orientation codes, so the value can be written straight to metadata.
    check("orientation_is_exif",
          kRawOrientationUnknown == 0 && kRawOrientationTopLeft == 1 &&
          kRawOrientationBottomRight == 3 && kRawOrientationLeftTop == 5 &&
          kRawOrientationRightTop == 6 && kRawOrientationLeftBottom == 8);

    check("cfa_limits",
          kRawMaxCfaRepeat == 8 &&
          kRawMaxCfaPatternCount == kRawMaxCfaRepeat * kRawMaxCfaRepeat);

    check("backend_names",
          std::strcmp(raw_backend_name(kRawDecoderBackendDngSdk), "dng_sdk") == 0 &&
          std::strcmp(raw_backend_name(kRawDecoderBackendRawSpeed3), "rawspeed3") == 0 &&
          std::strcmp(raw_backend_name(kRawDecoderBackendLibRawNative), "libraw_native") == 0 &&
          std::strcmp(raw_backend_name(kRawDecoderBackendUnknown), "unknown") == 0);

    check("frontend_names",
          std::strcmp(raw_frontend_name(kRawFrontendDngSdk), "dng_sdk") == 0 &&
          std::strcmp(raw_frontend_name(kRawFrontendLibRaw), "libraw") == 0);

    check("gpu_backend_names",
          std::strcmp(raw_gpu_backend_name(kRawGpuBackendMetal), "metal") == 0 &&
          std::strcmp(raw_gpu_backend_name(kRawGpuBackendVulkan), "vulkan") == 0 &&
          std::strcmp(raw_gpu_backend_name(kRawGpuBackendNone), "none") == 0);

    check("error_names",
          std::strcmp(raw_error_name(kRawErrLayoutUnsupported),
                      "kRawErrLayoutUnsupported") == 0 &&
          std::strcmp(raw_error_name(kRawSuccess), "kRawSuccess") == 0 &&
          std::strcmp(raw_error_name(kRawErrCancelled), "kRawErrCancelled") == 0 &&
          std::strcmp(raw_error_name(static_cast<RawErrorCode>(-999)),
                      "kRawErrUnknown") == 0);

    // The struct must stay POD so it can cross the C boundary unchanged.
    RawGpuInput input;
    std::memset(&input, 0, sizeof(input));
    check("gpu_input_is_zeroable",
          input.plane_count == 0 && input.planes == nullptr &&
          input.layout.cfa_pattern == nullptr &&
          input.camera_to_pcs.valid == 0);

    RawBlackLevelPattern black;
    check("black_pattern_capacity",
          sizeof(black.values) / sizeof(black.values[0]) == kRawMaxCfaPatternCount);

    RawColorTransform xform;
    check("color_transform_capacity",
          sizeof(xform.m) / sizeof(xform.m[0]) == 12);

    // P19/Round 1/Round 2: the contract version is bumped whenever the plain-C
    // surface changes. Phase 17 shipped version 1 implicitly; Phase 19 added
    // the linear-RGB acceptance and RawGpuInput::component_black (2); Round 1
    // Task 1.3 adds RawDevelopParams::auto_exposure_mode/auto_exposure_ev (3);
    // Round 2 Task 2.3 adds RawDevelopParams::shadows (4).
    check("contract_version", kRawContractVersion == 4);

    RawDevelopParams dev;
    std::memset(&dev, 0, sizeof(dev));
    check("auto_exposure_mode_default_is_on", kRawAutoExposureOn == 0);
    check("auto_exposure_mode_off_value", kRawAutoExposureOff == 1);
    check("develop_params_is_zeroable_to_auto_exposure_on",
          dev.auto_exposure_mode == kRawAutoExposureOn &&
          dev.auto_exposure_ev == 0.0f);
    // memset(0) bypasses the C++ default member initialiser (there is no
    // portable way for a POD field to survive a raw zero-fill), so this is
    // 0.0f here -- documented above the field in raw_pipeline_contract.h.
    // The 5.0f default is exercised by aggregate-init, checked next.
    check("develop_params_shadows_is_zeroable", dev.shadows == 0.0f);

    RawDevelopParams dev_aggregate_init{};
    check("develop_params_shadows_default_is_5",
          dev_aggregate_init.shadows == 5.0f);

    RawGpuInput cb;
    std::memset(&cb, 0, sizeof(cb));
    check("component_black_capacity",
          sizeof(cb.component_black) / sizeof(cb.component_black[0]) == 4 &&
          cb.component_black[0] == 0.0f && cb.component_black[3] == 0.0f);

    if (failures != 0) {
        std::printf("[RawContractABI] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawContractABI] ALL PASS\n");
    return 0;
}
