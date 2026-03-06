#include "DngDecoder.h"
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
  p.parsed = true;
  std::cerr << "[DBG] LR Params: Exposure2012=" << p.exposure2012
            << " Contrast2012=" << p.contrast2012
            << " Saturation=" << p.saturation << " Vibrance=" << p.vibrance
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

class DngDecoder::Impl {
public:
  std::vector<uint16_t> bayerData;

  DngErrorCode decode(const std::string &filePath, DngMetadata &outMetadata) {
    try {
      dng_host host;
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
          }
        }

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

        for (int i = 0; i < 3; ++i)
          for (int j = 0; j < 3; ++j)
            outMetadata.camToSrgb[i * 3 + j] = camToSrgb[i][j];

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

      // Build stage 1 image (Raw Bayer)
      std::cerr << "[DBG] Reading stage1 image...\n";
      negative->ReadStage1Image(host, stream, info);

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
};

DngDecoder::DngDecoder() : impl_(std::make_unique<Impl>()) {}
DngDecoder::~DngDecoder() = default;

DngErrorCode DngDecoder::decodeFile(const std::string &filePath,
                                    DngMetadata &outMetadata) {
  return impl_->decode(filePath, outMetadata);
}

const uint16_t *DngDecoder::getRawBuffer() const {
  return impl_->bayerData.data();
}

size_t DngDecoder::getRawBufferSize() const {
  return impl_->bayerData.size() * sizeof(uint16_t);
}
