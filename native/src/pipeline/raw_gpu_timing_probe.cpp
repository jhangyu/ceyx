// raw_gpu_timing_probe.cpp — Phase-0 per-stage GPU timing instrument
// (T-P0a, docs/logs/2026-09-19/spec-b-kernel-levers.md §3).
//
// WHY: RawTimingDiagnostics (include/raw_ffi_api.h:114-139) has no Stage-3 and
// no Stage-4 field, and gpu_submit_wait_ms is a host-side subtraction. The
// ~9.5 ms GPU window therefore cannot be split between raw_bayer_demosaic
// (Stage 3) and dng_render_stage4 (Stage 4) by anything that exists today, so
// no kernel lever can be judged. This TU supplies that split.
//
// MECHANISM: Halide declares halide_metal_command_buffer_completion_handler
// WEAK in its Metal runtime (tests/tmp/p2-halide-runtime-syms.txt:281). A
// strong definition compiled INTO libdng_decoder_native.dylib wins over the
// archive's weak one — exactly the mechanism dng_metal_context.cpp already
// uses for halide_metal_acquire_context. The handler fires on completion of
// every command buffer the Metal runtime commits, which is where
// MTLCommandBuffer's GPUStartTime / GPUEndTime live. Those are DEVICE-side
// timestamps (true execution), as opposed to the host-side encode+commit
// window a host timer measures (halide_metal_run commits and never waits).
//
// ABI: log-only, per lead ruling 1 (spec §0.3). No FFI struct grows; the
// RawTimingDiagnostics ABI is frozen. Output goes to stderr as key=value
// lines and is consumed by native/tests/tmp analysis, never by Dart.
//
// GATE: compiled in unconditionally, inert unless CEYX_GPU_TIMING=1 is set in
// the environment. A build-time gate would mean the measured binary is not the
// shipped binary; a runtime gate keeps them byte-identical. Inert costs one
// relaxed atomic load per completed command buffer and nothing else.
//
// LANE / STAGE ATTRIBUTION (read this before changing it):
//   * The handler runs on a Metal-owned thread, NOT the decode thread, so the
//     decode lane cannot come from thread-local state here. It comes from the
//     command buffer's own MTLCommandQueue: dng_metal_context.cpp binds each
//     decode thread STICKILY to one queue for that thread's whole life, so
//     queue identity IS lane identity. A first-seen registry maps queue
//     pointer -> small dense lane index.
//   * A MTLCommandQueue is serial: command buffers committed on it complete in
//     commit order. Submission index is therefore a per-lane monotonic counter
//     incremented in completion order and equals commit order. Stages are
//     labelled by that index, never guessed from duration (spec §3.3).
//   * All shared state is behind one mutex held for bookkeeping only; nothing
//     Metal-blocking happens under it.
//
// The mutex and the registry are INTENTIONALLY LEAKED (never destroyed), for
// the same reason dng_metal_context.cpp leaks its pool mutex: Halide's Metal
// teardown runs from atexit/static destructors, i.e. after function-local
// statics in this TU would already be gone, and locking a destroyed std::mutex
// aborts.

#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <objc/message.h>
#include <objc/runtime.h>

#include "HalideRuntime.h"
#include "HalideRuntimeMetal.h"

namespace {

// objc_msgSend is declared with a fixed prototype; every send below goes
// through an explicitly cast function pointer so the compiler emits the right
// calling convention for the actual return type. Same rule and same reason as
// dng_metal_context.cpp:103-106.
using MsgSendObject = void *(*)(void *, SEL);
using MsgSendConstCharPointer = const char *(*)(void *, SEL);

// CFTimeInterval (double) returns. On arm64 objc_msgSend returns floating
// point in d0 and objc_msgSend_fpret does not exist; on x86_64 a double return
// must go through objc_msgSend_fpret or the value read back is garbage.
#if defined(__x86_64__)
extern "C" double objc_msgSend_fpret(void *, SEL, ...);
#define CEYX_MSGSEND_DOUBLE objc_msgSend_fpret
#else
#define CEYX_MSGSEND_DOUBLE objc_msgSend
#endif
using MsgSendDouble = double (*)(void *, SEL);

bool object_responds_to_selector(void *object, SEL selector) {
  if (!object) return false;
  Class object_class = object_getClass(reinterpret_cast<id>(object));
  return class_getInstanceMethod(object_class, selector) != nullptr;
}

// -1 unread, 0 off, 1 on. Read once; the gate is a process-lifetime property.
std::atomic<int> g_gate{-1};

bool timing_enabled() {
  int cached = g_gate.load(std::memory_order_relaxed);
  if (cached >= 0) return cached != 0;
  const char *value = std::getenv("CEYX_GPU_TIMING");
  const int decided =
      (value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0)
          ? 1
          : 0;
  g_gate.store(decided, std::memory_order_relaxed);
  return decided != 0;
}

std::mutex &probe_lock() {
  static std::mutex *m = new std::mutex();
  return *m;
}

struct LaneState {
  void *queue = nullptr;
  long long submissions = 0;
};

std::vector<LaneState> &lanes() {
  static std::vector<LaneState> *v = new std::vector<LaneState>();
  return *v;
}

// Caller must hold probe_lock(). Returns (lane_index, submission_index).
void assign(void *queue, int *lane_out, long long *submission_out) {
  std::vector<LaneState> &table = lanes();
  for (size_t i = 0; i < table.size(); ++i) {
    if (table[i].queue == queue) {
      *lane_out = static_cast<int>(i);
      *submission_out = table[i].submissions++;
      return;
    }
  }
  LaneState fresh;
  fresh.queue = queue;
  fresh.submissions = 1;
  table.push_back(fresh);
  *lane_out = static_cast<int>(table.size()) - 1;
  *submission_out = 0;
}

}  // namespace

extern "C" {

// Reset the per-lane submission counters. Exported so a measurement harness
// can bracket a decode; a no-op when the gate is off. Never called from the
// decode path itself, so it adds nothing to the measured window.
void ceyx_gpu_timing_reset() {
  if (!timing_enabled()) return;
  std::lock_guard<std::mutex> guard(probe_lock());
  lanes().clear();
}

// Content marker, for proving this TU is the one linked in.
const char *ceyx_gpu_timing_marker() { return "ceyx_gpu_timing_probe_v1"; }

int halide_metal_command_buffer_completion_handler(
    void *user_context, struct halide_metal_command_buffer *buffer,
    char **returned_error_string) {
  (void)user_context;

  void *command_buffer = reinterpret_cast<void *>(buffer);

  // --- Error propagation: replicate the stock weak handler's contract -------
  // The default implementation reports a failed command buffer back through
  // *returned_error_string and a device_run_failed code; dropping that would
  // turn every GPU fault into a silent wrong image.
  int result = halide_error_code_success;
  if (command_buffer != nullptr) {
    SEL error_selector = sel_registerName("error");
    if (object_responds_to_selector(command_buffer, error_selector)) {
      void *error = reinterpret_cast<MsgSendObject>(objc_msgSend)(
          command_buffer, error_selector);
      if (error != nullptr) {
        result = halide_error_code_device_run_failed;
        if (returned_error_string != nullptr) {
          SEL description = sel_registerName("localizedDescription");
          const char *text = nullptr;
          if (object_responds_to_selector(error, description)) {
            void *string = reinterpret_cast<MsgSendObject>(objc_msgSend)(
                error, description);
            SEL utf8 = sel_registerName("UTF8String");
            if (string != nullptr && object_responds_to_selector(string, utf8)) {
              text = reinterpret_cast<MsgSendConstCharPointer>(objc_msgSend)(
                  string, utf8);
            }
          }
          *returned_error_string = strdup(text != nullptr ? text
                                                          : "Metal command "
                                                            "buffer failed");
        }
      }
    }
  }

  // --- Gate --------------------------------------------------------------
  if (!timing_enabled() || command_buffer == nullptr) return result;

  SEL gpu_start = sel_registerName("GPUStartTime");
  SEL gpu_end = sel_registerName("GPUEndTime");
  if (!object_responds_to_selector(command_buffer, gpu_start) ||
      !object_responds_to_selector(command_buffer, gpu_end)) {
    return result;
  }
  const double start_s = reinterpret_cast<MsgSendDouble>(CEYX_MSGSEND_DOUBLE)(
      command_buffer, gpu_start);
  const double end_s = reinterpret_cast<MsgSendDouble>(CEYX_MSGSEND_DOUBLE)(
      command_buffer, gpu_end);

  void *queue = nullptr;
  SEL queue_selector = sel_registerName("commandQueue");
  if (object_responds_to_selector(command_buffer, queue_selector)) {
    queue = reinterpret_cast<MsgSendObject>(objc_msgSend)(command_buffer,
                                                          queue_selector);
  }

  int lane = 0;
  long long submission = 0;
  {
    std::lock_guard<std::mutex> guard(probe_lock());
    assign(queue, &lane, &submission);
  }

  // Observed at lane width 8 on an M3 Ultra (native/tests/tmp/p0b-w8.txt, first
  // baseline attempt): a command buffer can complete with GPUStartTime == 0.
  // Metal leaves both timestamps at 0 for a buffer that never actually executed
  // on the GPU. Subtracting then yields busy_ms ≈ 1.9e9 — a number that is
  // obviously absurd in isolation but silently destroys any mean it is averaged
  // into. The record is still EMITTED (dropping it would hide how often this
  // happens) but is explicitly marked invalid, so an analysis pass excludes it
  // by reading a flag rather than by guessing a plausibility threshold.
  const int valid = (start_s > 0.0 && end_s >= start_s) ? 1 : 0;

  // Fixed key order, one line per submission, stderr (spec §3.4). start/end are
  // absolute seconds on the GPU timebase so an analysis pass can compute the
  // UNION of overlapping intervals rather than their sum — overlapping buffers
  // must not be double-counted.
  std::fprintf(stderr,
               "[CEYX_GPU_TIMING] lane=%d submission=%lld valid=%d busy_ms=%.6f "
               "gpu_start_s=%.9f gpu_end_s=%.9f queue=%p\n",
               lane, submission, valid,
               valid ? (end_s - start_s) * 1000.0 : -1.0, start_s, end_s,
               queue);
  return result;
}

}  // extern "C"

#else  // not (__APPLE__ && !DNG_FORCE_VULKAN)

extern "C" const char *ceyx_gpu_timing_marker() {
  return "ceyx_gpu_timing_probe_v1_disabled";
}
extern "C" void ceyx_gpu_timing_reset() {}

#endif
