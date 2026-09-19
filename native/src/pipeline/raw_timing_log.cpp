// raw_timing_log.cpp — implementation of the single CPU-phase timing emitter.
// Contract, rationale and line schema: native/include/raw_timing_log.h.
//
// This TU deliberately depends on nothing but the two diagnostics structs and
// the C library: it is linked into the shipped dylib unconditionally and must
// stay inert (one relaxed atomic load) when the gate is off.

#include "raw_timing_log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "raw_ffi_api.h"
#include "raw_pipeline_contract.h"

namespace {

// -1 unread, 0 off, 1 on. The gate is a process-lifetime property, so it is
// read once; re-reading getenv per decode would put a lock inside the measured
// window on some libc implementations.
std::atomic<int> g_gate{-1};

bool gate_enabled() {
  const int cached = g_gate.load(std::memory_order_relaxed);
  if (cached >= 0) return cached != 0;
  const char *value = std::getenv("CEYX_RAW_TIMING_LOG");
  // Historically this gate accepted exactly "1" and nothing else. Keeping that
  // strictness (rather than the GPU probe's "any non-empty non-zero") means an
  // existing CEYX_RAW_TIMING_LOG=1 invocation behaves identically, and a typo
  // does not silently produce a half-instrumented run.
  const int decided =
      (value != nullptr && std::strcmp(value, "1") == 0) ? 1 : 0;
  g_gate.store(decided, std::memory_order_relaxed);
  return decided != 0;
}

// Leaked on purpose, for the same reason raw_gpu_timing_probe.cpp leaks its
// mutex: teardown ordering between this TU's function-local statics and the
// threads that may still emit is not guaranteed, and locking a destroyed
// std::mutex aborts.
std::mutex &registry_lock() {
  static std::mutex *m = new std::mutex();
  return *m;
}

std::vector<std::thread::id> &thread_registry() {
  static std::vector<std::thread::id> *v = new std::vector<std::thread::id>();
  return *v;
}

// Dense first-seen index for this thread, cached thread-locally so the shared
// vector is scanned exactly once per thread rather than once per decode.
int lane_for_this_thread() {
  thread_local int cached_lane = -1;
  if (cached_lane >= 0) return cached_lane;
  const std::thread::id self = std::this_thread::get_id();
  std::lock_guard<std::mutex> guard(registry_lock());
  std::vector<std::thread::id> &table = thread_registry();
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i] == self) {
      cached_lane = static_cast<int>(i);
      return cached_lane;
    }
  }
  table.push_back(self);
  cached_lane = static_cast<int>(table.size()) - 1;
  return cached_lane;
}

}  // namespace

extern "C" {

int raw_timing_log_enabled(void) { return gate_enabled() ? 1 : 0; }

int raw_timing_log_lane(void) {
  if (!gate_enabled()) return -1;
  return lane_for_this_thread();
}

void raw_timing_log_emit(const RawTimingDiagnostics *timing,
                         const RawDecodeDiagnostics *diag) {
  if (!gate_enabled()) return;

  // Fixed key set regardless of which half is present: a parser must never
  // have to branch on a variable schema. A missing half prints zeros, which is
  // distinguishable from a real measurement only by context — acceptable
  // because both call sites in the shared decode path always pass both.
  RawTimingDiagnostics empty_timing{};
  RawDecodeDiagnostics empty_diag{};
  const RawTimingDiagnostics &t = timing ? *timing : empty_timing;
  const RawDecodeDiagnostics &d = diag ? *diag : empty_diag;

  // One fprintf: stderr is line-buffered per call, and emitting the whole
  // record in a single write is what keeps lanes from interleaving mid-line at
  // w8. Splitting this into several fprintf calls WILL shred the output.
  std::fprintf(stderr,
               "[RawTiming] schema=2 lane=%d host_to_device_copy_ms=%.3f "
               "device_to_host_copy_ms=%.3f host_copy_ms=%.3f "
               "auto_exposure_ms=%.3f gpu_submit_wait_ms=%.3f "
               "gpu_process_ms=%.3f raw_unpack_ms=%.3f total_ms=%.3f "
               "unified_memory_path_active=%u "
               "source_mosaic_copy_milliseconds=%.3f\n",
               lane_for_this_thread(), t.host_to_device_copy_ms,
               t.device_to_host_copy_ms, t.host_copy_ms, t.auto_exposure_ms,
               t.gpu_submit_wait_ms, d.gpu_process_ms, d.raw_unpack_ms,
               d.total_ms, t.unified_memory_path_active,
               t.source_mosaic_copy_milliseconds);
}

const char *raw_timing_log_marker(void) { return "raw_timing_log_v2"; }

}  // extern "C"
