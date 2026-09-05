// dng_metal_api_gate.h — ceyx cold-start gate for Halide v21's unsynchronised
// metal_api_supports_set_bytes / metal_api_checked_device memo cache.
// R4 item 3, parallel-decode campaign, 2026-09-05.  EVIDENCE TRACK B.
//
// =============================================================================
// THE DEFECT (read out of the shipped runtime, not hypothesised)
// =============================================================================
// Halide v21's _halide_metal_run memoises one bit of API capability in two
// process-global words.  Disassembled from halide_runtime.a with relocations
// (ceyx/docs/logs/2026-09-05/item3/arx/memo_region.txt):
//
//     e178  ldr  x8, [x25]     ; LOAD  metal_api_checked_device
//     e188  cmp  x8, x0        ; is the memo for THIS device?
//     e18c  b.eq 0xe1c0        ; hit -> fast path
//     e194  bl   buffer_supports_set_bytes
//     e1a4  strb w0, [x10]     ; STORE metal_api_supports_set_bytes
//     e1ac  str  x0, [x25]     ; STORE metal_api_checked_device
//     e1c0  ldrb w8, [x10]     ; fast path LOAD of the flag
//
// Every access is a plain ldr/str/ldrb/strb: no ldar, no stlr, no dmb, no atomic
// and no lock anywhere in the block.  The stores are also in the wrong order for
// a lock-free publish — the flag is written first and the device second, both
// unordered — so a thread taking the fast path can see a current-looking device
// word next to a stale flag byte and launch down the wrong argument-passing
// path.  Before this campaign only one decode ran at a time and the branch could
// not be entered concurrently; with N lanes it is entered by N threads at once.
//
// =============================================================================
// WHY A GATE IS THE FIX, AND WHAT IT DOES NOT FIX
// =============================================================================
// The write cannot be edited: it is inside a vendored binary Halide runtime with
// no sources in this tree, and unlike the device-copy entry points that
// dng_copy_lock.cpp overrides, halide_metal_run exports no *_already_locked
// internal to delegate to.  So this TU does not remove the unsynchronised write;
// it removes every concurrent EXECUTION of it, by admitting exactly one thread
// into the region where the memo may be written until the memo is populated.
// After that the branch is never taken again and every thread only reads.
// KNOWN LIMITATION, declared rather than hidden: if a second MTLDevice ever
// appears the memo re-arms and the race returns.  One shared MTLDevice for the
// process lifetime is enforced today by dng_metal_context.cpp:12.  Parking lot.
//
// =============================================================================
// WHY IT CANNOT BE BYPASSED (full argument in docs/logs/2026-09-05/item3-callers.md)
// =============================================================================
// Every reference to either word in the whole runtime archive is inside
// _halide_metal_run — one writer function, no second reader.  Inside it there is
// exactly one call to halide_metal_acquire_context (0xddf8) and exactly one to
// halide_metal_release_context (0xde5c); both enumerations are complete.  The
// release call sits at a LOWER address than the memo and therefore reads as if
// it ran first: it does not.  0xde5c is the shared epilogue — drain pool,
// restore registers, ret at 0xde8c — reached only by the branches at 0xdfc8 and
// 0xe3b8, both past the memo.  So acquire DOMINATES the memo block and release
// POST-DOMINATES it, and both are ceyx's own strong overrides.  A bracket taken
// in those two hooks strictly contains the racy code, and nothing else in the
// process can reach it.
//
// =============================================================================
// RE-ENTRANCY — the corner that would turn this into a deadlock
// =============================================================================
// halide_metal_acquire_context is re-entered by the same thread at depth
// (dng_metal_context.cpp:228 increments a per-thread counter).  A non-recursive
// mutex taken on every entry would self-deadlock on the second one.  The gate is
// therefore depth-counted per thread and touches the mutex ONLY at depth 0.
// Lock order: enter() is called AFTER the queue pool's own mutex has been
// released (dng_metal_context.cpp:249) and exit() BEFORE it is taken, so the
// gate mutex and the pool mutex are never held simultaneously and cannot invert.
// A failed acquire returns before enter() is reached, so a release that arrives
// with no matching enter finds depth 0 and does nothing.
//
// =============================================================================
// HEADER-ONLY ON PURPOSE
// =============================================================================
// All state is function-local in inline functions, so there is no new
// translation unit and no build-file edit for the gate itself — only the test
// target needs registering.  Two call sites include this header; both get the
// same state.
//
// CEYX_METAL_API_GATE=0 disables the gate WITHOUT removing the instrument: the
// occupancy counter still runs, which is exactly what the RED arm needs.  It is
// the only difference between the two arms, so both are one binary and their
// identity is provable by UUID rather than by argv alone.

#ifndef DNG_METAL_API_GATE_H
#define DNG_METAL_API_GATE_H

#if defined(__APPLE__) && !defined(DNG_FORCE_VULKAN)

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>

// Halide's two words, declared by their real identity.  Data-symbol mangling
// encodes namespace and name only, never type, so this binds to exactly
// __ZN6Halide7Runtime8Internal5Metal24metal_api_checked_deviceE — the same
// symbol nm reports as `weak external` in the shipped dylib.  The gate reads the
// REAL state rather than a shadow copy of it.
namespace Halide {
namespace Runtime {
namespace Internal {
namespace Metal {
extern void *metal_api_checked_device;
}  // namespace Metal
}  // namespace Internal
}  // namespace Runtime
}  // namespace Halide

namespace ceyx {

inline std::mutex &metal_api_gate_mutex() {
    static std::mutex m;
    return m;
}

// Set once the memo has been observed populated; after that the gate is a single
// relaxed-acquire load on the fast path.
inline std::atomic<bool> &metal_api_gate_done() {
    static std::atomic<bool> f{false};
    return f;
}

// TRACK B INSTRUMENT.  Occupancy of the bracket while the memo is still
// unpopulated — i.e. the window in which halide_metal_run may execute the racy
// stores.  The bracket is a SUPERSET of the memo block (copy_to_host and
// copy_to_device also acquire a context without running a kernel), so the
// counter can over-count but can never under-count.  That direction matters: a
// superset cannot manufacture a false GREEN, only a conservative RED.
inline std::atomic<int> &metal_api_gate_occupancy() {
    static std::atomic<int> n{0};
    return n;
}

inline std::atomic<int> &metal_api_gate_max_occupancy() {
    static std::atomic<int> n{0};
    return n;
}

inline bool metal_api_gate_enabled() {
    static const bool on = []() {
        const char *e = std::getenv("CEYX_METAL_API_GATE");
        return !(e != nullptr && std::strcmp(e, "0") == 0);
    }();
    return on;
}

inline int &metal_api_gate_depth() {
    static thread_local int d = 0;
    return d;
}

inline bool &metal_api_gate_holding() {
    static thread_local bool h = false;
    return h;
}

// Called at the SUCCESSFUL return of halide_metal_acquire_context, after the
// queue-pool mutex has been released.
inline void metal_api_gate_enter() {
    if (metal_api_gate_done().load(std::memory_order_acquire)) {
        return;  // memo populated: reads only from here on, no gating
    }
    if (metal_api_gate_depth()++ > 0) {
        return;  // nested acquire on this thread; the outer one holds
    }
    if (metal_api_gate_enabled()) {
        metal_api_gate_mutex().lock();
        metal_api_gate_holding() = true;
    }
    const int now = metal_api_gate_occupancy().fetch_add(1, std::memory_order_relaxed) + 1;
    int prev = metal_api_gate_max_occupancy().load(std::memory_order_relaxed);
    while (now > prev &&
           !metal_api_gate_max_occupancy().compare_exchange_weak(
               prev, now, std::memory_order_relaxed)) {
    }
}

// Called at the top of halide_metal_release_context, before the queue-pool mutex
// is taken.
inline void metal_api_gate_exit() {
    if (metal_api_gate_depth() == 0) {
        return;  // fast path, or an acquire that failed before entering
    }
    if (--metal_api_gate_depth() > 0) {
        return;
    }
    metal_api_gate_occupancy().fetch_sub(1, std::memory_order_relaxed);
    if (metal_api_gate_holding()) {
        // Read under the gate mutex, and the only writer of this word inside the
        // bracket was this very thread, so the gate adds no race of its own.
        // With the gate DISABLED this read is skipped entirely, which keeps the
        // RED arm free of any access the instrument itself introduced.
        if (Halide::Runtime::Internal::Metal::metal_api_checked_device != nullptr) {
            metal_api_gate_done().store(true, std::memory_order_release);
        }
        metal_api_gate_holding() = false;
        metal_api_gate_mutex().unlock();
    }
}

}  // namespace ceyx

// Content marker, so a built binary is provably the one under test by symbol
// rather than by mtime.
//
// __attribute__((used)) IS LOad-BEARING AND WAS ADDED BECAUSE THE CHECK CAUGHT
// ITS ABSENCE. Without it the first build of this header produced a binary whose
// symbol table had NO `ceyx_metal_api_gate_v1` at all: an inline function whose
// address is never taken is inlined at its only call site and no out-of-line
// copy is emitted, so the marker existed in the source and not in the artifact.
// A marker that can vanish while the code it vouches for is present is worse
// than no marker — it fails in the direction of a false negative on identity.
// `used` forces the out-of-line definition to be emitted in every translation
// unit that includes this header.
extern "C" __attribute__((used)) inline const char *ceyx_metal_api_gate_v1(void) {
    return "ceyx_metal_api_gate_v1";
}

// Track-B observable. `used` for the same reason as the marker above: an
// inspector must be able to see it in the symbol table of the binary that
// produced the numbers.
extern "C" __attribute__((used)) inline int ceyx_metal_api_gate_max_occupancy(void) {
    return ceyx::metal_api_gate_max_occupancy().load(std::memory_order_relaxed);
}

#else  // not Apple, or Vulkan backend

namespace ceyx {
inline void metal_api_gate_enter() {}
inline void metal_api_gate_exit() {}
}  // namespace ceyx

#endif  // __APPLE__ && !DNG_FORCE_VULKAN

#endif  // DNG_METAL_API_GATE_H
