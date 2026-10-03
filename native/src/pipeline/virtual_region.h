#pragma once
// virtual_region.h -- one reserve / commit-on-grow / decommit adapter for every
// leg (PARITY.md clause 1); the only home of these OS calls for DecodeArena and
// ZeroCoordBuffer.
//
// Observable contract, identical on every platform:
//   VirtualRegion(n)       reserves n bytes of address space: no pages, no
//                          commit charge.
//   ensure_committed(end)  [base, end) is readable and writable; never-touched
//                          or decommitted pages read zero. Returns false when
//                          the OS refuses (commit limit) or end > capacity;
//                          callers treat that exactly like exhaustion.
//   decommit()             every committed page goes back to the OS -- physical
//                          pages and commit charge -- immediately; the
//                          reservation stays; the next ensure_committed()
//                          re-commits. Contents become undefined.
// Windows binds MEM_RESERVE / MEM_COMMIT / MEM_DECOMMIT. POSIX reserves with a
// read-write private anonymous mapping (no enforced commit charge under the
// default overcommit policy) and returns pages with madvise: Apple
// MADV_FREE_REUSABLE (the only variant measured to drop phys_footprint at
// once), elsewhere MADV_DONTNEED (immediate on a private anonymous mapping;
// MADV_FREE returns pages only under memory pressure).

#include <cstddef>
#include <cstdint>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

class VirtualRegion {
 public:
  // Commit step: bounds the number of commit calls per decode (24 for a
  // 1.5 GiB arena) while keeping the charge within one granule of use.
  static constexpr size_t kCommitGranuleBytes = size_t{64} * 1024 * 1024;

  VirtualRegion() = default;

  explicit VirtualRegion(size_t reserve_bytes) {
    if (reserve_bytes == 0) return;
#if defined(_WIN32)
    base_ = static_cast<uint8_t *>(
        VirtualAlloc(nullptr, reserve_bytes, MEM_RESERVE, PAGE_NOACCESS));
#else
    void *p = mmap(nullptr, reserve_bytes, PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    base_ = (p == MAP_FAILED) ? nullptr : static_cast<uint8_t *>(p);
#endif
    capacity_ = base_ != nullptr ? reserve_bytes : 0;
  }

  VirtualRegion(VirtualRegion &&other) noexcept { swap(other); }

  VirtualRegion &operator=(VirtualRegion &&other) noexcept {
    if (this != &other) {
      release();
      swap(other);
    }
    return *this;
  }

  VirtualRegion(const VirtualRegion &) = delete;
  VirtualRegion &operator=(const VirtualRegion &) = delete;

  ~VirtualRegion() { release(); }

  uint8_t *base() const { return base_; }
  size_t capacity() const { return capacity_; }
  size_t committed_bytes() const { return committed_; }

  bool ensure_committed(size_t end) {
    if (base_ == nullptr || end > capacity_) return false;
    if (end <= committed_) return true;
    size_t target = (end + kCommitGranuleBytes - 1) / kCommitGranuleBytes *
                    kCommitGranuleBytes;
    if (target < end || target > capacity_) target = capacity_;
#if defined(_WIN32)
    if (VirtualAlloc(base_ + committed_, target - committed_, MEM_COMMIT,
                     PAGE_READWRITE) == nullptr) {
      return false;
    }
#endif
    committed_ = target;
    return true;
  }

  void decommit() {
    if (base_ == nullptr || committed_ == 0) return;
#if defined(_WIN32)
    VirtualFree(base_, committed_, MEM_DECOMMIT);
#elif defined(__APPLE__) && defined(MADV_FREE_REUSABLE)
    if (madvise(base_, committed_, MADV_FREE_REUSABLE) != 0) {
      madvise(base_, committed_, MADV_FREE);
    }
#else
    madvise(base_, committed_, MADV_DONTNEED);
#endif
    committed_ = 0;
  }

 private:
  void swap(VirtualRegion &other) noexcept {
    std::swap(base_, other.base_);
    std::swap(capacity_, other.capacity_);
    std::swap(committed_, other.committed_);
  }

  void release() {
    if (base_ == nullptr) return;
#if defined(_WIN32)
    VirtualFree(base_, 0, MEM_RELEASE);
#else
    munmap(base_, capacity_);
#endif
    base_ = nullptr;
    capacity_ = 0;
    committed_ = 0;
  }

  uint8_t *base_ = nullptr;
  size_t capacity_ = 0;
  size_t committed_ = 0;
};
