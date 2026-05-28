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
  - name: "pipelineSingleFlightMutex"
    description: "File-scope std::mutex accessor; both public FFI entries lock it to serialize warmup vs decode and prevent races on shared native pools / DeviceHandoffState."
    lines: "793-801"
  - name: "dng_pipeline_v2_warmup_for_size"
    description: "Idle-time warm hook; locks single-flight mutex and primes pipeline pools + polynomial3 scratch."
    lines: "803-810"
  - name: "dng_pipeline_v2_run_stage3 / dng_pipeline_v2_decode_to_rgb"
    description: "Shared Stage3 orchestration + top-level decode entry returning RGB and timing; production FFI uses ConcurrentDngHost so Stage1/2 materialization follows matrix threading. decode_to_rgb takes single-flight mutex."
    lines: "812-955"
---
*/
#include "dng_pipeline_v2.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include <dng_color_space.h>
#include <dng_simple_image.h>
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
#include "dng_opcodelist2_halide.h"
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
    if (opcode.OpcodeID() == dngOpcode_WarpRectilinear) {
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

// Phase 10 Sprint D-B F1: process-level mmap pool for Stage3 workspace.
// Avoids ~262ms eager zero-fill in FFI path (dng_pipeline_v2_decode_to_rgb).
// test_decode harness passes a pre-sized vector; pool is bypassed in that case.
class Stage3WorkspacePool {
 public:
  uint16_t *acquire(size_t elements) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t need_bytes = elements * sizeof(uint16_t);
    if (need_bytes > capacity_bytes_) {
      if (ptr_) {
        munmap(ptr_, capacity_bytes_);
        ptr_ = nullptr;
        capacity_bytes_ = 0;
      }
      void *p = mmap(nullptr, need_bytes, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
      if (p == MAP_FAILED) return nullptr;
      ptr_ = static_cast<uint16_t *>(p);
      capacity_bytes_ = need_bytes;
    }
    return ptr_;
  }
 private:
  std::mutex mutex_;
  uint16_t *ptr_ = nullptr;
  size_t capacity_bytes_ = 0;
};

Stage3WorkspacePool &stage3WorkspacePool() {
  static Stage3WorkspacePool pool;
  return pool;
}

// Phase 10 Sprint E-rgb-pool: process-level mmap pool for RGB output buffer.
// Eliminates the 247ms zero-fill page-fault cost on every FFI call caused by
// std::vector<uint8_t>::resize(W*H*3) constructing a fresh 72 MB allocation.
// Pattern identical to Stage3WorkspacePool (Phase 10 Sprint D-round2).
class RgbOutputPool {
 public:
  uint8_t *acquire(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bytes > capacity_bytes_) {
      if (ptr_) {
        munmap(ptr_, capacity_bytes_);
        ptr_ = nullptr;
        capacity_bytes_ = 0;
      }
      void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
      if (p == MAP_FAILED) return nullptr;
      ptr_ = static_cast<uint8_t *>(p);
      capacity_bytes_ = bytes;
    }
    return ptr_;
  }
  size_t capacity() const { return capacity_bytes_; }
 private:
  std::mutex mutex_;
  uint8_t *ptr_ = nullptr;
  size_t capacity_bytes_ = 0;
};

RgbOutputPool &rgbOutputPool() {
  static RgbOutputPool pool;
  return pool;
}

bool warmPipelinePoolsForSize(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);
  const size_t stage3Elements = w * h * 3;
  const size_t rgbBytes = w * h * 3;

  uint16_t *stage3 = stage3WorkspacePool().acquire(stage3Elements);
  uint8_t *rgb = rgbOutputPool().acquire(rgbBytes);
  if (!stage3 || !rgb) {
    return false;
  }
  std::memset(stage3, 0, stage3Elements * sizeof(uint16_t));
  std::memset(rgb, 0, rgbBytes);
  return true;
}

class ScopedStage2DeviceHandoff {
 public:
  explicit ScopedStage2DeviceHandoff(bool enabled) {
    halide_stage2_ol2_clear_device_handoff();
    halide_stage2_ol2_set_device_handoff_enabled(enabled);
  }

  ~ScopedStage2DeviceHandoff() {
    halide_stage2_ol2_set_device_handoff_enabled(false);
  }
};

// Returns a raw pointer to Stage3 workspace.
// If caller supplied a pre-sized vector (harness path), use it directly.
// If caller supplied an empty/wrong-size vector, resize and use it.
// Otherwise (FFI path with nullptr or empty), use the process pool (mmap lazy).
uint16_t *prepareStage3WorkspacePtr(std::vector<uint16_t> *callerWorkspace,
                                    size_t elements,
                                    DngPipelineStage3Timing *timing) {
  const auto resizeStart = Clock::now();
  uint16_t *ptr = nullptr;
  if (callerWorkspace && callerWorkspace->size() == elements) {
    ptr = callerWorkspace->data();
  } else if (callerWorkspace && !callerWorkspace->empty()) {
    callerWorkspace->resize(elements);
    ptr = callerWorkspace->data();
  } else {
    ptr = stage3WorkspacePool().acquire(elements);
  }
  const auto resizeEnd = Clock::now();
  if (timing) {
    timing->resize_ms =
        std::chrono::duration<double, std::milli>(resizeEnd - resizeStart).count();
  }
  return ptr;
}

// Phase 10 Sprint D-B F2: zero-copy Stage2 borrow for contiguous dng_simple_image.
// Bayer Stage2 images produced by Adobe DNG SDK are row-major contiguous uint16,
// so we can borrow the pointer directly and skip the 48MB memcpy (84ms).
bool borrowStage2Bayer16(dng_image *stage2,
                         uint16_t *&src,
                         uint32_t &width,
                         uint32_t &height,
                         uint32_t &planes) {
  if (!stage2) return false;
  if (stage2->PixelType() != ttShort) return false;
  auto *simple = dynamic_cast<dng_simple_image *>(stage2);
  if (!simple) return false;
  dng_pixel_buffer pb;
  simple->GetPixelBuffer(pb);
  if (pb.fPixelType != ttShort || pb.fPixelSize != sizeof(uint16_t))
    return false;
  if (pb.fPlanes != 1) return false;
  if (pb.fPlaneStep != 1) return false;
  const int32 W = static_cast<int32>(pb.fArea.W());
  const int32 H = static_cast<int32>(pb.fArea.H());
  if (W <= 0 || H <= 0) return false;
  if (pb.fRowStep != W) return false;
  if (pb.fColStep != 1) return false;
  if (!pb.fData) return false;
  src = static_cast<uint16_t *>(pb.fData);
  width = static_cast<uint32_t>(W);
  height = static_cast<uint32_t>(H);
  planes = 1;
  return true;
}

// Pointer-based overloads — same bodies as makeImageFromInterleaved16 /
// putStage3Data but accept raw uint16_t* instead of std::vector.
AutoPtr<dng_image> makeImageFromInterleaved16Ptr(dng_host &host, uint32_t width,
                                                 uint32_t height, uint32_t planes,
                                                 const uint16_t *data) {
  dng_point size(static_cast<int32>(height), static_cast<int32>(width));
  AutoPtr<dng_image> image(host.Make_dng_image(dng_rect(size), planes, ttShort));
  dng_pixel_buffer buffer;
  buffer.fArea = image->Bounds();
  buffer.fPlane = 0;
  buffer.fPlanes = planes;
  buffer.fPixelType = ttShort;
  buffer.fPixelSize = sizeof(uint16_t);
  buffer.fData = const_cast<uint16_t *>(data);
  buffer.fRowStep = static_cast<int32>(width * planes);
  buffer.fColStep = static_cast<int32>(planes);
  buffer.fPlaneStep = 1;
  image->Put(buffer);
  return AutoPtr<dng_image>(image.Release());
}

void putStage3DataPtr(dng_image &image, const uint16_t *data,
                      uint32_t width, uint32_t height, uint32_t planes) {
  (void)height;
  dng_pixel_buffer buffer;
  buffer.fArea = image.Bounds();
  buffer.fPlane = 0;
  buffer.fPlanes = planes;
  buffer.fPixelType = ttShort;
  buffer.fPixelSize = sizeof(uint16_t);
  buffer.fData = const_cast<uint16_t *>(data);
  buffer.fRowStep = static_cast<int32>(width * planes);
  buffer.fColStep = static_cast<int32>(planes);
  buffer.fPlaneStep = 1;
  image.Put(buffer);
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

  // Phase 10 Sprint D-B F2: try zero-copy borrow of Stage2 contiguous buffer.
  uint16_t *stage2Ptr = nullptr;
  std::vector<uint16_t> stage2Data;
  const auto extractStart = Clock::now();
  bool borrowed = borrowStage2Bayer16(stage2, stage2Ptr, width, height, planes);
  if (!borrowed) {
    // Fallback: copy via SDK Get() path (non-contiguous or non-simple_image).
    if (!copyImageToInterleaved16(stage2, stage2Data, width, height, planes) ||
        planes != 1) {
      return false;
    }
    stage2Ptr = stage2Data.data();
  } else if (planes != 1) {
    return false;
  }
  const auto extractEnd = Clock::now();
  if (timing) {
    timing->extract_stage2_ms =
        std::chrono::duration<double, std::milli>(extractEnd - extractStart).count();
  }

  const dng_opcode_list &opcodeList3 = negative.OpcodeList3();
  // Phase 10 Sprint D-B F1: use pool-backed ptr instead of vector resize.
  const size_t stage3Elements = static_cast<size_t>(width) * height * 3;
  uint16_t *stage3Ptr = prepareStage3WorkspacePtr(stage3Workspace, stage3Elements, timing);
  if (!stage3Ptr) return false;

  bool fused = false;
  AutoPtr<dng_image> stage3;
  bool stage3Allocated = false;

  if (config.route.fused_demosaic_warp &&
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
              stage2Ptr, static_cast<int>(width),
              static_cast<int>(height), params,
              WarpRectilinearMode::HALIDE_METAL, stage3Ptr);
      if (!handle) {
        handle = demosaic_warp_rectilinear_halide_dispatch(
            stage2Ptr, static_cast<int>(width),
            static_cast<int>(height), params,
            WarpRectilinearMode::HALIDE_CPU, stage3Ptr);
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
    // Phase 8.1.6 D-1 fix: previously called undefined demosaic_ahd_halide;
    // replaced with the existing bilinear halide entry point used elsewhere
    // in the pipeline. Required for separate (non-fused) Stage3 path build.
    demosaic_bilinear_halide(stage2Ptr, static_cast<int>(width),
                             static_cast<int>(height), stage3Ptr);
    const auto demosaicEnd = Clock::now();
    if (timing) {
      timing->demosaic_ms =
          std::chrono::duration<double, std::milli>(demosaicEnd - demosaicStart).count();
    }
  }

  if (fused && stage3Allocated) {
    // Stage3 image was pre-allocated during latency hiding; copy pixels in.
    const auto putStart = Clock::now();
    putStage3DataPtr(*stage3.Get(), stage3Ptr, width, height, 3);
    const auto putEnd = Clock::now();
    if (timing) {
      timing->make_image_ms +=
          std::chrono::duration<double, std::milli>(putEnd - putStart).count();
    }
  } else {
    const auto makeStart = Clock::now();
    stage3.Reset(
        makeImageFromInterleaved16Ptr(host, width, height, 3, stage3Ptr)
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
// rgb_ptr / rgb_size are pool-backed (RgbOutputPool); the function fills them
// in from the pool and writes pixel data through the render path.
bool runStage4ToRgb(dng_host &host, dng_negative &negative,
                    const PipelineConfig &config,
                    uint32_t inputWidth, uint32_t inputHeight,
                    uint8_t *&rgb_ptr, size_t &rgb_size,
                    uint32_t &outW, uint32_t &outH);

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
                               uint8_t *&rgb_ptr,
                               size_t &rgb_size,
                               uint32_t &outW,
                               uint32_t &outH) {
  if (!config.route.fused_demosaic_warp || !config.route.stage3_stage4_device_handoff)
    return false;

  const dng_opcode_list &opcodeList3 = negative.OpcodeList3();
  if (opcodeList3.Count() != 1 ||
      opcodeList3.Entry(0).OpcodeID() != dngOpcode_WarpRectilinear)
    return false;

  dng_image *stage2 = const_cast<dng_image *>(negative.Stage2Image());
  uint32_t width = 0, height = 0, planes = 0;

  // Phase 10 Sprint D-B F2: zero-copy borrow of Stage2 contiguous buffer.
  uint16_t *stage2Ptr = nullptr;
  std::vector<uint16_t> stage2Data;
  const auto extractStart = Clock::now();
  bool borrowed = borrowStage2Bayer16(stage2, stage2Ptr, width, height, planes);
  if (!borrowed) {
    if (!copyImageToInterleaved16(stage2, stage2Data, width, height, planes) ||
        planes != 1) {
      return false;
    }
    stage2Ptr = stage2Data.data();
  } else if (planes != 1) {
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

  // Phase 10 Sprint D-B F1: use pool-backed ptr instead of vector resize.
  const size_t stage3Elements = static_cast<size_t>(width) * height * 3;
  uint16_t *stage3Ptr = prepareStage3WorkspacePtr(stage3Workspace, stage3Elements, timing);
  if (!stage3Ptr) return false;

  const auto fusedStart = Clock::now();

  DemosaicWarpHalideHandle *handle = demosaic_warp_rectilinear_halide_dispatch(
      stage2Ptr, static_cast<int>(width), static_cast<int>(height),
      warpParams, WarpRectilinearMode::HALIDE_METAL, stage3Ptr);
  if (!handle)
    return false;

  // CPU work while GPU runs Stage3 (latency hiding):
  // 1. Pre-alloc Stage4 output buffer via RgbOutputPool (no page fault on warm calls)
  const size_t stage4OutSize = static_cast<size_t>(inputWidth) * inputHeight * 3;
  rgb_ptr = rgbOutputPool().acquire(stage4OutSize);
  if (!rgb_ptr) return false;
  rgb_size = stage4OutSize;

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
        host, negative, renderer, deviceBuf, srcScale,
        rgb_ptr, rgb_size, outW, outH);
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
    putStage3DataPtr(*stage3Stub.Get(), stage3Ptr, width, height, 3);
  } else if (!fused) {
    stage3Stub.Reset(
        makeImageFromInterleaved16Ptr(host, width, height, 3, stage3Ptr).Release());
  }
  if (!stage3Stub.Get())
    return false;
  negative.SetStage3Image(stage3Stub);

  if (!runStage4ToRgb(host, negative, config, inputWidth, inputHeight,
                      rgb_ptr, rgb_size, outW, outH))
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
                    uint8_t *&rgb_ptr, size_t &rgb_size,
                    uint32_t &outW, uint32_t &outH) {
  dng_render renderer(host, negative);
  renderer.SetMaximumSize(std::max(inputWidth, inputHeight));
  renderer.SetFinalPixelType(ttByte);
  renderer.SetFinalSpace(dng_space_sRGB::Get());

  // Acquire pool buffer: mmap pages stay committed across calls → 0ms on warm paths.
  const size_t needed = static_cast<size_t>(inputWidth) * inputHeight * 3;
  rgb_ptr = rgbOutputPool().acquire(needed);
  if (!rgb_ptr) return false;
  rgb_size = needed;

  bool ok = render_stage4_halide(host, negative, renderer,
                                 RenderHalideMode::HALIDE_METAL,
                                 rgb_ptr, rgb_size, outW, outH);
  if (ok)
    return true;

  // SDK fallback: renderer.Render() may produce a different output size.
  // Acquire a fresh pool buffer at the fallback size if needed.
  AutoPtr<dng_image> finalImage(renderer.Render());
  if (!finalImage.Get())
    return false;
  outW = finalImage->Width();
  outH = finalImage->Height();
  const size_t fallbackNeeded = static_cast<size_t>(outW) * outH * 3;
  if (fallbackNeeded > rgb_size) {
    rgb_ptr = rgbOutputPool().acquire(fallbackNeeded);
    if (!rgb_ptr) return false;
    rgb_size = fallbackNeeded;
  }

  dng_pixel_buffer buffer;
  buffer.fArea = finalImage->Bounds();
  buffer.fPlane = 0;
  buffer.fPlanes = 3;
  buffer.fPixelType = ttByte;
  buffer.fPixelSize = 1;
  buffer.fData = rgb_ptr;
  buffer.fRowStep = static_cast<int32>(outW * 3);
  buffer.fColStep = 3;
  buffer.fPlaneStep = 1;
  finalImage->Get(buffer);
  return true;
}

bool runLossyStage2Stage4DeviceHandoff(dng_host &host,
                                        dng_negative &negative,
                                        const PipelineConfig &config,
                                        uint32_t inputWidth,
                                        uint32_t inputHeight,
                                        uint8_t *&rgb_ptr,
                                        size_t &rgb_size,
                                        uint32_t &outW,
                                        uint32_t &outH,
                                        bool &restoreFailed) {
  restoreFailed = false;
  if (!config.route.stage2_stage4_device_handoff)
    return false;

  Stage2Opcode2DeviceHandoff handoff;
  if (!halide_stage2_ol2_get_device_handoff(handoff))
    return false;
  auto restoreHostStage2 = [&restoreFailed]() {
    if (!halide_stage2_ol2_device_handoff_copy_to_host()) {
      restoreFailed = true;
    }
    halide_stage2_ol2_clear_device_handoff();
    return false;
  };
  if (negative.OpcodeList3().Count() != 0)
    return restoreHostStage2();
  if (!handoff.device_buffer || handoff.width == 0 || handoff.height == 0 ||
      handoff.planes < 3 || handoff.pixel_range == 0)
    return restoreHostStage2();

  // Acquire pool buffer before calling render to avoid page fault.
  const size_t needed = static_cast<size_t>(inputWidth) * inputHeight * 3;
  rgb_ptr = rgbOutputPool().acquire(needed);
  if (!rgb_ptr) return restoreHostStage2();
  rgb_size = needed;

  dng_render renderer(host, negative);
  renderer.SetMaximumSize(std::max(inputWidth, inputHeight));
  renderer.SetFinalPixelType(ttByte);
  renderer.SetFinalSpace(dng_space_sRGB::Get());

  const float srcScale = 1.0f / static_cast<float>(handoff.pixel_range);
  const bool ok = render_stage4_halide_from_device_buffer(
      host, negative, renderer, handoff.device_buffer, srcScale,
      rgb_ptr, rgb_size, outW, outH);
  if (ok) {
    halide_stage2_ol2_clear_device_handoff();
    return true;
  }

  return restoreHostStage2();
}

} // namespace

// Single-flight mutex serializing the public FFI entry points against the
// background warmup hook.  Both paths mutate shared native pools (RGBA output
// pool, polynomial scratch caches) and DeviceHandoffState; without this lock a
// warmup invoked from idle could race a concurrent decode and corrupt those
// shared resources.  Function-local static guarantees thread-safe init.
static std::mutex &pipelineSingleFlightMutex() {
  static std::mutex m;
  return m;
}

bool dng_pipeline_v2_warmup_for_size(int32_t width, int32_t height) {
  std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
  if (!warmPipelinePoolsForSize(width, height)) {
    return false;
  }
  halide_prewarm_polynomial3_for_size(width, height);
  return true;
}

bool dng_pipeline_v2_run_stage3(dng_host &host,
                                dng_negative &negative,
                                bool use_halide_bayer,
                                DngPipelineStage3Timing *timing,
                                std::vector<uint16_t> *stage3_workspace) {
  if (timing) {
    *timing = DngPipelineStage3Timing{};
  }
  const PipelineConfig config = PipelineConfig::loadFromEnv();
  if (use_halide_bayer) {
    if (runHalideStage3ForBayer(host, negative, config, timing, stage3_workspace)) {
      return true;
    }
    std::cerr << "[PipelineV2] Halide Stage3 failed; falling back to SDK Stage3\n";
  }
  return runSdkStage3(host, negative, timing);
}

bool dng_pipeline_v2_decode_to_rgb(const char *file_path,
                                   DngPipelineV2Result &result) {
  std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
  result = DngPipelineV2Result{};
  if (!file_path || !file_path[0]) {
    result.error_code = -1;
    return false;
  }

  try {
    const PipelineConfig config = PipelineConfig::loadFromEnv();
    const uint32_t decodeThreads =
        config.threads.area_threads > 0 ? config.threads.area_threads : 20;
    ConcurrentDngHost host(decodeThreads);
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

    // Lazy actual-size prewarm: fire the batched polynomial3 kernel at the
    // real image dimensions now that they are known.  This ensures Metal has
    // compiled and cached the pipeline state for (inputWidth × inputHeight)
    // before BuildStage2Image triggers the real MapPolynomial dispatch, saving
    // ~40ms on the first decode of a given resolution.  The call is a no-op on
    // Bayer images (no MapPolynomial in OpcodeList2) and on repeated decodes of
    // the same size (per-size cache in halide_prewarm_polynomial3_for_size).
    if (!isBayer) {
      halide_prewarm_polynomial3_for_size(
          static_cast<int>(inputWidth), static_cast<int>(inputHeight));
    }

    {
      const bool enableStage2DeviceHandoff =
          !isBayer && config.route.stage2_stage4_device_handoff;
      ScopedStage2DeviceHandoff guard(enableStage2DeviceHandoff);
      negative->BuildStage2Image(host);
    }

    // Phase 10 Sprint D-B F1: do NOT eagerly resize stage3Workspace here.
    // FFI path gets a lazy mmap pool via prepareStage3WorkspacePtr; this
    // eliminates the ~262ms zero-fill that was inside decodeStart-decodeEnd.
    // test_decode harness passes its own pre-sized vector so is unaffected.
    std::vector<uint16_t> stage3Workspace;
    DngPipelineStage3Timing stage3Timing;

    // Phase 8.2.2: try fused Stage3+4 device handoff when applicable.
    bool allDone = false;
    if (isBayer) {
      allDone = runHalideStage3And4Fused(
          host, *negative, config, inputWidth, inputHeight,
          &stage3Timing, &stage3Workspace,
          result.rgb_ptr, result.rgb_size, result.width, result.height);
    } else {
      bool restoreFailed = false;
      allDone = runLossyStage2Stage4DeviceHandoff(
          host, *negative, config, inputWidth, inputHeight,
          result.rgb_ptr, result.rgb_size, result.width, result.height,
          restoreFailed);
      if (restoreFailed) {
        result.error_code = -5;
        return false;
      }
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
    if (!runStage4ToRgb(host, *negative, config, inputWidth, inputHeight,
                        result.rgb_ptr, result.rgb_size,
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
