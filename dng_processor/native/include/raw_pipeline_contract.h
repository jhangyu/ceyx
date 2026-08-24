#ifndef RAW_PIPELINE_CONTRACT_H_
#define RAW_PIPELINE_CONTRACT_H_

/* Plain-C anti-corruption boundary between any RAW decoder frontend and the
 * shared Halide GPU pipeline (spec section 2.5).
 *
 * HARD RULE: this header must never include or name a DNG SDK, LibRaw,
 * RawSpeed or Halide type. The moment a decoder-specific pointer appears here,
 * the architecture the spec describes has been violated.
 *
 * Error values start at -201 so they can never collide with DngErrorCode
 * (0, -1..-8, -100, -101) if both ever share one int32_t field. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RawSampleModel {
    kRawSampleModelCfa = 0,
    kRawSampleModelMonochrome = 1,
    kRawSampleModelLinearRgb = 2,
    kRawSampleModelLinearYCbCr = 3,
    kRawSampleModelLayered = 4,
    kRawSampleModelMultiFrame = 5,
    kRawSampleModelUnknown = 6
} RawSampleModel;

typedef enum RawSampleType {
    kRawSampleTypeU16 = 0,
    kRawSampleTypeF32 = 1
} RawSampleType;

typedef enum RawMemoryLayout {
    kRawMemoryLayoutPlanar = 0,
    kRawMemoryLayoutInterleaved = 1
} RawMemoryLayout;

typedef enum RawGeometry {
    kRawGeometryRectilinear = 0,
    kRawGeometryStaggered = 1,
    kRawGeometryShiftedFrames = 2
} RawGeometry;

typedef enum RawColorKey {
    kRawColorKeyRed = 0,
    kRawColorKeyGreen = 1,
    kRawColorKeyBlue = 2,
    kRawColorKeyCyan = 3,
    kRawColorKeyMagenta = 4,
    kRawColorKeyYellow = 5,
    kRawColorKeyWhite = 6,
    kRawColorKeyFujiGreen = 7,
    kRawColorKeyUnknown = 8
} RawColorKey;

/* Values are the EXIF orientation codes; 0 means "not determined". */
typedef enum RawOrientation {
    kRawOrientationUnknown = 0,
    kRawOrientationTopLeft = 1,
    kRawOrientationTopRight = 2,
    kRawOrientationBottomRight = 3,
    kRawOrientationBottomLeft = 4,
    kRawOrientationLeftTop = 5,
    kRawOrientationRightTop = 6,
    kRawOrientationRightBottom = 7,
    kRawOrientationLeftBottom = 8
} RawOrientation;

typedef enum RawDecoderBackend {
    kRawDecoderBackendUnknown = 0,
    kRawDecoderBackendDngSdk = 1,
    kRawDecoderBackendRawSpeed3 = 2,
    kRawDecoderBackendLibRawNative = 3
} RawDecoderBackend;

typedef enum RawFrontend {
    kRawFrontendUnknown = 0,
    kRawFrontendDngSdk = 1,
    kRawFrontendLibRaw = 2
} RawFrontend;

typedef enum RawGpuBackend {
    kRawGpuBackendNone = 0,
    kRawGpuBackendMetal = 1,
    kRawGpuBackendVulkan = 2
} RawGpuBackend;

typedef enum RawErrorCode {
    kRawSuccess = 0,
    kRawErrNullPath = -201,
    kRawErrProbeFailed = -202,
    kRawErrParseFailed = -203,
    kRawErrUnpackFailed = -204,
    kRawErrLayoutUnsupported = -205,
    kRawErrMetadataInvalid = -206,
    kRawErrGpuUnavailable = -207,
    kRawErrKernelFailed = -208,
    kRawErrAllocationFailed = -209,
    kRawErrSizeOverflow = -210
} RawErrorCode;

/* Read-only borrowed view. Never owns the buffer (spec section 5.1.2). */
typedef struct RawPlaneView {
    const void* data;
    size_t byte_size;
    uint32_t width;
    uint32_t height;
    int64_t row_stride_bytes;
    int64_t pixel_stride_bytes;
} RawPlaneView;

typedef struct RawRect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} RawRect;

/* DNG SDK CFA repeat ceiling (dng_sdk_limits.h); spec section 3.3.3. */
#define kRawMaxCfaRepeat 8
#define kRawMaxCfaPatternCount (kRawMaxCfaRepeat * kRawMaxCfaRepeat)

typedef struct RawLayoutDescriptor {
    RawSampleModel sample_model;
    RawSampleType sample_type;
    RawMemoryLayout memory_layout;
    RawGeometry geometry;
    uint32_t plane_count;
    uint32_t components_per_pixel;

    uint32_t cfa_repeat_width;
    uint32_t cfa_repeat_height;
    const RawColorKey* cfa_pattern; /* row-major; NULL for non-CFA */
    size_t cfa_pattern_count;
} RawLayoutDescriptor;

/* values[row * repeat_width + col]; 1x1 means a single scalar in values[0]. */
typedef struct RawBlackLevelPattern {
    uint32_t repeat_width;
    uint32_t repeat_height;
    float values[kRawMaxCfaPatternCount];
} RawBlackLevelPattern;

/* Row-major out_rows x in_cols, out_rows <= 3, in_cols <= 4.
 * valid == 0 means the camera matrix is absent; downstream must NOT invent
 * one (spec section 4.1.9). */
typedef struct RawColorTransform {
    int32_t valid;
    uint32_t out_rows;
    uint32_t in_cols;
    float m[12];
} RawColorTransform;

typedef struct RawGpuInput {
    const RawPlaneView* planes;
    size_t plane_count;
    RawLayoutDescriptor layout;

    RawRect active_area;
    RawRect default_crop;
    RawOrientation orientation;

    RawBlackLevelPattern black;
    float white_level[4];
    float as_shot_neutral[4];
    RawColorTransform camera_to_pcs;

    /* Observability only. Nothing downstream may branch on this
     * (spec section 6.5). */
    RawDecoderBackend decoder_backend;
} RawGpuInput;

typedef enum RawOutputColorSpace {
    kRawOutputColorSpaceSrgb = 0
} RawOutputColorSpace;

typedef struct RawDevelopParams {
    float exposure_ev;
    float tone_curve_strength;
    RawOutputColorSpace output_space;
    uint32_t max_output_long_edge; /* 0 = full resolution */
} RawDevelopParams;

/* Every field required by spec section 6.5. */
typedef struct RawDecodeDiagnostics {
    RawFrontend frontend;
    RawDecoderBackend unpack_backend;
    uint32_t rawspeed_flags;
    uint32_t rawspeed_warning_bits;
    RawSampleModel sample_model;
    uint32_t cfa_repeat_width;  /* 0 means "none" */
    uint32_t cfa_repeat_height;
    RawGpuBackend gpu_backend;
    double raw_unpack_ms;
    double gpu_process_ms;
    double total_ms;
    int64_t raw_repack_bytes;
} RawDecodeDiagnostics;

static inline const char* raw_error_name(RawErrorCode code) {
    switch (code) {
        case kRawSuccess: return "kRawSuccess";
        case kRawErrNullPath: return "kRawErrNullPath";
        case kRawErrProbeFailed: return "kRawErrProbeFailed";
        case kRawErrParseFailed: return "kRawErrParseFailed";
        case kRawErrUnpackFailed: return "kRawErrUnpackFailed";
        case kRawErrLayoutUnsupported: return "kRawErrLayoutUnsupported";
        case kRawErrMetadataInvalid: return "kRawErrMetadataInvalid";
        case kRawErrGpuUnavailable: return "kRawErrGpuUnavailable";
        case kRawErrKernelFailed: return "kRawErrKernelFailed";
        case kRawErrAllocationFailed: return "kRawErrAllocationFailed";
        case kRawErrSizeOverflow: return "kRawErrSizeOverflow";
        default: return "kRawErrUnknown";
    }
}

/* Spellings fixed by spec section 6.5 so log lines are machine-checkable. */
static inline const char* raw_backend_name(RawDecoderBackend backend) {
    switch (backend) {
        case kRawDecoderBackendDngSdk: return "dng_sdk";
        case kRawDecoderBackendRawSpeed3: return "rawspeed3";
        case kRawDecoderBackendLibRawNative: return "libraw_native";
        default: return "unknown";
    }
}

static inline const char* raw_frontend_name(RawFrontend frontend) {
    switch (frontend) {
        case kRawFrontendDngSdk: return "dng_sdk";
        case kRawFrontendLibRaw: return "libraw";
        default: return "unknown";
    }
}

static inline const char* raw_gpu_backend_name(RawGpuBackend backend) {
    switch (backend) {
        case kRawGpuBackendMetal: return "metal";
        case kRawGpuBackendVulkan: return "vulkan";
        default: return "none";
    }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* RAW_PIPELINE_CONTRACT_H_ */
