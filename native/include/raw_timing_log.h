// raw_timing_log.h — the ONE per-decode CPU-phase timing emitter.
//
// WHY THIS EXISTS (read before adding a second one):
//   Before this module, the CPU-phase [RawTiming] line was printed by an
//   inline block inside ceyx_decode_into_ffi.cpp. That placement made the CPU
//   phases observable ONLY through the FFI entry. probe_concurrent_raw — the
//   multi-lane driver this whole measurement campaign runs at w1..w8 — calls
//   raw_pipeline_decode_file_into directly and therefore bypassed the FFI
//   entry by design, so at any lane width above one there was no way to
//   attribute raw_unpack / auto_exposure cost to a lane. Every measurement of
//   those phases had to be taken at w1 and extrapolated.
//
//   Hoisting the emit into the shared decode path (raw_gpu_pipeline.cpp's
//   three public decode_file_*_into entries, which all funnel through
//   decodeFileImpl) closes that gap: every caller of the shared path — FFI,
//   probe, tests — emits the same line, per lane, at any width.
//
// GATE: CEYX_RAW_TIMING_LOG=1. Off by default and read once per process into
// an atomic, so a default run pays one relaxed load per decode and emits
// nothing at all. The gate is runtime, never build-time, so the measured
// binary is byte-identical to the shipped one.
//
// LANE IDENTITY: lane= is a dense first-seen index per OS thread, assigned by
// this module. It is NOT the same namespace as the lane= emitted by
// raw_gpu_timing_probe.cpp, which derives its index from the Metal command
// queue. Both are first-seen-order over the same set of decode threads, so
// they agree whenever threads take their first decode in the same order they
// take their first GPU submission — which is the normal case but is NOT
// guaranteed. Do not join the two streams on lane= without saying so.
//
// LINE SHAPE (schema 2): consumed by native/tests/run_timing_harness.py and by
// run_decode_matrix.py, both of which parse key=value pairs generically and
// never by column position. Schema 1 was the same line without schema= and
// without lane=. Adding keys is backward compatible for both parsers; removing
// or renaming one is not — bump schema= and update both parsers in the same
// commit if you do.
//
//   [RawTiming] schema=2 lane=<int> host_to_device_copy_ms=... \
//     device_to_host_copy_ms=... host_copy_ms=... auto_exposure_ms=... \
//     gpu_submit_wait_ms=... gpu_process_ms=... raw_unpack_ms=... \
//     total_ms=... unified_memory_path_active=<uint> \
//     source_mosaic_copy_milliseconds=...

#ifndef CEYX_RAW_TIMING_LOG_H
#define CEYX_RAW_TIMING_LOG_H

struct RawTimingDiagnostics;
struct RawDecodeDiagnostics;

#ifdef __cplusplus
extern "C" {
#endif

// True when CEYX_RAW_TIMING_LOG=1 is set. Decided once per process.
int raw_timing_log_enabled(void);

// Dense first-seen lane index for the calling thread. Stable for the thread's
// lifetime. Returns -1 when the gate is off (no registry is built then).
int raw_timing_log_lane(void);

// Emit one [RawTiming] line for a completed decode. No-op when the gate is
// off. Both pointers may be null; missing halves print as zeros so the key set
// is fixed and a parser never has to cope with a variable schema.
void raw_timing_log_emit(const struct RawTimingDiagnostics *timing,
                         const struct RawDecodeDiagnostics *diag);

// Content marker, for proving this TU is the one linked into a given binary
// (nm/strings positive control). Same idiom as ceyx_gpu_timing_marker.
const char *raw_timing_log_marker(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // CEYX_RAW_TIMING_LOG_H
