#pragma once

// Per-decode state container. Mutex rework (2026-09-03), plan Task 4.
//
// Everything a decode mutates lives here, so two concurrent decodes share no
// object either one can name. This replaces the previous approach of guarding
// each shared static individually — see
// docs/logs/2026-09-03/mutex-rework-spec.md §2.4 for why that approach was
// rejected (an audit of this codebase missed three shared items; a design whose
// correctness depends on the inventory being complete is the wrong bet when the
// failure mode is wrong pixels).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "HalideBuffer.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

class dng_host;

// Bump allocator over lazily-backed zero pages.
//
// No per-allocation free, no free list, no best-fit scan, no eviction policy:
// the whole region is reclaimed by reset() at decode end. Pages arrive on first
// touch (MAP_ANON / MEM_RESERVE|MEM_COMMIT), which preserves the property the
// buffers this replaces were built around (dng_pipeline.cpp cites a ~262ms
// eager zero-fill avoided by the mmap-backed Stage3WorkspacePool).
class DecodeArena {
 public:
  explicit DecodeArena(size_t reserve_bytes) {
    if (reserve_bytes == 0) {
      base_ = nullptr;
      capacity_ = 0;
      return;
    }
#if defined(_WIN32)
    void *p = VirtualAlloc(nullptr, reserve_bytes, MEM_RESERVE | MEM_COMMIT,
                           PAGE_READWRITE);
    base_ = (p == nullptr) ? nullptr : static_cast<uint8_t *>(p);
#else
    void *p = mmap(nullptr, reserve_bytes, PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    base_ = (p == MAP_FAILED) ? nullptr : static_cast<uint8_t *>(p);
#endif
    capacity_ = base_ ? reserve_bytes : 0;
  }

  ~DecodeArena() {
    if (!base_) return;
#if defined(_WIN32)
    // MEM_RELEASE requires size 0 and the exact base returned by VirtualAlloc.
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, capacity_);
#endif
  }

  DecodeArena(const DecodeArena &) = delete;
  DecodeArena &operator=(const DecodeArena &) = delete;

  // Returns nullptr on exhaustion. The arena deliberately does NOT grow: a
  // growing arena would move memory a caller is still holding, which is the
  // exact bug class this design removes.
  void *allocate(size_t bytes, size_t align = 64) {
    if (!base_) return nullptr;
    // align must be a power of two; the mask below assumes it.
    if (align == 0 || (align & (align - 1)) != 0) return nullptr;
    const size_t aligned = (offset_ + (align - 1)) & ~(align - 1);
    if (aligned < offset_) return nullptr;             // overflow on rounding
    if (bytes > capacity_ - aligned) return nullptr;   // overflow-safe bound
    offset_ = aligned + bytes;
    if (offset_ > high_water_) high_water_ = offset_;
    return base_ + aligned;
  }

  void reset() { offset_ = 0; }
  size_t high_water() const { return high_water_; }
  size_t capacity() const { return capacity_; }

 private:
  uint8_t *base_ = nullptr;
  size_t capacity_ = 0;
  size_t offset_ = 0;
  size_t high_water_ = 0;
};

// M-5: Value object grouping the writeback destination pointer and its
// interleaved layout geometry. Moved here from dng_opcodelist2_halide.cpp by
// plan Task 5 (A2) together with DeviceHandoffState.
struct WritebackTarget {
  uint16_t *base = nullptr;
  int32_t col_step = 0;
  int32_t row_step = 0;
  int32_t plane_step = 0;

  // True iff a valid writeback destination has been captured.
  bool has_target() const { return base != nullptr; }

  // Reset to empty / invalid state.
  void reset() { *this = WritebackTarget{}; }
};

// A2: Stage-2 -> Stage-4 device-handoff state. Moved verbatim from the
// anonymous namespace of dng_opcodelist2_halide.cpp EXCEPT that its
// `std::mutex mu` is gone: the object now lives on a per-decode context, so
// only the decode that owns it can name it and publish -> consume -> reset
// cannot interleave with another decode's sequence.
// The old struct's `requested` field is NOT reproduced here: it was the
// process-global enable flag that plan Task 5 (A6) replaces with
// DecodeContext::handoff_enabled below. Keeping both would leave two writable
// copies of one fact — exactly the shape this rework exists to remove.
struct DeviceHandoffState {
  bool valid = false;
  bool host_copied = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t planes = 0;
  uint32_t pixel_range = 0;
  std::unique_ptr<Halide::Runtime::Buffer<uint16_t>> buffer;
  // Interleaved RGB scratch host storage that backs dst_buf in
  // run_polynomial3_kernel when defer_copy_to_host=true. Sized
  // W*H*planes*sizeof(uint16_t); grows on demand but never shrinks within one
  // decode. Lives here so it outlives the call frame and the device buffer can
  // safely reference it.
  std::vector<uint16_t> poly3_scratch;
  // M-5: Destination for scattering GPU results back into the dng_image host
  // memory. Owned by the decode that published it — no cross-decode validity
  // question exists, which is what the single-flight mutex used to answer.
  WritebackTarget writeback;
};

struct DecodeContext {
  explicit DecodeContext(size_t arena_reserve_bytes)
      : arena(arena_reserve_bytes) {}
  DecodeArena arena;

  // A2/A6: device-handoff state and its enable flag. No mutex: only the
  // decode that owns this context can name it, so publish -> consume -> reset
  // is safe by construction rather than by lock discipline.
  DeviceHandoffState handoff;
  bool handoff_enabled = false;

  // B1: OpcodeList2 dispatch error channel. Was a process-global pair whose
  // own comment named pipelineSingleFlightMutex as its protection. Under a
  // shared lock, one decode clearing the flag between another's set and check
  // made the second return SUCCESS with the MapPolynomial colour-correction
  // opcode never applied. Per-decode makes that unreachable.
  bool ol2_dispatch_failed = false;
  std::string ol2_dispatch_failure_msg;

  // A3: Stage-2 device-resident dst scratch. Device memory, so not arena
  // memory; reused across planes within one decode, freed with the context.
  Halide::Runtime::Buffer<uint16_t> stage2_device_dst;
  int stage2_dst_w = 0;
  int stage2_dst_h = 0;
};

// Reaches the context from any dng_host the SDK hands to a bridge. Returns
// nullptr for a plain dng_host (the test_decode harness constructs one), and
// callers MUST handle that by falling back to their pre-existing behaviour.
// Defined in dng_pipeline.cpp, the one TU that already sees both types.
DecodeContext *dng_decode_context_for(dng_host &host);
