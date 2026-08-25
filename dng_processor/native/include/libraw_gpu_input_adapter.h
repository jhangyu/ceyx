#ifndef LIBRAW_GPU_INPUT_ADAPTER_H_
#define LIBRAW_GPU_INPUT_ADAPTER_H_

// The ONLY LibRaw -> RawGpuInput adapter in the tree (spec section 2.3.7).
//
// Field mapping, enum normalization and contract validation. Explicitly not
// allowed here: a second camera lookup, demosaic, tone, gamma, or a full-frame
// rotation (spec section 6.4.4). No branch on backend or camera vendor.

#include "libraw_frontend.h"
#include "raw_pipeline_contract.h"

// Maps a LibRaw CFA colour index through idata.cdesc ("RGBG", "GMCY", ...).
// An unrecognised descriptor character maps to kRawColorKeyUnknown - never to
// Green, because a coerced key silently mis-colours the image
// (spec section 3.3.5).
RawColorKey raw_color_key_from_libraw(uint32_t libraw_index, uint32_t colors,
                                      const char* cdesc);

// LibRaw sizes.flip -> EXIF orientation. Unknown values map to
// kRawOrientationUnknown, which the validator then rejects (spec section 4.1.8).
//
// flip is a dcraw bit-field, NOT an EXIF code: bit 2 transposes, bit 1 mirrors
// vertically, bit 0 mirrors horizontally. The inverse of LibRaw's own EXIF->flip
// table, third_party/libraw/src/metadata/tiff.cpp:631
// (`t_flip = "50132467"[get2() & 7] - '0'`, get2() being EXIF tag 0x0112), is
// therefore the authority here:
//   flip 0->EXIF 1  flip 1->EXIF 2  flip 2->EXIF 4  flip 3->EXIF 3
//   flip 4->EXIF 5  flip 5->EXIF 8  flip 6->EXIF 6  flip 7->EXIF 7
// Corroborated by third_party/libraw/src/metadata/identify.cpp:1294-1306
// (270 deg -> flip 5, 180 -> 3, 90 -> 6) and by composing the bit semantics.
// NOTE: this contradicts plan Task 7, which specifies flip 5 -> EXIF 5; that is
// a plan defect (round-4 finding F-R4-01), upheld deviation.
RawOrientation raw_orientation_from_libraw_flip(int32_t flip);

// Folds LibRaw's THREE black-level terms into one repeating tile.
//
// After open_file() + unpack() LibRaw has not run adjust_bl(), so the effective
// black at a site is
//     color.black
//   + color.cblack[FC(row, col)]                                  <- per channel
//   + color.cblack[6 + (row % cblack[4]) * cblack[5] + col % cblack[5]]
// exactly as LibRaw subtracts it in
// third_party/libraw/src/preprocessing/subtract_black.cpp:31-51. adjust_bl()
// (third_party/libraw/src/utils/utils_libraw.cpp:468-549), which is what folds
// color.black into cblack[0..3], is reachable only from subtract_black.cpp:20,
// dcraw_process.cpp:85 and raw2image.cpp:428 - none of which the frontend calls.
// Dropping the middle term produces a per-channel colour cast (round-4 finding
// F-R4-02).
//
// Declared here rather than kept static because no corpus file has a nonzero
// cblack[0..3], so a synthetic unit test is the only coverage available (same
// reasoning as raw_frontend_pixels_live_in_raw_image).
//
// channel_index is the per-site LibRaw colour index tile (what FC returns),
// cfa_w x cfa_h row-major; null/zero dims means "no CFA" and the per-channel
// term is skipped. channel_black is cblack[0..3]. spatial_black is &cblack[6]
// with spatial_w == cblack[5] and spatial_h == cblack[4], row-major.
//
// The emitted tile is the element-wise sum over lcm(cfa, spatial) dimensions.
// If that lcm exceeds the contract's 8x8 ceiling the function FAILS with
// kRawErrLayoutUnsupported rather than truncating to a wrong tile.
RawErrorCode raw_black_pattern_from_libraw(uint32_t black_scalar,
                                           const uint32_t* channel_black,
                                           const uint8_t* channel_index,
                                           uint32_t cfa_w, uint32_t cfa_h,
                                           const uint32_t* spatial_black,
                                           uint32_t spatial_w, uint32_t spatial_h,
                                           RawBlackLevelPattern* out,
                                           char* reason_out, size_t reason_cap);

// Row-major 3x3 inverse. Returns false (leaving out9 untouched) when any input
// is non-finite or the matrix is not invertible. Used to turn LibRaw's
// camera-from-XYZ cam_xyz into the camera-to-PCS direction the contract field
// names and Task 8 consumes - see docs/logs/2026-08-25/r5-camera-to-pcs-ruling.md.
bool raw_invert_3x3(const float in9[9], float out9[9]);

class LibRawGpuInputAdapter {
 public:
    LibRawGpuInputAdapter() = default;
    ~LibRawGpuInputAdapter() = default;
    LibRawGpuInputAdapter(const LibRawGpuInputAdapter&) = delete;
    LibRawGpuInputAdapter& operator=(const LibRawGpuInputAdapter&) = delete;

    // Fills *out_input / *out_develop and returns the result of
    // raw_validate_gpu_input, so no caller can skip validation.
    //
    // Ownership: this object owns the plane array and CFA pattern array that
    // *out_input points at. *this must outlive *out_input.
    RawErrorCode build(const LibRawFrontendContext& ctx,
                       RawGpuInput* out_input,
                       RawDevelopParams* out_develop,
                       char* reason_out,
                       size_t reason_cap);

 private:
    RawPlaneView planes_[1]{};
    RawColorKey cfa_pattern_[kRawMaxCfaPatternCount]{};
};

#endif  // LIBRAW_GPU_INPUT_ADAPTER_H_
