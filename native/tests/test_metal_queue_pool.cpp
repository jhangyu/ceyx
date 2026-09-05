// test_metal_queue_pool.cpp — R2-T3 (b): proves the R1-T2 per-thread Metal
// command-queue pool actually pools (count > 1) and respects its cap during a
// concurrent decode, using the DNG_METAL_QUEUE_LOG=1 evidence line emitted by
// dng_metal_context.cpp ("queuepool|ev=bind|thread=...|queue=...|count=%d|cap=%d").
//
// Unlike test_concurrent_decode (which compiles pipeline .cpp sources directly
// and therefore links ZERO ceyx queue-pool code, per r2t1-opus FINDING 3 —
// ceyx/tmp/verify/r2t1-instrument-validation.txt), this target links the real
// dng_decoder_native SHARED library (same as probe_concurrent_raw), so a green
// here is evidence about the actual shipping artifact's queue pool, not a
// symbol-free stand-in.
//
// Usage: test_metal_queue_pool <dng_file>
// Exit 0 iff: every decode succeeded, AND max observed count > 1 (pool is
// actually multi-queue, not degenerated to 1), AND max observed count <= cap
// (never exceeds kMaxDecodeSlots).

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dng_ffi_api.h"

namespace {
constexpr int kThreads = 5;
}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dng_file>\n", argv[0]);
    return 2;
  }
  const std::string dng_file = argv[1];

  // Must be set BEFORE the first decode: dng_metal_context.cpp's
  // logging_enabled() reads DNG_METAL_QUEUE_LOG into a function-local static
  // on first call and never re-reads it. Getting this order wrong does not
  // fail loudly -- it produces bind_lines=0, which is a CONFIDENT, PLAUSIBLE
  // zero indistinguishable from "the pool never bound" (the exact failure
  // shape -- an instrument that looks like it ran but never did -- that has
  // cost this campaign the most across prior rounds). That is why this test
  // treats bind_lines==0 as a hard fail rather than folding it into
  // max_count>1: a zero here must always be legible as "the log never
  // fired", never silently reinterpreted as "count happened to be 0".
  setenv("DNG_METAL_QUEUE_LOG", "1", 1);

  char log_path[] = "/tmp/test_metal_queue_pool_log.XXXXXX";
  const int log_fd = mkstemp(log_path);
  if (log_fd < 0) {
    std::fprintf(stderr, "mkstemp failed: %s\n", std::strerror(errno));
    return 2;
  }
  close(log_fd);
  // The bind log is written to stderr inside dng_metal_context.cpp; capture
  // the whole process's stderr into the temp file for the duration of the
  // concurrent decode, then read it back for the assertion.
  if (!freopen(log_path, "w", stderr)) {
    std::fprintf(stdout, "freopen(stderr) failed\n");
    return 2;
  }

  std::atomic<int> failures{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < kThreads; ++t) {
    pool.emplace_back([&]() {
      DngResult *r = dng_decode_and_process(dng_file.c_str());
      if (!r || r->error_code != 0) {
        failures.fetch_add(1);
      }
      if (r) dng_free_result(r);
    });
  }
  for (auto &th : pool) th.join();

  std::fflush(stderr);
  // Restore stdout-reachable reporting: reopen stderr to the controlling
  // terminal is not portable, so print the verdict to stdout instead (this
  // binary's PASS/FAIL line and RC are what the harness captures).
  FILE *log = std::fopen(log_path, "r");
  if (!log) {
    std::fprintf(stdout, "could not reopen log %s\n", log_path);
    return 2;
  }
  int max_count = 0;
  int cap = 0;
  int bind_lines = 0;
  char line[512];
  while (std::fgets(line, sizeof(line), log)) {
    int count = 0, this_cap = 0;
    // "queuepool|ev=bind|thread=0x...|queue=0x...|count=%d|cap=%d"
    const char *count_tag = std::strstr(line, "count=");
    const char *cap_tag = std::strstr(line, "cap=");
    if (std::strstr(line, "queuepool|ev=bind") && count_tag && cap_tag) {
      count = std::atoi(count_tag + std::strlen("count="));
      this_cap = std::atoi(cap_tag + std::strlen("cap="));
      ++bind_lines;
      if (count > max_count) max_count = count;
      cap = this_cap;  // constant across lines; last write wins, harmless.
    }
  }
  std::fclose(log);
  std::remove(log_path);

  const bool decodes_ok = (failures.load() == 0);
  const bool pool_actually_pooled = (bind_lines > 0 && max_count > 1);
  const bool within_cap = (cap > 0 && max_count <= cap);

  std::fprintf(stdout,
               "METAL_QUEUE_POOL bind_lines=%d max_count=%d cap=%d "
               "decodes_ok=%d pool_actually_pooled=%d within_cap=%d\n",
               bind_lines, max_count, cap, decodes_ok ? 1 : 0,
               pool_actually_pooled ? 1 : 0, within_cap ? 1 : 0);
  std::fflush(stdout);

  return (decodes_ok && pool_actually_pooled && within_cap) ? 0 : 1;
}
