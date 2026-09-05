// dng_metal_context.cpp — strong override of the weak Halide v21 Metal context
// hooks. See dng_metal_context.h for the why. R1-T2, 2026-09-05.
//
// MECHANISM: Halide declares halide_metal_acquire_context /
// halide_metal_release_context WEAK. A strong definition compiled INTO
// libdng_decoder_native.dylib wins over the archive's weak one (verified in
// R1-T3 with `nm -m`: the symbols flip from "weak external" to "external"). An
// executable-side definition would NOT win, because macOS two-level namespace
// binds the dylib to its own copy.
//
// DESIGN (queue assignment is STICKY per thread — read this before changing it)
//   * One shared MTLDevice, created once, retained for process lifetime.
//   * Up to `cap` MTLCommandQueues, created lazily, retained for process
//     lifetime. R4 item 1: the cap now FOLLOWS the configured decode slot count
//     (the user's lane-width setting) instead of a hardcoded 4, and is re-read
//     per call rather than cached, so a mid-session width change reaches the
//     queue pool. DNG_METAL_QUEUE_CAP still overrides, clamped to
//     [1, PipelineConfig::kAbsoluteMaxDecodeSlots].
//   * A thread's queue is chosen on its FIRST acquire and never changes
//     afterwards. This is the correctness crux: Halide takes and releases the
//     context once PER ENTRY POINT (R1-T3 logs: 46 acquires / 46 releases for
//     one DNG decode, depth returns to 0 between entries), so a queue that is
//     returned to a free pool at depth 0 could hand the same decode a DIFFERENT
//     queue for `run` and for `copy_to_host`. halide_metal_run only COMMITS
//     (metal_v21.cpp:861) — the wait happens later inside copy_to_host — and
//     Metal orders command buffers only WITHIN one queue. A per-entry queue
//     would therefore let the copy-back start before the kernel finished:
//     precisely the nondeterminism campaign AC5 exists to catch. Sticky binding
//     makes "all entries of one decode use one queue" structural.
//   * When more decode threads exist than `cap`, extra threads SHARE an
//     existing queue round-robin (stable for that thread's life). Sharing a
//     serial queue is always CORRECT — it is exactly what the stock runtime
//     does for every thread; it only costs overlap. This is a deliberate
//     deviation from the plan's "block on a condition variable when all queues
//     are busy": with release-at-depth-0 impossible (see above), a binding is
//     only ever released at thread exit, so a blocking design would deadlock
//     the 5th lane behind four permanently-bound queues. Degrading to sharing
//     is never less safe than stock behaviour. (Pre-R4 this sentence ended
//     "and keeps cap at 4 per ruling 4"; the cap is now the configured lane
//     width, but the sharing argument is unchanged — it depends only on there
//     being MORE threads than queues, not on any particular cap value.)
//   * NO LOCK IS HELD ACROSS ANY GPU WAIT. The pool mutex covers only map
//     lookup, queue creation and bookkeeping, and is unlocked before this
//     function returns — the caller does all Metal work (encode / commit /
//     waitUntilCompleted) outside it. That is the entire fix.
//   * create == false with no binding for the key takes stock "no context"
//     semantics: success, existing device (may be null), NULL queue. This path
//     is guaranteed to execute in production — the process-teardown
//     halide_metal_device_release arrives on the main thread after every decode
//     thread has joined (R1-T3 mandatory carry-forward). It must never crash
//     and must never be handed a queue belonging to another key.

#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)

#include "dng_metal_context.h"
#include "dng_pipeline_config.h"
// R4 item 1: dng_decode_slot_count_relaxed() — the queue cap follows the
// configured decode slot count. See queue_cap() for why the RELAXED accessor
// is mandatory here rather than the locking one.
#include "dng_pipeline.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <pthread.h>

#include <objc/message.h>
#include <objc/runtime.h>

struct halide_metal_device;
struct halide_metal_command_queue;

extern "C" void *MTLCreateSystemDefaultDevice(void);

namespace {

using MsgSend = void *(*)(void *, SEL);

// INTENTIONALLY LEAKED (never destroyed). Halide's Metal teardown runs from
// static destructors / atexit (halide_metal_device_release), i.e. AFTER
// function-local statics in this TU would have been destroyed — locking a
// destroyed std::mutex aborts with "mutex lock failed: Invalid argument"
// (observed by the R1-T3 probe). The stock runtime has the same property (its
// thread_lock is a POD spinlock with no destructor), so leaking is parity.
std::mutex &pool_lock() {
  static std::mutex *m = new std::mutex();
  return *m;
}

struct Binding {
  void *queue = nullptr;
  int depth = 0;
};

std::unordered_map<uintptr_t, Binding> &bindings() {
  static auto *m = new std::unordered_map<uintptr_t, Binding>();
  return *m;
}

std::vector<void *> &queues() {
  static auto *v = new std::vector<void *>();
  return *v;
}

void *g_device = nullptr;      // pool_lock()
uint64_t g_round_robin = 0;    // pool_lock()

int queue_cap() {
  // R4 item 1: the queue count FOLLOWS the configured decode slot count.
  //
  // This used to cache a literal 4 in a `static const`. Two things were wrong
  // with that once the slot count became configurable: the literal kept the
  // old clamp alive in a second place (an 8-slot pool round-robining over 4
  // queues, i.e. the parallelism the user asked for silently halved), and the
  // CACHE froze whatever value was current at first use, so a mid-session
  // lane-width change could never reach the queue pool — which ruling r-5
  // requires it to. The value is therefore re-read on every call.
  //
  // It MUST be read through the relaxed (atomic) accessor rather than
  // dng_decode_slot_count(): this function is called from inside pool_lock(),
  // and the locking accessor would both nest the slot-pool mutex underneath
  // the queue-pool mutex and construct the entire slot pool as a side effect
  // of what is only a bookkeeping question.
  //
  // The env override is retained for diagnostics, with its upper bound raised
  // from a bare 8 to the allocation-sanity constant so it can still express
  // any value the slot pool can actually be configured to.
  const char *env = std::getenv("DNG_METAL_QUEUE_CAP");
  if (env && env[0]) {
    const long kEnvMax =
        static_cast<long>(PipelineConfig::kAbsoluteMaxDecodeSlots);
    const long parsed = std::strtol(env, nullptr, 10);
    if (parsed < 1) return 1;
    if (parsed > kEnvMax) return static_cast<int>(kEnvMax);
    return static_cast<int>(parsed);
  }
  const size_t slots = dng_decode_slot_count_relaxed();
  return slots < 1 ? 1 : static_cast<int>(slots);
}

bool logging_enabled() {
  static const bool on = []() {
    const char *env = std::getenv("DNG_METAL_QUEUE_LOG");
    return env && env[0];
  }();
  return on;
}

// Called under pool_lock(). Creates the device on first use; returns false if
// the system has no Metal device (caller then returns the stock error code and
// never reports success with a null queue).
bool ensure_device_locked() {
  if (!g_device) {
    g_device = MTLCreateSystemDefaultDevice();
    if (g_device) {
      // MTLCreateSystemDefaultDevice returns +1; retain again so an autorelease
      // pool draining on a decode thread can never take the process's device.
      reinterpret_cast<MsgSend>(objc_msgSend)(g_device, sel_registerName("retain"));
    }
  }
  return g_device != nullptr;
}

// Called under pool_lock(). Picks this thread's queue for the rest of its life:
// a fresh queue while below cap, otherwise an existing one round-robin.
void *assign_queue_locked() {
  auto &qs = queues();
  if (static_cast<int>(qs.size()) < queue_cap()) {
    void *q = reinterpret_cast<MsgSend>(objc_msgSend)(
        g_device, sel_registerName("newCommandQueue"));
    if (q) {
      qs.push_back(q);  // +1 from new*, retained for process lifetime
      return q;
    }
    // Queue creation failed; fall through and share an existing one if any.
    if (qs.empty()) return nullptr;
  }
  if (qs.empty()) return nullptr;
  return qs[static_cast<size_t>(g_round_robin++ % qs.size())];
}

}  // namespace

namespace ceyx {

int dng_metal_queue_count() {
  std::lock_guard<std::mutex> g(pool_lock());
  return static_cast<int>(queues().size());
}

int dng_metal_queue_cap() {
  return queue_cap();
}

const char *dng_metal_context_marker() {
  return "ceyx_metal_queue_pool_v1";
}

}  // namespace ceyx

extern "C" const char *ceyx_metal_queue_pool_v1(void) {
  return "ceyx_metal_queue_pool_v1";
}

extern "C" int halide_metal_acquire_context(void *user_context,
                                            struct halide_metal_device **device_ret,
                                            struct halide_metal_command_queue **queue_ret,
                                            bool create) {
  (void)user_context;  // 0x0 on every entry today (R1-T3); the key is the thread.
  const uintptr_t key = reinterpret_cast<uintptr_t>(pthread_self());

  void *device = nullptr;
  void *queue = nullptr;
  bool created_queue = false;
  int live_queues = 0;

  {
    std::lock_guard<std::mutex> g(pool_lock());

    auto &map = bindings();
    auto it = map.find(key);
    if (it != map.end()) {
      ++it->second.depth;
      device = g_device;
      queue = it->second.queue;
    } else if (!create) {
      // Stock no-context semantics: success, whatever device exists, no queue.
      device = g_device;
      queue = nullptr;
    } else {
      if (!ensure_device_locked()) {
        return -1;  // halide_error_code_generic_error
      }
      const size_t before = queues().size();
      queue = assign_queue_locked();
      if (!queue) {
        return -1;
      }
      created_queue = queues().size() > before;
      map[key] = Binding{queue, 1};
      device = g_device;
    }
    live_queues = static_cast<int>(queues().size());
  }
  // Pool lock released HERE. Everything the caller does with this queue —
  // encode, commit, waitUntilCompleted — happens with no lock of ours held.

  if (created_queue && logging_enabled()) {
    std::fprintf(stderr,
                 "queuepool|ev=bind|thread=0x%llx|queue=%p|count=%d|cap=%d\n",
                 static_cast<unsigned long long>(key), queue, live_queues,
                 queue_cap());
    std::fflush(stderr);
  }

  if (device_ret) *device_ret = reinterpret_cast<struct halide_metal_device *>(device);
  if (queue_ret) *queue_ret = reinterpret_cast<struct halide_metal_command_queue *>(queue);
  return 0;
}

extern "C" int halide_metal_release_context(void *user_context) {
  (void)user_context;
  const uintptr_t key = reinterpret_cast<uintptr_t>(pthread_self());
  std::lock_guard<std::mutex> g(pool_lock());
  auto &map = bindings();
  auto it = map.find(key);
  if (it != map.end() && it->second.depth > 0) {
    --it->second.depth;
  }
  // The binding itself is deliberately NOT dropped at depth 0: see the STICKY
  // note in the header comment. Queues are retained for process lifetime, so
  // there is nothing to free here.
  return 0;
}

#endif  // __APPLE__ && !DNG_FORCE_VULKAN
