/*
---
file_summary: "Production DNG pipeline v2 entry used by Flutter FFI; runs SDK Stage1/2, Halide Stage3/4 where applicable, and returns RGB8/RGBA8."
functions:
  - name: "pipelineVerbose / requireGpuBackend"
    description: "L-11 runtime verbose gate (DNG_PIPELINE_VERBOSE env) + GPU capability gate helper."
    lines: "82-92"
  - name: "copyImageToInterleaved16 / allocStage3Image"
    description: "Stage3 image <-> uint16 interleaved buffer helpers; alloc/put are split so latency hiding can pre-allocate while GPU runs. Pointer-based variants (makeImageFromInterleaved16Ptr / putStage3DataPtr / prepareStage3WorkspacePtr) are the production path."
    lines: "93-122"
  - name: "applyOpcodeList3"
    description: "Apply OpcodeList3 to a Stage3 image, using Halide WarpRectilinear when config allows."
    lines: "126-154"
  - name: "RgbaOutputPool"
    description: "W7-B checkout-style pool; W1 fixes: best-fit acquire, H-2 defensive release, M-12 free_ cap ≤2."
    lines: "227-309"
  - name: "ScopedRgbaCheckout / acquireStage4OutputBuffer"
    description: "H-1 RAII scope-guard for checkout pool + M-8 size-assert on reuse path."
    lines: "311-359"
  - name: "warmPipelinePoolsForSize"
    description: "Idle-time pool prewarm; L-5 per-size warmed cache skips redundant memset."
    lines: "361-406"
  - name: "extractStage2Bayer16"
    description: "Phase 11 W4 — borrow-or-copy Stage2 Bayer with timing."
    lines: "479-504"
  - name: "runHalideStage3ForBayer"
    description: "Run production Bayer Stage3 with fused demosaic+WarpRectilinear fast path; Phase 8.2.1 Path D."
    lines: "543-697"
  - name: "runHalideStage3And4Fused"
    description: "Phase 8.2.2 — Stage3→Stage4 GPU device handoff. M-1 renamed params, M-7 deferred stage3Stub."
    lines: "715-852"
  - name: "runSdkStage3 / runStage3WithConfig / runStage4ToRgb"
    description: "SDK Stage3, M-2 config-taking Stage3 orchestrator, Stage4 render via Halide GPU."
    lines: "854-919"
  - name: "ParsedDngMetadata / parseDngFile / decodeStages"
    description: "DNG parse + Stage1/2/3/4 decode orchestration. H-1 ScopedRgbaCheckout guard in decodeStages."
    lines: "974-1140"
  - name: "pipelineSingleFlightMutex"
    description: "File-scope std::mutex accessor; serializes warmup vs decode."
    lines: "1143-1146"
  - name: "dng_pipeline_warmup_for_size"
    description: "Idle-time warm hook; locks single-flight mutex and primes pipeline pools."
    lines: "1148-1174"
  - name: "dng_pipeline_run_stage3 / dng_pipeline_decode_to_rgb"
    description: "M-2 public Stage3 delegates to runStage3WithConfig; decode_to_rgb with L-11 once-per-process GPU banner."
    lines: "1177-1261"
---
*/
#include "dng_pipeline.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
// W4 (2026-08-21, Windows port): <sys/mman.h>/<unistd.h> do not exist in the
// MSVC/clang-cl environment. The only user is Stage3WorkspacePool below, which
// gets a VirtualAlloc branch with the same lazy-commit semantics.
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <dng_color_space.h>
#include <dng_simple_image.h>
#include <dng_exceptions.h>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_lens_correction.h>
#include <dng_mosaic_info.h>
#include <dng_negative.h>
#include <dng_opcodes.h>
#include <dng_tag_values.h>
#include <dng_pixel_buffer.h>
#include <dng_render.h>

#include "ConcurrentDngHost.h"
#include "dng_cfa_phase.h"
#include "dng_error_codes.h"  // W5: unified DngErrorCode enum
#include "dng_mosaic_halide.h"
#include "dng_opcodelist2_halide.h"
#include "dng_render_halide.h"
#include "dng_warp_halide.h"
#include "dng_halide_device.h"

namespace {

using Clock = std::chrono::high_resolution_clock;

// L-11: runtime gate for pipeline informational/diagnostic stderr banners.
// Follows the DNG_STAGE1_TIMING / DNG_MAP_POLY_TIMING lazy-cache pattern
// (DiagnosticConfig category in dng_pipeline_config.h).
bool pipelineVerbose() {
  static const bool cached = []() {
    const char *v = std::getenv("DNG_PIPELINE_VERBOSE");
    return v && v[0] != '0';
  }();
  return cached;
}

bool requireGpuBackend(const char *path) {
  if (dng_halide_gpu_available()) {
    return true;
  }
  fprintf(stderr, "[Pipeline] GPU capability gate FAILED for %s: "
          "backend=%s; decode requires Metal (macOS) or Vulkan (Android). "
          "No SDK-CPU fallback route available.\n",
          path, dng_halide_gpu_backend_name());
  return false;
}

// Reads the Bayer CFA phase out of the parsed DNG metadata (see
// dng_cfa_phase.h for the mapping and why ActiveArea needs no correction).
// Falls back to RGGB (0, 0) with a VERBOSE note when the phase cannot be
// determined; a wrong-but-plausible phase still beats refusing to decode, and
// the note makes it diagnosable.
void resolveCfaPhase(const dng_negative &negative, int &red_x, int &red_y) {
  const char *reason = nullptr;
  if (dng_resolve_cfa_phase(negative, red_x, red_y, &reason)) {
    return;
  }
  if (pipelineVerbose()) {
    fprintf(stderr,
            "[Pipeline] CFA phase undetermined (%s); falling back to RGGB "
            "(red_x=0, red_y=0)\n",
            reason ? reason : "unknown");
  }
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

AutoPtr<dng_image> allocStage3Image(dng_host &host, uint32_t width,
                                    uint32_t height, uint32_t planes) {
  dng_point size(static_cast<int32>(height), static_cast<int32>(width));
  return AutoPtr<dng_image>(
      host.Make_dng_image(dng_rect(size), planes, ttShort));
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
                                                WarpRectilinearMode::HALIDE_GPU);
      if (!applied) {
        std::cerr << "[Pipeline] WarpRectilinear HALIDE_GPU failed; "
                     "refusing CPU/SDK fallback\n";
        return false;
      }
    }

    if (!applied) {
      opcode.Apply(host, negative, image);
    }
  }
  return true;
}

// Phase 10 Sprint D-B F1: process-level mmap pool for Stage3 workspace.
// Avoids ~262ms eager zero-fill in FFI path (dng_pipeline_decode_to_rgb).
// test_decode harness passes a pre-sized vector; pool is bypassed in that case.
class Stage3WorkspacePool {
 public:
  uint16_t *acquire(size_t elements) {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t need_bytes = elements * sizeof(uint16_t);
    if (need_bytes > capacity_bytes_) {
      if (ptr_) {
#if defined(_WIN32)
        // MEM_RELEASE requires the size argument to be 0 and the address to be
        // the exact base returned by VirtualAlloc.
        VirtualFree(ptr_, 0, MEM_RELEASE);
#else
        munmap(ptr_, capacity_bytes_);
#endif
        ptr_ = nullptr;
        capacity_bytes_ = 0;
      }
#if defined(_WIN32)
      // MEM_RESERVE|MEM_COMMIT on Windows commits lazily backed zero pages,
      // the same property MAP_ANON gives us here: the ~262ms eager zero-fill
      // is avoided because physical pages arrive on first touch.
      void *p = VirtualAlloc(nullptr, need_bytes, MEM_RESERVE | MEM_COMMIT,
                             PAGE_READWRITE);
      if (p == nullptr) return nullptr;
#else
      void *p = mmap(nullptr, need_bytes, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
      if (p == MAP_FAILED) return nullptr;
#endif
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

// 7.1: checkout-style RGB output pool. Mirrors RgbaOutputPool ownership model
// (checked_out_ tracking, capped free_ list, best-fit acquire, unknown-pointer
// defensive release, debug count accessor). Used on the fuse_rgba_output=false
// path (test harness / rollback). Production FFI path uses RgbaOutputPool.
class RgbOutputPool {
 public:
  uint8_t *acquire(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Best-fit: find the smallest free slot with cap >= bytes.
    size_t bestIdx = free_.size();  // sentinel: no fit found
    size_t bestCap = SIZE_MAX;
    for (size_t i = 0; i < free_.size(); ++i) {
      if (free_[i].cap >= bytes && free_[i].cap < bestCap) {
        bestIdx = i;
        bestCap = free_[i].cap;
      }
    }
    if (bestIdx < free_.size()) {
      uint8_t *p = free_[bestIdx].buf.release();
      size_t cap = free_[bestIdx].cap;
      free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(bestIdx));
      checked_out_.emplace(p, cap);
      return p;
    }
    uint8_t *p = new (std::nothrow) uint8_t[bytes];  // no value-init
    if (p) checked_out_.emplace(p, bytes);
    return p;
  }
  bool release(uint8_t *ptr) {
    if (!ptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = checked_out_.find(ptr);
    if (it == checked_out_.end()) {
      // H-2 pool defense: unknown pointer — absorb and log rather than
      // returning false and letting the caller fall through to delete[]
      // (which risks double-free if the pointer was pool-owned earlier).
      fprintf(stderr, "[RgbOutputPool] release() called with unknown "
              "pointer %p; absorbing to prevent double-free\n",
              static_cast<void *>(ptr));
      return true;
    }
    size_t cap = it->second;
    checked_out_.erase(it);
    // M-12: cap free_ at kMaxFreeSlots to bound memory growth.
    // Evict the largest slot (most wasteful) to make room.
    if (free_.size() >= kMaxFreeSlots) {
      size_t largestIdx = 0;
      for (size_t i = 1; i < free_.size(); ++i) {
        if (free_[i].cap > free_[largestIdx].cap)
          largestIdx = i;
      }
      // If the incoming buffer is larger than all cached slots,
      // discard it directly instead of evicting a smaller one.
      if (cap > free_[largestIdx].cap) {
        delete[] ptr;
        return true;
      }
      free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(largestIdx));
    }
    free_.push_back({std::unique_ptr<uint8_t[]>(ptr), cap});
    return true;
  }
  // Debug accessor: number of buffers currently checked out.
  size_t checked_out_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checked_out_.size();
  }

 private:
  static constexpr size_t kMaxFreeSlots = 2;
  struct Slot {
    std::unique_ptr<uint8_t[]> buf;
    size_t cap;
  };
  mutable std::mutex mutex_;
  std::vector<Slot> free_;
  std::unordered_map<uint8_t *, size_t> checked_out_;
};

RgbOutputPool &rgbOutputPool() {
  static RgbOutputPool pool;
  return pool;
}

// W7-B (P15): checkout-style RGBA output pool. See dng_pipeline.h for the
// rationale (distinct per-decode buffers so a zero-copy buffer handed to Dart
// is not clobbered by the next decode). Mirrors the FFI W7-A RgbaPool that it
// supersedes; consolidating here gives a single owner so both the Android
// fused bridge (acquire) and the FFI free path (release) reach the same pool.
//
// W1 fixes (H-1/M-12/H-2):
//   - acquire: best-fit selection (smallest free slot >= bytes)
//   - release: unknown-pointer defense (logged no-op, prevents caller delete[])
//   - free_ capped at kMaxFreeSlots; excess released (largest first)
class RgbaOutputPool {
 public:
  uint8_t *acquire(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    // Best-fit: find the smallest free slot with cap >= bytes.
    size_t bestIdx = free_.size();  // sentinel: no fit found
    size_t bestCap = SIZE_MAX;
    for (size_t i = 0; i < free_.size(); ++i) {
      if (free_[i].cap >= bytes && free_[i].cap < bestCap) {
        bestIdx = i;
        bestCap = free_[i].cap;
      }
    }
    if (bestIdx < free_.size()) {
      uint8_t *p = free_[bestIdx].buf.release();
      size_t cap = free_[bestIdx].cap;
      free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(bestIdx));
      checked_out_.emplace(p, cap);
      return p;
    }
    uint8_t *p = new (std::nothrow) uint8_t[bytes];  // no value-init
    if (p) checked_out_.emplace(p, bytes);
    return p;
  }
  bool release(uint8_t *ptr) {
    if (!ptr) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = checked_out_.find(ptr);
    if (it == checked_out_.end()) {
      // H-2 pool defense: unknown pointer — absorb and log rather than
      // returning false and letting the caller fall through to delete[]
      // (which risks double-free if the pointer was pool-owned earlier).
      fprintf(stderr, "[RgbaOutputPool] release() called with unknown "
              "pointer %p; absorbing to prevent double-free\n",
              static_cast<void *>(ptr));
      return true;
    }
    size_t cap = it->second;
    checked_out_.erase(it);
    // M-12: cap free_ at kMaxFreeSlots to bound memory growth.
    // Evict the largest slot (most wasteful) to make room.
    if (free_.size() >= kMaxFreeSlots) {
      size_t largestIdx = 0;
      for (size_t i = 1; i < free_.size(); ++i) {
        if (free_[i].cap > free_[largestIdx].cap)
          largestIdx = i;
      }
      // If the incoming buffer is larger than all cached slots,
      // discard it directly instead of evicting a smaller one.
      if (cap > free_[largestIdx].cap) {
        delete[] ptr;
        return true;
      }
      free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(largestIdx));
    }
    free_.push_back({std::unique_ptr<uint8_t[]>(ptr), cap});
    return true;
  }
  // Debug accessor: number of buffers currently checked out.
  // Exposed so the Batch 1 leak check can assert this doesn't grow
  // across repeated failed decodes.
  size_t checked_out_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return checked_out_.size();
  }

 private:
  static constexpr size_t kMaxFreeSlots = 2;
  struct Slot {
    std::unique_ptr<uint8_t[]> buf;
    size_t cap;
  };
  mutable std::mutex mutex_;
  std::vector<Slot> free_;
  std::unordered_map<uint8_t *, size_t> checked_out_;
};

RgbaOutputPool &rgbaOutputPool() {
  static RgbaOutputPool pool;
  return pool;
}

// W1 H-1: RAII guard for checkout-style RGBA output pool buffers. Releases
// the buffer back to the pool on scope exit unless disarmed by commit().
// Active only when config.fuse_rgba_output is true (checkout-pool path).
class ScopedRgbaCheckout {
 public:
  explicit ScopedRgbaCheckout(uint8_t *&ptr_ref, bool active)
      : ptr_ref_(ptr_ref), active_(active) {}
  ~ScopedRgbaCheckout() {
    if (active_ && ptr_ref_) {
      rgbaOutputPool().release(ptr_ref_);
      ptr_ref_ = nullptr;
    }
  }
  void commit() { active_ = false; }
  ScopedRgbaCheckout(const ScopedRgbaCheckout &) = delete;
  ScopedRgbaCheckout &operator=(const ScopedRgbaCheckout &) = delete;
 private:
  uint8_t *&ptr_ref_;
  bool active_;
};

// 7.1: RAII guard for checkout-style RGB output pool buffers. Mirrors
// ScopedRgbaCheckout; active when config.fuse_rgba_output is false (RGB path).
class ScopedRgbCheckout {
 public:
  explicit ScopedRgbCheckout(uint8_t *&ptr_ref, bool active)
      : ptr_ref_(ptr_ref), active_(active) {}
  ~ScopedRgbCheckout() {
    if (active_ && ptr_ref_) {
      rgbOutputPool().release(ptr_ref_);
      ptr_ref_ = nullptr;
    }
  }
  void commit() { active_ = false; }
  ScopedRgbCheckout(const ScopedRgbCheckout &) = delete;
  ScopedRgbCheckout &operator=(const ScopedRgbCheckout &) = delete;
 private:
  uint8_t *&ptr_ref_;
  bool active_;
};

// Acquire the Stage4 output buffer. On the fused path (fuse_rgba_output=true)
// this is an interleaved RGBA8 buffer from the checkout-style RgbaOutputPool;
// otherwise an interleaved RGB8 buffer from the checkout-style RgbOutputPool.
// Reuses an already-set ptr (e.g. a device-handoff fallback re-entering
// runStage4ToRgb) to avoid leaking a previously checked-out buffer.
// R2 sized decode: the single place that decides the Stage4 MaximumSize cap.
// maxDim <= 0, or a maxDim at least as large as the input, yields exactly the
// previous expression (max of input extents) — so the full-resolution path is
// unchanged by construction rather than by inspection.
uint32_t stage4MaximumSize(uint32_t inputWidth, uint32_t inputHeight,
                           int32_t maxDim) {
  const uint32_t full = std::max(inputWidth, inputHeight);
  if (maxDim <= 0) {
    return full;
  }
  const uint32_t requested = static_cast<uint32_t>(maxDim);
  return requested < full ? requested : full;
}

bool acquireStage4OutputBuffer(const PipelineConfig &config,
                               uint32_t width, uint32_t height,
                               uint8_t *&ptr, size_t &size) {
  const size_t need = config.fuse_rgba_output
      ? static_cast<size_t>(width) * height * 4
      : static_cast<size_t>(width) * height * 3;
  if (ptr) {
    // M-8: size assert on reuse path. Within a single decode, all acquires
    // share the same W/H (from ParsedDngMetadata), so this is a latent defense
    // against future callers that might re-enter with different dimensions.
    if (size < need) {
      fprintf(stderr, "[Pipeline] acquireStage4OutputBuffer: reuse buffer "
              "size %zu < needed %zu; refusing reuse\n", size, need);
      return false;
    }
    return true;
  }
  size = need;
  if (config.fuse_rgba_output) {
    ptr = rgbaOutputPool().acquire(size);
  } else {
    ptr = rgbOutputPool().acquire(size);
  }
  return ptr != nullptr;
}

bool warmPipelinePoolsForSize(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);
  const size_t stage3ElementCount = w * h * 3;
  const size_t rgbByteCount = w * h * 3;

  uint16_t *stage3 = stage3WorkspacePool().acquire(stage3ElementCount);
  uint8_t *rgb = rgbOutputPool().acquire(rgbByteCount);
  if (!stage3 || !rgb) {
    if (rgb) rgbOutputPool().release(rgb);
    return false;
  }

  // L-5: per-size warmed cache — skip redundant memset when the same
  // size was already warmed. Pools are grow-only (new[]), so a prior
  // acquire at this size guarantees the pages are already committed.
  // No mutex needed: always called under pipelineSingleFlightMutex.
  static int32_t warmedW = 0, warmedH = 0;
  const bool alreadyWarmed = (width == warmedW && height == warmedH);
  if (!alreadyWarmed) {
    std::memset(stage3, 0, stage3ElementCount * sizeof(uint16_t));
    std::memset(rgb, 0, rgbByteCount);
  }
  // 7.1: release RGB buffer back to checkout pool after warming.
  rgbOutputPool().release(rgb);
  // W7 (M-11): seed one committed RGBA8 buffer into the checkout pool so the
  // first fused decode does not pay the ~96MB first-touch page-fault. Previously
  // Android-only (W7-B); now all platforms use the fused RGBA path.
  const size_t rgbaByteCount = w * h * 4;
  uint8_t *rgba = rgbaOutputPool().acquire(rgbaByteCount);
  if (!rgba) {
    return false;
  }
  if (!alreadyWarmed) {
    std::memset(rgba, 0, rgbaByteCount);
  }
  rgbaOutputPool().release(rgba);
  if (!alreadyWarmed) {
    warmedW = width;
    warmedH = height;
  }
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
    timing->workspace_acquire_ms =
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

// Phase 11 W4 — consolidates the Stage2 borrow + fallback-copy + extract timing
// block that previously lived verbatim in both runHalideStage3ForBayer and
// runHalideStage3And4Fused. Pure refactor; behaviour preserved bit-exact:
// on success stage2Ptr points either into the borrowed simple_image buffer or
// into stage2FallbackData.data() (caller-owned vector kept alive by reference).
bool extractStage2Bayer16(dng_image *stage2,
                          uint16_t *&stage2Ptr,
                          std::vector<uint16_t> &stage2FallbackData,
                          uint32_t &width,
                          uint32_t &height,
                          uint32_t &planes,
                          DngPipelineStage3Timing *timing) {
  const auto extractStart = Clock::now();
  bool borrowed = borrowStage2Bayer16(stage2, stage2Ptr, width, height, planes);
  if (!borrowed) {
    if (!copyImageToInterleaved16(stage2, stage2FallbackData, width, height,
                                  planes) ||
        planes != 1) {
      return false;
    }
    stage2Ptr = stage2FallbackData.data();
  } else if (planes != 1) {
    return false;
  }
  const auto extractEnd = Clock::now();
  if (timing) {
    timing->extract_stage2_ms =
        std::chrono::duration<double, std::milli>(extractEnd - extractStart)
            .count();
  }
  return true;
}

// Pointer-based overloads — accept raw uint16_t* instead of std::vector.
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

  // Phase 10 Sprint D-B F2 + Phase 11 W4: zero-copy borrow with copy fallback,
  // consolidated into extractStage2Bayer16.
  uint16_t *stage2Ptr = nullptr;
  std::vector<uint16_t> stage2Data;
  if (!extractStage2Bayer16(stage2, stage2Ptr, stage2Data, width, height,
                            planes, timing)) {
    return false;
  }

  const dng_opcode_list &opcodeList3 = negative.OpcodeList3();
  // Phase 10 Sprint D-B F1: use pool-backed ptr instead of vector resize.
  const size_t stage3ElementCount = static_cast<size_t>(width) * height * 3;
  uint16_t *stage3Ptr = prepareStage3WorkspacePtr(stage3Workspace, stage3ElementCount, timing);
  if (!stage3Ptr) return false;

  int cfaRedX = 0;
  int cfaRedY = 0;
  resolveCfaPhase(negative, cfaRedX, cfaRedY);

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

      // Path D — async dispatch the fused GPU kernel, then run
      // CPU-only Make_dng_image while the GPU is in flight, then sync+copy.
      const auto fusedStart = Clock::now();
      DemosaicWarpHalideHandle *handle =
          demosaic_warp_rectilinear_halide_dispatch(
              stage2Ptr, static_cast<int>(width),
              static_cast<int>(height), params,
              WarpRectilinearMode::HALIDE_GPU, stage3Ptr,
              cfaRedX, cfaRedY);

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
#if defined(__ANDROID__)
        // W0: Stage3 timing for test_decode_android host path (non-FFI).
        // L-11: gated behind DNG_PIPELINE_VERBOSE env flag.
        if (pipelineVerbose()) {
          fprintf(stderr,
              "[Stage3-Perf] (host-path) fused_demosaic_warp=%.1f ms"
              " make_image=%.1f ms\n",
              timing->fused_demosaic_warp_ms,
              timing->make_image_ms);
        }
#endif
      }
    }
  }

  if (!fused) {
    const auto demosaicStart = Clock::now();
    // Separate GPU path: call the AOT entry directly so failure propagates
    // instead of using demosaic_bilinear_halide()'s CPU reference fallback.
    if (!demosaic_bilinear_halide_aot(stage2Ptr, static_cast<int>(width),
                                      static_cast<int>(height), stage3Ptr,
                                      cfaRedX, cfaRedY)) {
      std::cerr << "[Pipeline] Stage3 demosaic Halide AOT failed; "
                   "refusing CPU fallback\n";
      return false;
    }
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
// M-1: stage4_out_ptr/stage4_out_size — internal names reflect that the buffer
// may hold either RGB8 or RGBA8 depending on config.fuse_rgba_output.
bool runStage4ToRgb(dng_host &host, dng_negative &negative,
                    const PipelineConfig &config,
                    uint32_t inputWidth, uint32_t inputHeight,
                    int32_t maxDim,
                    uint8_t *&stage4_out_ptr, size_t &stage4_out_size,
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
                               int32_t maxDim,
                               DngPipelineStage3Timing *timing,
                               std::vector<uint16_t> *stage3Workspace,
                               uint8_t *&stage4_out_ptr,
                               size_t &stage4_out_size,
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

  // Phase 10 Sprint D-B F2 + Phase 11 W4: see extractStage2Bayer16.
  uint16_t *stage2Ptr = nullptr;
  std::vector<uint16_t> stage2Data;
  if (!extractStage2Bayer16(stage2, stage2Ptr, stage2Data, width, height,
                            planes, timing)) {
    return false;
  }

  const auto &warpOpcode =
      static_cast<const dng_opcode_WarpRectilinear &>(opcodeList3.Entry(0));
  WarpRectilinearParams warpParams;
  if (!extractWarpRectilinearParams(warpOpcode, negative.PixelAspectRatio(),
                                    warpParams))
    return false;

  // Phase 10 Sprint D-B F1: use pool-backed ptr instead of vector resize.
  const size_t stage3ElementCount = static_cast<size_t>(width) * height * 3;
  uint16_t *stage3Ptr = prepareStage3WorkspacePtr(stage3Workspace, stage3ElementCount, timing);
  if (!stage3Ptr) return false;

  int cfaRedX = 0;
  int cfaRedY = 0;
  resolveCfaPhase(negative, cfaRedX, cfaRedY);

  const auto fusedStart = Clock::now();

  DemosaicWarpHalideHandle *handle = demosaic_warp_rectilinear_halide_dispatch(
      stage2Ptr, static_cast<int>(width), static_cast<int>(height),
      warpParams, WarpRectilinearMode::HALIDE_GPU, stage3Ptr,
      cfaRedX, cfaRedY);
  if (!handle)
    return false;

  // CPU work while GPU runs Stage3 (latency hiding):
  // 1. Setup Stage4 renderer. R2 sized decode: moved ahead of the output-buffer
  //    acquire because the buffer is now sized by OUTPUT extent, which is not
  //    known until the renderer's MaximumSize is set. Both are cheap CPU work
  //    inside the same latency-hiding window, so the ordering swap costs
  //    nothing.
  dng_render renderer(host, negative);
  renderer.SetMaximumSize(stage4MaximumSize(inputWidth, inputHeight, maxDim));
  renderer.SetFinalPixelType(ttByte);
  renderer.SetFinalSpace(dng_space_sRGB::Get());

  // 2. Pre-alloc Stage4 output buffer (no page fault on warm calls). W7-B: on
  //    the Android fused path this is an RGBA8 checkout buffer; otherwise RGB8.
  //    Sized by output extent: on the full-resolution path that equals the
  //    input extent (unchanged); on the sized path it is the reason the RGBA
  //    allocation shrinks with maxDim.
  uint32_t plannedW = inputWidth;
  uint32_t plannedH = inputHeight;
  dng_render_stage4_output_size(negative, renderer, plannedW, plannedH);
  if (!acquireStage4OutputBuffer(config, plannedW, plannedH, stage4_out_ptr,
                                 stage4_out_size))
    return false;

  // 3. Stage3 stub: deferred to success/fallback paths (M-7).
  //    No downstream consumer reads stage3Stub pixels on the device handoff
  //    success path; a minimal 1×1 stub saves ~146MB vs the full-size alloc.
  //    The fallback path allocates full-size when it needs real pixel data.

  // Try Stage4 from device buffer.  Stage3 GPU may still be running; the GPU
  // serial command queue (Metal/Vulkan) guarantees Stage4 won't read until Stage3 finishes.
  halide_buffer_t *deviceBuf = demosaic_warp_halide_get_device_buffer(handle);
  const float srcScale = 1.0f / 65535.0f;
  bool stage4Ok = false;
  if (deviceBuf) {
    stage4Ok = render_stage4_halide_from_device_buffer(
        host, negative, renderer, deviceBuf, srcScale, config,
        stage4_out_ptr, stage4_out_size, outW, outH);
  }

  const auto fusedEnd = Clock::now();
  if (timing) {
    timing->fused_demosaic_warp_ms =
        std::chrono::duration<double, std::milli>(fusedEnd - fusedStart).count();
#if defined(__ANDROID__)
    // W0: Stage3 timing for FFI device-handoff path (fused Stage3+4).
    // L-11: gated behind DNG_PIPELINE_VERBOSE env flag.
    if (pipelineVerbose()) {
      fprintf(stderr,
          "[Stage3-Perf] (fused-FFI) fused_demosaic_warp=%.1f ms"
          " extract_stage2=%.1f ms\n",
          timing->fused_demosaic_warp_ms,
          timing->extract_stage2_ms);
    }
#endif
  }

  if (stage4Ok) {
    // M-7: minimal 1×1 stub — no downstream consumer reads pixels on the
    // device handoff success path. SetStage3Image only needs a valid image
    // for SDK bookkeeping integrity.
    AutoPtr<dng_image> stage3Stub;
    stage3Stub.Reset(allocStage3Image(host, 1, 1, 3).Release());
    negative.SetStage3Image(stage3Stub);
    demosaic_warp_rectilinear_halide_cancel(handle);
    if (timing) {
      timing->total_ms = timing->extract_stage2_ms + timing->fused_demosaic_warp_ms;
    }
    return true;
  }

  // Stage4 device handoff failed (e.g. resample needed or params error).
  // Finish Stage3, build a Stage3 image, then run the normal GPU Stage4 path.
  std::cerr << "[Pipeline] 8.2.2 device handoff Stage4 failed; "
               "using finish()+Stage4 host-copy path\n";
  // M-7: full-size alloc for fallback — the Stage3 data needs to be injected
  // into the negative for the normal Stage4 render path.
  AutoPtr<dng_image> stage3Stub;
  stage3Stub.Reset(allocStage3Image(host, width, height, 3).Release());
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

  if (!runStage4ToRgb(host, negative, config, inputWidth, inputHeight, maxDim,
                      stage4_out_ptr, stage4_out_size, outW, outH))
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

// M-2: internal Stage3 orchestration with explicit PipelineConfig, avoiding
// a redundant PipelineConfig::loadFromEnv() when the caller (decodeStages)
// already holds a config. The public dng_pipeline_run_stage3 delegates
// here after loading config for backward-compatible external callers.
bool runStage3WithConfig(dng_host &host,
                         dng_negative &negative,
                         bool use_halide_bayer,
                         const PipelineConfig &config,
                         DngPipelineStage3Timing *timing,
                         std::vector<uint16_t> *stage3_workspace) {
  if (timing) {
    *timing = DngPipelineStage3Timing{};
  }
  if (use_halide_bayer) {
    if (!requireGpuBackend("Stage3")) {
      return false;
    }
    if (runHalideStage3ForBayer(host, negative, config, timing, stage3_workspace)) {
      return true;
    }
    std::cerr << "[Pipeline] Halide Stage3 failed; refusing SDK CPU fallback\n";
    return false;
  }
  return runSdkStage3(host, negative, timing);
}

bool runStage4ToRgb(dng_host &host, dng_negative &negative,
                    const PipelineConfig &config,
                    uint32_t inputWidth, uint32_t inputHeight,
                    int32_t maxDim,
                    uint8_t *&stage4_out_ptr, size_t &stage4_out_size,
                    uint32_t &outW, uint32_t &outH) {
  dng_render renderer(host, negative);
  renderer.SetMaximumSize(stage4MaximumSize(inputWidth, inputHeight, maxDim));
  renderer.SetFinalPixelType(ttByte);
  renderer.SetFinalSpace(dng_space_sRGB::Get());

  // Acquire pool buffer: warm pages stay committed → 0ms on warm paths.
  // W7-B: RGBA8 checkout buffer on the Android fused path, else RGB8. Reuses an
  // already-set stage4_out_ptr when re-entered from the device-handoff fallback.
  // R2 sized decode: sized by OUTPUT extent (equals input on the full-res path).
  uint32_t plannedW = inputWidth;
  uint32_t plannedH = inputHeight;
  dng_render_stage4_output_size(negative, renderer, plannedW, plannedH);
  if (!acquireStage4OutputBuffer(config, plannedW, plannedH, stage4_out_ptr,
                                 stage4_out_size))
    return false;

  bool ok = render_stage4_halide(host, negative, renderer,
                                 RenderHalideMode::HALIDE_GPU,
                                 config,
                                 stage4_out_ptr, stage4_out_size, outW, outH);
  if (ok)
    return true;

  std::cerr << "[Pipeline] Stage4 Halide GPU failed; refusing SDK CPU fallback\n";
  return false;
}

bool runLossyStage2Stage4DeviceHandoff(dng_host &host,
                                        dng_negative &negative,
                                        const PipelineConfig &config,
                                        uint32_t inputWidth,
                                        uint32_t inputHeight,
                                        uint8_t *&stage4_out_ptr,
                                        size_t &stage4_out_size,
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
  // W7-B: RGBA8 checkout buffer on the Android fused path, else RGB8.
  if (!acquireStage4OutputBuffer(config, inputWidth, inputHeight, stage4_out_ptr,
                                 stage4_out_size))
    return restoreHostStage2();

  // R2 sized decode: this is the non-Bayer (lossy) route, which has no Stage3
  // image and no scaled kernel, so it always renders full resolution. decodeStages
  // zeroes the effective maxDim before reaching here and logs the fallback, so
  // there is deliberately no maxDim to thread into this SetMaximumSize.
  dng_render renderer(host, negative);
  renderer.SetMaximumSize(std::max(inputWidth, inputHeight));
  renderer.SetFinalPixelType(ttByte);
  renderer.SetFinalSpace(dng_space_sRGB::Get());

  const float srcScale = 1.0f / static_cast<float>(handoff.pixel_range);
  const bool ok = render_stage4_halide_from_device_buffer(
      host, negative, renderer, handoff.device_buffer, srcScale, config,
      stage4_out_ptr, stage4_out_size, outW, outH);
  if (ok) {
    halide_stage2_ol2_clear_device_handoff();
    return true;
  }

  return restoreHostStage2();
}

struct ParsedDngMetadata {
  bool isBayer;
  uint32_t inputWidth;
  uint32_t inputHeight;
};

bool parseDngFile(ConcurrentDngHost &host,
                  dng_file_stream &stream,
                  dng_info &info,
                  AutoPtr<dng_negative> &negative,
                  ParsedDngMetadata &metadata,
                  Clock::time_point &decodeStart,
                  DngPipelineResult &result) {
  info.Parse(host, stream);
  info.PostParse(host);
  if (!info.IsValidDNG() || info.fMainIndex >= info.fIFDCount) {
    result.error_code = kDngErrParseFailed;
    return false;
  }

  dng_negative *negativeRaw = host.Make_dng_negative();
  negative.Reset(negativeRaw);
  negative->Parse(host, stream, info);
  negative->PostParse(host, stream, info);

  const dng_ifd &rawIFD = *info.fIFD[info.fMainIndex];
  metadata.isBayer = rawIFD.fPhotometricInterpretation == piCFA;
  metadata.inputWidth = rawIFD.fImageWidth;
  metadata.inputHeight = rawIFD.fImageLength;
  decodeStart = Clock::now();
  negative->ReadStage1Image(host, stream, info);
  return true;
}

bool decodeStages(ConcurrentDngHost &host,
                  const PipelineConfig &config,
                  dng_negative &negative,
                  const ParsedDngMetadata &metadata,
                  int32_t maxDim,
                  const Clock::time_point &decodeStart,
                  DngPipelineResult &result) {
  const bool isBayer = metadata.isBayer;
  const uint32_t inputWidth = metadata.inputWidth;
  const uint32_t inputHeight = metadata.inputHeight;

  // R2 sized decode: only the Bayer/CFA route builds a Stage3 image and can
  // reach the scaled Stage4 kernel. Non-Bayer (lossy) input has no Stage3 image
  // at all, so a sized request degrades to full resolution here — once, at the
  // routing point, rather than at each downstream SetMaximumSize. Loud by
  // design: a silently cropped or silently full-size result is the failure mode
  // this whole path exists to avoid.
  int32_t effectiveMaxDim = maxDim;
  if (effectiveMaxDim > 0 && !isBayer) {
    std::cerr << "[Pipeline] sized decode unsupported for this path; "
                 "falling back to full resolution\n";
    effectiveMaxDim = 0;
  }

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

  // W0: Stage1/2 boundary timing for per-stage Android ledger.
  // decodeStart was set just before ReadStage1Image in parseDngFile.
  const auto stage1End = Clock::now();
  const double stage1_ms =
      std::chrono::duration<double, std::milli>(stage1End - decodeStart).count();

  {
    const bool enableStage2DeviceHandoff =
        !isBayer && config.route.stage2_stage4_device_handoff;
    ScopedStage2DeviceHandoff guard(enableStage2DeviceHandoff);
    // M-6: clear any stale dispatch-failure flag before entering BuildStage2.
    halide_stage2_ol2_clear_dispatch_failure();
    negative.BuildStage2Image(host);
    // M-6: check the dispatch-failure flag set by the OL2 bridge (status-return
    // path, replaces the old DngOl2DispatchError throw-as-control-flow).
    if (halide_stage2_ol2_dispatch_failed()) {
      result.error_code = kDngErrOl2DispatchFailed;
      std::cerr << "[Pipeline] OL2 dispatch failed (status-return)\n";
      return false;
    }
  }

  const auto stage2End = Clock::now();
  const double stage2_ms =
      std::chrono::duration<double, std::milli>(stage2End - stage1End).count();
#if defined(__ANDROID__)
  // L-11: gated behind DNG_PIPELINE_VERBOSE env flag.
  if (pipelineVerbose()) {
    fprintf(stderr,
        "[Stage12-Perf] stage1=%.1f ms stage2=%.1f ms\n",
        stage1_ms, stage2_ms);
  }
#endif

  // Phase 10 Sprint D-B F1: do NOT eagerly resize stage3Workspace here.
  // FFI path gets a lazy mmap pool via prepareStage3WorkspacePtr; this
  // eliminates the ~262ms zero-fill that was inside decodeStart-decodeEnd.
  // test_decode harness passes its own pre-sized vector so is unaffected.
  std::vector<uint16_t> stage3Workspace;
  DngPipelineStage3Timing stage3Timing;

  // H-1: RAII guard for the checkout-style output pools. If any path below
  // acquires a buffer (via acquireStage4OutputBuffer) but then fails, the
  // guard releases it back to the pool on scope exit — preventing the
  // checked-out buffer from leaking permanently. Disarmed on success.
  // Exactly one guard is active per decode: RGBA when fuse_rgba_output is
  // true (production FFI path), RGB when false (test harness / rollback).
  ScopedRgbaCheckout checkoutGuard(result.rgb_ptr, config.fuse_rgba_output);
  ScopedRgbCheckout rgbCheckoutGuard(result.rgb_ptr, !config.fuse_rgba_output);

  // Phase 8.2.2: try fused Stage3+4 device handoff when applicable.
  bool allDone = false;
  if (isBayer) {
    allDone = runHalideStage3And4Fused(
        host, negative, config, inputWidth, inputHeight, effectiveMaxDim,
        &stage3Timing, &stage3Workspace,
        result.rgb_ptr, result.rgb_size, result.width, result.height);
  } else {
    bool restoreFailed = false;
    allDone = runLossyStage2Stage4DeviceHandoff(
        host, negative, config, inputWidth, inputHeight,
        result.rgb_ptr, result.rgb_size, result.width, result.height,
        restoreFailed);
    if (restoreFailed) {
      result.error_code = kDngErrStage2HandoffRestoreFailed;
      return false;
    }
  }

  const auto decodeEnd = Clock::now();
  result.decode_ms =
      std::chrono::duration<double, std::milli>(decodeEnd - decodeStart).count();

  if (allDone) {
    result.process_ms = 0;
    result.error_code = kDngSuccess;
    checkoutGuard.commit();
    rgbCheckoutGuard.commit();
    return true;
  }

  // Normal Stage3 + Stage4 path (fused not applicable or dispatch failed).
  // M-2: use runStage3WithConfig to avoid redundant PipelineConfig::loadFromEnv().
  bool stage3Ok =
      runStage3WithConfig(host, negative, isBayer, config, &stage3Timing,
                          isBayer ? &stage3Workspace : nullptr);
  if (!stage3Ok) {
    result.error_code = kDngErrStage3Failed;
    return false;
  }

  const auto processStart = Clock::now();
  if (!runStage4ToRgb(host, negative, config, inputWidth, inputHeight,
                      effectiveMaxDim, result.rgb_ptr, result.rgb_size,
                      result.width, result.height)) {
    result.error_code = kDngErrStage4Failed;
    return false;
  }
  const auto processEnd = Clock::now();
  result.process_ms =
      std::chrono::duration<double, std::milli>(processEnd - processStart).count();
  result.error_code = kDngSuccess;
  checkoutGuard.commit();
  rgbCheckoutGuard.commit();
  return true;
}

} // namespace

// W7-B (P15): public shared RGBA output pool accessors (declared in
// dng_pipeline.h). Forward to the anonymous-namespace singleton, whose names
// remain in scope for the rest of this translation unit.
uint8_t *dng_rgba_output_acquire(size_t bytes) {
  return rgbaOutputPool().acquire(bytes);
}

bool dng_rgba_output_release(uint8_t *ptr) {
  return rgbaOutputPool().release(ptr);
}

size_t dng_rgba_output_checked_out_count() {
  return rgbaOutputPool().checked_out_count();
}

// 7.1: public shared RGB output pool accessors (mirrors RGBA pattern above).
bool dng_rgb_output_release(uint8_t *ptr) {
  return rgbOutputPool().release(ptr);
}

size_t dng_rgb_output_checked_out_count() {
  return rgbOutputPool().checked_out_count();
}

// Single-flight mutex serializing the public FFI entry points against the
// background warmup hook.  Both paths mutate shared native pools (RGBA output
// pool, polynomial scratch caches) and DeviceHandoffState; without this lock a
// warmup invoked from idle could race a concurrent decode and corrupt those
// shared resources.  Function-local static guarantees thread-safe init.
static std::mutex &pipelineSingleFlightMutex() {
  static std::mutex m;
  return m;
}

// L-4: atomic counter incremented at decode_to_rgb entry so warmup can
// cheaply detect a pending decode and yield between sub-steps.  Function-local
// static for thread-safe init matching pipelineSingleFlightMutex pattern.
static std::atomic<int> &pendingDecodeCount() {
  static std::atomic<int> count{0};
  return count;
}

#if defined(__ANDROID__)
// R3-3: VkPipelineCache persistence bridge (weak — resolves to the Halide
// Vulkan runtime fork when compiled in, nullptr otherwise; see
// native/halide_runtime_fork/README.md). prepare() eagerly creates the
// context + cache object and returns the status bitmask (bit 4 = the cache
// file was loaded AND validated this launch — a rejected/corrupted envelope
// does NOT set it).
extern "C" __attribute__((weak)) int dng_vk_pipeline_cache_prepare(void);
#endif

bool dng_pipeline_warmup_for_size(int32_t width, int32_t height) {
  // L-4: warmup lock split — each sub-step acquires and releases
  // pipelineSingleFlightMutex independently so a pending decode can
  // interleave between steps rather than blocking for the full warmup
  // duration.  Pending-decode detection uses an atomic counter incremented
  // at decode_to_rgb entry; warmup yields early (returns true = no error)
  // when a decode is waiting, letting the decode proceed immediately while
  // the remaining prewarm steps run on a future idle call.

  // Step 1: GPU capability check.
  {
    std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
    if (!requireGpuBackend("warmup")) {
      return false;
    }
  }

  // Yield to pending decode between steps.
  if (pendingDecodeCount().load(std::memory_order_acquire) > 0) return true;

  // Step 2: Pool touch (stage3 workspace, RGB output, RGBA output pools).
  {
    std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
    if (!warmPipelinePoolsForSize(width, height)) {
      return false;
    }
  }

  if (pendingDecodeCount().load(std::memory_order_acquire) > 0) return true;

#if defined(__ANDROID__)
  // R3-3 Option B (user-approved trade-off, 2026-07-05): when the persistent
  // VkPipelineCache was loaded from disk this launch (cross-launch HIT,
  // status bit 4), skip the full-size S3/S4 prewarm dispatches below —
  // pipeline compilation, their main purpose, is already served from the
  // cache at first decode. Measured effect (cc5bf709): second-launch warmup
  // ~1500 → ~200-300ms; app-cold first decode inherits the buffer-alloc/D2H
  // staging cold cost (~560ms vs 428ms with full prewarm) — a KNOWN,
  // user-approved limitation; launch→first-image total improves ~1550→~810ms.
  // Keyed off bit 4 only: control (no cache path), corrupted or
  // driver-mismatched cache files all keep the full prewarm. Defeatable via
  // DNG_VK_SKIP_PREWARM_ON_HIT=0 (and DNG_VK_PIPELINE_CACHE=0 disables the
  // whole feature). No-op on macOS/fork-less builds (weak symbol = nullptr).
  if (&dng_vk_pipeline_cache_prepare != nullptr) {
    const char *defeat = std::getenv("DNG_VK_SKIP_PREWARM_ON_HIT");
    const bool allowSkip = !(defeat && defeat[0] == '0' && defeat[1] == '\0');
    if (allowSkip) {
      const int cacheStatus = dng_vk_pipeline_cache_prepare();
      if (pipelineVerbose()) {
        std::fprintf(stderr,
                     "[Warmup] VkPipelineCache prepare status=%d skip_prewarm=%d\n",
                     cacheStatus, (cacheStatus & 4) ? 1 : 0);
        std::fflush(stderr);
      }
      if (cacheStatus & 4) {
        // Pools are touched (Step 2); pipelines come from the cache.
        return true;
      }
    }
  }
#endif

  // Step 3: Stage3 fused demosaic+WarpRectilinear GPU pipeline prewarm.
  // T9 (W7-C): do NOT prewarm polynomial3 here. polynomial3 is the Stage2
  // lossy MapPolynomial pipeline; the lossless Bayer main path (S3 fused + S4
  // render) never dispatches it, so building it during the startup warmup is
  // ~300ms of pure waste plus a ~3x144MB eager memset (Gotcha #62). Lossy
  // decodes self-prewarm lazily at decodeStages() (`if (!isBayer)` guard,
  // per-size cached), so the only effect of skipping here is that the very
  // first lossy decode of a size pays the build instead of the idle warmup —
  // an acceptable trade for the common (lossless) path.
  // W7-E: prime the Stage3 fused demosaic+WarpRectilinear GPU pipeline at the
  // actual size so the first Bayer decode skips the Vulkan pipeline-creation
  // cold tax (no-op on non-Android; per-size cached inside).
  {
    std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
    dng_demosaic_warp_prewarm_for_size(width, height);
  }

  if (pendingDecodeCount().load(std::memory_order_acquire) > 0) return true;

  // G3 Step 3b: Stage3 prewarm at common sensor-padded dimensions.
  // Lossless DNGs have a raw sensor area larger than the DefaultCropSize
  // (e.g., Sony RX100: sensor 6048×4024, crop 6000×4000 — 24px border per
  // side). The startup warmup receives crop dimensions from the Dart side, but
  // Stage3 dispatches at the full sensor resolution. If the Vulkan pipeline
  // was only built at crop dimensions, the first lossless decode pays a
  // residual cold tax from buffer-allocation at the larger size.
  // Prewarm at (crop_w + 48, crop_h + 24) covers the most common sensor
  // margin pattern. Per-size cached: no-op if (crop_w+48 == crop_w etc.) or if
  // the exact size was already warmed.
  {
    const int32_t sensorW = width + 48;
    const int32_t sensorH = height + 24;
    if (sensorW != width || sensorH != height) {
      std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
      dng_demosaic_warp_prewarm_for_size(sensorW, sensorH);
    }
  }

  if (pendingDecodeCount().load(std::memory_order_acquire) > 0) return true;

  // Step 4: Stage4 render GPU pipeline prewarm (~440ms cold).
  // W7-E: prime the Stage4 render GPU pipeline at the actual size (matrix S4
  // cold ~440ms vs warm ~192ms). No-op on non-Android; per-size cached inside.
  // G3: pass the pool RGBA buffer so the prewarm's D2H warms the exact host
  // pages production will target — eliminates ~63ms first-decode copy_host
  // cold penalty (SM8650 measurement: 80.5→17.0ms after this change).
  {
    std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
    const size_t rgbaByteCount =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    uint8_t *rgba = rgbaOutputPool().acquire(rgbaByteCount);
    if (rgba) {
      dng_render_stage4_prewarm_for_size(width, height, rgba, rgbaByteCount);
      rgbaOutputPool().release(rgba);
    } else {
      dng_render_stage4_prewarm_for_size(width, height);
    }
  }

  return true;
}

// M-2: public API preserved for external callers (test_decode.cpp etc.).
// Loads PipelineConfig from env and delegates to the anonymous-namespace
// runStage3WithConfig helper.
bool dng_pipeline_run_stage3(dng_host &host,
                                dng_negative &negative,
                                bool use_halide_bayer,
                                DngPipelineStage3Timing *timing,
                                std::vector<uint16_t> *stage3_workspace) {
  const PipelineConfig config = PipelineConfig::loadFromEnv();
  return runStage3WithConfig(host, negative, use_halide_bayer, config,
                             timing, stage3_workspace);
}

bool dng_pipeline_decode_to_rgb(const char *file_path,
                                   DngPipelineResult &result) {
  // R2 sized decode: the old entry is a thin forward. max_dim 0 makes every
  // downstream cap resolve to the previous expression, so full-resolution
  // behaviour is unchanged by construction.
  return dng_pipeline_decode_to_rgb_sized(file_path, 0, result);
}

bool dng_pipeline_decode_to_rgb_sized(const char *file_path,
                                         int32_t max_dim,
                                         DngPipelineResult &result) {
  // L-4: signal pending decode so warmup yields between sub-steps.  Counter
  // is incremented before the mutex lock so warmup (which checks the counter
  // after releasing the mutex between steps) detects this decode immediately.
  // RAII guard ensures decrement on all exit paths including exceptions.
  pendingDecodeCount().fetch_add(1, std::memory_order_release);
  struct PendingDecodeGuard {
    ~PendingDecodeGuard() {
      pendingDecodeCount().fetch_sub(1, std::memory_order_release);
    }
  } pendingGuard;

  std::lock_guard<std::mutex> guard(pipelineSingleFlightMutex());
  result = DngPipelineResult{};
  if (!file_path || !file_path[0]) {
    result.error_code = kDngErrNullPath;
    return false;
  }

  try {
    PipelineConfig config = PipelineConfig::loadFromEnv();
    // W7 (M-11): fuse RGBA8 output on all platforms. Android Vulkan: the bridge
    // repacks planar→RGBA directly. macOS Metal: the Stage4 generator now writes
    // RGBA8 in-kernel (alpha=255), so the FFI rgb_to_rgba pass and the separate
    // ~72MB RGB intermediate are both eliminated. Config-gated: set to false for
    // rollback (requires reverting DngRenderStage4 generator to 3-channel RGB).
    // 7.1: env override for testing the RGB checkout path and rollback.
    // DNG_FUSE_RGBA=0 forces the non-fused RGB path; default remains true.
    // Consolidated into PipelineConfig::route.fuse_rgba (see loadFromEnv()).
    config.fuse_rgba_output = config.route.fuse_rgba;
    // L-11: gated behind DNG_PIPELINE_VERBOSE env flag.
    if (pipelineVerbose()) {
      fprintf(stderr, "[Route] DNG_FUSE_RGBA fuse_rgba_output=%s\n",
              config.fuse_rgba_output ? "true" : "false");
    }
    // L-11: GPU backend banner — once-per-process to avoid per-decode spam.
    {
      static std::once_flag backendBannerOnce;
      std::call_once(backendBannerOnce, [&]() {
        fprintf(stderr, "[Pipeline] GPU backend: %s (available=%s)\n",
                dng_halide_gpu_backend_name(),
                dng_halide_gpu_available() ? "yes" : "no");
      });
    }

    // W1-07 capability gate: fail-fast before any DNG parsing when GPU is
    // unavailable. RouteConfig has no SDK-CPU mode; the pipeline is
    // GPU-mandatory on all platforms. Log the rejection and return -6.
    if (!requireGpuBackend("decode")) {
      result.error_code = kDngErrGpuUnavailable;
      return false;
    }
    const uint32_t decodeThreads =
        config.threads.area_threads > 0 ? config.threads.area_threads
                                        : PipelineConfig::kDefaultAreaThreads;
    ConcurrentDngHost host(decodeThreads);
    dng_file_stream stream(file_path);

    dng_info info;
    AutoPtr<dng_negative> negative;
    ParsedDngMetadata metadata;
    Clock::time_point decodeStart;
    if (!parseDngFile(host, stream, info, negative, metadata, decodeStart,
                      result)) {
      return false;
    }
    const bool ok =
        decodeStages(host, config, *negative, metadata, max_dim, decodeStart,
                     result);
    // W7-B: on the fused path the orchestrators filled result.rgb_ptr with an
    // RGBA8 buffer from the checkout pool; expose it as rgba_ptr and null out
    // rgb_ptr so the FFI layer takes the zero-extra-pass path.
    if (ok && config.fuse_rgba_output && result.rgb_ptr) {
      result.rgba_ptr = result.rgb_ptr;
      result.rgba_size = result.rgb_size;
      result.rgb_ptr = nullptr;
      result.rgb_size = 0;
    }
    return ok;
  } catch (const dng_exception &e) {
    result.error_code = e.ErrorCode();
    std::cerr << "[Pipeline] DNG exception: " << result.error_code << "\n";
    return false;
  // M-6: DngOl2DispatchError catch removed — dispatch failure is now handled
  // via halide_stage2_ol2_dispatch_failed() status-return in decodeStages().
  } catch (const std::exception &e) {
    result.error_code = kDngErrStdException;
    std::cerr << "[Pipeline] Exception: " << e.what() << "\n";
    return false;
  } catch (...) {
    result.error_code = kDngErrUnknownException;
    std::cerr << "[Pipeline] Unknown exception\n";
    return false;
  }
}
