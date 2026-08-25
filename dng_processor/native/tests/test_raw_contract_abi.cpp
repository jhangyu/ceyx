// Pins the plain-C contract: enum values, struct sizes, and the name helpers.
// These values are consumed by every later task, so a silent renumbering here
// is a cross-task bug. Output contract: one line per case, final
// "[RawContractABI] ALL PASS"; exit 0 only when every case passed.
#include <cstdio>
#include <cstring>

#include "raw_pipeline_contract.h"

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

    // P19: the contract version is bumped whenever the plain-C surface changes.
    // Phase 17 shipped version 1 implicitly; Phase 19 adds the linear-RGB
    // acceptance and RawGpuInput::component_black, so the value is 2.
    check("contract_version", kRawContractVersion == 2);

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
