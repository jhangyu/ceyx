/**
 * dng_mosaic_halide.h
 *
 * Phase 5.3 - Halide-based Demosaicing Header
 *
 * Provides C interface for Halide-based demosaicing algorithms.
 */

#ifndef __DNG_MOSAIC_HALIDE_H__
#define __DNG_MOSAIC_HALIDE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * CFA phase convention shared by every entry in this header (2026-08-16).
 *
 * `red_x` / `red_y` are the column / row parity of the red CFA site: red lives
 * wherever `x % 2 == red_x` and `y % 2 == red_y`, blue at the diagonally
 * opposite parity, green on the other two. The four standard Bayer phases map
 * as: RGGB=(0,0), GRBG=(1,0), GBRG=(0,1), BGGR=(1,1). Values outside {0,1}
 * are normalized to 0 (RGGB).
 *
 * Callers must derive the phase from the file's CFAPattern tag; passing (0,0)
 * unconditionally is the pre-2026-08-16 bug that mis-colored every non-RGGB
 * sensor.
 */

/**
 * Bilinear demosaic for Bayer CFA pattern.
 *
 * @param input: 16-bit grayscale CFA image (width * height pixels)
 * @param width: image width
 * @param height: image height
 * @param output: 16-bit RGB output buffer (width * height * 3 pixels)
 * @param red_x, red_y: CFA phase (see convention note above)
 */
void demosaic_bilinear_halide(const uint16_t* input,
                              int width,
                              int height,
                              uint16_t* output,
                              int red_x,
                              int red_y);

/**
 * CPU reference bilinear demosaic for Bayer CFA pattern.
 *
 * This is retained as a deterministic fallback/reference path. The default
 * demosaic_bilinear_halide entry uses the Halide AOT implementation
 * unconditionally; the historical DNG_DEMOSAIC_AOT runtime switch was
 * retired in commit 49d8111 (env-switch sweep) and is no longer consulted.
 */
void demosaic_pattern_bilinear(const uint16_t* input,
                               int width,
                               int height,
                               uint16_t* output,
                               int red_x,
                               int red_y);

/**
 * Halide AOT bilinear demosaic entry.
 *
 * @return 1 on success, 0 on failure.
 */
int demosaic_bilinear_halide_aot(const uint16_t* input,
                                 int width,
                                 int height,
                                 uint16_t* output,
                                 int red_x,
                                 int red_y);

/**
 * Compatibility entry for the Stage3 bilinear demosaic call site.
 *
 * @param input: 16-bit grayscale CFA image
 * @param width: image width
 * @param height: image height
 * @param output: 16-bit RGB output buffer
 *
 * This dispatches to the bilinear Halide AOT implementation by default, with
 * CPU bilinear fallback when AOT is disabled or unavailable.
 */
void demosaic_bilinear_compat(const uint16_t* input,
                              int width,
                              int height,
                              uint16_t* output,
                              int red_x,
                              int red_y);

/**
 * Expand a CFA phase into the 2x2 Bayer color-key pattern.
 *
 * @param pattern: out, 4 values in row-major order (pattern[row * 2 + col]),
 *                 0=R, 1=G, 2=B (DNG SDK ColorKeyCode)
 * @param red_x, red_y: CFA phase (see convention note above)
 */
void get_cfa_pattern(int pattern[4], int red_x, int red_y);

#ifdef __cplusplus
}
#endif

#endif /* __DNG_MOSAIC_HALIDE_H__ */
