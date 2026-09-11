// dng_metal_context.h — ceyx strong override of the Halide v21 Metal context
// hooks (R1-T2, parallel-decode campaign 2026-09-05).
//
// WHY: the stock Halide Metal runtime keeps ONE process-wide MTLCommandQueue
// behind a spinlock that is held across the GPU wait inside copy_to_host
// (metal_v21.cpp:284/:818-838). A MTLCommandQueue executes its command buffers
// serially, so one queue for every decode is, by itself, a hard serializer —
// this is the headless 5-way "staircase". This TU replaces the process-wide
// context with ONE shared MTLDevice and a small pool of MTLCommandQueues bound
// to decode threads, and it holds its pool mutex ONLY for map/free-list
// bookkeeping — never across any GPU wait.
//
// KEY = OS THREAD, per the R1-T3 audit verdict (amended, authorized):
// tmp/verify/r1-queue-affinity.txt. `user_context` is 0x0 on every Halide entry
// today, and each decode is one synchronous FFI call that cannot migrate OS
// threads mid-call, so the calling thread is a structurally stable key for
// exactly as long as a queue must stay pinned to one decode.
//
// Apple-only; on every other platform this TU compiles to nothing (the GPU
// backend there is Vulkan, see dng_halide_device.cpp).

#ifndef DNG_METAL_CONTEXT_H
#define DNG_METAL_CONTEXT_H

#include <cstdint>

namespace ceyx {

// Number of MTLCommandQueues created so far (test/inspection marker).
int dng_metal_queue_count();

// Queue-pool ceiling. Default 4, matching the DecodeSlotPool ceiling the
// campaign is not allowed to change; env DNG_METAL_QUEUE_CAP clamps to [1,8].
int dng_metal_queue_cap();

// Content marker; returns "ceyx_metal_queue_pool_v1".
const char *dng_metal_context_marker();

// The process-wide MTLDevice this pool owns, as an opaque handle, or nullptr
// when no Metal device has been created yet (or the system has none).
//
// WHY IT EXISTS: C3 of the GPU copy-elimination campaign
// (docs/logs/2026-09-11/plan-gpu-copy-elimination.md §5.5) allocates its own
// small Stage4 parameter MTLBuffers and must allocate them from the SAME device
// the decode queues were created from. The device is already held here, so C3's
// entire prerequisite is this one accessor; C2 later adds its capability probe
// and mode accessors beside it.
//
// The pool mutex is taken for the pointer read ONLY and released before the
// caller does any Metal work, per this TU's no-lock-across-a-GPU-wait rule. The
// device is retained for process lifetime, so the handle stays valid and the
// caller must NOT release it. This function never CREATES the device: it
// reports what a decode has already created, and answers nullptr before then.
//
// Apple-only definition; declared unconditionally so every platform can compile
// a call site behind its own guard.
void *metal_shared_device_handle();

// Same handle, but CREATES the process device when it does not exist yet.
// Returns nullptr only when the system genuinely has no Metal device.
//
// WHY IT EXISTS (R2 AC1 root cause): the device is created lazily inside the
// first kernel dispatch of a decode, i.e. LATER in the decode than C1's arena
// first touch. The arena therefore saw a null device on a lane's very first
// decode, answered "no arena", and that decode ran unarened — so the warmup
// decode allocated nothing and the NEXT, smaller frame sized the regions, which
// made the largest frame later grow all three. Observed as allocation=3 with
// growth=3, where growth must be 0 after warmup.
//
// WHY THIS IS NOT THE SIDE EFFECT metal_shared_device_handle() REFUSES TO HAVE:
// that accessor answers observational questions (probes, diagnostics) which must
// not move device creation onto a non-decode path, and its contract is
// unchanged. This variant is called ONLY from the GPU decode path, where device
// creation is imminent and intended inside the same call — it moves the creation
// slightly earlier within a decode that was going to create it regardless. Use
// the plain accessor for anything observational.
//
// Same locking rule as above: the pool mutex covers creation and the pointer
// read only, and is released before the caller does any Metal work.
void *metal_shared_device_handle_ensure_created();

// ---------------------------------------------------------------------------
// C2 zero-copy capability gate (GPU copy-elimination campaign, plan §4.1).
//
// ONE decision point: the probe runs inside ensure_device_locked(), i.e. at
// device selection, and its verdict is computed once and never recomputed
// (plan §2.3, §4.1.2). No other translation unit may call `hasUnifiedMemory`
// itself; a second query site is a defect.
//
// The names below are FROZEN by plan §4.1.4 and §4.5 and are referenced
// verbatim by other sections' acceptance criteria. Do not rename.
// ---------------------------------------------------------------------------

// How the effective mode was decided. Values are stable and are reported
// verbatim through the debug counters probe, so they may be appended to but
// never renumbered.
enum class ZeroCopyCapabilityOverrideState : int {
  kNone = 0,       // no override; the probe's verdict is used
  kForcedOff = 1,  // behave as if the device reported non-unified memory
  kForcedOn = 2,   // assert the unified path regardless of the probe
};

// THE single query every pipeline site uses to choose unified vs fallback.
// Returns the EFFECTIVE mode: capability AND override already resolved.
//
// Side effect, deliberate and matching metal_shared_device_handle_ensure_created():
// if no device exists yet this CREATES it (and thereby runs the probe), because
// every caller is on the GPU decode path and a "not probed yet" answer of false
// on a lane's first decode is exactly the class of defect that produced R2's
// AC1 null-device-at-first-decode failure. It is never a lock-across-GPU-work:
// the pool mutex covers device creation and the state read only.
//
// On non-Metal builds (non-Apple, or DNG_FORCE_VULKAN) this is defined to
// return false unconditionally, so call sites need no `#if` (plan §4.6).
bool zero_copy_path_is_enabled();

// The RAW capability of the selected device, override-independent. Diagnostics
// and the one-time log line only — the pipeline must NOT branch on this.
// Observational, therefore side-effect-free: it never creates the device and
// answers false before any decode has created one.
bool metal_device_has_unified_memory();

// In-process form of CEYX_ZERO_COPY_CAPABILITY_OVERRIDE, for a test binary that
// cannot re-exec with a different environment. Must be called before the first
// decode on any thread. It re-runs only the MODE RESOLUTION, never the device
// selection, and never emits a second capability log line. An explicit call
// takes precedence over the environment variable.
void set_zero_copy_capability_override_for_testing(
    ZeroCopyCapabilityOverrideState override_state);

// Immutable snapshot of the capability gate, for the debug/probe FFI entry
// ceyx_debug_zero_copy_capability_counters() (plan §4.5), which lives in the
// FFI layer and is owned by another file. Observational and side-effect-free:
// it never creates the device, so before any decode it reports the not-yet-
// probed state (capability_probe_has_run=false, device_has_unified_memory=false,
// and zero_copy_path_is_enabled false unless the in-process testing hook has
// already forced it on). On non-Metal builds it always reports that state.
struct ZeroCopyCapabilityStateSnapshot {
  bool capability_probe_has_run = false;
  bool device_has_unified_memory = false;
  bool zero_copy_path_is_enabled = false;
  int devices_enumerated = 0;
  ZeroCopyCapabilityOverrideState capability_override_state =
      ZeroCopyCapabilityOverrideState::kNone;
};

ZeroCopyCapabilityStateSnapshot zero_copy_capability_state_snapshot();

// ---------------------------------------------------------------------------
// C2 counters (plan §4.5). FROZEN names, read by
// ceyx_debug_zero_copy_capability_counters() in the FFI layer.
//
// Defined here rather than at their increment sites because the sites live in
// three different files owned by three implementers, while the reader is one
// FFI entry: one definition point keeps the counters link-visible to everyone
// and lets the C2 sub-tasks land in any order.
//
// Monotonic, process-wide, relaxed atomics — they are diagnostic tallies, never
// control flow, so no ordering is needed between them and the decode work they
// count. Debug/probe only; nothing is added to DngResult.
//
// The zero_copy_note_* mutators are the increment side. Ownership of the
// increment SITES (lead ruling, 2026-09-11):
//   * destination wrapped      — Stage4 (dng_render_halide.cpp), at the
//                                wrap-success point.
//   * source mosaic wrapped    — RAW pipeline (raw_gpu_pipeline.cpp).
//   * alignment degraded       — RAW pipeline (raw_gpu_pipeline.cpp), NOT
//                                Stage4: Stage4 cannot distinguish a degraded
//                                decode from a plain fallback decode, so
//                                counting it there would conflate the two and
//                                hide exactly the permanent degradation this
//                                counter exists to expose.
// All seven functions are defined on EVERY platform (on non-Metal builds the
// counters simply stay at zero), so no call site needs an #if.
// ---------------------------------------------------------------------------

// Decodes that wrapped the caller destination (unified-wrapped path).
uint64_t zero_copy_destination_wrap_count();
void zero_copy_note_destination_wrapped();

// Decodes that hit the alignment degradation (unified-degraded path). This is
// the counter that stops a silent PERMANENT degradation from reading as
// success, so it must be incremented on every degraded decode, not once.
uint64_t zero_copy_destination_alignment_degradation_count();
void zero_copy_note_destination_alignment_degraded();

// Decodes that wrapped the arena source region instead of letting Halide
// upload — proves the input-side change actually ran.
uint64_t zero_copy_source_mosaic_wrap_count();
void zero_copy_note_source_mosaic_wrapped();

// Resets all three to zero. For test entries that need a known starting point;
// production never calls it. (Counter GATES should still read DELTAS, per the
// round-2 do-not-retry note on the AC1 driver.)
void reset_zero_copy_counters_for_testing();

}  // namespace ceyx

// C-ABI content marker, so a built binary can be proven to contain this pool by
// symbol (nm) rather than by mtime (campaign AC4).
extern "C" const char *ceyx_metal_queue_pool_v1(void);

// C-ABI content marker for the C2 capability gate (plan §4.5), modelled on the
// marker above: returns "ceyx_zero_copy_capability_v1". Defined on every
// platform so `nm` can prove a built binary contains this slice.
extern "C" const char *ceyx_zero_copy_capability_marker(void);

#endif  // DNG_METAL_CONTEXT_H
