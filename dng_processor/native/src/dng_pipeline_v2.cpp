/*
---
file_summary: "Production DNG pipeline v2 entry used by Flutter FFI; runs SDK Stage1/2, Halide Stage3/4 where applicable, and returns RGB8."
functions:
  - name: "copyImageToInterleaved16 / makeImageFromInterleaved16 / allocStage3Image / putStage3Data"
    description: "Stage3 image <-> uint16 interleaved buffer helpers; alloc/put are split so latency hiding can pre-allocate while GPU runs."
    lines: "63-130"
  - name: "applyOpcodeList3"
    description: "Apply OpcodeList3 to a Stage3 image, using Halide WarpRectilinear when config allows."
    lines: "132-160"
  - name: "prepareStage3Workspace"
    description: "Reuse caller-owned Stage3 output storage, preserving the prealloc contract across shared orchestration."
    lines: "162-177"
  - name: "runHalideStage3ForBayer"
    description: "Run production Bayer Stage3 with fused demosaic+WarpRectilinear fast path; Phase 8.2.1 Path D — async dispatch + Make_dng_image overlap to hide GPU sync_wait."
    lines: "179-310"
  - name: "runSdkStage3 / runStage4ToRgb"
    description: "SDK Stage3 fallback and Stage4 render through Halide Metal."
    lines: "312-370"
  - name: "dng_pipeline_v2_run_stage3 / dng_pipeline_v2_decode_to_rgb"
    description: "Shared Stage3 orchestration + top-level decode entry returning RGB and timing."
    lines: "372-470"
---
*/
#include "dng_pipeline_v2.h"

#include <chrono>
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

AutoPtr<dng_image> allocStage3Image(dng_host &host, uint32_t width,
                                    uint32_t height, uint32_t planes) {
  dng_point size(static_cast<int32>(height), static_cast<int32>(width));
  return AutoPtr<dng_image>(
      host.Make_dng_image(dng_rect(size), planes, ttShort));
}

void putStage3Data(dng_image &image, const std::vector<uint16_t> &data,
                   uint32_t width, uint32_t height, uint32_t planes) {
  (void)height;
  dng_pixel_buffer buffer;
  buffer.fArea = image.Bounds();
  buffer.fPlane = 0;
  buffer.fPlanes = planes;
  buffer.fPixelType = ttShort;
  buffer.fPixelSize = sizeof(uint16_t);
  buffer.fData = const_cast<uint16_t *>(data.data());
  buffer.fRowStep = static_cast<int32>(width * planes);
  buffer.fColStep = static_cast<int32>(planes);
  buffer.fPlaneStep = 1;
  image.Put(buffer);
}

bool applyOpcodeList3(dng_host &host, dng_negative &negative,
                      const dng_opcode_list &opcodeList3,
                      const PipelineConfig &config,
                      AutoPtr<dng_image> &image) {
  for (uint32_t i = 0; i < opcodeList3.Count(); ++i) {
    dng_opcode &opcode = const_cast<dng_opcode &>(opcodeList3.Entry(i));
    if (!opcode.AboutToApply(host, negative))
      continue;

    bool applied = false;
    if (opcode.OpcodeID() == dngOpcode_WarpRectilinear &&
        !config.debug.warp_bit_exact) {
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

std::vector<uint16_t> &prepareStage3Workspace(std::vector<uint16_t> &fallback,
                                              std::vector<uint16_t> *callerWorkspace,
                                              size_t elements,
                                              DngPipelineStage3Timing *timing) {
  std::vector<uint16_t> &workspace = callerWorkspace ? *callerWorkspace : fallback;
  const auto resizeStart = Clock::now();
  if (workspace.size() != elements) {
    workspace.resize(elements);
  }
  const auto resizeEnd = Clock::now();
  if (timing) {
    timing->resize_ms =
        std::chrono::duration<double, std::milli>(resizeEnd - resizeStart).count();
  }
  return workspace;
}

bool runHalideStage3ForBayer(dng_host &host,
                             dng_negative &negative,
                             const PipelineConfig &config,
                             DngPipelineStage3Timing *timing,
                             std::vector<uint16_t> *stage3Workspace) {
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
  std::vector<uint16_t> localStage3Data;
  std::vector<uint16_t> &stage3Data =
      prepareStage3Workspace(localStage3Data, stage3Workspace,
                             static_cast<size_t>(width) * height * 3, timing);
  bool fused = false;
  AutoPtr<dng_image> stage3;
  bool stage3Allocated = false;

  if (!config.debug.warp_bit_exact && config.debug.fused_demosaic_warp &&
      opcodeList3.Count() == 1 &&
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

      // Path D — async dispatch the fused Metal kernel, then run
      // CPU-only Make_dng_image while the GPU is in flight, then sync+copy.
      const auto fusedStart = Clock::now();
      DemosaicWarpHalideHandle *handle =
          demosaic_warp_rectilinear_halide_dispatch(
              stage2Data.data(), static_cast<int>(width),
              static_cast<int>(height), params,
              WarpRectilinearMode::HALIDE_METAL, stage3Data.data());
      if (!handle) {
        handle = demosaic_warp_rectilinear_halide_dispatch(
            stage2Data.data(), static_cast<int>(width),
            static_cast<int>(height), params,
            WarpRectilinearMode::HALIDE_CPU, stage3Data.data());
      }

      if (handle) {
        // Latency hiding: allocate the dng_image container while the GPU
        // kernel runs. Make_dng_image is a CPU-side malloc that does not
        // depend on Stage3 pixel data.
        const auto makeStart = Clock::now();
        stage3.Reset(
            allocStage3Image(host, width, height, 3).Release());
        const auto makeEnd = Clock::now();
        if (timing) {
          timing->make_image_ms =
              std::chrono::duration<double, std::milli>(makeEnd - makeStart)
                  .count();
        }
        stage3Allocated = true;

        fused = demosaic_warp_rectilinear_halide_finish(handle);
        if (!fused) {
          // finish destroys the handle on both success and failure; nothing
          // to clean up here. Drop the prematurely-allocated stage3 so the
          // non-fused fallback below can re-build it normally.
          stage3.Reset();
          stage3Allocated = false;
        }
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

  if (fused && stage3Allocated) {
    // Stage3 image was pre-allocated during latency hiding; copy pixels in.
    const auto putStart = Clock::now();
    putStage3Data(*stage3.Get(), stage3Data, width, height, 3);
    const auto putEnd = Clock::now();
    if (timing) {
      timing->make_image_ms +=
          std::chrono::duration<double, std::milli>(putEnd - putStart).count();
    }
  } else {
    const auto makeStart = Clock::now();
    stage3.Reset(
        makeImageFromInterleaved16(host, width, height, 3, stage3Data)
            .Release());
    const auto makeEnd = Clock::now();
    if (timing) {
      timing->make_image_ms =
          std::chrono::duration<double, std::milli>(makeEnd - makeStart).count();
    }
  }
  if (!fused) {
    const auto opcodeStart = Clock::now();
    if (!applyOpcodeList3(host, negative, opcodeList3, config, stage3))
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

// Forward declaration needed because runHalideStage3And4Fused is defined before
// runStage4ToRgb but may call it in its fallback path.
bool runStage4ToRgb(dng_host &host, dng_negative &negative,
                    const PipelineConfig &config,
                    uint32_t inputWidth, uint32_t inputHeight,
                    std::vector<uint8_t> &rgb, uint32_t &outW, uint32_t &outH);

// Phase 8.2.2 — Stage3→Stage4 GPU device handoff.
// Dispatches Stage3 (async), does Stage4 CPU prep while GPU runs, then calls
// Stage4 directly from the device buffer (no Stage3 copy_to_host).
// Returns true when BOTH Stage3 and Stage4 completed (via device path or
// finish()+Stage4 fallback).  Returns false only when pre-conditions are not
// met and no work was started — caller should run the normal Stage3+Stage4.
bool runHalideStage3And4Fused(dng_host &host,
                               dng_negative &negative,
                               const PipelineConfig &config,
                               uint32_t inputWidth,
                               uint32_t inputHeight,
                               DngPipelineStage3Timing *timing,
                               std::vector<uint16_t> *stage3Workspace,
                               std::vector<uint8_t> &rgb,
                               uint32_t &outW,
                               uint32_t &outH) {
  if (config.debug.warp_bit_exact || config.debug.demosaic_bit_exact)
    return false;
  if (!config.debug.fused_demosaic_warp || !config.debug.stage3_stage4_device_handoff)
    return false;

  const dng_opcode_list &opcodeList3 = negative.OpcodeList3();
  if (opcodeList3.Count() != 1 ||
      opcodeList3.Entry(0).OpcodeID() != dngOpcode_WarpRectilinear)
    return false;

  dng_image *stage2 = const_cast<dng_image *>(negative.Stage2Image());
  uint32_t width = 0, height = 0, planes = 0;
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

  const auto &warpOpcode =
      static_cast<const dng_opcode_WarpRectilinear &>(opcodeList3.Entry(0));
  WarpRectilinearParams warpParams;
  if (!extractWarpRectilinearParams(warpOpcode, negative.PixelAspectRatio(),
                                    warpParams))
    return false;

  std::vector<uint16_t> localStage3Data;
  std::vector<uint16_t> &stage3Data = prepareStage3Workspace(
      localStage3Data, stage3Workspace,
      static_cast<size_t>(width) * height * 3, timing);

  const auto fusedStart = Clock::now();

  DemosaicWarpHalideHandle *handle = demosaic_warp_rectilinear_halide_dispatch(
      stage2Data.data(), static_cast<int>(width), static_cast<int>(height),
      warpParams, WarpRectilinearMode::HALIDE_METAL, stage3Data.data());
  if (!handle)
    return false;

  // CPU work while GPU runs Stage3 (latency hiding):
  // 1. Pre-alloc Stage4 output buffer (avoids first-touch page fault)
  const size_t stage4OutSize = static_cast<size_t>(inputWidth) * inputHeight * 3;
  if (rgb.size() != stage4OutSize)
    rgb.resize(stage4OutSize);

  // 2. Allocate stub Stage3 image for negative.SetStage3Image() integrity
  AutoPtr<dng_image> stage3Stub;
  stage3Stub.Reset(allocStage3Image(host, width, height, 3).Release());

  // 3. Setup Stage4 renderer
  dng_render renderer(host, negative);
  renderer.SetMaximumSize(std::max(inputWidth, inputHeight));
  renderer.SetFinalPixelType(ttByte);
  renderer.SetFinalSpace(dng_space_sRGB::Get());

  // Try Stage4 from device buffer.  Stage3 GPU may still be running; the Metal
  // serial command queue guarantees Stage4 won't read until Stage3 finishes.
  halide_buffer_t *deviceBuf = demosaic_warp_halide_get_device_buffer(handle);
  const float srcScale = 1.0f / 65535.0f;
  bool stage4Ok = false;
  if (deviceBuf) {
    stage4Ok = render_stage4_halide_from_device_buffer(
        host, negative, renderer, deviceBuf, srcScale, rgb, outW, outH);
  }

  const auto fusedEnd = Clock::now();
  if (timing) {
    timing->fused_demosaic_warp_ms =
        std::chrono::duration<double, std::milli>(fusedEnd - fusedStart).count();
  }

  if (stage4Ok) {
    negative.SetStage3Image(stage3Stub);
    demosaic_warp_rectilinear_halide_cancel(handle);
    if (timing) {
      timing->total_ms = timing->extract_stage2_ms + timing->fused_demosaic_warp_ms;
    }
    return true;
  }

  // Stage4 device handoff failed (e.g. resample needed or params error).
  // Fall back: finish Stage3 → build Stage3 image → run Stage4 normally.
  std::cerr << "[PipelineV2] 8.2.2 device handoff Stage4 failed; "
               "falling back to finish()+Stage4\n";
  bool fused = demosaic_warp_rectilinear_halide_finish(handle);
  if (fused && stage3Stub.Get()) {
    putStage3Data(*stage3Stub.Get(), stage3Data, width, height, 3);
  } else if (!fused) {
    stage3Stub.Reset(
        makeImageFromInterleaved16(host, width, height, 3, stage3Data).Release());
  }
  if (!stage3Stub.Get())
    return false;
  negative.SetStage3Image(stage3Stub);

  if (!runStage4ToRgb(host, negative, config, inputWidth, inputHeight, rgb,
                      outW, outH))
    return false;

  if (timing) {
    timing->total_ms = timing->extract_stage2_ms + timing->fused_demosaic_warp_ms;
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
                    const PipelineConfig &config,
                    uint32_t inputWidth, uint32_t inputHeight,
                    std::vector<uint8_t> &rgb, uint32_t &outW,
                    uint32_t &outH) {
  dng_host *renderHost = &host;
  std::unique_ptr<ConcurrentDngHost> renderHostOverride;
  const uint32_t renderThreads = config.threads.render_area_threads;
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
                                DngPipelineStage3Timing *timing,
                                std::vector<uint16_t> *stage3_workspace) {
  if (timing) {
    *timing = DngPipelineStage3Timing{};
  }
  const PipelineConfig config = PipelineConfig::loadFromEnv();
  if (use_halide_bayer && !config.debug.warp_bit_exact &&
      !config.debug.demosaic_bit_exact) {
    if (runHalideStage3ForBayer(host, negative, config, timing, stage3_workspace)) {
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

    std::vector<uint16_t> stage3Workspace;
    if (isBayer) {
      stage3Workspace.resize(static_cast<size_t>(inputWidth) * inputHeight * 3);
    }
    DngPipelineStage3Timing stage3Timing;
    const PipelineConfig config = PipelineConfig::loadFromEnv();

    // Phase 8.2.2: try fused Stage3+4 device handoff when applicable.
    bool allDone = false;
    if (isBayer) {
      allDone = runHalideStage3And4Fused(
          host, *negative, config, inputWidth, inputHeight,
          &stage3Timing, &stage3Workspace,
          result.rgb, result.width, result.height);
    }

    const auto decodeEnd = Clock::now();
    result.decode_ms =
        std::chrono::duration<double, std::milli>(decodeEnd - decodeStart).count();

    if (allDone) {
      result.process_ms = 0;
      result.error_code = 0;
      return true;
    }

    // Normal Stage3 + Stage4 path (fused not applicable or dispatch failed).
    bool stage3Ok =
        dng_pipeline_v2_run_stage3(host, *negative, isBayer, &stage3Timing,
                                   isBayer ? &stage3Workspace : nullptr);
    if (!stage3Ok) {
      result.error_code = -3;
      return false;
    }

    const auto processStart = Clock::now();
    if (!runStage4ToRgb(host, *negative, config, inputWidth, inputHeight, result.rgb,
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
