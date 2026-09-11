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

}  // namespace ceyx

// C-ABI content marker, so a built binary can be proven to contain this pool by
// symbol (nm) rather than by mtime (campaign AC4).
extern "C" const char *ceyx_metal_queue_pool_v1(void);

#endif  // DNG_METAL_CONTEXT_H
