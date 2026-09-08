// probe_concurrent_raw.cpp — Task 1 go/no-go probe.
//
// Drives the ALREADY LOCK-FREE RAW path (raw_gpu_pipeline.cpp has no mutex and
// no mutable file-scope state) from N threads in ONE process, so the measured
// scaling reflects Halide's own GPU-context serialisation with ceyx's DNG
// single-flight mutex out of the picture.
//
// Usage: probe_concurrent_raw <threads> <raw_file>...
// Prints one line: "PROBE threads=<N> files=<M> wall_ms=<W>"
// Exit 0 iff every decode succeeded.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

// raw_ffi_api.h includes dng_ffi_api.h.
// WP5: the probe now drives raw_pipeline_decode_file_into (raw_gpu_pipeline.h)
// with a thread-owned buffer, instead of the legacy allocating RAW C ABI
// entry, which
// allocated its output from the native RGBA pool and is deleted by this work
// package. This also FIXES the probe: since WP3 collapsed RgbaCheckoutGuard to
// borrow-only, the null-destination route this file used was refused with
// "makeRgbaCheckout called with no caller_dst" (error -209), so every decode
// failed and the binary exited 1. The measured property -- concurrent scaling
// of the lock-free RAW path -- is unchanged and is now actually measurable.
#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <threads> <raw_file>...\n", argv[0]);
    return 2;
  }
  const int threads = std::atoi(argv[1]);
  if (threads < 1) {
    std::fprintf(stderr, "threads must be >= 1\n");
    return 2;
  }
  std::vector<std::string> files;
  for (int i = 2; i < argc; ++i) files.emplace_back(argv[i]);

  std::atomic<size_t> next{0};
  std::atomic<int> failures{0};

  const auto t0 = std::chrono::steady_clock::now();
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; ++t) {
    pool.emplace_back([&]() {
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= files.size()) return;
        // Mirrors the production develop-parameter construction in
        // raw_ffi_api.cpp exactly; max_dim 0 == full resolution.
        RawDevelopParams develop{};
        develop.exposure_ev = 0.0f;
        develop.tone_curve_strength = 1.0f;
        develop.output_space = kRawOutputColorSpaceSrgb;
        develop.max_output_long_edge = 0u;

        uint32_t pw = 0, ph = 0;
        if (raw_pipeline_probe_output_size(files[i].c_str(), 0, &pw, &ph) !=
                kRawSuccess ||
            pw == 0 || ph == 0) {
          failures.fetch_add(1);
          continue;
        }
        std::vector<uint8_t> dst(static_cast<size_t>(pw) * ph * 4);
        RawPipelineResult raw{};
        const RawErrorCode rc = raw_pipeline_decode_file_into(
            files[i].c_str(), develop, dst.data(), dst.size(), raw);
        // rgba_ptr == dst.data() is the ownership check: no native allocation
        // happened behind the decode. Nothing is released to any pool -- dst
        // is thread-owned and dies with this iteration (invariant I1).
        if (rc != kRawSuccess || raw.rgba_ptr != dst.data()) {
          failures.fetch_add(1);
        }
      }
    });
  }
  for (auto &th : pool) th.join();
  const auto t1 = std::chrono::steady_clock::now();

  const double wall_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::printf("PROBE threads=%d files=%zu wall_ms=%.1f\n", threads,
              files.size(), wall_ms);
  std::fflush(stdout);
  return failures.load() == 0 ? 0 : 1;
}
