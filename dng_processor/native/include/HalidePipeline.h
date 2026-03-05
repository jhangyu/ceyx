#pragma once

#include "DngDecoder.h"
#include <cstdint>
#include <vector>

/// Halide JIT Pipeline: Bayer raw -> 8-bit sRGB RGBA
///
/// Phase 3+5a pipeline:
///   1. Black-level subtraction & linearisation [0,1]
///   2. White balance (handled internally by CameraToPCS matrix)
///   3. AHD demosaicing (RGGB CFA)
///   4. Camera -> sRGB (composite matrix)
///   5. BaselineExposure compensation
///   5a. HueSatMap 3D LUT colour correction (Phase 5 — saturation fix)
///   5b. LookTable 3D LUT (if present)
///   6. sRGB gamma correction
///   7. Clamp -> uint8 RGBA (A=255)
class HalidePipeline {
public:
  /// @param bayerData         Raw 16-bit Bayer buffer (width×height uint16_t)
  /// @param width             Image width
  /// @param height            Image height
  /// @param blackLevel        BlackLevel tag
  /// @param whiteLevel        WhiteLevel tag
  /// @param asShotNeutral     AsShotNeutral[3] white balance coefficients
  /// @param camToSrgb         Camera->sRGB [9] row-major composite matrix
  /// @param baselineExposure  BaselineExposure in EV
  /// @param metadata          Full DngMetadata (for HueSatMap / LookTable)
  /// @param outWidth          [out] output width
  /// @param outHeight         [out] output height
  /// @return Heap-allocated RGBA buffer (caller frees with delete[]).
  ///         Returns nullptr on failure.
  static uint8_t *process(const uint16_t *bayerData, int width, int height,
                          uint32_t blackLevel, uint32_t whiteLevel,
                          const double asShotNeutral[3],
                          const double camToSrgb[9], double baselineExposure,
                          const DngMetadata &metadata, int &outWidth,
                          int &outHeight);
};
