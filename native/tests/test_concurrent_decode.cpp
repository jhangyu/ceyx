// test_concurrent_decode.cpp — the concurrent-vs-serial correctness gate.
//
// Decodes each input DNG and dumps interleaved RGB8 to
// <out_dir>/decode_<index>.raw, where <index> is the file's ARGUMENT position
// (among non-flag arguments), not its completion order — so a serial run and a
// concurrent run produce index-comparable dumps for stage4_channel_compare.py.
// A sidecar <out_dir>/decode_<index>.dims records "<width> <height>" so the
// comparison driver can pass the right --width/--height for mixed-resolution
// corpora (stage4_channel_compare.py defaults to 6000x4000).
//
// Usage:
//   test_concurrent_decode <out_dir> <threads> [--warmup] [--repeat R]
//                          <dng_file>...
// Exit 0 iff every decode succeeded AND every pool is fully checked in.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "dng_pipeline.h"
#include "dng_pipeline_config.h"
#include "pipeline/concurrent_dng_host.h"

namespace {

// Plan Task 6 invariant 2 (R8/N2): PerformAreaTaskThreads() divides the
// area-task fan-out by the in-flight decode count, floored at 1, and does so
// AFTER the explicit > env > hardware_concurrency chain resolves. The host is
// constructed exactly the way production constructs it — a non-zero explicit
// requestedThreads_ (dng_pipeline.cpp passes kDefaultAreaThreads by default)
// — so this exercises the branch production actually takes rather than the
// zero-argument fallback, which is the mistake N2 calls out.
int runThreadsDivisorTest() {
  const uint32_t hwRaw = std::thread::hardware_concurrency();
  const uint32_t hw = hwRaw > 0 ? hwRaw : 1;
  int failures = 0;
  // Two production-shaped constructions, both with a non-zero explicit
  // requestedThreads_ (the branch production takes):
  //   requested = kDefaultAreaThreads (20) — the literal default in
  //       dng_pipeline.cpp when DNG_AREA_THREADS is unset.
  //   requested = hardware_concurrency() — what DNG_AREA_THREADS=<hw> yields,
  //       and the case in which the AC's "hardware_concurrency() / T" is the
  //       expected value literally. With requested=20 on a machine with more
  //       than 20 logical cores the hw ceiling does not bind, so the resolved
  //       base is 20 and the expected value is 20/T; expecting hw/T there
  //       would be asserting a number the resolution chain never produces.
  const uint32_t requestedCases[2] = {PipelineConfig::kDefaultAreaThreads, hw};
  for (uint32_t requested : requestedCases) {
    for (size_t T : {size_t{1}, size_t{2}, size_t{4}}) {
      ConcurrentDngHost host(requested);
      host.setInFlightOverrideForTest(T);
      const uint32_t got = host.PerformAreaTaskThreads();
      const uint32_t base = requested < hw ? requested : hw;  // ceiling at :62
      uint32_t want = base / static_cast<uint32_t>(T);
      if (want < 1) want = 1;
      const bool ok = (got == want);
      std::printf("[threads-test] requested=%u T=%zu hw=%u base=%u -> %u "
                  "(want %u) %s\n",
                  requested, T, hw, base, got, want, ok ? "OK" : "FAIL");
      if (!ok) ++failures;
    }
  }
  // Divisor must never produce zero: a zero thread count deadlocks the area
  // task. Probe with an in-flight count far above hardware_concurrency().
  {
    ConcurrentDngHost host(PipelineConfig::kDefaultAreaThreads);
    host.setInFlightOverrideForTest(static_cast<size_t>(hw) * 64 + 7);
    const uint32_t got = host.PerformAreaTaskThreads();
    const bool ok = (got >= 1);
    std::printf("[threads-test] T=%u (oversubscribed) -> %u (want >=1) %s\n",
                hw * 64 + 7, got, ok ? "OK" : "FAIL");
    if (!ok) ++failures;
  }
  std::printf("THREADS_DIVISOR failures=%d\n", failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}

// Write interleaved RGB8. The fused path hands back RGBA8; drop alpha.
bool writeRgb(const std::string &path, const DngPipelineResult &r) {
  const size_t pixels = static_cast<size_t>(r.width) * r.height;
  std::vector<uint8_t> rgb(pixels * 3);
  if (r.rgba_ptr) {
    for (size_t p = 0; p < pixels; ++p) {
      rgb[p * 3 + 0] = r.rgba_ptr[p * 4 + 0];
      rgb[p * 3 + 1] = r.rgba_ptr[p * 4 + 1];
      rgb[p * 3 + 2] = r.rgba_ptr[p * 4 + 2];
    }
  } else if (r.rgb_ptr) {
    std::memcpy(rgb.data(), r.rgb_ptr, rgb.size());
  } else {
    return false;
  }
  FILE *fh = std::fopen(path.c_str(), "wb");
  if (!fh) return false;
  const size_t wrote = std::fwrite(rgb.data(), 1, rgb.size(), fh);
  std::fclose(fh);
  return wrote == rgb.size();
}

void writeDims(const std::string &path, const DngPipelineResult &r) {
  FILE *fh = std::fopen(path.c_str(), "w");
  if (!fh) return;
  std::fprintf(fh, "%u %u\n", r.width, r.height);
  std::fclose(fh);
}

void releaseResult(DngPipelineResult &r) {
  if (r.rgba_ptr) dng_rgba_output_release(r.rgba_ptr);
  if (r.rgb_ptr) dng_rgb_output_release(r.rgb_ptr);
  r.rgba_ptr = nullptr;
  r.rgb_ptr = nullptr;
}

} // namespace

int main(int argc, char **argv) {
  // Plan Task 6 Step 6: the gate driver needs N before it can oversubscribe by
  // two, so this has to work without any decode arguments.
  if (argc >= 2 && std::string(argv[1]) == "--print-slots") {
    std::printf("%zu\n", dng_decode_slot_count());
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "--threads-test") {
    return runThreadsDivisorTest();
  }
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: %s <out_dir> <threads> [--warmup] [--repeat R] "
                 "<dng_file>...\n",
                 argv[0]);
    return 2;
  }
  const std::string outDir = argv[1];
  const int threads = std::atoi(argv[2]);
  if (threads < 1) return 2;

  bool warmupThread = false;
  int repeat = 1;
  // Round 5 review F1: number of SERIAL decodes to run, to completion, before
  // the concurrent burst starts. This is the only way to express the shape that
  // discriminates the M1 fix: the prime decode both sets the process-global
  // `warmed` key AND grows exactly one slot's poly3_scratch, so the burst that
  // follows puts FRESH slots behind an ALREADY-CLOSED gate. With all threads
  // starting together (the previous behaviour) they race through the gate
  // before any of them inserts the key, and the pre-M1 code pre-grows too —
  // which is why the earlier 8-decode run was green either way.
  int primeCount = 0;
  std::vector<std::string> files;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--warmup") {
      warmupThread = true;
    } else if (a == "--prime") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--prime needs a value\n");
        return 2;
      }
      primeCount = std::atoi(argv[++i]);
      if (primeCount < 0) return 2;
    } else if (a == "--repeat") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "--repeat needs a value\n");
        return 2;
      }
      repeat = std::atoi(argv[++i]);
      if (repeat < 1) return 2;
    } else {
      files.emplace_back(a);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "no input files\n");
    return 2;
  }

  // Round 5 review F1: serial prime phase. Runs to completion before any
  // worker thread exists, so the burst below starts with the `warmed` gate
  // already closed and only ONE slot pre-grown.
  for (int p = 0; p < primeCount; ++p) {
    const std::string &f = files[static_cast<size_t>(p) % files.size()];
    DngPipelineResult r{};
    std::fprintf(stderr, "[prime %d/%d] %s\n", p + 1, primeCount, f.c_str());
    std::fflush(stderr);
    if (!dng_pipeline_decode_to_rgb_sized(f.c_str(), 0, r)) {
      std::fprintf(stderr, "[concurrent] prime decode failed %s err=%d\n",
                   f.c_str(), r.error_code);
      releaseResult(r);
      return 1;
    }
    releaseResult(r);
  }
  if (primeCount > 0) {
    std::fprintf(stderr, "[prime] done, burst starts now\n");
    std::fflush(stderr);
  }

  std::atomic<size_t> next{0};
  std::atomic<int> failures{0};
  // Invariant 1 (R7): never more than N contexts live at once, even with more
  // caller threads than slots. Sampled from the decode loop; the pool's own
  // high-water counter (checked after the join) is the authoritative figure,
  // because a sample taken here can miss a peak that occurred between calls.
  std::atomic<size_t> maxObservedInFlight{0};
  std::atomic<bool> decodesDone{false};

  // Gate G0: a warmup thread looping dng_pipeline_warmup_for_size at varying
  // sizes for the duration, so warmup-vs-decode (the race the mutex was
  // introduced to close) is exercised, not just decode-vs-decode.
  std::thread warmer;
  if (warmupThread) {
    warmer = std::thread([&]() {
      static const int32_t kSizes[][2] = {
          {6000, 4000}, {4000, 3000}, {6048, 4024}, {2048, 1536}};
      size_t k = 0;
      while (!decodesDone.load(std::memory_order_acquire)) {
        const int32_t w = kSizes[k % 4][0];
        const int32_t h = kSizes[k % 4][1];
        ++k;
        if (!dng_pipeline_warmup_for_size(w, h)) {
          std::fprintf(stderr, "[concurrent] warmup failed %dx%d\n", w, h);
          failures.fetch_add(1);
          return;
        }
      }
    });
  }

  const size_t total = files.size() * static_cast<size_t>(repeat);
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&, t]() {
      // Per-thread start jitter so the threads do not march in lockstep.
      std::this_thread::sleep_for(std::chrono::microseconds(t * 137));
      for (;;) {
        const size_t slot = next.fetch_add(1);
        if (slot >= total) return;
        const size_t i = slot % files.size();
        DngPipelineResult r{};
        if (!dng_pipeline_decode_to_rgb_sized(files[i].c_str(), 0, r)) {
          std::fprintf(stderr, "[concurrent] decode failed %s err=%d\n",
                       files[i].c_str(), r.error_code);
          failures.fetch_add(1);
          releaseResult(r);
          continue;
        }
        {
          const size_t now = dng_decode_in_flight_count();
          size_t prev = maxObservedInFlight.load();
          while (now > prev &&
                 !maxObservedInFlight.compare_exchange_weak(prev, now)) {}
        }
        // DUMP ONLY ON THE FIRST PASS (slot < files.size()).
        //
        // With --repeat R the work queue is R*N slots and the file index is
        // slot % N, so passes 2..R revisit the same indices. Writing on every
        // pass let two threads open and write the SAME decode_<i>.raw
        // concurrently, outside any lock — which could fabricate a pixel
        // mismatch out of interleaved writes, or mask a real one by having the
        // last writer overwrite a corrupt dump with a good one. Either way the
        // gate's own instrument would be producing the signal it is supposed
        // to be measuring.
        //
        // Every slot still DECODES, so the repeats keep exercising pool
        // growth/reuse/eviction exactly as intended; only the dump is
        // first-pass-only, which is all the comparison needs.
        if (slot < files.size()) {
          char name[64];
          std::snprintf(name, sizeof(name), "/decode_%zu.raw", i);
          if (!writeRgb(outDir + name, r)) {
            std::fprintf(stderr, "[concurrent] write failed index %zu\n", i);
            failures.fetch_add(1);
          }
          std::snprintf(name, sizeof(name), "/decode_%zu.dims", i);
          writeDims(outDir + name, r);
        }
        releaseResult(r);
      }
    });
  }
  for (auto &th : pool) th.join();
  decodesDone.store(true, std::memory_order_release);
  if (warmer.joinable()) warmer.join();

  // Pool leak check: every checkout must have been returned. A non-zero count
  // here means a guard is missing on some exit path, which under concurrency
  // is how the "two decodes share one buffer" bug also manifests.
  const size_t rgbaOut = dng_rgba_output_checked_out_count();
  const size_t rgbOut = dng_rgb_output_checked_out_count();
  if (rgbaOut != 0 || rgbOut != 0) {
    std::fprintf(stderr, "[concurrent] pool leak rgba=%zu rgb=%zu\n", rgbaOut,
                 rgbOut);
    failures.fetch_add(1);
  }

  // Plan Task 6 AC: with N slots and N+2 caller threads, at most N contexts may
  // ever have been live. The pool's high-water counter is incremented under the
  // same lock that hands out slots, so it cannot miss a peak.
  const size_t slots = dng_decode_slot_count();
  const size_t poolHighWater = dng_decode_max_in_flight_observed();

  // Round 5 review F2: the LOAD-BEARING assertions are these two, because they
  // are maintained by the decode body, not by the pool auditing its own free
  // list. `poolHighWater <= slots` is an invariant of the pool's data structure
  // and cannot fail; a pool that handed one context to two callers would keep
  // it green. bodyAliases is exactly that failure, and bodyHighWater counts
  // decodes actually executing.
  const size_t bodyHighWater = dng_decode_body_max_in_flight();
  const size_t bodyAliases = dng_decode_body_alias_events();
  if (bodyHighWater > slots) {
    std::fprintf(stderr,
                 "[concurrent] slot bound violated (decode body): %zu > %zu\n",
                 bodyHighWater, slots);
    failures.fetch_add(1);
  }
  if (bodyAliases != 0) {
    std::fprintf(stderr,
                 "[concurrent] context aliasing: %zu decode(s) found their "
                 "DecodeContext already occupied\n",
                 bodyAliases);
    failures.fetch_add(1);
  }
  // Kept for cross-checking only: if the pool's own count and the decode-body
  // count disagree, one of the two instruments is wrong and the run is not
  // evidence of anything.
  if (poolHighWater > slots) {
    std::fprintf(stderr, "[concurrent] slot bound violated: %zu > %zu\n",
                 poolHighWater, slots);
    failures.fetch_add(1);
  }
  if (maxObservedInFlight.load() > slots) {
    std::fprintf(stderr,
                 "[concurrent] slot bound violated (sampled): %zu > %zu\n",
                 maxObservedInFlight.load(), slots);
    failures.fetch_add(1);
  }

  // Plan Task 7 AC: the Stage4 scratch free list must never exceed its cap.
  // SIZE_MAX means Stage4ScratchPool was not compiled into this build
  // (DNG_STAGE4_SPLIT_KERNEL undefined — i.e. the macOS Metal layout). Declare
  // the skip rather than let an unobservable configuration read as a pass.
  const size_t stage4FreeHighWater = dng_stage4_scratch_free_high_water();
  constexpr size_t kStage4FreeCap = 4;  // Stage4ScratchPool::kMaxFreeSlots
  const bool stage4PoolCompiled = (stage4FreeHighWater != static_cast<size_t>(-1));
  if (!stage4PoolCompiled) {
    std::printf("STAGE4_SCRATCH_CAP skipped=1 reason=pool_not_compiled "
                "(DNG_STAGE4_SPLIT_KERNEL undefined)\n");
  } else if (stage4FreeHighWater > kStage4FreeCap) {
    std::fprintf(stderr,
                 "[concurrent] stage4 scratch free-list cap violated: %zu > %zu\n",
                 stage4FreeHighWater, kStage4FreeCap);
    failures.fetch_add(1);
  }

  std::printf("SLOTS slots=%zu max_in_flight_body=%zu context_alias_events=%zu "
              "max_in_flight_pool=%zu max_in_flight_sampled=%zu "
              "arena_high_water_bytes=%zu stage4_free_high_water=%zu "
              "stage4_free_cap=%zu\n",
              slots, bodyHighWater, bodyAliases, poolHighWater,
              maxObservedInFlight.load(),
              dng_decode_arena_high_water_bytes(), stage4FreeHighWater,
              kStage4FreeCap);

  std::printf("CONCURRENT threads=%d files=%zu repeat=%d warmup=%d "
              "failures=%d\n",
              threads, files.size(), repeat, warmupThread ? 1 : 0,
              failures.load());
  std::fflush(stdout);
  return failures.load() == 0 ? 0 : 1;
}
