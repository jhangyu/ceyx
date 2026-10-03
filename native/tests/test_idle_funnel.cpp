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
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ceyx_decode_into.h"
#include "dng_ffi_api.h"
#include "raw_ffi_api.h"
#include "test_report.h"

namespace {

constexpr const char kReportPrefix[] = "IdleFunnel";

struct FunnelCounters {
  uint64_t funnel_calls = 0, runs = 0, skipped = 0, errors = 0, page = 0,
           page_unavailable = 0, last = 0;
};

FunnelCounters read_counters() {
  FunnelCounters c;
  const int32_t rc = ceyx_debug_idle_funnel_counters(
      &c.funnel_calls, &c.runs, &c.skipped, &c.errors, &c.page,
      &c.page_unavailable, &c.last);
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

void report(const char* name, bool ok, const char* detail) {
  test_report::report(kReportPrefix, name, ok, detail);
}

}  // namespace

int main(int argc, char** argv) {
  // F0: the process-end GPU release with no GPU context is a no-op and creates
  // nothing: a funnel pass right after it still finds no GPU runtime.
  ceyx_native_release_gpu();
  FunnelCounters a = read_counters();
  ceyx_native_idle_shrink(0);
  FunnelCounters b = read_counters();
  report("F0_release_gpu_creates_nothing_without_gpu",
         b.skipped - a.skipped == 1 && b.runs == a.runs && b.errors == a.errors, "");

  // F1: no GPU use yet -> the release step must SKIP and must not create a device.
  a = read_counters();
  const int64_t r1 = ceyx_native_idle_shrink(0);
  b = read_counters();
  report("F1_funnel_counts_once", b.funnel_calls - a.funnel_calls == 1, "");
  report("F1_release_skipped_without_gpu",
         b.skipped - a.skipped == 1 && b.runs == a.runs && b.errors == a.errors && r1 >= 0, "");

  // F5 / TC-1455 (memory-reclamation M4.1): the post-shrink page return is ONE
  // step inside the funnel and reports on every leg.
  {
    const FunnelCounters before = read_counters();
    (void)ceyx_native_idle_shrink(0);
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
  ceyx_native_idle_shrink(0);
  b = read_counters();
  report("F2_release_runs_after_gpu_use", b.runs - a.runs == 1 && b.errors == a.errors, "");
  // F3: an immediate second pass is harmless (nothing left to free).
  ceyx_native_idle_shrink(0);
  FunnelCounters c = read_counters();
  report("F3_second_pass_runs_without_error", c.runs - b.runs == 1 && c.errors == b.errors, "");
  // F4: decoding after a release re-grows the pool and succeeds.
  report("F4_decode_after_release_succeeds", decode_one(argv[1]), "");
  ceyx_native_release_gpu();
  return test_report::finish(kReportPrefix);
}
