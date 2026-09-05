// dng_copy_lock.h — ceyx strong override of the Halide v21 device-copy entry
// points (R2-T2, "Lock B", parallel-decode campaign 2026-09-05).
//
// WHY: Halide v21 serializes halide_copy_to_host / halide_copy_to_device /
// halide_buffer_copy behind ONE process-wide mutex (device_interface.cpp:28),
// and on Metal that mutex is held across a GPU wait AND a bulk memcpy
// (metal_v21.cpp:769 copy_to_device = copy_memory + commit/waitUntilCompleted;
// :813 copy_to_host = commit/waitUntilCompleted + copy_memory). R2-T1 attributed
// 79.00 ms/decode — 88.5 % of the post-Lock-A ARW residual — to waiting on it.
//
// WHAT THIS TU DOES: it provides STRONG definitions of the three entry points
// that displace Halide's weak ones inside libdng_decoder_native.dylib, keeps the
// upstream bodies verbatim by calling the exported *_already_locked internals,
// and replaces the process-wide mutex with ADDRESS-STRIPED PER-BUFFER mutexes.
// See the header comment of dng_copy_lock.cpp for the concurrent-entry safety
// analysis with upstream file:line evidence.
//
// Apple-only, behind the same guard as dng_metal_context.cpp: the safety
// argument is specific to the Metal backend plus R1-T2's per-thread queue pool.
// On every other platform this TU compiles to nothing.

#ifndef DNG_COPY_LOCK_H
#define DNG_COPY_LOCK_H

namespace ceyx {

// Number of address stripes (test/inspection marker). Compile-time constant.
int dng_copy_lock_stripes();

// Content marker; returns "ceyx_copy_lock_v1".
const char *dng_copy_lock_marker();

}  // namespace ceyx

// C-ABI content marker, so a built binary can be proven to contain this change
// by symbol (nm) rather than by mtime.
extern "C" const char *ceyx_copy_lock_v1(void);

// Self-check for the deadlock-shaped corner: two threads taking the same pair of
// buffers in opposite order, both for a cross-stripe pair and for a pair that
// collides on one stripe. Returns 0 on success; a real deadlock is caught by an
// internal watchdog that exits(3). Driven by tmp/r2t2/selftest_stripe.cpp.
extern "C" int ceyx_copy_lock_selftest(void);

#endif  // DNG_COPY_LOCK_H
