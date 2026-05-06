/*
---
file_summary: "Production DNG pipeline v2 entry used by Flutter FFI; runs SDK Stage1/2, Halide Stage3/4 where applicable, and returns RGB8."
functions:
  - name: "parsePositiveEnvU32"
    description: "Parse positive integer environment variables for optional render thread override."
    lines: "67-73"
  - name: "warpBitExactEnabled"
    description: "Check whether Stage3 must use SDK OpcodeList3 bit-exact behavior."
    lines: "75-78"
  - name: "copyImageToInterleaved16"
    description: "Read a DNG SDK image into a tightly packed uint16 interleaved vector."
    lines: "80-104"
  - name: "makeImageFromInterleaved16"
    description: "Create a DNG SDK image from a tightly packed uint16 interleaved vector."
    lines: "106-124"
  - name: "applyOpcodeList3"
    description: "Apply OpcodeList3 to a Stage3 image, using Halide WarpRectilinear when allowed."
    lines: "126-151"
  - name: "runHalideStage3ForBayer"
    description: "Run production Bayer Stage3 with fused demosaic+WarpRectilinear fast path and SDK-compatible fallbacks."
    lines: "153-252"
  - name: "runSdkStage3"
    description: "Run SDK BuildStage3Image and record timing."
    lines: "254-265"
  - name: "runStage4ToRgb"
    description: "Run Stage4 render through Halide Metal unless disabled by mode/fallback."
    lines: "267-310"
  - name: "dng_pipeline_v2_run_stage3"
    description: "Shared production/test Stage3 orchestration entry."
    lines: "314-328"
  - name: "dng_pipeline_v2_decode_to_rgb"
    description: "Top-level production decode entry: parse DNG, run stages, and return RGB output/timing."
    lines: "330-400"
---
*/
#include "dng_pipeline_v2.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>

#include <dng_color_space.h>
#include <dng_exceptions.h>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_lens_correction.h>
#include <dng_negative.h>
#include <dng_opcodes.h>
#include <dng_pixel_buffer.h>
#include <dng_render.h>

#include "ConcurrentDngHost.h"
#include "dng_mosaic_halide.h"
#include "dng_render_halide.h"
#include "dng_warp_halide.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

uint32_t parsePositiveEnvU32(const char *key) {
  const char *v = std::getenv(key);
  if (!v || !v[0])
    return 0;
  const long parsed = std::strtol(v, nullptr, 10);
  return parsed > 0 ? static_cast<uint32_t>(parsed) : 0;
}

bool warpBitExactEnabled() {
  const char *v = std::getenv("DNG_WARP_BIT_EXACT");
  return v && v[0] && v[0] != '0';
}

bool copyImageToInterleaved16(const dng_image *image, std::vector<uint16_t> &out,
                              uint32_t &width, uint32_t &height,
                              uint32_t &planes) {
  if (!image)
    return false;
  width = image->Width();
  height = image->Height();
  planes = image->Planes();
  if (width == 0 || height == 0 || planes == 0 || image->PixelType() != ttShort)
    return false;

  out.resize(static_cast<size_t>(width) * height * planes);
  dng_pixel_buffer buffer;
  buffer.fArea = image->Bounds();
  buffer.fPlane = 0;
  buffer.fPlanes = planes;
  buffer.fPixelType = ttShort;
  buffer.fPixelSize = sizeof(uint16_t);
  buffer.fData = out.data();
  buffer.fRowStep = static_cast<int32>(width * planes);
  buffer.fColStep = static_cast<int32>(planes);
  buffer.fPlaneStep = 1;
  const_cast<dng_image *>(image)->Get(buffer);
  return true;
}

AutoPtr<dng_image> makeImageFromInterleaved16(dng_host &host, uint32_t width,
                                              uint32_t height, uint32_t planes,
                                              const std::vector<uint16_t> &data) {
  dng_point size(static_cast<int32>(height), static_cast<int32>(width));
  AutoPtr<dng_image> image(host.Make_dng_image(dng_rect(size), planes, ttShort));

  dng_pixel_buffer buffer;
  buffer.fArea = image->Bounds();
  buffer.fPlane = 0;
  buffer.fPlanes = planes;
  buffer.fPixelType = ttShort;
  buffer.fPixelSize = sizeof(uint16_t);
  buffer.fData = const_cast<uint16_t *>(data.data());
  buffer.fRowStep = static_cast<int32>(width * planes);
  buffer.fColStep = static_cast<int32>(planes);
  buffer.fPlaneStep = 1;
  image->Put(buffer);
  return AutoPtr<dng_image>(image.Release());
}

bool applyOpcodeList3(dng_host &host, dng_negative &negative,
                      const dng_opcode_list &opcodeList3,
                      AutoPtr<dng_image> &image) {
  for (uint32_t i = 0; i < opcodeList3.Count(); ++i) {
    dng_opcode &opcode = const_cast<dng_opcode &>(opcodeList3.Entry(i));
    if (!opcode.AboutToApply(host, negative))
      continue;

    bool applied = false;
    if (opcode.OpcodeID() == dngOpcode_WarpRectilinear && !warpBitExactEnabled()) {
      const auto &warpOpcode =
          static_cast<const dng_opcode_WarpRectilinear &>(opcode);
      applied = apply_warp_rectilinear_to_image(host, negative, warpOpcode, image,
                                                WarpRectilinearMode::HALIDE_METAL);
      if (!applied) {
        applied = apply_warp_rectilinear_to_image(host, negative, warpOpcode, image,
                                                  WarpRectilinearMode::HALIDE_CPU);
      }
    }

    if (!applied) {
      opcode.Apply(host, negative, image);
    }
  }
  return true;
}

bool runHalideStage3ForBayer(dng_host &host,
                             dng_negative &negative,
                             DngPipelineStage3Timing *timing) {
  const auto totalStart = Clock::now();
  dng_image *stage2 = const_cast<dng_image *>(negative.Stage2Image());
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t planes = 0;
  std::vector<uint16_t> stage2Data;
  const auto extractStart = Clock::now();
  if (!copyImageToInterleaved16(stage2, stage2Data, width, height, planes) ||
      planes != 1) {
    return false;
  }
  const auto extractEnd = Clock::now();
  if (timing) {
    timing->extract_stage2_ms =
        std::chrono::duration<double, std::milli>(extractEnd - extractStart).count();
  }

  const dng_opcode_list &opcodeList3 = negative.OpcodeList3();
  const auto resizeStart = Clock::now();
  std::vector<uint16_t> stage3Data(static_cast<size_t>(width) * height * 3);
  const auto resizeEnd = Clock::now();
  if (timing) {
    timing->resize_ms =
        std::chrono::duration<double, std::milli>(resizeEnd - resizeStart).count();
  }
  bool fused = false;

  if (!warpBitExactEnabled() && opcodeList3.Count() == 1 &&
      opcodeList3.Entry(0).OpcodeID() == dngOpcode_WarpRectilinear) {
    const auto setupStart = Clock::now();
    const auto &warpOpcode =
        static_cast<const dng_opcode_WarpRectilinear &>(opcodeList3.Entry(0));
    WarpRectilinearParams params;
    if (extractWarpRectilinearParams(warpOpcode, negative.PixelAspectRatio(),
                                     params)) {
      const auto setupEnd = Clock::now();
      if (timing) {
        timing->fast_warp_setup_ms =
            std::chrono::duration<double, std::milli>(setupEnd - setupStart).count();
      }
      const auto fusedStart = Clock::now();
      fused = demosaic_warp_rectilinear_halide(
          stage2Data.data(), static_cast<int>(width), static_cast<int>(height),
          params, WarpRectilinearMode::HALIDE_METAL, stage3Data.data());
      if (!fused) {
        fused = demosaic_warp_rectilinear_halide(
            stage2Data.data(), static_cast<int>(width), static_cast<int>(height),
            params, WarpRectilinearMode::HALIDE_CPU, stage3Data.data());
      }
      const auto fusedEnd = Clock::now();
      if (timing) {
        timing->fused_demosaic_warp_ms =
            std::chrono::duration<double, std::milli>(fusedEnd - fusedStart).count();
      }
    }
  }

  if (!fused) {
    const auto demosaicStart = Clock::now();
    demosaic_ahd_halide(stage2Data.data(), static_cast<int>(width),
                        static_cast<int>(height), stage3Data.data());
    const auto demosaicEnd = Clock::now();
    if (timing) {
      timing->demosaic_ms =
          std::chrono::duration<double, std::milli>(demosaicEnd - demosaicStart).count();
    }
  }

  const auto makeStart = Clock::now();
  AutoPtr<dng_image> stage3 =
      makeImageFromInterleaved16(host, width, height, 3, stage3Data);
  const auto makeEnd = Clock::now();
  if (timing) {
    timing->make_image_ms =
        std::chrono::duration<double, std::milli>(makeEnd - makeStart).count();
  }
  if (!fused) {
    const auto opcodeStart = Clock::now();
    if (!applyOpcodeList3(host, negative, opcodeList3, stage3))
      return false;
    const auto opcodeEnd = Clock::now();
    if (timing) {
      timing->apply_opcode3_ms =
          std::chrono::duration<double, std::milli>(opcodeEnd - opcodeStart).count();
    }
  }
  const auto putStart = Clock::now();
  negative.SetStage3Image(stage3);
  const auto putEnd = Clock::now();
  if (timing) {
    timing->inject_put_ms =
        std::chrono::duration<double, std::milli>(putEnd - putStart).count();
    timing->total_ms =
        std::chrono::duration<double, std::milli>(putEnd - totalStart).count();
  }
  return true;
}

bool runSdkStage3(dng_host &host,
                  dng_negative &negative,
                  DngPipelineStage3Timing *timing) {
  const auto start = Clock::now();
  negative.BuildStage3Image(host);
  const auto end = Clock::now();
  if (timing) {
    timing->sdk_build_ms = std::chrono::duration<double, std::milli>(end - start).count();
    timing->total_ms = timing->sdk_build_ms;
  }
  return true;
}

bool runStage4ToRgb(dng_host &host, dng_negative &negative,
                    uint32_t inputWidth, uint32_t inputHeight,
                    std::vector<uint8_t> &rgb, uint32_t &outW,
                    uint32_t &outH) {
  dng_host *renderHost = &host;
  std::unique_ptr<ConcurrentDngHost> renderHostOverride;
  const uint32_t renderThreads = parsePositiveEnvU32("DNG_RENDER_AREA_THREADS");
  if (renderThreads > 0 && renderThreads != host.PerformAreaTaskThreads()) {
    renderHostOverride = std::make_unique<ConcurrentDngHost>(renderThreads);
    renderHost = renderHostOverride.get();
  }

  dng_render renderer(*renderHost, negative);
  renderer.SetMaximumSize(std::max(inputWidth, inputHeight));
  renderer.SetFinalPixelType(ttByte);
  renderer.SetFinalSpace(dng_space_sRGB::Get());

  rgb.resize(static_cast<size_t>(inputWidth) * inputHeight * 3);
  bool ok = render_stage4_halide(*renderHost, negative, renderer,
                                 RenderHalideMode::HALIDE_METAL, rgb, outW,
                                 outH);
  if (ok)
    return true;

  AutoPtr<dng_image> finalImage(renderer.Render());
  if (!finalImage.Get())
    return false;
  outW = finalImage->Width();
  outH = finalImage->Height();
  rgb.resize(static_cast<size_t>(outW) * outH * 3);

  dng_pixel_buffer buffer;
  buffer.fArea = finalImage->Bounds();
  buffer.fPlane = 0;
  buffer.fPlanes = 3;
  buffer.fPixelType = ttByte;
  buffer.fPixelSize = 1;
  buffer.fData = rgb.data();
  buffer.fRowStep = static_cast<int32>(outW * 3);
  buffer.fColStep = 3;
  buffer.fPlaneStep = 1;
  finalImage->Get(buffer);
  return true;
}

} // namespace

bool dng_pipeline_v2_run_stage3(dng_host &host,
                                dng_negative &negative,
                                bool use_halide_bayer,
                                DngPipelineStage3Timing *timing) {
  if (timing) {
    *timing = DngPipelineStage3Timing{};
  }
  if (use_halide_bayer && !warpBitExactEnabled()) {
    if (runHalideStage3ForBayer(host, negative, timing)) {
      return true;
    }
    std::cerr << "[PipelineV2] Halide Stage3 failed; falling back to SDK Stage3\n";
  }
  return runSdkStage3(host, negative, timing);
}

bool dng_pipeline_v2_decode_to_rgb(const char *file_path,
                                   DngPipelineV2Result &result) {
  result = DngPipelineV2Result{};
  if (!file_path || !file_path[0]) {
    result.error_code = -1;
    return false;
  }

  try {
    dng_host host;
    dng_file_stream stream(file_path);

    dng_info info;
    info.Parse(host, stream);
    info.PostParse(host);
    if (!info.IsValidDNG() || info.fMainIndex >= info.fIFDCount) {
      result.error_code = -2;
      return false;
    }

    dng_negative *negativeRaw = host.Make_dng_negative();
    AutoPtr<dng_negative> negative(negativeRaw);
    negative->Parse(host, stream, info);
    negative->PostParse(host, stream, info);

    const dng_ifd &rawIFD = *info.fIFD[info.fMainIndex];
    const bool isBayer = rawIFD.fPhotometricInterpretation == piCFA;
    const uint32_t inputWidth = rawIFD.fImageWidth;
    const uint32_t inputHeight = rawIFD.fImageLength;

    const auto decodeStart = Clock::now();
    negative->ReadStage1Image(host, stream, info);
    negative->BuildStage2Image(host);

    DngPipelineStage3Timing stage3Timing;
    bool stage3Ok =
        dng_pipeline_v2_run_stage3(host, *negative, isBayer, &stage3Timing);
    const auto decodeEnd = Clock::now();
    result.decode_ms =
        std::chrono::duration<double, std::milli>(decodeEnd - decodeStart).count();

    if (!stage3Ok) {
      result.error_code = -3;
      return false;
    }

    const auto processStart = Clock::now();
    if (!runStage4ToRgb(host, *negative, inputWidth, inputHeight, result.rgb,
                        result.width, result.height)) {
      result.error_code = -4;
      return false;
    }
    const auto processEnd = Clock::now();
    result.process_ms =
        std::chrono::duration<double, std::milli>(processEnd - processStart).count();
    result.error_code = 0;
    return true;
  } catch (const dng_exception &e) {
    result.error_code = e.ErrorCode();
    std::cerr << "[PipelineV2] DNG exception: " << result.error_code << "\n";
    return false;
  } catch (const std::exception &e) {
    result.error_code = -100;
    std::cerr << "[PipelineV2] Exception: " << e.what() << "\n";
    return false;
  } catch (...) {
    result.error_code = -101;
    std::cerr << "[PipelineV2] Unknown exception\n";
    return false;
  }
}
