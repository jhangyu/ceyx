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

struct DecodeContext {
  explicit DecodeContext(size_t arena_reserve_bytes)
      : arena(arena_reserve_bytes) {}
  DecodeArena arena;
  // Task 5 adds: device-handoff state, handoff enable flag, OL2 error channel.
};

// Reaches the context from any dng_host the SDK hands to a bridge. Returns
// nullptr for a plain dng_host (the test_decode harness constructs one), and
// callers MUST handle that by falling back to their pre-existing behaviour.
// Defined in dng_pipeline.cpp, the one TU that already sees both types.
DecodeContext *dng_decode_context_for(dng_host &host);
