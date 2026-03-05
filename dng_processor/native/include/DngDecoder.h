#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Error codes for FFI boundary
enum class DngErrorCode : int32_t {
  SUCCESS = 0,
  FILE_NOT_FOUND = -1,
  PARSE_ERROR = -2,
  UNSUPPORTED_FORMAT = -3,
  MEMORY_ERROR = -4,
  UNKNOWN_ERROR = -99
};

// Extracted Metadata structure matching the required Phase 2 output
struct DngMetadata {
  uint32_t width;
  uint32_t height;
  uint32_t blackLevel;
  uint32_t whiteLevel;

  // 3x3 Matrices stored in row-major order
  double colorMatrix1[9];
  double colorMatrix2[9];
  double forwardMatrix[9];

  // White balance / AsShotNeutral
  double asShotNeutral[3];

  // Exposure compensation (EV stops)
  double baselineExposure;

  // Composite Camera->sRGB matrix (Phase 3)
  // Computed as: sRGB_from_XYZ x ForwardMatrix x diag(1/AsShotNeutral)
  double camToSrgb[9];

  // ProfileToneCurve: interleaved (input, output) pairs, [0..1] range
  // Max 128 control points = 256 doubles
  // (Kept for future Phase 5 color curve work)
  double toneCurvePoints[256];
  uint32_t toneCurveCount; // number of (input,output) pairs, 0 = use default

  // Raw XMP data string extracted from the DNG file
  // This contains Lightroom edits (crs:*) which must be parsed in Flutter
  std::string rawXmp;

  // -----------------------------------------------------------------------
  // HueSatMap (DCP Profile) — Phase 5 saturation fix
  // -----------------------------------------------------------------------
  // HSBModify entry: hueShift (degrees), satScale (mult), valScale (mult)
  struct HSBEntry {
    float hueShift; // delta hue in degrees (0 = no change)
    float satScale; // saturation scale factor (1.0 = no change)
    float valScale; // value scale factor (1.0 = no change)
  };

  // HueSatMap1 (for calibration illuminant 1)
  // Stored in val-hue-sat order (outer->inner), size = hD*sD*vD
  uint32_t hsmHueDivisions; // 0 if not present
  uint32_t hsmSatDivisions;
  uint32_t hsmValDivisions;
  std::vector<HSBEntry> hsmData; // size = hD * sD * vD when valid

  // LookTable (post-HueSatMap style correction)
  uint32_t ltHueDivisions; // 0 if not present
  uint32_t ltSatDivisions;
  uint32_t ltValDivisions;
  std::vector<HSBEntry> ltData; // size = hD * sD * vD when valid

  // -----------------------------------------------------------------------
  // Lightroom XMP parameters (parsed from rawXmp, crs:* namespace)
  // Phase 5.1 — XMP Lightroom parameter extraction
  // -----------------------------------------------------------------------
  struct LightroomParams {
    double exposure2012; // crs:Exposure2012 in EV (default 0.0)
    double contrast2012; // crs:Contrast2012 in % (default 0.0, not yet applied)
    double saturation;   // crs:Saturation in % [-100, +100] (default 0.0)
    double vibrance;     // crs:Vibrance in % [-100, +100] (default 0.0)
    bool parsed; // true if rawXmp was found and these fields were extracted
    LightroomParams()
        : exposure2012(0.0), contrast2012(0.0), saturation(0.0), vibrance(0.0),
          parsed(false) {}
  };
  LightroomParams lrParams;
};

class DngDecoder {
public:
  DngDecoder();
  ~DngDecoder();

  // Decode from file path
  DngErrorCode decodeFile(const std::string &filePath,
                          DngMetadata &outMetadata);

  // Get the extracted 16-bit Bayer raw data
  const uint16_t *getRawBuffer() const;
  size_t getRawBufferSize() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};
