#pragma once

// Native EXIF orientation pass (native-rotation spec, Task 1, Design C).
//
// Pure 8-case EXIF orientation transform over an RGBA8 buffer. This TU has
// NO dependency on Halide, the DNG SDK, LibRaw, or any other ceyx header, so
// it compiles and links in milliseconds standalone and is reusable as both
// the CPU fallback path (ceyx_decode_into_buffer_oriented, Task 2) and the
// GPU-strided design's CPU reference oracle if that design is ever adopted.
//
// Semantics mirror Halcyon's lib/services/image_pipeline/exif_orientation.dart
// table exactly: quarter-turns-clockwise are applied FIRST, then a horizontal
// mirror. Values outside 1..8 are treated as 1 (identity) -- an unrecognised
// tag is not a reason to refuse to show the photo.
//
//   EXIF | quarterTurnsCw | mirrored | transposes W/H?
//   1    | 0              | false    | no   (no-op)
//   2    | 0              | true     | no   (in-place, per-row reverse)
//   3    | 2              | false    | no   (in-place, whole-buffer reverse)
//   4    | 2              | true     | no   (in-place, row swap top<->bottom)
//   5    | 1              | true     | yes  (scratch -> dst, tiled transpose)
//   6    | 1              | false    | yes  (scratch -> dst, tiled transpose)
//   7    | 3              | true     | yes  (scratch -> dst, tiled transpose)
//   8    | 3              | false    | yes  (scratch -> dst, tiled transpose)

#include <cstdint>
#include <cstddef>

#if defined(_WIN32)
#define CEYX_FFI_EXPORT __declspec(dllexport)
#else
#define CEYX_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Error codes for the orientation layer. Deliberately disjoint from every
/// other ceyx error scale (DNG: 0..-101, RAW: -201..-212, decode-into:
/// -301..-303), so a caller can tell which layer spoke without knowing which
/// route the file took.
enum CeyxOrientError {
  kCeyxOrientErrBadArgs  = -401,
  kCeyxOrientErrOverlap  = -402,
  // Productionization plan section 1.6 (Task 2). The GPU Stage4 kernel returned
  // non-zero, or copy_to_host() returned non-zero, on an oriented dispatch.
  // Outlives ceyx_orient_rgba: the enum stays after the CPU pass is deleted.
  kCeyxOrientErrKernel   = -403,
};

/// Applies EXIF orientation 1..8 to an RGBA8 frame of width x height pixels.
///
/// src == dst is legal ONLY when the orientation does not transpose
/// (1, 2, 3, 4); for 5..8 the buffers MUST NOT overlap at all, and overlap is
/// refused with kCeyxOrientErrOverlap BEFORE any write happens.
///
/// dst_capacity must be >= width*height*4 (the ORIENTED byte count, which for
/// every orientation equals the unoriented byte count since transposition
/// only swaps width and height). null src/dst, non-positive width/height, or
/// an undersized dst_capacity return kCeyxOrientErrBadArgs, also BEFORE any
/// write.
///
/// out_width/out_height receive the ORIENTED extent (swapped for 5..8) on
/// every success, and are zeroed on every failure (when non-null).
///
/// Returns 0 on success, or one of the CeyxOrientError values above.
CEYX_FFI_EXPORT int32_t ceyx_orient_rgba(const uint8_t *src, uint8_t *dst,
                                         size_t dst_capacity, int32_t width,
                                         int32_t height,
                                         int32_t exif_orientation,
                                         int32_t *out_width,
                                         int32_t *out_height);

/// True (1) when exif_orientation swaps width and height (5, 6, 7, 8);
/// false (0) for every other int, including values outside 1..8.
CEYX_FFI_EXPORT int32_t ceyx_orientation_transposes(int32_t exif_orientation);

#ifdef __cplusplus
} // extern "C"

// R-1 consolidation (Round 2 parking-lot): header-only helper mirroring the
// same 8-way EXIF transpose predicate as ceyx_orientation_transposes() above.
// Deliberately NOT a call to that function: ceyx_orientation_transposes is
// defined in ceyx_orient.cpp, which Phase 4 deletes; this header outlives it,
// so pipeline call sites route through this inline instead of the doomed TU.
// Behavior must stay identical to `o >= 5 && o <= 8`.
static inline bool ceyx_orientation_transposes_inline(int32_t exif_orientation) {
  return exif_orientation >= 5 && exif_orientation <= 8;
}

// T7b: affine coefficients for the fused GPU Stage4 orientation permutation.
//
// The Stage4 kernels no longer decide anything about orientation. They receive
// six int32 scalars and evaluate only
//
//     ux = a_x*x + b_x*y + c_x
//     uy = a_y*x + b_y*y + c_y
//
// where (x, y) is the ORIENTED output coordinate and (ux, uy) the UNORIENTED
// source coordinate. This function is the whole decision, and it runs on the
// host in plain C++.
//
// Why the decision moved here: two in-kernel formulations of the same
// permutation mis-lowered on Adreno 750 / Vulkan while their CPU controls were
// 8/8 correct -- F-T6-1 (8-way `select` equality chain; CSE collapse across
// duplicate arm values) and F-T8-1 (branch-free flags; every orientation with
// >= 1 flag set behaved as if more were set, consistent with
// cast<int32_t>(bool) lowering to selects internally). See
// docs/logs/2026-09-07/Task_t6_android_device_gate.md and
// Task_t8_android_device_gate.md. Host arithmetic was never in doubt in either
// gate, so the branch belongs here and the kernel keeps only multiply-add.
//
// `out` receives {a_x, b_x, c_x, a_y, b_y, c_y} in that order -- the same order
// the kernels declare the inputs, immediately after src_scale. uw/uh are the
// UNORIENTED output extents; the transposed extents are the dst buffer's, never
// these. Orientations outside 1..8 yield the identity, matching
// ceyx_orient_rgba's "invalid -> 1".
//
// Header-only on purpose: like ceyx_orientation_transposes_inline above, this
// must survive Phase 4's deletion of ceyx_orient.cpp, so it takes no link
// dependency on that TU.
static inline void ceyx_orient_affine_coeffs(int32_t exif_orientation,
                                             int32_t uw, int32_t uh,
                                             int32_t out[6]) {
  int32_t a_x = 1, b_x = 0, c_x = 0;
  int32_t a_y = 0, b_y = 1, c_y = 0;
  switch (exif_orientation) {
    case 2:  a_x = -1; c_x = uw - 1;                                  break;
    case 3:  a_x = -1; c_x = uw - 1; b_y = -1; c_y = uh - 1;          break;
    case 4:                          b_y = -1; c_y = uh - 1;          break;
    case 5:  a_x = 0; b_x = 1;       a_y = 1;  b_y = 0;               break;
    case 6:  a_x = 0; b_x = 1;       a_y = -1; b_y = 0; c_y = uh - 1; break;
    case 7:  a_x = 0; b_x = -1; c_x = uw - 1;
                                     a_y = -1; b_y = 0; c_y = uh - 1; break;
    case 8:  a_x = 0; b_x = -1; c_x = uw - 1;
                                     a_y = 1;  b_y = 0;               break;
    case 1:
    default: break;  // identity; invalid orientations behave as 1
  }
  out[0] = a_x; out[1] = b_x; out[2] = c_x;
  out[3] = a_y; out[4] = b_y; out[5] = c_y;
}
#endif
