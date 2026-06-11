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
 * Bilinear demosaic for Bayer CFA pattern.
 *
 * @param input: 16-bit grayscale CFA image (width * height pixels)
 * @param width: image width
 * @param height: image height
 * @param output: 16-bit RGB output buffer (width * height * 3 pixels)
 */
void demosaic_bilinear_halide(const uint16_t* input,
                              int width,
                              int height,
                              uint16_t* output);

/**
 * CPU reference bilinear demosaic for Bayer CFA pattern.
 *
 * This is retained as a deterministic fallback/reference path. The default
 * demosaic_bilinear_halide entry uses the Halide AOT implementation
 * unconditionally.
 */
void demosaic_pattern_bilinear(const uint16_t* input,
                               int width,
                               int height,
                               uint16_t* output);

/**
 * Halide AOT bilinear demosaic entry.
 *
 * @return 1 on success, 0 on failure.
 */
int demosaic_bilinear_halide_aot(const uint16_t* input,
                                 int width,
                                 int height,
                                 uint16_t* output);

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
                              uint16_t* output);

/**
 * Get the CFA pattern from DNG metadata
 *
 * @param pattern: array of 4 values representing 2x2 Bayer pattern
 *                0=R, 1=G, 2=B (standard RGGB pattern)
 */
void get_cfa_pattern(int pattern[4]);

#ifdef __cplusplus
}
#endif

#endif /* __DNG_MOSAIC_HALIDE_H__ */
