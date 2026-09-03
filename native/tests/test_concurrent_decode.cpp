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

namespace {

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
  std::vector<std::string> files;
  for (int i = 3; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--warmup") {
      warmupThread = true;
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

  std::atomic<size_t> next{0};
  std::atomic<int> failures{0};
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
        char name[64];
        std::snprintf(name, sizeof(name), "/decode_%zu.raw", i);
        if (!writeRgb(outDir + name, r)) {
          std::fprintf(stderr, "[concurrent] write failed index %zu\n", i);
          failures.fetch_add(1);
        }
        std::snprintf(name, sizeof(name), "/decode_%zu.dims", i);
        writeDims(outDir + name, r);
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

  std::printf("CONCURRENT threads=%d files=%zu repeat=%d warmup=%d "
              "failures=%d\n",
              threads, files.size(), repeat, warmupThread ? 1 : 0,
              failures.load());
  std::fflush(stdout);
  return failures.load() == 0 ? 0 : 1;
}
