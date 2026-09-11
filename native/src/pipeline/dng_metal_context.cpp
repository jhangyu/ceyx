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
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <pthread.h>

#include <TargetConditionals.h>
#include <objc/message.h>
#include <objc/runtime.h>

// R4 item 3: cold-start gate for Halide's unsynchronised
// metal_api_supports_set_bytes / metal_api_checked_device memo cache.
// (Block authored by impl-copylock-opus; applied by lead-r1-opus.)
#include "dng_metal_api_gate.h"

struct halide_metal_device;
struct halide_metal_command_queue;

extern "C" void *MTLCreateSystemDefaultDevice(void);

// C2 §4.1.1: macOS-only C entry point. Weak-imported so this TU also links (and
// takes the single-device path) on platforms that do not provide it, e.g. the
// iOS-style SDKs where MTLCreateSystemDefaultDevice is the only device source.
#if TARGET_OS_OSX
extern "C" void *MTLCopyAllDevices(void) __attribute__((weak_import));
#endif

namespace {

using MsgSend = void *(*)(void *, SEL);

// C2 §4.1.1 ABI TRAP: `hasUnifiedMemory` returns objc BOOL (signed char on
// arm64), NOT a pointer. Invoking it through the `MsgSend` alias above would
// read a full pointer-width register whose upper bits are undefined on some
// ABIs, i.e. a garbage verdict that happens to look plausible. Every
// BOOL-returning selector in this TU goes through this cast instead.
using MsgSendBool = bool (*)(void *, SEL);
using MsgSendUnsignedLong = unsigned long (*)(void *, SEL);
using MsgSendObjectAtIndex = void *(*)(void *, SEL, unsigned long);
using MsgSendConstCharPointer = const char *(*)(void *, SEL);

// True when `object`'s class actually implements `selector`. hasUnifiedMemory
// is macOS 10.15+ / iOS 13+; sending it blind to an older device object would
// raise an unrecognized-selector exception rather than answer "not unified".
bool object_responds_to_selector(void *object, SEL selector) {
  if (!object) return false;
  // objc/runtime.h types `id` as `struct objc_object *`, which a `void *` does
  // not implicitly convert to in C++ — hence the explicit cast, the same reason
  // the objc_msgSend calls in this TU go through reinterpret_cast'd function
  // pointer types rather than the declared prototype.
  Class object_class = object_getClass(reinterpret_cast<id>(object));
  return class_getInstanceMethod(object_class, selector) != nullptr;
}

bool device_reports_unified_memory(void *device) {
  SEL has_unified_memory = sel_registerName("hasUnifiedMemory");
  if (!object_responds_to_selector(device, has_unified_memory)) return false;
  return reinterpret_cast<MsgSendBool>(objc_msgSend)(device, has_unified_memory);
}

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

// ---------------------------------------------------------------------------
// C2 capability-gate state (plan §4.1.2). Single source of truth, all fields
// guarded by pool_lock(), all written exactly once by run_capability_probe_locked()
// except the override/mode pair, which set_zero_copy_capability_override_for_testing()
// may re-resolve (mode only — never the device selection, never a second log line).
// ---------------------------------------------------------------------------
bool g_capability_probe_has_run = false;              // pool_lock()
bool g_device_has_unified_memory = false;             // pool_lock()
bool g_zero_copy_path_enabled = false;                // pool_lock()
int g_devices_enumerated = 0;                         // pool_lock()
ceyx::ZeroCopyCapabilityOverrideState g_capability_override_state =
    ceyx::ZeroCopyCapabilityOverrideState::kNone;     // pool_lock()
// Set by the in-process testing hook; makes it win over the environment
// variable, which is otherwise read once at probe time.
bool g_capability_override_set_explicitly = false;    // pool_lock()

const char *override_state_name(ceyx::ZeroCopyCapabilityOverrideState state) {
  switch (state) {
    case ceyx::ZeroCopyCapabilityOverrideState::kForcedOff: return "forced_off";
    case ceyx::ZeroCopyCapabilityOverrideState::kForcedOn: return "forced_on";
    case ceyx::ZeroCopyCapabilityOverrideState::kNone: break;
  }
  return "none";
}

// CEYX_ZERO_COPY_CAPABILITY_OVERRIDE, plan §4.1.4. Read ONCE, at probe time —
// never per decode, because a mode that can change between two kernels of one
// decode is the same hazard the sticky-queue design above exists to prevent.
ceyx::ZeroCopyCapabilityOverrideState read_capability_override_from_environment() {
  const char *env = std::getenv("CEYX_ZERO_COPY_CAPABILITY_OVERRIDE");
  if (!env || !env[0]) return ceyx::ZeroCopyCapabilityOverrideState::kNone;
  if (std::strcmp(env, "forced_off") == 0) {
    return ceyx::ZeroCopyCapabilityOverrideState::kForcedOff;
  }
  if (std::strcmp(env, "forced_on") == 0) {
    return ceyx::ZeroCopyCapabilityOverrideState::kForcedOn;
  }
  // Any other value: use the probe (documented in §4.1.4 as "unset/any other").
  return ceyx::ZeroCopyCapabilityOverrideState::kNone;
}

// Capability AND override resolved into the one effective mode. Called under
// pool_lock().
bool resolve_zero_copy_mode_locked() {
  switch (g_capability_override_state) {
    case ceyx::ZeroCopyCapabilityOverrideState::kForcedOff: return false;
    case ceyx::ZeroCopyCapabilityOverrideState::kForcedOn: return true;
    case ceyx::ZeroCopyCapabilityOverrideState::kNone: break;
  }
  return g_device_has_unified_memory;
}

// Called under pool_lock() with g_device already selected and retained.
// Evaluates the capability once and emits the single pre-registered log line.
void run_capability_probe_locked() {
  if (g_capability_probe_has_run) return;
  g_capability_probe_has_run = true;

  g_device_has_unified_memory = device_reports_unified_memory(g_device);
  if (!g_capability_override_set_explicitly) {
    g_capability_override_state = read_capability_override_from_environment();
  }
  g_zero_copy_path_enabled = resolve_zero_copy_mode_locked();

  const char *device_name = "unknown";
  SEL name_selector = sel_registerName("name");
  if (object_responds_to_selector(g_device, name_selector)) {
    void *name_object =
        reinterpret_cast<MsgSend>(objc_msgSend)(g_device, name_selector);
    SEL utf8_selector = sel_registerName("UTF8String");
    if (object_responds_to_selector(name_object, utf8_selector)) {
      const char *utf8 = reinterpret_cast<MsgSendConstCharPointer>(objc_msgSend)(
          name_object, utf8_selector);
      if (utf8 && utf8[0]) device_name = utf8;
    }
  }

  // Plan §4.1.3: unconditional (NOT behind DNG_METAL_QUEUE_LOG) — the
  // acceptance gates grep for this line, and AC6's "gate forced off" evidence
  // depends on reading the mode without setting an env var.
  std::fprintf(stderr,
               "zerocopy|ev=capability|device=%s|unified_memory=%d|"
               "devices_enumerated=%d|mode=%s|override=%s\n",
               device_name, g_device_has_unified_memory ? 1 : 0,
               g_devices_enumerated,
               g_zero_copy_path_enabled ? "unified" : "fallback",
               override_state_name(g_capability_override_state));
  std::fflush(stderr);
}

// C2 §4.1.1: prefer the integrated/unified device over the system default,
// which on a dual-GPU Mac is typically the discrete one. Returns a device with
// TWO retains owed to us (parity with the MTLCreateSystemDefaultDevice path
// below, which is +1 from the constructor plus one explicit retain), or nullptr
// when no unified device was found or enumeration is unavailable.
// Called under pool_lock(); sets g_devices_enumerated.
void *select_unified_device_locked() {
#if TARGET_OS_OSX
  if (MTLCopyAllDevices == nullptr) return nullptr;  // weak-import absent
  void *devices = MTLCopyAllDevices();               // +1 NSArray
  if (!devices) return nullptr;

  void *selected = nullptr;
  const unsigned long count = reinterpret_cast<MsgSendUnsignedLong>(objc_msgSend)(
      devices, sel_registerName("count"));
  g_devices_enumerated = static_cast<int>(count);
  SEL object_at_index = sel_registerName("objectAtIndex:");
  for (unsigned long i = 0; i < count; ++i) {
    void *device = reinterpret_cast<MsgSendObjectAtIndex>(objc_msgSend)(
        devices, object_at_index, i);
    if (device_reports_unified_memory(device)) {
      selected = device;
      break;
    }
  }
  if (selected) {
    // The array owns the element; once we release the array the element could
    // go away. Retain twice, matching the deliberate over-retain at the
    // creation site below: an autorelease pool draining on a decode thread must
    // never be able to take the process's device.
    SEL retain = sel_registerName("retain");
    reinterpret_cast<MsgSend>(objc_msgSend)(selected, retain);
    reinterpret_cast<MsgSend>(objc_msgSend)(selected, retain);
  }
  reinterpret_cast<MsgSend>(objc_msgSend)(devices, sel_registerName("release"));
  return selected;
#else
  // iOS-style single-device platforms: no enumeration API, probe the default
  // device directly (plan §4.1.1). g_devices_enumerated is set by the caller.
  return nullptr;
#endif
}

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
    // C2 §4.1.1: prefer an integrated/unified device over the system default.
    g_device = select_unified_device_locked();  // already retained twice
    if (!g_device) {
      g_device = MTLCreateSystemDefaultDevice();
      if (g_device) {
        // MTLCreateSystemDefaultDevice returns +1; retain again so an autorelease
        // pool draining on a decode thread can never take the process's device.
        reinterpret_cast<MsgSend>(objc_msgSend)(g_device, sel_registerName("retain"));
        if (g_devices_enumerated < 1) g_devices_enumerated = 1;
      }
    }
  }
  // C2 §2.3: the capability gate is decided HERE and nowhere else — this is
  // already the one place the process device is created, so the probe inherits
  // that once-ness for free.
  if (g_device) run_capability_probe_locked();
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

void *metal_shared_device_handle() {
  // Pointer read only; the lock is released on return, long before the caller
  // touches Metal (plan §8.2 hazard 4). Deliberately does NOT call
  // ensure_device_locked(): creating the process device as a side effect of an
  // inspection would move device creation off the decode path that owns it, so
  // a caller that runs before any decode correctly sees nullptr.
  std::lock_guard<std::mutex> g(pool_lock());
  return g_device;
}

void *metal_shared_device_handle_ensure_created() {
  // The decode-path variant: it DOES create the device, because its only caller
  // is C1's arena first touch, which runs inside a decode that is about to
  // dispatch a kernel and would have created the device a moment later anyway
  // (halide_metal_acquire_context above takes this same ensure_device_locked()
  // path). See dng_metal_context.h for why this is not the inspection side
  // effect the plain accessor refuses to have.
  //
  // Creation happens under pool_lock() exactly as the acquire path does, so two
  // lanes racing here cannot produce two devices; the lock is released on return
  // and no GPU work happens under it (plan §8.2 hazard 4).
  std::lock_guard<std::mutex> g(pool_lock());
  if (!ensure_device_locked()) return nullptr;
  return g_device;
}

bool zero_copy_path_is_enabled() {
  std::lock_guard<std::mutex> g(pool_lock());
  // Creating the device here is the documented, intended side effect: every
  // caller is on the GPU decode path (see the header). Answering "false,
  // not probed yet" on a lane's first decode is precisely the R2 AC1 defect
  // shape — a first decode silently taking a different path than every
  // subsequent one.
  if (!g_capability_probe_has_run) {
    if (!ensure_device_locked()) {
      // No Metal device at all: the fallback path is the only correct answer,
      // and nothing has been probed, so a later call still retries.
      return false;
    }
  }
  return g_zero_copy_path_enabled;
}

bool metal_device_has_unified_memory() {
  // Observational: never creates the device (same rule as
  // metal_shared_device_handle()); answers false before any decode.
  std::lock_guard<std::mutex> g(pool_lock());
  return g_device_has_unified_memory;
}

void set_zero_copy_capability_override_for_testing(
    ZeroCopyCapabilityOverrideState override_state) {
  std::lock_guard<std::mutex> g(pool_lock());
  g_capability_override_set_explicitly = true;
  g_capability_override_state = override_state;
  // Re-resolve the MODE ONLY. Device selection is untouched, and no second
  // capability log line is emitted (plan §4.1.3: exactly one).
  g_zero_copy_path_enabled = resolve_zero_copy_mode_locked();
}

ZeroCopyCapabilityStateSnapshot zero_copy_capability_state_snapshot() {
  std::lock_guard<std::mutex> g(pool_lock());
  ZeroCopyCapabilityStateSnapshot snapshot;
  snapshot.capability_probe_has_run = g_capability_probe_has_run;
  snapshot.device_has_unified_memory = g_device_has_unified_memory;
  snapshot.zero_copy_path_is_enabled = g_zero_copy_path_enabled;
  snapshot.devices_enumerated = g_devices_enumerated;
  snapshot.capability_override_state = g_capability_override_state;
  return snapshot;
}

}  // namespace ceyx

extern "C" const char *ceyx_metal_queue_pool_v1(void) {
  return "ceyx_metal_queue_pool_v1";
}

extern "C" const char *ceyx_zero_copy_capability_marker(void) {
  return "ceyx_zero_copy_capability_v1";
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

  // R4 item 3, B1. Placed AFTER the pool mutex is released, so the gate mutex
  // and the pool mutex are never held at once and cannot invert; AFTER the
  // early `return -1` paths, so a failed acquire never takes the gate (a
  // release with no matching enter finds thread depth 0 and does nothing); and
  // on the success path only — exactly the calls that can go on to execute
  // Halide's unsynchronised memo block. The gate is depth-counted per thread
  // and touches its mutex only at depth 0, because both context functions are
  // re-entered by the same thread at depth; without that it would self-deadlock
  // on the nested acquire.
  ceyx::metal_api_gate_enter();
  return 0;
}

extern "C" int halide_metal_release_context(void *user_context) {
  (void)user_context;
  // R4 item 3, B2. Before the pool lock, for the same non-inversion reason.
  ceyx::metal_api_gate_exit();
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

#else  // !(__APPLE__ && !DNG_FORCE_VULKAN)

// ---------------------------------------------------------------------------
// Non-Metal builds (non-Apple, or DNG_FORCE_VULKAN). Plan §4.6: the C2
// capability gate must still have a definition here, returning "fallback"
// unconditionally, so that raw_gpu_pipeline.cpp contains NO `#if` — the branch
// is a runtime bool on every platform and AC4's "both paths identical output"
// stays a property of one source text. Vulkan has an equivalent capability
// (halide_vulkan_wrap_* / VK_MEMORY_PROPERTY_HOST_VISIBLE); adopting it is
// explicitly a later phase, not this slice.
//
// Everything else in this TU (the queue pool, the device accessors) remains
// Metal-only, exactly as before.
// ---------------------------------------------------------------------------

#include "dng_metal_context.h"

namespace ceyx {

bool zero_copy_path_is_enabled() { return false; }

bool metal_device_has_unified_memory() { return false; }

void set_zero_copy_capability_override_for_testing(
    ZeroCopyCapabilityOverrideState override_state) {
  // No unified path exists to force on or off here; accepted and ignored so a
  // shared test binary compiles and links on every platform.
  (void)override_state;
}

ZeroCopyCapabilityStateSnapshot zero_copy_capability_state_snapshot() {
  return ZeroCopyCapabilityStateSnapshot{};
}

}  // namespace ceyx

extern "C" const char *ceyx_zero_copy_capability_marker(void) {
  return "ceyx_zero_copy_capability_v1";
}

#endif  // __APPLE__ && !DNG_FORCE_VULKAN

// ---------------------------------------------------------------------------
// C2 counters (plan §4.5). OUTSIDE the platform guard on purpose: the increment
// sites in raw_gpu_pipeline.cpp / dng_render_halide.cpp and the reader in
// raw_ffi_api.cpp are all platform-agnostic source text, so the symbols must
// exist on every build or those files would need an #if each. On non-Metal
// builds nothing ever increments them and they read zero, which is the honest
// answer there.
// ---------------------------------------------------------------------------

#include "dng_metal_context.h"

#include <atomic>
#include <cstdint>

namespace {

// Relaxed: diagnostic tallies only, never control flow, so they need no
// ordering with respect to the decode work they count. Function-local statics
// with constant initialisation — no init-order hazard, unlike the pool objects
// above, because these are trivially constructible.
std::atomic<uint64_t> g_zero_copy_destination_wrap_count{0};
std::atomic<uint64_t> g_zero_copy_destination_alignment_degradation_count{0};
std::atomic<uint64_t> g_zero_copy_source_mosaic_wrap_count{0};

}  // namespace

namespace ceyx {

uint64_t zero_copy_destination_wrap_count() {
  return g_zero_copy_destination_wrap_count.load(std::memory_order_relaxed);
}

void zero_copy_note_destination_wrapped() {
  g_zero_copy_destination_wrap_count.fetch_add(1, std::memory_order_relaxed);
}

uint64_t zero_copy_destination_alignment_degradation_count() {
  return g_zero_copy_destination_alignment_degradation_count.load(
      std::memory_order_relaxed);
}

void zero_copy_note_destination_alignment_degraded() {
  g_zero_copy_destination_alignment_degradation_count.fetch_add(
      1, std::memory_order_relaxed);
}

uint64_t zero_copy_source_mosaic_wrap_count() {
  return g_zero_copy_source_mosaic_wrap_count.load(std::memory_order_relaxed);
}

void zero_copy_note_source_mosaic_wrapped() {
  g_zero_copy_source_mosaic_wrap_count.fetch_add(1, std::memory_order_relaxed);
}

void reset_zero_copy_counters_for_testing() {
  g_zero_copy_destination_wrap_count.store(0, std::memory_order_relaxed);
  g_zero_copy_destination_alignment_degradation_count.store(
      0, std::memory_order_relaxed);
  g_zero_copy_source_mosaic_wrap_count.store(0, std::memory_order_relaxed);
}

}  // namespace ceyx
