#include "DngDecoder.h"
#include "ConcurrentDngHost.h"
#include "HalidePipeline.h"
#include <cstdlib>
#include <dng_camera_profile.h>
#include <dng_color_space.h>
#include <dng_color_spec.h>
#include <dng_exceptions.h>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_hue_sat_map.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_linearization_info.h>
#include <dng_matrix.h>
#include <dng_memory.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_xmp.h>
#include <iostream>
#include <thread>
#include <future>
#include <vector>
#include <atomic>
#include <mutex>
#include <exception>
#include <dng_area_task.h>

#if defined(USE_LIBJPEG_TURBO)
#include <turbojpeg.h>
#endif

/*
---
file_summary: "Native DNG 檔案解析核心，使用 Adobe DNG SDK 進行解壓縮與 Metadata
提取" modules:
  - name: "Helper Functions"
    description: "XMP 參數解析與 HubSatMap 結構轉換"
    lines: "20-104"
  - name: "DngDecoder::Impl"
    description: "核心解碼類別，執行 C++ DNG SDK API 讀取"
    lines: "106-345"
  - name: "DngDecoder (Public API)"
    description: "DngDecoder 共用介面定義"
    lines: "347-362"
---
*/

// ---------------------------------------------------------------------------
// Phase 5.1: Parse a float crs:Key value from raw XMP string.
// Handles two XMP serialisation formats:
//   Attribute: crs:Key="+0.25"
//   Element:   <crs:Key>+0.25</crs:Key>
// Returns defaultVal if key not found or parsing fails.
// ---------------------------------------------------------------------------
static double parseCrsFloat(const std::string &xmp, const std::string &key,
                            double defaultVal = 0.0) {
  // Try attribute format: key="..."
  std::string attrSearch = key + "=\"";
  size_t pos = xmp.find(attrSearch);
  if (pos != std::string::npos) {
    size_t start = pos + attrSearch.size();
    size_t end = xmp.find('"', start);
    if (end != std::string::npos) {
      try {
        return std::stod(xmp.substr(start, end - start));
      } catch (...) {
      }
    }
  }
  // Try element format: <key>...</key>
  std::string elemOpen = "<" + key + ">";
  std::string elemClose = "</" + key + ">";
  pos = xmp.find(elemOpen);
  if (pos != std::string::npos) {
    size_t start = pos + elemOpen.size();
    size_t end = xmp.find(elemClose, start);
    if (end != std::string::npos) {
      try {
        return std::stod(xmp.substr(start, end - start));
      } catch (...) {
      }
    }
  }
  return defaultVal;
}

// Parse all relevant Lightroom crs:* parameters from rawXmp.
static DngMetadata::LightroomParams
parseLightroomParams(const std::string &xmp) {
  DngMetadata::LightroomParams p;
  if (xmp.empty())
    return p;
  p.exposure2012 = parseCrsFloat(xmp, "crs:Exposure2012");
  p.contrast2012 = parseCrsFloat(xmp, "crs:Contrast2012");
  p.saturation = parseCrsFloat(xmp, "crs:Saturation");
  p.vibrance = parseCrsFloat(xmp, "crs:Vibrance");
  
  const char* colors[] = {"Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"};
  for (int i = 0; i < 8; ++i) {
    p.hslHue[i] = parseCrsFloat(xmp, std::string("crs:HueAdjustment") + colors[i]);
    p.hslSat[i] = parseCrsFloat(xmp, std::string("crs:SaturationAdjustment") + colors[i]);
    p.hslLum[i] = parseCrsFloat(xmp, std::string("crs:LuminanceAdjustment") + colors[i]);
  }
  
  p.parsed = true;
  std::cerr << "[DBG] LR Params: Exposure2012=" << p.exposure2012
            << " Contrast2012=" << p.contrast2012
            << " Saturation=" << p.saturation << " Vibrance=" << p.vibrance
            << " HSL_Red=(" << p.hslHue[0] << "," << p.hslSat[0] << "," << p.hslLum[0] << ")"
            << "\n";
  return p;
}

// Helper: copy a dng_hue_sat_map into our flat HSBEntry vector
static void extractHueSatMap(const dng_hue_sat_map &src, uint32_t &hD,
                             uint32_t &sD, uint32_t &vD,
                             std::vector<DngMetadata::HSBEntry> &out) {
  hD = sD = vD = 0;
  out.clear();
  if (!src.IsValid())
    return;

  uint32 hDu, sDu, vDu;
  src.GetDivisions(hDu, sDu, vDu);
  hD = hDu;
  sD = sDu;
  vD = vDu;

  uint32_t total = hDu * sDu * vDu;
  out.resize(total);

  const auto *deltas = src.GetConstDeltas();
  for (uint32_t i = 0; i < total; ++i) {
    out[i].hueShift = deltas[i].fHueShift;
    out[i].satScale = deltas[i].fSatScale;
    out[i].valScale = deltas[i].fValScale;
  }

  std::cerr << "[DBG] HueSatMap: " << hDu << "x" << sDu << "x" << vDu << " ("
            << total << " entries)\n";
}

#if defined(USE_LIBJPEG_TURBO)
/******************************************************************************/
// TurboJPEG-based tile decoder for lossless JPEG
// Phase 8.6: Bypasses DNG SDK's slow lossless JPEG decoder
/******************************************************************************/
class TurboJpegDecoder {
public:
  // Decode a lossless JPEG tile using libjpeg-turbo
  // Returns true on success, false on failure
  static bool decodeLosslessJpeg(const uint8_t* jpegData, size_t jpegSize,
                                 uint16_t* outputBuffer, int width, int height) {
    tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
    if (!handle) {
      std::cerr << "[TJ] Failed to initialize TurboJPEG: " << tj3GetErrorStr(handle) << "\n";
      return false;
    }

    if (tj3DecompressHeader(handle, jpegData, jpegSize) < 0) {
      std::cerr << "[TJ] Failed to read JPEG header: " << tj3GetErrorStr(handle) << "\n";
      tj3Destroy(handle);
      return false;
    }

    int jpegWidth = tj3Get(handle, TJPARAM_JPEGWIDTH);
    int jpegHeight = tj3Get(handle, TJPARAM_JPEGHEIGHT);
    int precision = tj3Get(handle, TJPARAM_PRECISION);
    int isLossless = tj3Get(handle, TJPARAM_LOSSLESS);

    if (precision > 16) {
      std::cerr << "[TJ] Unsupported bit depth: " << precision << "\n";
      tj3Destroy(handle);
      return false;
    }

    int result = -1;
    if (precision <= 8) {
      std::vector<uint8_t> tempBuffer(width * height);
      result = tj3Decompress8(handle, jpegData, jpegSize,
                              tempBuffer.data(), width, TJPF_GRAY);
      if (result == 0) {
        for (int i = width * height - 1; i >= 0; i--) {
          outputBuffer[i] = static_cast<uint16_t>(tempBuffer[i]) << 8;
        }
      }
    } else if (precision <= 12) {
      std::vector<int16_t> tempBuffer(width * height);
      result = tj3Decompress12(handle, jpegData, jpegSize,
                               tempBuffer.data(), width, TJPF_GRAY);
      if (result == 0) {
        for (int i = width * height - 1; i >= 0; i--) {
          outputBuffer[i] = static_cast<uint16_t>(tempBuffer[i]) << 4;
        }
      }
    } else {
      result = tj3Decompress16(handle, jpegData, jpegSize,
                                reinterpret_cast<uint16_t*>(outputBuffer), width, TJPF_GRAY);
    }

    if (result < 0) {
      std::cerr << "[TJ] Decompression failed: " << tj3GetErrorStr(handle) << "\n";
      tj3Destroy(handle);
      return false;
    }

    tj3Destroy(handle);
    return true;
  }

  // Decode a baseline (lossy) JPEG tile using libjpeg-turbo
  static bool decodeBaselineJpeg(const uint8_t* jpegData, size_t jpegSize,
                                  uint16_t* outputBuffer, int width, int height) {
    tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
    if (!handle) {
      std::cerr << "[TJ] Failed to initialize TurboJPEG\n";
      return false;
    }

    if (tj3DecompressHeader(handle, jpegData, jpegSize) < 0) {
      std::cerr << "[TJ] Failed to read JPEG header: " << tj3GetErrorStr(handle) << "\n";
      tj3Destroy(handle);
      return false;
    }

    int jpegWidth = tj3Get(handle, TJPARAM_JPEGWIDTH);
    int jpegHeight = tj3Get(handle, TJPARAM_JPEGHEIGHT);
    int precision = tj3Get(handle, TJPARAM_PRECISION);

    int result = -1;
    if (precision <= 8) {
      std::vector<uint8_t> tempBuffer(jpegWidth * jpegHeight);
      result = tj3Decompress8(handle, jpegData, jpegSize,
                               tempBuffer.data(), jpegWidth, TJPF_GRAY);
      if (result == 0) {
        for (int i = jpegWidth * jpegHeight - 1; i >= 0; i--) {
          outputBuffer[i] = static_cast<uint16_t>(tempBuffer[i]) << 8;
        }
      }
    } else if (precision <= 12) {
      std::vector<int16_t> tempBuffer(jpegWidth * jpegHeight);
      result = tj3Decompress12(handle, jpegData, jpegSize,
                               tempBuffer.data(), jpegWidth, TJPF_GRAY);
      if (result == 0) {
        for (int i = jpegWidth * jpegHeight - 1; i >= 0; i--) {
          outputBuffer[i] = static_cast<uint16_t>(tempBuffer[i]) << 4;
        }
      }
    } else {
      result = tj3Decompress16(handle, jpegData, jpegSize,
                                reinterpret_cast<uint16_t*>(outputBuffer), jpegWidth, TJPF_GRAY);
    }

    if (result < 0) {
      std::cerr << "[TJ] Baseline JPEG decompress failed: " << tj3GetErrorStr(handle) << "\n";
      tj3Destroy(handle);
      return false;
    }

    tj3Destroy(handle);
    return true;
  }

  // Decode a baseline (lossy) JPEG tile to YUV420 planar format
  // Returns true on success, false on failure
  // Y, U, V planes are allocated by caller
  static bool decodeBaselineJpegToYUV(const uint8_t* jpegData, size_t jpegSize,
                                     uint8_t* yPlane, uint8_t* uPlane, uint8_t* vPlane,
                                     int width, int height) {
    tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
    if (!handle) {
      std::cerr << "[TJ] Failed to initialize TurboJPEG\n";
      return false;
    }

    if (tj3DecompressHeader(handle, jpegData, jpegSize) < 0) {
      std::cerr << "[TJ] Failed to read JPEG header: " << tj3GetErrorStr(handle) << "\n";
      tj3Destroy(handle);
      return false;
    }

    int jpegWidth = tj3Get(handle, TJPARAM_JPEGWIDTH);
    int jpegHeight = tj3Get(handle, TJPARAM_JPEGHEIGHT);

    // Calculate YUV420 dimensions
    int ySize = jpegWidth * jpegHeight;
    int uvSize = (jpegWidth / 2) * (jpegHeight / 2);

    // Set up YUV420 planar buffers
    uint8_t* yuvPlanes[3] = { yPlane, uPlane, vPlane };
    int strides[3] = { jpegWidth, jpegWidth / 2, jpegWidth / 2 };

    int result = tj3DecompressToYUVPlanes8(handle, jpegData, jpegSize,
                                          yuvPlanes, strides);

    if (result < 0) {
      std::cerr << "[TJ] YUV decompress failed: " << tj3GetErrorStr(handle) << "\n";
      tj3Destroy(handle);
      return false;
    }

    tj3Destroy(handle);
    return true;
  }
};
#endif

class DngDecoder::Impl {
public:
  std::vector<uint16_t> bayerData;
  std::vector<uint8_t> rgbaData;  // For YCbCr DNG output
  bool isYCbCr = false;           // Flag indicating YCbCr mode

  DngErrorCode decode(const std::string &filePath, DngMetadata &outMetadata) {
    try {
      ConcurrentDngHost host;
      std::cerr << "[DBG] Opening stream...\n";
      dng_file_stream stream(filePath.c_str());

      std::cerr << "[DBG] Parsing info...\n";
      dng_info info;
      info.Parse(host, stream);
      info.PostParse(host);

      std::cerr << "[DBG] IsValidDNG check...\n";
      if (!info.IsValidDNG()) {
        return DngErrorCode::UNSUPPORTED_FORMAT;
      }

      std::cerr << "[DBG] Creating negative...\n";
      dng_negative *negativeTemplate = host.Make_dng_negative();
      AutoPtr<dng_negative> negative(negativeTemplate);
      negative->Parse(host, stream, info);
      negative->PostParse(host, stream, info);

      std::cerr << "[DBG] Extracting XMP metadata...\n";
      outMetadata.rawXmp = "";
      if (info.fShared.Get() != nullptr && info.fShared->fXMPCount > 0) {
        try {
          uint32 count = info.fShared->fXMPCount;
          uint64 offset = info.fShared->fXMPOffset;
          outMetadata.rawXmp.resize(count);
          stream.SetReadPosition(offset);
          stream.Get(outMetadata.rawXmp.data(), count);
          std::cerr << "[DBG] Successfully extracted physical XMP (" << count
                    << " bytes)\n";
          // Phase 5.1: Parse Lightroom crs:* parameters from the XMP string
          outMetadata.lrParams = parseLightroomParams(outMetadata.rawXmp);
        } catch (...) {
          std::cerr << "[WARN] Failed to read physical XMP stream.\n";
        }
      } else {
        std::cerr << "[DBG] No XMP block found in DNG info.\n";
      }

      std::cerr << "[DBG] Extracting profile...\n";
      outMetadata.toneCurveCount = 0;
      outMetadata.hsmHueDivisions = 0;
      outMetadata.hsmSatDivisions = 0;
      outMetadata.hsmValDivisions = 0;
      outMetadata.ltHueDivisions = 0;
      outMetadata.ltSatDivisions = 0;
      outMetadata.ltValDivisions = 0;

      if (negative->ProfileCount() > 0) {
        const dng_camera_profile &profile = negative->ProfileByIndex(0);
        std::cerr << "[DBG] Profile: " << profile.Name().Get() << "\n";

        // ColorMatrix 1 & 2
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            outMetadata.colorMatrix1[i * 3 + j] = profile.ColorMatrix1()[i][j];
            if (profile.HasColorMatrix2())
              outMetadata.colorMatrix2[i * 3 + j] =
                  profile.ColorMatrix2()[i][j];

            if (profile.ForwardMatrix1().NotEmpty())
              outMetadata.forwardMatrix[i * 3 + j] =
                  profile.ForwardMatrix1()[i][j];

            if (profile.ForwardMatrix2().NotEmpty())
              outMetadata.forwardMatrix2[i * 3 + j] =
                  profile.ForwardMatrix2()[i][j];

            outMetadata.cameraCalibration1[i * 3 + j] =
                negative->CameraCalibration1()[i][j];
            outMetadata.cameraCalibration2[i * 3 + j] =
                negative->CameraCalibration2()[i][j];
          }
        }

        outMetadata.illuminant1 = profile.CalibrationIlluminant1();
        outMetadata.illuminant2 = profile.CalibrationIlluminant2();

        // ProfileToneCurve
        const dng_tone_curve &curve = profile.ToneCurve();
        uint32_t nPts = static_cast<uint32_t>(curve.fCoord.size());
        if (!curve.IsNull() && nPts > 0 && nPts <= 128) {
          outMetadata.toneCurveCount = nPts;
          for (uint32_t k = 0; k < nPts; ++k) {
            outMetadata.toneCurvePoints[k * 2 + 0] = curve.fCoord[k].h;
            outMetadata.toneCurvePoints[k * 2 + 1] = curve.fCoord[k].v;
          }
          std::cerr << "[DBG] ProfileToneCurve: " << nPts << " points\n";
        } else {
          std::cerr << "[DBG] No ProfileToneCurve\n";
        }

        // ---------------------------------------------------------------
        // Phase 5: Extract HueSatMap & LookTable from DCP profile
        // These are the most impactful for saturation accuracy.
        // ---------------------------------------------------------------
        // Use HueSatMapForWhite() to get the correctly-interpolated map
        // for the actual shot's white point (between illuminant 1 & 2).
        {
          dng_xy_coord whiteXY;
          if (negative->HasCameraNeutral()) {
            // Build a temporary color spec to get white XY
            dng_camera_profile_id profID;
            AutoPtr<dng_color_spec> spec(negative->MakeColorSpec(profID));
            spec->SetWhiteXY(spec->NeutralToXY(negative->CameraNeutral()));
            whiteXY = spec->WhiteXY();
          } else {
            whiteXY = D65_xy_coord(); // fallback
          }

          const dng_hue_sat_map *hsmPtr = profile.HueSatMapForWhite(whiteXY);
          if (hsmPtr && hsmPtr->IsValid()) {
            extractHueSatMap(*hsmPtr, outMetadata.hsmHueDivisions,
                             outMetadata.hsmSatDivisions,
                             outMetadata.hsmValDivisions, outMetadata.hsmData);
          } else {
            // Fallback: use HueSatDeltas1 directly
            extractHueSatMap(profile.HueSatDeltas1(),
                             outMetadata.hsmHueDivisions,
                             outMetadata.hsmSatDivisions,
                             outMetadata.hsmValDivisions, outMetadata.hsmData);
          }
        }

        // LookTable extraction
        if (profile.LookTable().IsValid()) {
          extractHueSatMap(profile.LookTable(), outMetadata.ltHueDivisions,
                           outMetadata.ltSatDivisions,
                           outMetadata.ltValDivisions, outMetadata.ltData);
        }
      }

      // AnalogBalance
      {
        for (int i = 0; i < 3; i++) {
          outMetadata.analogBalance[i] = negative->AnalogBalance(i);
        }
      }

      // AsShotNeutral (WB)
      std::cerr << "[DBG] Extracting WB...\n";
      if (negative->HasCameraNeutral()) {
        const auto &neutral = negative->CameraNeutral();
        for (uint32 i = 0; i < neutral.Count() && i < 3; ++i) {
          outMetadata.asShotNeutral[i] = neutral[i];
        }
      } else {
        outMetadata.asShotNeutral[0] = 1.0;
        outMetadata.asShotNeutral[1] = 1.0;
        outMetadata.asShotNeutral[2] = 1.0;
      }

      // ================================================================
      // Compute composite Camera->sRGB matrix
      // ================================================================
      std::cerr << "[DBG] Computing cam_to_srgb matrix...\n";
      {
        dng_camera_profile_id profileID;
        AutoPtr<dng_color_spec> spec(negative->MakeColorSpec(profileID));

        if (negative->HasCameraNeutral()) {
          spec->SetWhiteXY(spec->NeutralToXY(negative->CameraNeutral()));
        } else if (negative->HasCameraWhiteXY()) {
          spec->SetWhiteXY(negative->CameraWhiteXY());
        }

        dng_matrix camToSrgb =
            dng_space_sRGB::Get().MatrixFromPCS() * spec->CameraToPCS();

        // Phase 6.6: Compute Camera -> ProPhoto and ProPhoto -> sRGB
        dng_matrix cameraToProPhoto =
            dng_space_ProPhoto::Get().MatrixFromPCS() * spec->CameraToPCS();
        dng_matrix proPhotoToSrgb =
            dng_space_sRGB::Get().MatrixFromPCS() * dng_space_ProPhoto::Get().MatrixToPCS();

        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 3; ++j) {
            outMetadata.camToSrgb[i * 3 + j] = camToSrgb[i][j];
            outMetadata.cameraToProPhoto[i * 3 + j] = cameraToProPhoto[i][j];
            outMetadata.proPhotoToSrgb[i * 3 + j] = proPhotoToSrgb[i][j];
          }
        }

        std::cerr << "[DBG] cam_to_srgb matrix:\n";
        for (int r = 0; r < 3; r++) {
          std::cerr << "  [";
          for (int c = 0; c < 3; c++) {
            if (c)
              std::cerr << ", ";
            std::cerr << outMetadata.camToSrgb[r * 3 + c];
          }
          std::cerr << "]\n";
        }
      }

      // ================================================================
      // Phase 8.6: Try TurboJPEG path for lossless JPEG with Bayer CFA
      // ================================================================
#if defined(USE_LIBJPEG_TURBO)
      {
        const dng_ifd &rawIFD = *info.fIFD[info.fMainIndex];
        uint32 compression = rawIFD.fCompression;
        uint32 samplesPerPixel = rawIFD.fSamplesPerPixel;

        // Phase 9.1: TurboJPEG path for Lossless JPEG (compression=7, 1 sample/pixel Bayer CFA)
        // Phase 10: YCbCr path for Lossy JPEG (compression=34892, 3 samples/pixel YCbCr)
        bool isLosslessJpeg = (compression == 7);
        bool isLossyJPEG = (compression == 34892);

        if (isLosslessJpeg && samplesPerPixel == 1) {
          std::cerr << "[DBG] Lossless JPEG with Bayer CFA detected, trying TurboJPEG path...\n";

          // Create a temporary stream to read tile data
          dng_file_stream tileStream(filePath.c_str());

          // Extract black/white levels and baseline exposure from negative
          const dng_linearization_info *linInfo = negative->GetLinearizationInfo();
          uint32 blackLevel = 0, whiteLevel = 65535;
          if (linInfo) {
            blackLevel = static_cast<uint32_t>(linInfo->fBlackLevel[0][0][0]);
            whiteLevel = static_cast<uint32_t>(linInfo->fWhiteLevel[0]);
          }
          double baselineExp = negative->BaselineExposure();

          if (decodeStage1ImageWithTurboJPEG(host, tileStream, info, outMetadata)) {
            std::cerr << "[DBG] TurboJPEG decoding successful!\n";
            outMetadata.blackLevel = blackLevel;
            outMetadata.whiteLevel = whiteLevel;
            outMetadata.baselineExposure = baselineExp;
            return DngErrorCode::SUCCESS;
          }

          // TurboJPEG failed (expected for Lossless JPEG SOF3)
          // Fall back to DNG SDK which now uses multi-threaded tile reading
          std::cerr << "[DBG] TurboJPEG path failed, falling back to DNG SDK (multi-threaded)\n";
        } else if (isLossyJPEG) {
          // Phase 10.5.2.1: Lossy JPEG uses DNG SDK directly (no TurboJPEG)
          // This matches the approach used for Lossless JPEG
          std::cerr << "[DBG] Lossy JPEG (YCbCr) detected, using DNG SDK path\n";
        } else {
          std::cerr << "[DBG] JPEG compression=" << compression
                    << " samplesPerPixel=" << samplesPerPixel
                    << ", using DNG SDK path\n";
        }
      }
#endif

      // Build stage 1 image (Raw Bayer) using standard DNG SDK path
      std::cerr << "[DBG] Reading stage1 image (standard DNG SDK path)...\n";
      auto t_stage1 = std::chrono::steady_clock::now();
      negative->ReadStage1Image(host, stream, info);
      auto t_stage1_end = std::chrono::steady_clock::now();
      double stage1_ms = std::chrono::duration<double, std::milli>(t_stage1_end - t_stage1).count();
      std::cerr << "[TIMING] ReadStage1Image: " << stage1_ms << " ms\n";

      std::cerr << "[DBG] Getting stage1 image pointer...\n";
      const dng_image *stage1Image = negative->Stage1Image();
      if (!stage1Image) {
        return DngErrorCode::PARSE_ERROR;
      }

      const dng_rect rect = stage1Image->Bounds();
      outMetadata.width = rect.W();
      outMetadata.height = rect.H();
      std::cerr << "[DBG] Image size: " << outMetadata.width << "x"
                << outMetadata.height << "\n";

      // Black level / White level
      const dng_linearization_info *linInfo = negative->GetLinearizationInfo();
      if (linInfo) {
        outMetadata.blackLevel =
            static_cast<uint32_t>(linInfo->fBlackLevel[0][0][0]);
        outMetadata.whiteLevel = static_cast<uint32_t>(linInfo->fWhiteLevel[0]);
      } else {
        outMetadata.blackLevel = 0;
        outMetadata.whiteLevel = 65535;
      }

      outMetadata.baselineExposure = negative->BaselineExposure();
      std::cerr << "[DBG] BaselineExposure=" << outMetadata.baselineExposure
                << "\n";

      // Extract pixels
      std::cerr << "[DBG] Extracting pixels...\n";
      uint32 planes = stage1Image->Planes();
      uint32 pixelType = stage1Image->PixelType();
      uint32 pixelSize = stage1Image->PixelSize();
      std::cerr << "[DBG] Planes: " << planes << " PixelType: " << pixelType
                << " PixelSize: " << pixelSize << "\n";

      // Phase 10.5.1: Detect YCbCr format (Lossy JPEG fallback)
      // YCbCr format: 3 planes (Y, Cb, Cr), 8-bit (ttByte)
      // Bayer format: 1 plane, 16-bit (ttShort)
      if (planes == 3 && pixelType == ttByte) {
        std::cerr << "[DBG] YCbCr format detected (Lossy JPEG), using direct YUV444->RGB\n";

        int width = static_cast<int>(outMetadata.width);
        int height = static_cast<int>(outMetadata.height);

        // Read interleaved YCbCr from stage1Image
        // Format: Y0 Cb0 Cr0 Y1 Cb1 Cr1 ... (interleaved YUV444)
        dng_pixel_buffer ycbcrBuffer;
        ycbcrBuffer.fArea = rect;
        ycbcrBuffer.fPlane = 0;
        ycbcrBuffer.fPlanes = 3;
        ycbcrBuffer.fPixelType = ttByte;
        ycbcrBuffer.fPixelSize = 1;
        ycbcrBuffer.fRowStep = width * 3;
        ycbcrBuffer.fColStep = 3;
        ycbcrBuffer.fPlaneStep = 1;

        std::vector<uint8_t> interleavedBuffer((size_t)width * height * 3);
        ycbcrBuffer.fData = interleavedBuffer.data();
        stage1Image->Get(ycbcrBuffer);

        // Phase 10.5.2.1: Use cameraToRGB matrix like DNG SDK render pipeline
        // fCameraToRGB = ProPhoto::MatrixFromPCS() * CameraToPCS()
        dng_camera_profile_id profileID;
        AutoPtr<dng_color_spec> spec(negative->MakeColorSpec(profileID));
        if (negative->HasCameraNeutral()) {
          spec->SetWhiteXY(spec->NeutralToXY(negative->CameraNeutral()));
        } else if (negative->HasCameraWhiteXY()) {
          spec->SetWhiteXY(negative->CameraWhiteXY());
        }
        dng_matrix camToRgbMat =
            dng_space_ProPhoto::Get().MatrixFromPCS() * spec->CameraToPCS();

        std::cerr << "[DBG] Converting YCbCr to RGB using cameraToRGB matrix\n";

        std::vector<uint8_t> rgbBuffer((size_t)width * height * 4);

        // Phase 10.5.2.1: Use cameraToRGB matrix for YCbCr->RGB conversion
        // YCbCr is encoded as: Y (0-255), Cb/Cr centered at 128
        for (int y = 0; y < height; y++) {
          for (int x = 0; x < width; x++) {
            int idx = y * width * 3 + x * 3;
            float yVal = static_cast<float>(interleavedBuffer[idx]);
            float cbVal = static_cast<float>(interleavedBuffer[idx + 1]) - 128.0f;  // Center at 0
            float crVal = static_cast<float>(interleavedBuffer[idx + 2]) - 128.0f;  // Center at 0

            // Apply cameraToRGB matrix: [r0 r1 r2; g0 g1 g2; b0 b1 b2] * [Y; Cb; Cr]
            float r = camToRgbMat[0][0] * yVal + camToRgbMat[0][1] * cbVal + camToRgbMat[0][2] * crVal;
            float g = camToRgbMat[1][0] * yVal + camToRgbMat[1][1] * cbVal + camToRgbMat[1][2] * crVal;
            float b = camToRgbMat[2][0] * yVal + camToRgbMat[2][1] * cbVal + camToRgbMat[2][2] * crVal;

            // Clamp to [0, 255]
            int rInt = r < 0.0f ? 0 : (r > 255.0f ? 255 : static_cast<int>(r + 0.5f));
            int gInt = g < 0.0f ? 0 : (g > 255.0f ? 255 : static_cast<int>(g + 0.5f));
            int bInt = b < 0.0f ? 0 : (b > 255.0f ? 255 : static_cast<int>(b + 0.5f));

            int outIdx = (y * width + x) * 4;
            rgbBuffer[outIdx + 0] = static_cast<uint8_t>(rInt);
            rgbBuffer[outIdx + 1] = static_cast<uint8_t>(gInt);
            rgbBuffer[outIdx + 2] = static_cast<uint8_t>(bInt);
            rgbBuffer[outIdx + 3] = 255;  // Alpha
          }
        }

        // Store RGBA output directly (no Halide pipeline needed for YUV444)
        isYCbCr = true;
        rgbaData = std::move(rgbBuffer);

        std::cerr << "[DBG] YCbCr fallback (YUV444) processing complete: " << width << "x" << height << "\n";
        return DngErrorCode::SUCCESS;
      }

      // Standard Bayer / LinearRaw processing path
      size_t totalPixels =
          static_cast<size_t>(outMetadata.width) * outMetadata.height * planes;
      bayerData.resize(totalPixels);

      dng_pixel_buffer buffer;
      buffer.fArea = rect;
      buffer.fPlane = 0;
      buffer.fPlanes = planes;
      buffer.fPixelType = ttShort;
      buffer.fPixelSize = sizeof(uint16_t);
      buffer.fData = bayerData.data();
      buffer.fRowStep = outMetadata.width * planes;
      buffer.fColStep = planes;
      buffer.fPlaneStep = 1;
      stage1Image->Get(buffer);

      return DngErrorCode::SUCCESS;
    } catch (const dng_exception &e) {
      std::cerr << "DNG Exception ErrorCode: " << e.ErrorCode() << "\n";
      return DngErrorCode::PARSE_ERROR;
    } catch (const std::exception &e) {
      std::cerr << "Std Exception: " << e.what() << "\n";
      return DngErrorCode::UNKNOWN_ERROR;
    } catch (...) {
      std::cerr << "Unknown Native Exception caught.\n";
      return DngErrorCode::UNKNOWN_ERROR;
    }
  }

#if defined(USE_LIBJPEG_TURBO)
  // Phase 8.6: Direct tile-based JPEG decoding using libjpeg-turbo
  // Bypasses DNG SDK's slow lossless JPEG decoder
  bool decodeStage1ImageWithTurboJPEG(dng_host &host,
                                       dng_stream &stream,
                                       const dng_info &info,
                                       DngMetadata &outMetadata) {
    const dng_ifd &rawIFD = *info.fIFD[info.fMainIndex];

    uint32 imageWidth = rawIFD.fImageWidth;
    uint32 imageHeight = rawIFD.fImageLength;
    uint32 tilesAcross = rawIFD.TilesAcross();
    uint32 tilesDown = rawIFD.TilesDown();
    uint32 tileWidth = rawIFD.fTileWidth;
    uint32 tileLength = rawIFD.fTileLength;
    uint32 samplesPerPixel = rawIFD.fSamplesPerPixel;
    uint32 compression = rawIFD.fCompression;

    bool isLossyJPEG = (compression == 34892);

    std::cerr << "[TJ] Image: " << imageWidth << "x" << imageHeight << "\n";
    std::cerr << "[TJ] Tiles: " << tilesAcross << "x" << tilesDown
              << " (tile=" << tileWidth << "x" << tileLength << ")\n";

    uint32 tileCount = tilesAcross * tilesDown;

    // Check for JPEGTables presence
    bool hasJPEGTables = (rawIFD.fJPEGTablesCount > 0 && rawIFD.fJPEGTablesOffset > 0);
    std::cerr << "[TJ] JPEGTables: count=" << rawIFD.fJPEGTablesCount
              << " offset=" << rawIFD.fJPEGTablesOffset << "\n";

    // Allocate arrays for tile offsets and byte counts
    std::vector<uint64_t> tileOffsets(tileCount);
    std::vector<uint32_t> tileByteCounts(tileCount);

    if (tileCount <= 32) {
      for (uint32 i = 0; i < tileCount; i++) {
        tileOffsets[i] = rawIFD.fTileOffset[i];
        tileByteCounts[i] = rawIFD.fTileByteCount[i];
      }
    } else {
      // Read from external arrays
      uint64 offsetsOffset = rawIFD.fTileOffsetsOffset;
      uint32 offsetsType = rawIFD.fTileOffsetsType;
      uint64 byteCountsOffset = rawIFD.fTileByteCountsOffset;
      uint32 byteCountsType = rawIFD.fTileByteCountsType;

      stream.SetReadPosition(offsetsOffset);
      for (uint32 i = 0; i < tileCount; i++) {
        tileOffsets[i] = stream.TagValue_uint32(offsetsType);
      }

      stream.SetReadPosition(byteCountsOffset);
      for (uint32 i = 0; i < tileCount; i++) {
        tileByteCounts[i] = stream.TagValue_uint32(byteCountsType);
      }
    }

    // Allocate output buffer
    size_t totalPixels = static_cast<size_t>(imageWidth) * imageHeight * samplesPerPixel;
    bayerData.resize(totalPixels);

    // Get thread count for parallel decoding
    uint32 threadCount = host.PerformAreaTaskThreads();
    std::cerr << "[TJ] Using " << threadCount << " threads for tile decoding\n";

    std::atomic<uint32_t> tilesProcessed(0);
    std::atomic<uint32_t> tilesFailed(0);

    // Parallel tile decoding
    std::vector<std::thread> threads;
    std::mutex dataMutex;

    auto decodeTileRange = [&](uint32 startTile, uint32 endTile) {
      for (uint32 tileIndex = startTile; tileIndex < endTile; ++tileIndex) {
        try {
          uint32 rowIndex = tileIndex / tilesAcross;
          uint32 colIndex = tileIndex % tilesAcross;

          uint64 tileOffset = tileOffsets[tileIndex];
          uint32 tileByteCount = tileByteCounts[tileIndex];

          uint32 tileX = colIndex * tileWidth;
          uint32 tileY = rowIndex * tileLength;
          uint32 tileW = std::min(tileWidth, imageWidth - tileX);
          uint32 tileH = std::min(tileLength, imageHeight - tileY);

          // Read compressed tile data (mutex only protects stream position and read)
          std::vector<uint8_t> compressedData;
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            stream.SetReadPosition(tileOffset);
            compressedData.resize(tileByteCount);
            stream.Get(compressedData.data(), tileByteCount);
          }

          // Allocate decode buffer for this tile
          std::vector<uint16_t> tileBuffer(tileW * tileH * samplesPerPixel);

          // Decode using TurboJPEG (parallel - no mutex needed, each tile is independent)
          bool success = false;
          if (isLossyJPEG) {
            success = TurboJpegDecoder::decodeBaselineJpeg(
                compressedData.data(), compressedData.size(),
                tileBuffer.data(), tileW, tileH);
          } else {
            success = TurboJpegDecoder::decodeLosslessJpeg(
                compressedData.data(), compressedData.size(),
                tileBuffer.data(), tileW, tileH);
          }

          if (!success) {
            std::cerr << "[TJ] Failed to decode tile " << tileIndex << "\n";
            tilesFailed++;
            continue;
          }

          // Copy decoded tile data to output buffer
          for (uint32 y = 0; y < tileH; ++y) {
            for (uint32 x = 0; x < tileW; ++x) {
              for (uint32 plane = 0; plane < samplesPerPixel; ++plane) {
                uint32 srcIdx = (y * tileW + x) * samplesPerPixel + plane;
                uint32 dstIdx = ((tileY + y) * imageWidth + (tileX + x)) * samplesPerPixel + plane;
                bayerData[dstIdx] = tileBuffer[srcIdx];
              }
            }
          }

          uint32_t processed = ++tilesProcessed;
          if (processed % 10 == 0) {
            std::cerr << "[TJ] Decoded " << processed << "/" << tileCount << " tiles\n";
          }
        } catch (const std::exception &e) {
          std::cerr << "[TJ] Exception decoding tile " << tileIndex << ": " << e.what() << "\n";
          tilesFailed++;
        } catch (...) {
          std::cerr << "[TJ] Unknown exception decoding tile " << tileIndex << "\n";
          tilesFailed++;
        }
      }
    };

    // Launch threads
    uint32 tilesPerThread = (tileCount + threadCount - 1) / threadCount;
    for (uint32 t = 0; t < threadCount; ++t) {
      uint32 startTile = t * tilesPerThread;
      uint32 endTile = std::min(startTile + tilesPerThread, tileCount);
      if (startTile >= tileCount) break;
      threads.emplace_back(decodeTileRange, startTile, endTile);
    }

    // Wait for all threads
    for (auto &thread : threads) {
      thread.join();
    }

    std::cerr << "[TJ] Tile decoding complete: " << tilesProcessed << " successful, " << tilesFailed << " failed\n";

    if (tilesFailed > 0) {
      return false;
    }

    outMetadata.width = imageWidth;
    outMetadata.height = imageHeight;
    return true;
  }

  // Phase 10.4: Thread-local TurboJPEG handle for multi-threaded tile decoding
  // Each thread gets its own handle to avoid race conditions
  static bool decodeBaselineJpegToYUV(const uint8_t *jpegData, size_t jpegSize,
                                       uint8_t *yPlane, uint8_t *uPlane, uint8_t *vPlane,
                                       int tileWidth, int tileHeight) {
  #if defined(USE_LIBJPEG_TURBO)
    // Thread-local handle - each thread has its own persistent handle
    static thread_local tjhandle tls_handle = nullptr;

    // Initialize handle on first use in this thread
    if (!tls_handle) {
      tls_handle = tj3Init(TJINIT_DECOMPRESS);
      if (!tls_handle) {
        std::cerr << "[TJ] Failed to init TurboJPEG decompressor\n";
        return false;
      }
    }

    if (tj3DecompressHeader(tls_handle, jpegData, jpegSize) != 0) {
      std::cerr << "[TJ] Failed to read JPEG header: " << tj3GetErrorStr(tls_handle) << "\n";
      return false;
    }

    // Set up YUV plane pointers and strides for YUV420
    unsigned char *yuvPlanes[3] = {
      yPlane,
      uPlane,
      vPlane
    };
    int strides[3] = {
      tileWidth,           // Y stride = width
      tileWidth / 2,       // U stride = width/2
      tileWidth / 2        // V stride = width/2
    };

    if (tj3DecompressToYUVPlanes8(tls_handle, jpegData, jpegSize,
                                   yuvPlanes, strides) != 0) {
      std::cerr << "[TJ] Failed to decompress to YUV: " << tj3GetErrorStr(tls_handle) << "\n";
      return false;
    }

    return true;
  #else
    return false;
  #endif
  }

  // Phase 10: Decode Lossy DNG tiles to YUV420, then convert to RGBA
  bool decodeStage1ImageWithYUV(dng_host &host,
                                dng_stream &stream,
                                const dng_info &info,
                                AutoPtr<dng_negative> &negative,
                                DngMetadata &outMetadata) {
    const dng_ifd &rawIFD = *info.fIFD[info.fMainIndex];

    uint32 imageWidth = rawIFD.fImageWidth;
    uint32 imageHeight = rawIFD.fImageLength;
    uint32 tilesAcross = rawIFD.TilesAcross();
    uint32 tilesDown = rawIFD.TilesDown();
    uint32 tileWidth = rawIFD.fTileWidth;
    uint32 tileLength = rawIFD.fTileLength;

    std::cerr << "[YCbCr] Image: " << imageWidth << "x" << imageHeight << "\n";
    std::cerr << "[YCbCr] Tiles: " << tilesAcross << "x" << tilesDown
              << " (tile=" << tileWidth << "x" << tileLength << ")\n";

    uint32 tileCount = tilesAcross * tilesDown;

    // Allocate arrays for tile offsets and byte counts
    std::vector<uint64_t> tileOffsets(tileCount);
    std::vector<uint32_t> tileByteCounts(tileCount);

    if (tileCount <= 32) {
      for (uint32 i = 0; i < tileCount; i++) {
        tileOffsets[i] = rawIFD.fTileOffset[i];
        tileByteCounts[i] = rawIFD.fTileByteCount[i];
      }
    } else {
      uint64 offsetsOffset = rawIFD.fTileOffsetsOffset;
      uint32 offsetsType = rawIFD.fTileOffsetsType;
      uint64 byteCountsOffset = rawIFD.fTileByteCountsOffset;
      uint32 byteCountsType = rawIFD.fTileByteCountsType;

      stream.SetReadPosition(offsetsOffset);
      for (uint32 i = 0; i < tileCount; i++) {
        tileOffsets[i] = stream.TagValue_uint32(offsetsType);
      }

      stream.SetReadPosition(byteCountsOffset);
      for (uint32 i = 0; i < tileCount; i++) {
        tileByteCounts[i] = stream.TagValue_uint32(byteCountsType);
      }
    }

    // YUV420 planar buffers: Y is full res, U/V are half in both dimensions
    int uvWidth = (imageWidth + 1) / 2;
    int uvHeight = (imageHeight + 1) / 2;
    std::vector<uint8_t> yPlaneBuffer((size_t)imageWidth * imageHeight);
    std::vector<uint8_t> uPlaneBuffer((size_t)uvWidth * uvHeight);
    std::vector<uint8_t> vPlaneBuffer((size_t)uvWidth * uvHeight);

    // Get thread count for parallel decoding
    // Phase 10.4: Use thread-local TurboJPEG handles for parallel tile decoding
    uint32 threadCount = std::thread::hardware_concurrency();
    if (threadCount == 0) threadCount = 4;
    if (threadCount > 16) threadCount = 16;
    std::cerr << "[YCbCr] Using " << threadCount << " threads for tile decoding\n";

    std::atomic<uint32_t> tilesProcessed(0);
    std::atomic<uint32_t> tilesFailed(0);

    // Parallel tile decoding
    std::vector<std::thread> threads;
    std::mutex dataMutex;

    auto decodeTileRange = [&](uint32 startTile, uint32 endTile) {
      for (uint32 tileIndex = startTile; tileIndex < endTile; ++tileIndex) {
        try {
          uint32 rowIndex = tileIndex / tilesAcross;
          uint32 colIndex = tileIndex % tilesAcross;

          uint64 tileOffset = tileOffsets[tileIndex];
          uint32 tileByteCount = tileByteCounts[tileIndex];

          uint32 tileX = colIndex * tileWidth;
          uint32 tileY = rowIndex * tileLength;
          uint32 tileW = std::min(tileWidth, imageWidth - tileX);
          uint32 tileH = std::min(tileLength, imageHeight - tileY);

          // Allocate decode buffers
          std::vector<uint8_t> yTileBuffer(tileW * tileH);
          std::vector<uint8_t> uTileBuffer((tileW + 1) / 2 * (tileH + 1) / 2);
          std::vector<uint8_t> vTileBuffer((tileW + 1) / 2 * (tileH + 1) / 2);

          // Read compressed tile data (protected by mutex)
          std::vector<uint8_t> compressedData;
          {
            std::lock_guard<std::mutex> lock(dataMutex);
            stream.SetReadPosition(tileOffset);
            compressedData.resize(tileByteCount);
            stream.Get(compressedData.data(), tileByteCount);
          }

          // Decode using TurboJPEG to YUV (parallel across threads with thread-local handle)
          bool success = decodeBaselineJpegToYUV(
              compressedData.data(), compressedData.size(),
              yTileBuffer.data(), uTileBuffer.data(), vTileBuffer.data(),
              tileW, tileH);

          if (!success) {
            std::cerr << "[YCbCr] Failed to decode tile " << tileIndex << "\n";
            tilesFailed++;
            continue;
          }

          // Copy Y plane to full buffer
          for (uint32 y = 0; y < tileH; ++y) {
            for (uint32 x = 0; x < tileW; ++x) {
              yPlaneBuffer[(tileY + y) * imageWidth + (tileX + x)] = yTileBuffer[y * tileW + x];
            }
          }

          // Copy U/V planes (subsampled)
          int uvTileW = (tileW + 1) / 2;
          int uvTileH = (tileH + 1) / 2;
          for (uint32 y = 0; y < uvTileH; ++y) {
            for (uint32 x = 0; x < uvTileW; ++x) {
              uint32 dstUVX = tileX / 2 + x;
              uint32 dstUVY = tileY / 2 + y;
              if (dstUVX < (uint32)uvWidth && dstUVY < (uint32)uvHeight) {
                uPlaneBuffer[dstUVY * uvWidth + dstUVX] = uTileBuffer[y * uvTileW + x];
                vPlaneBuffer[dstUVY * uvWidth + dstUVX] = vTileBuffer[y * uvTileW + x];
              }
            }
          }

          uint32_t processed = ++tilesProcessed;
          if (processed % 10 == 0) {
            std::cerr << "[YCbCr] Decoded " << processed << "/" << tileCount << " tiles\n";
          }
        } catch (const std::exception &e) {
          std::cerr << "[YCbCr] Exception decoding tile " << tileIndex << ": " << e.what() << "\n";
          tilesFailed++;
        } catch (...) {
          std::cerr << "[YCbCr] Unknown exception decoding tile " << tileIndex << "\n";
          tilesFailed++;
        }
      }
    };

    // Launch threads
    uint32 tilesPerThread = (tileCount + threadCount - 1) / threadCount;
    for (uint32 t = 0; t < threadCount; ++t) {
      uint32 startTile = t * tilesPerThread;
      uint32 endTile = std::min(startTile + tilesPerThread, tileCount);
      if (startTile >= tileCount) break;
      threads.emplace_back(decodeTileRange, startTile, endTile);
    }

    // Wait for all threads
    for (auto &thread : threads) {
      thread.join();
    }

    std::cerr << "[YCbCr] Tile decoding complete: " << tilesProcessed << " successful, " << tilesFailed << " failed\n";

    // Allow up to 5% tile failure rate (edge tiles may be corrupted in some DNGs)
    if (tilesFailed > tileCount / 20) {
      std::cerr << "[YCbCr] Too many tiles failed, falling back to DNG SDK\n";
      return false;
    }

    // Extract color matrices from negative - same approach as in decode()
    dng_camera_profile_id profileID;
    AutoPtr<dng_color_spec> spec(negative->MakeColorSpec(profileID));

    if (negative->HasCameraNeutral()) {
      spec->SetWhiteXY(spec->NeutralToXY(negative->CameraNeutral()));
    } else if (negative->HasCameraWhiteXY()) {
      spec->SetWhiteXY(negative->CameraWhiteXY());
    }

    // Phase 10.5.2.1: Use ProPhoto matrix like DNG SDK render pipeline
    // DNG SDK's fCameraToRGB = ProPhoto::MatrixFromPCS() * CameraToPCS()
    dng_matrix camToSrgbMat =
        dng_space_ProPhoto::Get().MatrixFromPCS() * spec->CameraToPCS();

    double camToSrgbArray[9];
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) {
        camToSrgbArray[i * 3 + j] = camToSrgbMat[i][j];
      }
    }

    // Call HalidePipeline::processYCbCr to convert YUV420 to RGBA
    int outW, outH;
    int width = static_cast<int>(imageWidth);
    int height = static_cast<int>(imageHeight);

    uint8_t *rgbaOut = HalidePipeline::processYCbCr(
        yPlaneBuffer.data(), uPlaneBuffer.data(), vPlaneBuffer.data(),
        width, height,
        camToSrgbArray, negative->BaselineExposure(),
        DngMetadata(), outW, outH);

    if (!rgbaOut) {
      std::cerr << "[YCbCr] Failed to process YCbCr to RGBA\n";
      return false;
    }

    // YCbCr mode: store RGBA directly in rgbaData
    isYCbCr = true;
    rgbaData.resize((size_t)outW * outH * 4);
    std::memcpy(rgbaData.data(), rgbaOut, rgbaData.size());

    delete[] rgbaOut;

    outMetadata.width = outW;
    outMetadata.height = outH;
    return true;
  }
#endif
};

DngDecoder::DngDecoder() : impl_(std::make_unique<Impl>()) {}
DngDecoder::~DngDecoder() = default;

DngErrorCode DngDecoder::decodeFile(const std::string &filePath,
                                    DngMetadata &outMetadata) {
  return impl_->decode(filePath, outMetadata);
}

DngErrorCode DngDecoder::extractPreviewJPEG(const std::string &filePath,
                                            std::vector<uint8_t> &outJpegData) {
  try {
    dng_host host;
    dng_file_stream stream(filePath.c_str());
    dng_info info;
    info.Parse(host, stream);
    info.PostParse(host);

    int bestPreviewIfd = -1;
    uint32 bestPreviewWidth = 0;

    for (uint32 i = 0; i < info.fIFDCount; i++) {
      const dng_ifd &ifd = *info.fIFD[i];
      if (ifd.fCompression == 7 && ifd.fPhotometricInterpretation == 6 &&
          ifd.fNewSubFileType == 1) {
        if (ifd.fImageWidth > bestPreviewWidth) {
          bestPreviewWidth = ifd.fImageWidth;
          bestPreviewIfd = i;
        }
      }
    }

    if (bestPreviewIfd != -1) {
      const dng_ifd &ifd = *info.fIFD[bestPreviewIfd];
      uint64 offset = ifd.fTileOffset[0];
      uint32 byteCount = ifd.fTileByteCount[0];

      outJpegData.resize(byteCount);
      stream.SetReadPosition(offset);
      stream.Get(outJpegData.data(), byteCount);
      return DngErrorCode::SUCCESS;
    }
    return DngErrorCode::UNSUPPORTED_FORMAT;
  } catch (...) {
    return DngErrorCode::PARSE_ERROR;
  }
}

const uint16_t *DngDecoder::getRawBuffer() const {
  return impl_->bayerData.data();
}

size_t DngDecoder::getRawBufferSize() const {
  return impl_->bayerData.size() * sizeof(uint16_t);
}

const uint8_t *DngDecoder::getRGBABuffer() const {
  return impl_->rgbaData.data();
}

size_t DngDecoder::getRGBABufferSize() const {
  return impl_->rgbaData.size();
}

bool DngDecoder::isYCbCrMode() const {
  return impl_->isYCbCr;
}
