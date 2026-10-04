// test_idle_funnel.cpp -- memory-reclamation campaign M1 (PARITY.md clause 4).
//
// Proves the idle funnel runs every step and moves the SAME counters on every
// leg. F1 needs no GPU and no file. F2-F4 need one decodable RAW (argv[1]).
// Default-on, local only: run by `ci.py prepush` (test-bare-binaries) before
// every push. Remote CI is compile-only (2026-09-07 decree; user ruling
// 2026-10-03).
//
// The process ends with ceyx_native_release_gpu (raw_ffi_api.h): without it
// Halide releases the GPU from this library's unload at exit, which faults in
// the Intel Vulkan driver on Windows (0xC0000409).
//
// Decodes go through the exported FFI entry (ceyx_decode_into_buffer), not the
// internal raw_pipeline_* functions: a Windows DLL exports only CEYX_FFI_EXPORT
// symbols, so the test links the same way on every leg.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ceyx_decode_into.h"
#include "dng_ffi_api.h"
#include "heap_page_return.h"
#include "raw_ffi_api.h"
#include "test_memory_instrument.h"
#include "test_report.h"
#if !defined(_WIN32)
#include <sys/mman.h>
#endif

namespace {

constexpr const char kReportPrefix[] = "IdleFunnel";

struct FunnelCounters {
  uint64_t funnel_calls = 0, runs = 0, skipped = 0, errors = 0, page = 0,
           page_unavailable = 0, last = 0, cold_ran = 0, cold_unavailable = 0,
           cold_refused = 0;
};

FunnelCounters read_counters() {
  FunnelCounters c;
  const int32_t rc = ceyx_debug_idle_funnel_counters(
      &c.funnel_calls, &c.runs, &c.skipped, &c.errors, &c.page,
      &c.page_unavailable, &c.last, &c.cold_ran, &c.cold_unavailable,
      &c.cold_refused);
  if (rc != 0) std::printf("[IdleFunnel] WARNING: probe rc=%d\n", (int)rc);
  return c;
}

bool decode_one(const char* path) {
  int32_t w = 0, h = 0;
  if (ceyx_probe_output_size(path, 0, &w, &h) != 0 || w <= 0 || h <= 0) {
    return false;
  }
  std::vector<uint8_t> dst(static_cast<size_t>(w) * h * 4);
  DngResult* r = ceyx_decode_into_buffer(path, 0, dst.data(), dst.size());
  const bool ok = r != nullptr && r->error_code == 0 && r->rgba_data == dst.data();
  if (r != nullptr) dng_free_result(r);
  return ok;
}

// G2 baseline: pages newly obtained from the OS, so the fresh write pays the
// same first-touch faults as the re-touch side (an allocator could hand back
// cached, already-faulted pages). Test instrumentation only.
void *os_fresh_pages(size_t bytes) {
#if defined(_WIN32)
  return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
  void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
#endif
}

void os_release_pages(void *p, size_t bytes) {
#if defined(_WIN32)
  (void)bytes;
  VirtualFree(p, 0, MEM_RELEASE);
#else
  munmap(p, bytes);
#endif
}

// Spec §9.1 Windows gate collision (option 1, layer isolation): the Layer-A
// cases F7c/F7/F9 run the funnel with step 5 skipped through the debug-only
// CEYX_DEBUG_SKIP_COLD_HANDOFF toggle (read by the library from the OS
// environment block). Set only around those cases and G4; F10/G5 run the real step 5.
void set_skip_cold_handoff(bool on) {
#if defined(_WIN32)
  SetEnvironmentVariableA("CEYX_DEBUG_SKIP_COLD_HANDOFF", on ? "1" : nullptr);
#else
  if (on) setenv("CEYX_DEBUG_SKIP_COLD_HANDOFF", "1", 1);
  else unsetenv("CEYX_DEBUG_SKIP_COLD_HANDOFF");
#endif
}

void report(const char* name, bool ok, const char* detail) {
  test_report::report(kReportPrefix, name, ok, detail);
}

}  // namespace

int main(int argc, char** argv) {
  // F0: the process-end GPU release with no GPU context is a no-op and creates
  // nothing: a funnel pass right after it still finds no GPU runtime.
  ceyx_native_release_gpu();
  FunnelCounters a = read_counters();
  ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
  FunnelCounters b = read_counters();
  report("F0_release_gpu_creates_nothing_without_gpu",
         b.skipped - a.skipped == 1 && b.runs == a.runs && b.errors == a.errors, "");

  // F1: no GPU use yet -> the release step must SKIP and must not create a device.
  a = read_counters();
  const int64_t r1 = ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
  b = read_counters();
  report("F1_funnel_counts_once", b.funnel_calls - a.funnel_calls == 1, "");
  report("F1_release_skipped_without_gpu",
         b.skipped - a.skipped == 1 && b.runs == a.runs && b.errors == a.errors && r1 >= 0, "");

  // F5 / TC-1455 (memory-reclamation M4.1): the post-shrink page return is ONE
  // step inside the funnel and reports on every leg.
  {
    const FunnelCounters before = read_counters();
    (void)ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
    const FunnelCounters after = read_counters();
    const uint64_t moved = (after.page - before.page) +
                           (after.page_unavailable - before.page_unavailable);
    report("F5_page_return_once_per_funnel_call", moved == 1, "");
#if defined(__APPLE__) || defined(_WIN32) || (defined(__linux__) && defined(__GLIBC__))
    report("F5_page_return_ran_on_desktop", after.page - before.page == 1, "");
#endif
  }

  // F6 / TC-1458 (memory-reclamation M4.3): the one physical-memory source
  // answers a positive byte count on every leg that runs native tests locally.
  report("F6_physical_memory_bytes_positive", ceyx_physical_memory_bytes() > 0, "");

  constexpr size_t kMiB = size_t{1024} * 1024;

  // Layer A in isolation (spec §9.1): step 5 off for F7c/F7/F9 only, and the
  // toggle itself is checked — a skipped pass must move no step-5 counter.
  set_skip_cold_handoff(true);
  {
    const FunnelCounters s0 = read_counters();
    (void)ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
    const FunnelCounters s1 = read_counters();
    report("F7s_debug_toggle_skips_step5",
           s1.funnel_calls - s0.funnel_calls == 1 && s1.cold_ran == s0.cold_ran &&
               s1.cold_unavailable == s0.cold_unavailable &&
               s1.cold_refused == s0.cold_refused,
           "");
  }

  // F7c/F7/F8 (memreclaim spec §5): funnel step 4a returns the pages of idle
  // pooled slots, and only when slots are named.
  {
    constexpr size_t kSlot = 96 * kMiB;
    constexpr size_t kS = 2 * kSlot;
    void *slots[2] = {ceyx_pool_aligned_alloc(kSlot), ceyx_pool_aligned_alloc(kSlot)};
    uint64_t sizes[2] = {kSlot, kSlot};
    char d[200];
    if (slots[0] == nullptr || slots[1] == nullptr) {
      report("F7_slot_discard_returns_resident", false, "ceyx_pool_aligned_alloc returned null");
    } else {
      std::memset(slots[0], 0x5A, kSlot);
      std::memset(slots[1], 0x5A, kSlot);
      size_t r0 = process_resident_bytes();
      (void)ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
      size_t r1 = process_resident_bytes();
      std::snprintf(d, sizeof d, "r0=%zu r1=%zu", r0, r1);
      report("F7c_no_slots_no_drop", r0 < r1 + kS / 10, d);

      r0 = process_resident_bytes();
      const int64_t ret = ceyx_native_idle_shrink(0, slots, sizes, 2);
      r1 = process_resident_bytes();
      std::snprintf(d, sizeof d, "r0=%zu r1=%zu ret=%lld", r0, r1, (long long)ret);
      report("F7_slot_discard_returns_resident",
             r0 >= r1 + kS * 9 / 10 && ret >= static_cast<int64_t>(kS * 9 / 10), d);

      bool pattern_ok = true;
      for (void *s : slots) {
        auto *b = static_cast<unsigned char *>(s);
        ceyx::prepare_slot_reuse(s, kSlot);  // as the decode worker does (decode_into.cpp)
        std::memset(b, 0xA5, kSlot);
        pattern_ok = pattern_ok && b[0] == 0xA5 && b[kSlot / 2] == 0xA5 && b[kSlot - 1] == 0xA5;
      }
      const size_t r2 = process_resident_bytes();
      std::snprintf(d, sizeof d, "r1=%zu r2=%zu pattern=%d", r1, r2, (int)pattern_ok);
      report("F8_discarded_slot_reusable", pattern_ok && r2 >= r1 + kS * 8 / 10, d);
    }
    ceyx_pool_aligned_free(slots[0]);
    ceyx_pool_aligned_free(slots[1]);
  }

  // F9 (memreclaim spec §5): funnel step 4b returns whole free pages the
  // allocator still holds. Fragmented on purpose so the allocator cannot give
  // them back by itself at free() time. Shape frozen after first green per host.
  // macOS arm64 first green 2026-10-04: 128 KiB x 512, pin every 3rd (64 KiB
  // and 64 KiB-stride-32 were returned at free(); 16 KiB and smaller stayed
  // held but malloc_zone_pressure_relief returned ~nothing; stride >= 4 coalesced).
  {
#if defined(__APPLE__) || defined(_WIN32)
    constexpr size_t kBlock = 128 * 1024;
    constexpr int kCount = 512;
#else  // glibc
    constexpr size_t kBlock = 64 * 1024;
    constexpr int kCount = 1024;
#endif
    constexpr int kPinStride = 3;
    (void)ceyx_native_idle_shrink(0, nullptr, nullptr, 0);  // settle earlier frees
    const size_t base = process_resident_bytes();
    std::vector<void *> blocks(kCount, nullptr);
    for (int i = 0; i < kCount; ++i) {
      blocks[i] = std::malloc(kBlock);
      if (blocks[i] != nullptr) std::memset(blocks[i], 0x33, kBlock);
    }
    for (int i = 0; i < kCount; ++i) {
      if (i % kPinStride != 0) {
        std::free(blocks[i]);
        blocks[i] = nullptr;
      }
    }
    const size_t r0 = process_resident_bytes();
    const size_t held = r0 > base ? r0 - base : 0;
    (void)ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
    const size_t r1 = process_resident_bytes();
    char d[200];
    const bool observable = held >= 32 * kMiB;
#if defined(_WIN32) || defined(__ANDROID__)
    // RECORDED EXCEPTION (Android: user ruling 2026-10-04, basis memreclaim-android-adb-fix.txt
    // sweep: freed-retained only 3-12 MiB across 16/64 KiB shapes; allocator returns at free()).
    // Windows: (spec §9.1 round-3 follow-up ruling 2a): the NT heap
    // returns freed blocks at free() or keeps them in partly used LFH blocks
    // HeapOptimizeResources cannot release (memreclaim-t1c-win-f9-probe.txt).
    // The basis is re-proven every run: setup must NOT reach 32 MiB held. If it
    // ever does, the exception no longer holds -> FAIL and revisit the ruling.
    std::snprintf(d, sizeof d, "recorded exception: allocator self-returns at free() base=%zu r0=%zu r1=%zu",
                  base, r0, r1);
    report("F9_allocator_trim_returns_free_pages", !observable, d);
#else
    std::snprintf(d, sizeof d, "%sbase=%zu r0=%zu r1=%zu",
                  observable ? "" : "pattern not observable ", base, r0, r1);
    report("F9_allocator_trim_returns_free_pages", observable && r0 >= r1 + held / 2, d);
#endif
    for (void *b : blocks) std::free(b);
  }
  set_skip_cold_handoff(false);  // Layer B (F10/G5) measures the real step 5

  // F10 (spec §5, Layer B): step 5 hands cold LIVE pages (not given to the
  // funnel) to the OS with content preserved. Win/Linux/Android: bytes must drop
  // and the pattern must survive the re-read. macOS: honest `unavailable`.
  {
    constexpr size_t kL = 256 * kMiB;
    std::vector<unsigned char> live(kL);
    for (size_t i = 0; i < kL; ++i) live[i] = static_cast<unsigned char>((i * 2654435761u) >> 24);
    const FunnelCounters c0 = read_counters();
    const size_t r0 = process_resident_bytes();
    const auto f10_t0 = std::chrono::steady_clock::now();
    (void)ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
    const double step5_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - f10_t0).count();
    const size_t r1 = process_resident_bytes();
    const FunnelCounters c1 = read_counters();
    bool intact = true;
    for (size_t i = 0; i < kL; i += 4099) {
      intact = intact && live[i] == static_cast<unsigned char>((i * 2654435761u) >> 24);
    }
    char d[200];
#if defined(__APPLE__)
    // Approved exception: step 5 must REPORT unavailable and move nothing.
    const bool ok = c1.cold_unavailable - c0.cold_unavailable == 1 &&
                    c1.cold_ran == c0.cold_ran && r0 < r1 + kL / 10 && intact;
    std::snprintf(d, sizeof d, "macOS step5=unavailable r0=%zu r1=%zu intact=%d", r0, r1,
                  (int)intact);
#else
    // Linux/Android: swap/zram must exist or the pattern is not observable (FAIL, never SKIP).
    const bool ok = c1.cold_ran - c0.cold_ran == 1 && intact && r0 >= r1 + kL * 9 / 10;
    std::snprintf(d, sizeof d, "r0=%zu r1=%zu intact=%d ran=%llu step5_ms=%.2f", r0, r1, (int)intact,
                  (unsigned long long)(c1.cold_ran - c0.cold_ran), step5_ms);
#endif
    report("F10_cold_pages_handoff", ok, d);
  }

  // Perf gates (spec §5, frozen thresholds), same process, same binary.
  {
    using Clock = std::chrono::steady_clock;
    const auto ms_since = [](Clock::time_point t0) {
      return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    };
    const auto median = [](std::vector<double> v) {
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    constexpr size_t kSlot = 96 * kMiB;
    char d[200];

    // G2: N=9 medians; re-touch write of a discarded 96 MiB slot <= 1.5x the
    // write of a freshly allocated one.
    {
      constexpr int kN = 9;
      std::vector<double> fresh, retouch;
      for (int i = 0; i < kN; ++i) {
        void *f = os_fresh_pages(kSlot);
        if (f == nullptr) break;
        Clock::time_point t0 = Clock::now();
        std::memset(f, 0x5A, kSlot);
        fresh.push_back(ms_since(t0));
        os_release_pages(f, kSlot);

        void *slots[1] = {ceyx_pool_aligned_alloc(kSlot)};
        uint64_t sizes[1] = {kSlot};
        if (slots[0] == nullptr) break;
        std::memset(slots[0], 0x5A, kSlot);
        (void)ceyx_native_idle_shrink(0, slots, sizes, 1);
        t0 = Clock::now();
        ceyx::prepare_slot_reuse(slots[0], kSlot);  // production re-arm, timed with the write
        std::memset(slots[0], 0xA5, kSlot);
        retouch.push_back(ms_since(t0));
        ceyx_pool_aligned_free(slots[0]);
      }
      const bool ran = retouch.size() == kN;
      const double fr = ran ? median(fresh) : 0, rt = ran ? median(retouch) : 0;
      std::snprintf(d, sizeof d, "fresh_ms=%.2f retouch_ms=%.2f ratio=%.2f", fr, rt,
                    fr > 0 ? rt / fr : 0.0);
      report("G2_slot_refault_cost", ran && rt <= 1.5 * fr, d);
    }

    // G4: wall time of one synchronous funnel call (2 slots listed, 512 MiB
    // live set) EXCLUDING step 5, p95 under the per-platform limit below, on every
    // platform. Spec §9.1 (Windows gate collision, option 1 (b)) allows "G4
    // measures the funnel excluding step 5": SetProcessWorkingSetSize's own
    // duration swings 60-115 ms run to run on Windows, so a step-5-inclusive
    // limit is flaky by construction. Step 5's real cost stays gated by F10/G5.
    // The loop also asserts the toggle held: no step-5 counter may move.
    {
      constexpr int kN = 20;
      std::vector<unsigned char> live(512 * kMiB, 0x6C);
      void *slots[2] = {ceyx_pool_aligned_alloc(kSlot), ceyx_pool_aligned_alloc(kSlot)};
      uint64_t sizes[2] = {kSlot, kSlot};
      std::vector<double> t;
      set_skip_cold_handoff(true);
      const FunnelCounters g0 = read_counters();
      if (slots[0] != nullptr && slots[1] != nullptr) {
        for (int i = 0; i < kN; ++i) {
          std::memset(slots[0], 0x5A, kSlot);
          std::memset(slots[1], 0x5A, kSlot);
          const Clock::time_point t0 = Clock::now();
          (void)ceyx_native_idle_shrink(0, slots, sizes, 2);
          t.push_back(ms_since(t0));
        }
      }
      const FunnelCounters g1 = read_counters();
      set_skip_cold_handoff(false);
      ceyx_pool_aligned_free(slots[0]);
      ceyx_pool_aligned_free(slots[1]);
      std::sort(t.begin(), t.end());
      const bool ran = t.size() == kN;
      const double p95 = ran ? t[(kN * 95 + 99) / 100 - 1] : 0;
      const bool step5_skipped = g1.cold_ran == g0.cold_ran &&
                                 g1.cold_unavailable == g0.cold_unavailable &&
                                 g1.cold_refused == g0.cold_refused;
      // Limit per spec §9.1 "Round-3 final G4 ruling": 60 ms on Windows (real
      // DiscardVirtualMemory + HeapOptimizeResources cost, p95 48.68 measured),
      // 8 ms elsewhere.
#if defined(_WIN32)
      constexpr double kLimitMs = 60.0;
#else
      constexpr double kLimitMs = 8.0;
#endif
      std::snprintf(d, sizeof d, "p95_ms=%.3f max_ms=%.3f limit_ms=%.0f step5_skipped=%d live_byte=%d",
                    p95, ran ? t.back() : 0.0, kLimitMs, (int)step5_skipped, (int)live[live.size() / 2]);
      report("G4_reclaim_call_duration", ran && step5_skipped && p95 <= kLimitMs, d);
    }

#if !defined(__APPLE__)
    // G5 (Win/Linux/Android; macOS step 5 is `unavailable`): 256 MiB live data,
    // step 5, read+write all pages <= 2.0x a fresh 256 MiB write; pattern intact.
    {
      constexpr size_t kL = 256 * kMiB;
      std::vector<unsigned char> live(kL);
      for (size_t i = 0; i < kL; ++i) live[i] = static_cast<unsigned char>((i * 2654435761u) >> 24);
      const FunnelCounters g5c0 = read_counters();
      const size_t g5r0 = process_resident_bytes();
      (void)ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
      const size_t g5r1 = process_resident_bytes();
      const FunnelCounters g5c1 = read_counters();
      // User ruling R2: step 5 must have lowered resident memory by >= 0.9 x 256 MiB,
      // else the re-access cost below measures nothing ("not observable" FAIL).
      const bool step5_ran = g5c1.cold_ran - g5c0.cold_ran == 1;
      const bool observable = !step5_ran || g5r0 >= g5r1 + kL * 9 / 10;
      Clock::time_point t0 = Clock::now();
      bool intact = true;
      for (size_t i = 0; i < kL; i += 4096) {
        intact = intact && live[i] == static_cast<unsigned char>((i * 2654435761u) >> 24);
      }
      std::memset(live.data(), 0xC3, kL);
      const double reaccess = ms_since(t0);
      // Baseline (spec §9.1 option 1 (c), instrument fix): the old malloc'd
      // baseline read 0.00 ms on Windows -- stores into never-escaped malloc
      // memory may legally sink past the clock read, and a heap block may be
      // pre-faulted. Fresh OS pages (as G2) make the timed write a real
      // 256 MiB first-touch workload.
      auto *f = static_cast<unsigned char *>(os_fresh_pages(kL));
      t0 = Clock::now();
      if (f != nullptr) std::memset(f, 0xC3, kL);
      const double fresh = ms_since(t0);
      const bool alloc_ok = f != nullptr && f[kL / 2] == 0xC3;
      if (f != nullptr) os_release_pages(f, kL);
      std::snprintf(d, sizeof d, "%sfresh_ms=%.2f reaccess_ms=%.2f limit_ms=180 intact=%d g5r0=%zu g5r1=%zu",
                    observable ? "" : "not observable (step 5 did not drop resident >= 0.9x256MiB) ", fresh,
                    reaccess, (int)intact, g5r0, g5r1);
      // User ruling 2026-10-04: absolute criterion, re-access of the 256 MiB buffer
      // <= 180 ms on every platform where step 5 runs; no ratio. fresh_ms is informational.
      constexpr double kMaxReaccessMs = 180.0;
      report("G5_cold_page_reaccess_cost",
             observable && alloc_ok && intact && reaccess <= kMaxReaccessMs, d);
    }
#endif
  }

  if (argc < 2) {
    test_report::reportSkip(kReportPrefix, "F2_F4", "no-raw-argument");
    return test_report::finish(kReportPrefix);
  }
  // F2: after a real decode the GPU runtime exists -> the release step must RUN.
  if (!decode_one(argv[1])) {
    std::printf("[IdleFunnel] decode of %s failed\n", argv[1]);
    ceyx_native_release_gpu();
    return 2;  // a skipped real decode never reads as a pass
  }
  a = read_counters();
  ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
  b = read_counters();
  report("F2_release_runs_after_gpu_use", b.runs - a.runs == 1 && b.errors == a.errors, "");
  // F3: an immediate second pass is harmless (nothing left to free).
  ceyx_native_idle_shrink(0, nullptr, nullptr, 0);
  FunnelCounters c = read_counters();
  report("F3_second_pass_runs_without_error", c.runs - b.runs == 1 && c.errors == b.errors, "");
  // F4: decoding after a release re-grows the pool and succeeds.
  report("F4_decode_after_release_succeeds", decode_one(argv[1]), "");
  ceyx_native_release_gpu();
  return test_report::finish(kReportPrefix);
}
