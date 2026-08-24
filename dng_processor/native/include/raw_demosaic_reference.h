#ifndef RAW_DEMOSAIC_REFERENCE_H_
#define RAW_DEMOSAIC_REFERENCE_H_

// Deterministic scalar references for the generic RAW demosaic kernels, plus
// the host-side AOT wrappers. The reference implements the IDENTICAL arithmetic
// as the Halide expression (same rounding constants), so a kernel-vs-reference
// difference is a codegen effect, not an algorithm comparison - which is what
// makes the >=99 dB / max_abs<=1 gate meaningful (spec section 11.2.1).

#include <cstdint>

// Interleaved RGB16 output, width*height*3 elements.
void raw_bayer_demosaic_reference(const uint16_t* src, uint32_t width,
                                  uint32_t height, int64_t row_stride_bytes,
                                  int32_t red_x, int32_t red_y,
                                  const float* black_tile,
                                  uint32_t black_repeat_width,
                                  uint32_t black_repeat_height,
                                  float inv_range, uint16_t* dst);

// Returns 1 on success, 0 on kernel failure.
int raw_bayer_demosaic_aot(const uint16_t* src, uint32_t width, uint32_t height,
                           int64_t row_stride_bytes, int32_t red_x, int32_t red_y,
                           const float* black_tile, uint32_t black_repeat_width,
                           uint32_t black_repeat_height, float inv_range,
                           uint16_t* dst);

#endif  // RAW_DEMOSAIC_REFERENCE_H_
