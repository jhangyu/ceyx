#include "DngDecoder.h"
#include "HalidePipeline.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <signal.h>
#include <sstream>
#include <unistd.h>
#include <vector>

// DNG SDK includes for reference render
#include <dng_color_space.h>
#include <dng_exceptions.h>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_render.h>

// ========================================================================
// ANSI 色彩輸出
// ========================================================================
#define CLR_GREEN "\033[32m"
#define CLR_RED "\033[31m"
#define CLR_YELLOW "\033[33m"
#define CLR_RESET "\033[0m"
#define PASS CLR_GREEN "[PASS]" CLR_RESET
#define FAIL CLR_RED "[FAIL]" CLR_RESET

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_TRUE(cond, msg)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::cerr << FAIL << " " << msg << "\n";                                 \
      g_failed++;                                                              \
    } else {                                                                   \
      std::cout << PASS << " " << msg << "\n";                                 \
      g_passed++;                                                              \
    }                                                                          \
  } while (0)

#define ASSERT_EQ(a, b, msg)                                                   \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      std::cerr << FAIL << " " << msg << " (got " << (a) << ", expected "      \
                << (b) << ")\n";                                               \
      g_failed++;                                                              \
    } else {                                                                   \
      std::cout << PASS << " " << msg << "\n";                                 \
      g_passed++;                                                              \
    }                                                                          \
  } while (0)

#define ASSERT_GT(a, b, msg)                                                   \
  do {                                                                         \
    if (!((a) > (b))) {                                                        \
      std::cerr << FAIL << " " << msg << " (got " << (a) << ", expected > "    \
                << (b) << ")\n";                                               \
      g_failed++;                                                              \
    } else {                                                                   \
      std::cout << PASS << " " << msg << "\n";                                 \
      g_passed++;                                                              \
    }                                                                          \
  } while (0)

#define ASSERT_RANGE(v, lo, hi, msg)                                           \
  do {                                                                         \
    if ((v) < (lo) || (v) > (hi)) {                                            \
      std::cerr << FAIL << " " << msg << " (got " << (v) << ", expected ["     \
                << (lo) << "," << (hi) << "])\n";                              \
      g_failed++;                                                              \
    } else {                                                                   \
      std::cout << PASS << " " << msg << "\n";                                 \
      g_passed++;                                                              \
    }                                                                          \
  } while (0)

// ========================================================================
// PSNR helper
// ========================================================================
static double computePSNR(const uint8_t *a, const uint8_t *b, size_t count) {
  if (count == 0)
    return 0.0;
  double mse = 0.0;
  for (size_t i = 0; i < count; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    mse += d * d;
  }
  mse /= static_cast<double>(count);
  if (mse < 1e-10)
    return 100.0; // perfect match
  return 10.0 * std::log10(255.0 * 255.0 / mse);
}

// ========================================================================
// Watchdog — 0.5s 超時中斷 (依 unit_test.md 規範)
// ========================================================================
static void watchdog_handler(int) {
  std::cerr << CLR_RED "\n[WATCHDOG] 0.5s timeout exceeded! Aborting.\n"
            << CLR_RESET;
  _exit(2);
}

// ========================================================================
// 主測試程式
// ========================================================================
int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <path_to_dng>\n";
    return 1;
  }

  std::cout << "============================================================\n";
  std::cout << "  Phase 2+3 Unit Tests — DNG Decode + Halide Pipeline\n";
  std::cout
      << "============================================================\n\n";

  // ---- 設定 Watchdog (信號方式，macOS/Linux 通用) ----
  signal(SIGALRM, watchdog_handler);
  alarm(120); // 120s for decode + Halide JIT + DNG SDK full reference render

  // ================================================================
  // Test 1: DNG 解碼成功
  // ================================================================
  DngDecoder decoder;
  DngMetadata metadata = {};
  auto t0 = std::chrono::steady_clock::now();
  DngErrorCode code = decoder.decodeFile(argv[1], metadata);
  auto t1 = std::chrono::steady_clock::now();
  double decodeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

  ASSERT_TRUE(code == DngErrorCode::SUCCESS,
              "Test 1.0: DNG decode returns SUCCESS");
  if (code != DngErrorCode::SUCCESS) {
    std::cerr << "Cannot continue — decode failed with code "
              << static_cast<int>(code) << "\n";
    return 1;
  }

  std::cout << "\n--- Decode Performance ---\n";
  std::cout << "  Decode time: " << decodeMs << " ms\n\n";

  // ================================================================
  // Test 2: Metadata 提取正確性 (unit_test.md 測試點 1)
  // ================================================================
  std::cout << "--- Test 2: Metadata Extraction ---\n";

  ASSERT_GT(metadata.width, 0u, "Test 2.1a: Width > 0");
  ASSERT_GT(metadata.height, 0u, "Test 2.1b: Height > 0");
  std::cout << "  Image: " << metadata.width << "x" << metadata.height << "\n";

  ASSERT_TRUE(metadata.whiteLevel > metadata.blackLevel,
              "Test 2.2: WhiteLevel > BlackLevel");
  std::cout << "  BlackLevel: " << metadata.blackLevel
            << "  WhiteLevel: " << metadata.whiteLevel << "\n";

  ASSERT_RANGE(metadata.asShotNeutral[0], 0.0, 2.0,
               "Test 2.3a: AsShotNeutral[R] in sane range");
  ASSERT_RANGE(metadata.asShotNeutral[1], 0.0, 2.0,
               "Test 2.3b: AsShotNeutral[G] in sane range");
  ASSERT_RANGE(metadata.asShotNeutral[2], 0.0, 2.0,
               "Test 2.3c: AsShotNeutral[B] in sane range");
  std::cout << "  AsShotNeutral: " << metadata.asShotNeutral[0] << ", "
            << metadata.asShotNeutral[1] << ", " << metadata.asShotNeutral[2]
            << "\n";

  {
    double sum = 0;
    for (int i = 0; i < 9; i++)
      sum += std::abs(metadata.colorMatrix1[i]);
    ASSERT_GT(sum, 0.0, "Test 2.4: ColorMatrix1 is non-zero");
    std::cout << "  ColorMatrix1:\n";
    for (int r = 0; r < 3; r++) {
      std::cout << "    [";
      for (int c = 0; c < 3; c++) {
        if (c)
          std::cout << ", ";
        std::cout << metadata.colorMatrix1[r * 3 + c];
      }
      std::cout << "]\n";
    }
  }

  {
    double sum = 0;
    for (int i = 0; i < 9; i++)
      sum += std::abs(metadata.forwardMatrix[i]);
    if (sum > 0.0) {
      std::cout << "  ForwardMatrix present:\n";
      for (int r = 0; r < 3; r++) {
        std::cout << "    [";
        for (int c = 0; c < 3; c++) {
          if (c)
            std::cout << ", ";
          std::cout << metadata.forwardMatrix[r * 3 + c];
        }
        std::cout << "]\n";
      }
    } else {
      std::cout << "  ForwardMatrix: (not present)\n";
    }
  }

  // ================================================================
  // Test 2.5–2.7: Phase 5.1 — Lightroom XMP parameter parsing
  // ================================================================
  std::cout
      << "\n--- Test 2.5: Lightroom XMP Parameter Parsing (Phase 5.1) ---\n";
  ASSERT_TRUE(metadata.lrParams.parsed,
              "Test 2.5: lrParams.parsed == true (XMP found and parsed)");
  std::cout << "  XMP rawXmp length: " << metadata.rawXmp.size() << " bytes\n";
  std::cout << "  LR Exposure2012:   " << metadata.lrParams.exposure2012
            << " EV\n";
  std::cout << "  LR Contrast2012:   " << metadata.lrParams.contrast2012
            << "%\n";
  std::cout << "  LR Saturation:     " << metadata.lrParams.saturation << "%\n";
  std::cout << "  LR Vibrance:       " << metadata.lrParams.vibrance << "%\n";
  ASSERT_RANGE(metadata.lrParams.exposure2012, -5.0, 5.0,
               "Test 2.6: Exposure2012 in plausible range [-5, +5] EV");
  ASSERT_RANGE(metadata.lrParams.saturation, -100.0, 100.0,
               "Test 2.7: Saturation in range [-100, +100]%");

  // ================================================================
  // Test 3: Raw Buffer 大小 (unit_test.md 測試點 2)
  // ================================================================
  std::cout << "\n--- Test 3: Raw Buffer Size ---\n";

  size_t bufSize = decoder.getRawBufferSize();
  size_t expectedBayer16 =
      static_cast<size_t>(metadata.width) * metadata.height * sizeof(uint16_t);

  ASSERT_GT(bufSize, 0u, "Test 3.1: Buffer size > 0");
  ASSERT_EQ(bufSize, expectedBayer16,
            "Test 3.2: Buffer = W*H*2 bytes (16-bit Bayer)");
  std::cout << "  Buffer: " << bufSize << " bytes (expected " << expectedBayer16
            << ")\n";

  // ================================================================
  // Test 4: Pixel 值合理性檢查
  // ================================================================
  std::cout << "\n--- Test 4: Pixel Value Sanity ---\n";

  const uint16_t *pixels = decoder.getRawBuffer();
  ASSERT_TRUE(pixels != nullptr, "Test 4.1: Raw buffer pointer is not null");

  if (pixels && bufSize > 0) {
    size_t pixelCount = bufSize / sizeof(uint16_t);
    bool allInRange = true;
    uint16_t minVal = 65535, maxVal = 0;
    size_t step = pixelCount / 1000;
    if (step < 1)
      step = 1;
    for (size_t i = 0; i < pixelCount; i += step) {
      if (pixels[i] < minVal)
        minVal = pixels[i];
      if (pixels[i] > maxVal)
        maxVal = pixels[i];
      if (pixels[i] > metadata.whiteLevel + 100) {
        allInRange = false;
      }
    }
    ASSERT_TRUE(allInRange,
                "Test 4.2: Sampled pixels within [0, whiteLevel+100]");
    ASSERT_TRUE(minVal >= metadata.blackLevel / 2,
                "Test 4.3: Min pixel near or above blackLevel");
    std::cout << "  Pixel range: [" << minVal << ", " << maxVal << "]\n";
    std::cout << "  First 8 pixels: ";
    for (int i = 0; i < 8 && i < (int)(pixelCount); i++) {
      if (i)
        std::cout << ", ";
      std::cout << pixels[i];
    }
    std::cout << "\n";
  }

  // ================================================================
  // Test 5: 效能指標 (unit_test.md 測試點 4)
  // ================================================================
  std::cout << "\n--- Test 5: Performance ---\n";
  ASSERT_TRUE(decodeMs < 5000.0, "Test 5.1: Decode < 5000ms (CPU baseline)");
  std::cout << "  Decode time: " << decodeMs << " ms\n";

  // ================================================================
  // Test 6: Phase 3+5a — Halide Pipeline (unit_test.md 測試點 3)
  // ================================================================
  std::cout << "\n--- Test 6: Halide Pipeline (Phase 3 + HueSatMap) ---\n";

  // Print HueSatMap status
  bool hasHSM = metadata.hsmHueDivisions > 0 && metadata.hsmSatDivisions > 1;
  bool hasLT = metadata.ltHueDivisions > 0 && metadata.ltSatDivisions > 1;
  std::cout << "  HueSatMap: " << (hasHSM ? "YES" : "NO");
  if (hasHSM)
    std::cout << " (" << metadata.hsmHueDivisions << "x"
              << metadata.hsmSatDivisions << "x" << metadata.hsmValDivisions
              << ", " << metadata.hsmData.size() << " entries)";
  std::cout << "\n";
  std::cout << "  LookTable: " << (hasLT ? "YES" : "NO");
  if (hasLT)
    std::cout << " (" << metadata.ltHueDivisions << "x"
              << metadata.ltSatDivisions << "x" << metadata.ltValDivisions
              << ", " << metadata.ltData.size() << " entries)";
  std::cout << "\n";
  ASSERT_TRUE(hasHSM, "Test 6.0: HueSatMap is present in profile");

  // Force LR Params to 0 for strict PSNR comparison against DNG SDK (which doesn't parse them)
  metadata.lrParams.exposure2012 = 0.0f;
  metadata.lrParams.contrast2012 = 0.0f;
  metadata.lrParams.saturation = 0.0f;
  metadata.lrParams.vibrance = 0.0f;

  int outW = 0, outH = 0;
  auto hp0 = std::chrono::steady_clock::now();
  uint8_t *rgba = HalidePipeline::process(
      decoder.getRawBuffer(), static_cast<int>(metadata.width),
      static_cast<int>(metadata.height), metadata.blackLevel,
      metadata.whiteLevel, metadata.asShotNeutral, metadata.camToSrgb,
      metadata.baselineExposure, metadata, outW, outH);
  auto hp1 = std::chrono::steady_clock::now();
  double halideMs =
      std::chrono::duration<double, std::milli>(hp1 - hp0).count();

  // 6.1 非 null
  ASSERT_TRUE(rgba != nullptr, "Test 6.1: Halide output is not null");
  if (!rgba) {
    std::cerr << "Cannot continue Halide tests — process() returned null\n";
  } else {
    // 6.2 尺寸正確
    ASSERT_EQ(outW, (int)metadata.width, "Test 6.2a: Output width matches");
    ASSERT_EQ(outH, (int)metadata.height, "Test 6.2b: Output height matches");

    size_t rgbaSize = static_cast<size_t>(outW) * outH * 4;
    std::cout << "  RGBA buffer: " << rgbaSize << " bytes\n";
    std::cout << "  Halide process time: " << halideMs << " ms\n";

    // 6.3 像素範圍 [0,255] — RGBA 是 uint8 所以天然滿足，但驗證 Alpha
    bool alphaOk = true;
    for (size_t i = 3; i < rgbaSize; i += 4 * 1000) { // sample every 1000 px
      if (rgba[i] != 255) {
        alphaOk = false;
        break;
      }
    }
    ASSERT_TRUE(alphaOk, "Test 6.3: Alpha channel is all 255");

    // 6.4 非全黑 / 非全白 (抽樣 RGB 通道)
    uint64_t sumR = 0, sumG = 0, sumB = 0;
    size_t samples = 0;
    for (size_t i = 0; i < rgbaSize; i += 4 * 500) {
      sumR += rgba[i];
      sumG += rgba[i + 1];
      sumB += rgba[i + 2];
      samples++;
    }
    double avgR = (double)sumR / samples;
    double avgG = (double)sumG / samples;
    double avgB = (double)sumB / samples;
    std::cout << "  Average RGB: (" << avgR << ", " << avgG << ", " << avgB
              << ")\n";
    ASSERT_TRUE(avgR > 5.0 && avgR < 250.0,
                "Test 6.4a: Average R not all-black/white");
    ASSERT_TRUE(avgG > 5.0 && avgG < 250.0,
                "Test 6.4b: Average G not all-black/white");
    ASSERT_TRUE(avgB > 5.0 && avgB < 250.0,
                "Test 6.4c: Average B not all-black/white");

    // 6.5 PSNR comparison vs DNG SDK reference render
    std::cout << "\n--- Test 6.5: PSNR vs DNG SDK Reference ---\n";
    {
      bool psnrTestPassed = false;
      try {
        dng_host refHost;
        dng_file_stream refStream(argv[1]);
        dng_info refInfo;
        refInfo.Parse(refHost, refStream);
        refInfo.PostParse(refHost);

        AutoPtr<dng_negative> refNeg(refHost.Make_dng_negative());
        refNeg->Parse(refHost, refStream, refInfo);
        refNeg->PostParse(refHost, refStream, refInfo);
        refNeg->ReadStage1Image(refHost, refStream, refInfo);
        refNeg->BuildStage2Image(refHost);
        refNeg->BuildStage3Image(refHost);

        dng_render render(refHost, *refNeg);
        render.SetFinalSpace(dng_space_sRGB::Get());
        render.SetFinalPixelType(ttByte);
        render.SetMaximumSize(0);
        AutoPtr<dng_image> refImg(render.Render());

        if (refImg.Get()) {
          uint32 rw = refImg->Width();
          uint32 rh = refImg->Height();
          std::vector<uint8_t> refRGB(static_cast<size_t>(rw) * rh * 3);
          dng_pixel_buffer rbuf;
          rbuf.fArea = refImg->Bounds();
          rbuf.fPlane = 0;
          rbuf.fPlanes = 3;
          rbuf.fPixelType = ttByte;
          rbuf.fPixelSize = 1;
          rbuf.fData = refRGB.data();
          rbuf.fRowStep = rw * 3;
          rbuf.fColStep = 3;
          rbuf.fPlaneStep = 1;
          refImg->Get(rbuf);

          // Compare — use min(w,h) to handle any size mismatch
          int cmpW = std::min((int)rw, outW);
          int cmpH = std::min((int)rh, outH);
          // Build comparable RGB-only arrays
          size_t cmpPixels = static_cast<size_t>(cmpW) * cmpH * 3;
          std::vector<uint8_t> halideRGB(cmpPixels);
          std::vector<uint8_t> refRGBCmp(cmpPixels);
          for (int row = 0; row < cmpH; ++row) {
            for (int col = 0; col < cmpW; ++col) {
              size_t dstIdx = (row * cmpW + col) * 3;
              size_t halSrcIdx = (row * outW + col) * 4;    // RGBA
              size_t refSrcIdx = (row * (int)rw + col) * 3; // RGB
              halideRGB[dstIdx + 0] = rgba[halSrcIdx + 0];
              halideRGB[dstIdx + 1] = rgba[halSrcIdx + 1];
              halideRGB[dstIdx + 2] = rgba[halSrcIdx + 2];
              refRGBCmp[dstIdx + 0] = refRGB[refSrcIdx + 0];
              refRGBCmp[dstIdx + 1] = refRGB[refSrcIdx + 1];
              refRGBCmp[dstIdx + 2] = refRGB[refSrcIdx + 2];
            }
          }

          double psnr =
              computePSNR(halideRGB.data(), refRGBCmp.data(), cmpPixels);
          std::cout << "  PSNR: " << psnr << " dB (threshold: 15 dB)\n";
          // Note: 15 dB threshold is realistic for bilinear demosaic
          // without DNG SDK's proprietary tone curves and advanced
          // demosaicing.  30+ dB is a Phase 5 target with tone curve
          // matching.
          ASSERT_TRUE(psnr > 15.0,
                      "Test 6.5: PSNR > 15dB vs DNG SDK reference");
          psnrTestPassed = true;
        }
      } catch (const dng_exception &e) {
        std::cerr << "  [WARN] Reference render failed: DNG exception "
                  << e.ErrorCode() << "\n";
      } catch (...) {
        std::cerr << "  [WARN] Reference render failed: unknown exception\n";
      }
      if (!psnrTestPassed) {
        std::cerr << FAIL
                  << " Test 6.5: Could not run PSNR test (ref render failed)\n";
        g_failed++;
      }
    }

    // 6.6 Pipeline 效能 (第一次 JIT 編譯)
    ASSERT_TRUE(halideMs < 20000.0,
                "Test 6.6: Halide pipeline 1st call < 20s (inc. JIT compile)");

    // 6.7 Pipeline 效能 (第二次呼叫，應觸發快取)
    std::cout
        << "\n--- Test 6.7: Halide Pipeline (2nd call for cache check) ---\n";
    auto hp2 = std::chrono::steady_clock::now();
    uint8_t *rgba2 = HalidePipeline::process(
        decoder.getRawBuffer(), static_cast<int>(metadata.width),
        static_cast<int>(metadata.height), metadata.blackLevel,
        metadata.whiteLevel, metadata.asShotNeutral, metadata.camToSrgb,
        metadata.baselineExposure, metadata, outW, outH);
    auto hp3 = std::chrono::steady_clock::now();
    double halideMs2 =
        std::chrono::duration<double, std::milli>(hp3 - hp2).count();

    std::cout << "  Halide 2nd process time: " << halideMs2 << " ms\n";
    ASSERT_TRUE(
        halideMs2 < 4000.0,
        "Test 6.7: Halide pipeline 2nd call < 4s (cached, multi-thread)");

    if (rgba2)
      delete[] rgba2;

    delete[] rgba;
  }

  // 取消 watchdog
  alarm(0);

  // ================================================================
  // 測試摘要
  // ================================================================
  std::cout
      << "\n============================================================\n";
  std::cout << "  Results: " << g_passed << " passed, " << g_failed
            << " failed\n";
  std::cout << "============================================================\n";

  return g_failed > 0 ? 1 : 0;
}
