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
RawOrientation raw_orientation_from_libraw_flip(int32_t flip);

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
