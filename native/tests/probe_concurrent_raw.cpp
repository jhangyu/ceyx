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

// raw_ffi_api.h includes dng_ffi_api.h; the RAW route returns the SAME
// DngResult layout (rgba_data / width / height / error_code / decode_ms /
// process_ms) and is freed with dng_free_result(). Do not hand-roll the free.
#include "raw_ffi_api.h"

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
        DngResult *r = raw_decode_and_process(files[i].c_str(), 0);
        if (!r || r->error_code != 0) {
          failures.fetch_add(1);
        }
        if (r) dng_free_result(r);
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
