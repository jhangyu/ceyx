#pragma once

/* RAW (LibRaw) route FFI surface.
 *
 * Extracted from dng_ffi_api.h on 2026-08-25. The RAW route returns the same
 * DngResult as the DNG route and reuses dng_free_result
 * for teardown, so this header includes dng_ffi_api.h rather than duplicating
 * the struct: the ABI is shared by design, and duplicating it would create two
 * definitions to keep in sync with plugin/lib/src/dng_bindings.dart.
 *
 * The full definition of RawDecodeDiagnostics lives in raw_pipeline_contract.h;
 * this header only forward-declares it, exactly as dng_ffi_api.h used to.
 */
#include <stdint.h>

#include "dng_ffi_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* WP5: the allocating generic-RAW decode entry that used to be declared here
 * is DELETED. Generic RAW is decoded through ceyx_decode_into_buffer
 * (ceyx_decode_into.h), which writes into a CALLER-OWNED buffer; the library
 * no longer allocates full-resolution RGBA output on any path. */
struct RawDecodeDiagnostics;

/* Diagnostics for the calling thread's most recent RAW decode.
 * Returns 0 on success, -1 when out is null or no decode has run. */
int32_t raw_last_diagnostics(struct RawDecodeDiagnostics *out);

/* Round 2 Task 2.4: colour-pipeline diagnostics channel, additive and
 * separate from RawDecodeDiagnostics (frozen -- Dart-visible layout, spec
 * section 12). New symbol, new struct, so no existing ABI moves.
 *
 * kRawCameraMatrixRoute* mirrors (by value, 0/1/2) the private constants
 * local to libraw_gpu_input_adapter.cpp (kRawCameraMatrixRouteNone/RgbCam/
 * CamXyz). Round 2 Task 2.6 threads the real per-decode value through
 * RawPipelineResult::color_diag (raw_gpu_pipeline.h) -- see
 * raw_last_color_diagnostics's contract below for what is now live. */
enum {
    kRawCameraMatrixRouteNone = 0,
    kRawCameraMatrixRouteRgbCam = 1,
    kRawCameraMatrixRouteCamXyz = 2
};

/* Mirrors RawAutoExposureStatus (raw_auto_exposure.h) by value; kept as a
 * plain uint32_t here rather than including that header, which is owned by
 * a parallel round-2 task. This name is retained as the literal 0 (== kOk)
 * for source compatibility with existing callers written against Task 2.4's
 * build; it is a real, meaningful status now (Task 2.6), not only a sentinel. */
enum { kRawColorAutoExposureStatusUnavailable = 0 };

typedef struct RawColorDiagnostics {
    uint32_t struct_size;
    float    auto_exposure_ev;      /* what Round 1 actually applied */
    uint32_t auto_exposure_status;  /* RawAutoExposureStatus (mirrored value) */
    uint32_t vendor_curve_applied;  /* 1 = LibRaw handed us curve-applied pixels */
    uint32_t matrix_route;          /* kRawCameraMatrixRoute* above */
    uint32_t clamped_mask;          /* bit0 exposure, bit1 tone, bit2 shadows, bit3 wb */
    char     reason[128];
} RawColorDiagnostics;

/* Colour diagnostics for the calling thread's most recent RAW decode.
 * Returns 0 on success, -1 when out is null or no decode has run yet
 * (mirrors raw_last_diagnostics's contract exactly).
 *
 * Round 2 Task 2.6 threads the real values through RawPipelineResult
 * (raw_gpu_pipeline.h's RawColorPipelineDiagnostics) from where each is
 * actually computed: auto_exposure_ev/status/reason from the auto-exposure
 * estimator inside LibRawGpuInputAdapter::build() (only when a CFA or
 * linear-RGB layout was attempted -- an unattempted layout leaves all three
 * at their zero-initialised default, matching auto_exposure_ev's existing
 * "no gain" contract); matrix_route from that same build() call's colour-
 * matrix routing; vendor_curve_applied from Round 2 Task 2.4's frontend-level
 * detection (LibRawRawView::vendor_curve_applied). `reason` is empty when
 * status is kOk or when auto-exposure was never attempted (DNG route,
 * decode failure before the adapter ran).
 * clamped_mask is NOT wired: no round-2 task reports WHICH RawDevelopParams
 * field a value was clamped from -- raw_contract_validate.cpp enforces the
 * ranges but does not report per-field clamp bits. Left at 0 rather than
 * guessed; a future task owns adding that reporting.
 * `struct_size` is always valid on a successful (0) return. */
int32_t raw_last_color_diagnostics(RawColorDiagnostics *out);

/* C4 (plan §6.4): sub-timing diagnostics channel, additive and separate from
 * RawDecodeDiagnostics (frozen -- Dart-visible layout, spec section 12) and
 * from RawColorDiagnostics above. New symbol, new struct, so no existing ABI
 * moves and no Dart file is touched.
 *
 * Splits the single `gpu_process_ms` window (raw_gpu_pipeline.cpp gpu_t0/
 * gpu_t1) into host<->device copy time, CPU auto-exposure time (previously
 * unattributed to either raw_unpack_ms or gpu_process_ms -- plan §6.3), and
 * the GPU submit/wait residue. gpu_submit_wait_ms is computed as
 * gpu_process_ms - host_copy_ms, never independently bracketed, so the parts
 * cannot drift out of agreement with the existing total.
 *
 * ABI WARNING (round-4 review B2): "additive/struct_size-versioned" above
 * describes THIS struct's own layout evolution (append-only, offsets never
 * move). It does NOT make RawTimingDiagnostics freely extensible across a
 * compiled boundary when it is embedded BY VALUE inside another struct that
 * itself crosses that boundary -- RawPipelineResult (raw_gpu_pipeline.h)
 * embeds `RawTimingDiagnostics timing{}` as a non-last member, so growing
 * this struct shifts every RawPipelineResult member that follows it and
 * changes RawPipelineResult's own sizeof. A binary built against an older
 * header that declares `RawPipelineResult result{}` on its stack and passes
 * &result into a NEWER dylib is a stack buffer overflow, not a compatible
 * call -- this actually happened once in this round (test_concurrent_raw_
 * decode_wrapped, RC=134 SIGABRT, until rebuilt in lockstep). Every producer
 * AND every consumer of RawPipelineResult must be recompiled together
 * whenever this struct's size changes; struct_size only protects callers
 * that go through the free-standing raw_last_timing_diagnostics() getter
 * below (which does honour it), not the embedded-by-value path. */
typedef struct RawTimingDiagnostics {
    uint32_t struct_size;            /* sizeof(RawTimingDiagnostics) by the producer */
    double   host_copy_ms;           /* host->device + device->host, frame-sized buffers */
    double   host_to_device_copy_ms; /* the upload half, reported separately for AC2 */
    double   device_to_host_copy_ms; /* the copy-back half, reported separately for AC2 */
    double   auto_exposure_ms;       /* CPU auto-exposure estimator, plan §6.3 */
    double   gpu_submit_wait_ms;     /* gpu_process_ms - host_copy_ms, plan §6.2 item 3 */
    uint32_t unified_memory_path_active;   /* 1 = zero-copy path taken this decode.
                                             * C2 (capability-gated zero-copy) has not
                                             * landed yet: this field is always 0 today
                                             * for every decode, not a per-decode signal
                                             * that the zero-copy gate evaluated false. */
    /* R4-T4 S3 (round-4 review B1 fix), additive/struct_size-versioned like the
     * rest of this struct -- NOT part of the frozen Dart-visible ABI list.
     * Appended at the end on purpose: no existing field's offset changes.
     *
     * The C2 unified-path source memcpy (raw_gpu_pipeline.cpp, ~48MB/decode)
     * runs BEFORE gpu_t0 opens, so it is deliberately kept OUT of
     * host_to_device_copy_ms/host_copy_ms/gpu_submit_wait_ms -- folding it in
     * there breaks the documented identity
     * gpu_submit_wait_ms == gpu_process_ms - host_copy_ms (the copy is not
     * inside the gpu_process_ms window it would be subtracted from). This
     * field names that time on its own, additively, so it is finally
     * attributed to something instead of being silently unattributed. */
    double   source_mosaic_copy_milliseconds;
} RawTimingDiagnostics;

/* Timing diagnostics for the calling thread's most recent RAW decode.
 * Returns 0 on success, -1 when out is null or no decode has run yet
 * (mirrors raw_last_color_diagnostics's contract exactly).
 *
 * `struct_size` is an INPUT, not an output-only field (round-4 review P-5):
 * the caller MUST set out->struct_size to sizeof(RawTimingDiagnostics) (the
 * caller's own compiled layout) before calling. The implementation copies
 * min(caller struct_size, producer sizeof) bytes and writes the producer's
 * actual size back into out->struct_size on return, so the versioning is
 * genuinely honoured by this getter (unlike the embedded-by-value path in
 * RawPipelineResult, which struct_size does NOT protect -- see the ABI
 * warning on RawTimingDiagnostics above).
 *
 * A caller that zero-initialises `out` and skips setting struct_size before
 * the call gets struct_size == 0 as input, which yields copy_bytes == 0 --
 * an all-zero struct returned with RC == 0. Treat struct_size == 0 as a
 * caller bug, not a valid "no decode yet" signal: callers must always set
 * struct_size to their own sizeof(RawTimingDiagnostics) before calling. */
int32_t raw_last_timing_diagnostics(struct RawTimingDiagnostics *out);

/* Internal call, same binary, never looked up via dlsym/FFI -- deliberately
 * NOT RAW_FFI_EXPORT'd and not part of the Dart-visible surface, mirroring
 * raw_record_decode_into_diagnostics below. Called unconditionally (success
 * or failure) from the decode-INTO entry point so a failed decode still
 * reports whatever sub-timings it accumulated before failing (plan §6.4). */
void raw_record_decode_timing_diagnostics(const struct RawTimingDiagnostics *timing);

/* R6 fix: wires the decode-INTO entry point (ceyx_decode_into_buffer's
 * generic-RAW arm, native/src/ffi/ceyx_decode_into_ffi.cpp) into the
 * thread-local diagnostics state the two queries above read, so they reflect
 * the most recent decode. WP5 deleted the allocating RAW entry that used to
 * share this duty, so this is now the SOLE writer -- which is the intended end
 * state. Before the R6 fix a decode-into call left both queries describing
 * whatever earlier allocating-entry call last ran on this thread (or "no decode
 * has run" if none had) -- stale-by-construction, not merely by timing.
 *
 * Internal call, same binary, never looked up via dlsym/FFI -- deliberately
 * NOT RAW_FFI_EXPORT'd and not part of the Dart-visible surface. `diag` is
 * required; `color_diag` may be null (a route with no colour pipeline, e.g. a
 * failure before the adapter ran, simply leaves the colour channel
 * unrecorded, exactly as the deleted allocating entry already treated a
 * failed build()). RawColorPipelineDiagnostics is forward-declared only: this header
 * never needs its layout, only a pointer to it. */
struct RawColorPipelineDiagnostics;
void raw_record_decode_into_diagnostics(
    const RawDecodeDiagnostics *diag,
    const struct RawColorPipelineDiagnostics *color_diag);

/* ===================================================================== */
/* C3 render-parameter upload cache probe (R1-T1, GPU copy-elimination    */
/* campaign, docs/logs/2026-09-11/plan-gpu-copy-elimination.md §5.2/§5.4). */
/*                                                                        */
/* DEBUG/PROBE API, in the same category as the pool live-address gauge:   */
/* it is NOT part of the Dart-visible surface and adds nothing to          */
/* DngResult. Defined in native/src/ffi/dng_ffi_api.cpp beside the         */
/* existing FFI_EXPORT block (precedent: dng_decode_configured_slots).     */
/*                                                                        */
/* Reports the PROCESS-WIDE totals since process start. The gate reads     */
/* deltas across decodes: same-settings repeat decode => uploads delta 0   */
/* and cache-hits delta 12. Both counters are reported because zero        */
/* uploads with zero hits means the caching code never ran at all, which   */
/* is a different and much worse outcome than zero uploads with twelve     */
/* hits. Returns 0 on success, -1 when BOTH out-pointers are null; either  */
/* out-pointer alone may be null.                                          */
/* ===================================================================== */
int32_t ceyx_debug_render_parameter_cache_counters(
    uint64_t *out_uploads_performed,
    uint64_t *out_cache_hits);

/* ===================================================================== */
/* C1 persistent device arena probe (R2-T1, GPU copy-elimination          */
/* campaign, docs/logs/2026-09-11/plan-gpu-copy-elimination.md §2.6/§3.3). */
/*                                                                        */
/* DEBUG/PROBE API, same category as the cache probe above: NOT part of    */
/* the Dart-visible surface and nothing is added to DngResult. Defined in  */
/* native/src/ffi/dng_ffi_api.cpp beside the existing FFI_EXPORT block     */
/* (precedent: dng_decode_configured_slots).                              */
/*                                                                        */
/* Reports PROCESS-WIDE totals since process start. AC1 is judged on the   */
/* allocation-count delta across N post-warmup decodes being 0, with the   */
/* growth count reported separately so a legitimate growth event is        */
/* visible instead of failing AC1, and the binding count reported because  */
/* an allocation delta of 0 produced by an arena that never ran is the     */
/* "arena silently not used" failure, not a pass.                          */
/*                                                                        */
/* Null-pointer convention (binding lead ruling, round-1 handoff):         */
/* individual out-pointers may be null and are then skipped; returns 0 if  */
/* at least one was filled, -1 only when ALL are null.                     */
/* ===================================================================== */
int32_t ceyx_debug_persistent_device_arena_counters(
    uint64_t *out_allocation_count,
    uint64_t *out_growth_reallocation_count,
    uint64_t *out_binding_count,
    uint64_t *out_resident_device_bytes,
    uint64_t *out_live_lane_count);

/* ===================================================================== */
/* Arena idle release (mem8 SR-1, T1; plan Halcyon                        */
/* docs/logs/2026-09-12/mem8-plan.md T1.2 step 7).                        */
/*                                                                        */
/* THE ONE NATIVE IDLE FUNNEL. ceyx_native_idle_shrink is the single      */
/* entry through which a quiescent host asks the native side to give      */
/* device bytes back; every native idle-release subsystem is reached      */
/* through it rather than through a mechanism of its own. Defined in      */
/* native/src/ffi/dng_ffi_api.cpp beside the arena probe above.           */
/*                                                                        */
/* Releases the device regions of every arena lane in excess of `floor`   */
/* and returns the BYTE COUNT released, so the Dart idle path has         */
/* something non-trivial to log. A negative floor is clamped to 0 (itself */
/* a legal floor, meaning "release every quiescent lane"), so -1 is       */
/* reserved and is not returned by the current implementation. A call     */
/* with nothing to release returns 0, which is SUCCESS, not an error.     */
/*                                                                        */
/* This export forwards verbatim to                                       */
/* raw_persistent_device_arena_shrink_to_lane_floor                       */
/* (native/include/raw_persistent_device_arena.h), whose contract it      */
/* carries in full -- INCLUDING clause (e): THE CALLER MUST GUARANTEE     */
/* DECODE QUIESCENCE. The per-lane live-binding refusal is a backstop,    */
/* not a lock; the pool's quiescence window is the only clock.            */
/*                                                                        */
/* Present on every platform: the non-Metal build compiles the portable   */
/* stub, which truthfully answers 0 because it holds no regions.          */
/* ===================================================================== */
int64_t ceyx_native_idle_shrink(int32_t floor);

/* ===================================================================== */
/* Arena idle-release probe (mem8 SR-1, T1; SR-10's volatile counter).    */
/*                                                                        */
/* DEBUG/PROBE API, same category as the two probes above: NOT part of    */
/* the Dart-visible surface and nothing is added to DngResult.            */
/*                                                                        */
/* Reports PROCESS-WIDE totals since process start, except                */
/* out_resident_lane_count, which is an INSTANTANEOUS derived reading:    */
/* lanes currently holding device bytes, distinct from the live lane      */
/* count, which counts lanes that EXIST. After a shrink the two differ,   */
/* and that difference is what the shrink accomplished.                   */
/*                                                                        */
/* out_volatile_device_bytes is a SEPARATE quantity, never a subtraction: */
/* a volatile region is still RESIDENT until the OS actually reclaims it, */
/* so `resident - volatile` would publish a number describing a state     */
/* that may never occur. It reads 0 until T17's purgeable marking lands,  */
/* and is present from this export's FIRST release on purpose -- widening */
/* the signature later would silently mismatch every harness already      */
/* built against the narrower typedef, which is exactly the trap that     */
/* keeps ceyx_debug_persistent_device_arena_counters above at five.       */
/*                                                                        */
/* Null-pointer convention, same as the probes above: individual          */
/* out-pointers may be null and are then skipped; returns 0 if at least   */
/* one was filled, -1 only when ALL are null.                             */
/* ===================================================================== */
int32_t ceyx_debug_arena_shrink_counters(
    uint64_t *out_shrink_calls,
    uint64_t *out_lanes_released,
    uint64_t *out_lanes_refused,
    uint64_t *out_bytes_released,
    uint64_t *out_resident_lane_count,
    uint64_t *out_volatile_device_bytes);

/* ===================================================================== */
/* C2 zero-copy capability-gate probe (R3-T4, GPU copy-elimination        */
/* campaign, docs/logs/2026-09-11/plan-gpu-copy-elimination.md §4.5).      */
/*                                                                        */
/* DEBUG/PROBE API, same category as the two probes above: NOT part of     */
/* the Dart-visible surface and nothing is added to DngResult. Defined in  */
/* native/src/ffi/raw_ffi_api.cpp (this route's own FFI TU, rather than    */
/* dng_ffi_api.cpp, which is outside this task's file ownership this       */
/* round).                                                                */
/*                                                                        */
/* out_zero_copy_path_is_enabled / out_capability_override_state report    */
/* the CURRENT gate state (plan §4.1.4's                                   */
/* ceyx::ZeroCopyCapabilityStateSnapshot), not a per-decode delta --        */
/* out_capability_override_state mirrors                                   */
/* ceyx::ZeroCopyCapabilityOverrideState by value (0=none, 1=forced_off,   */
/* 2=forced_on). The three counters are PROCESS-WIDE totals since process  */
/* start, read as deltas by the gates exactly like the two probes above:   */
/*   - out_destination_wrap_count: decodes that wrapped the caller's own   */
/*     destination buffer (unified-wrapped path);                         */
/*   - out_destination_alignment_degradation_count: decodes that hit the   */
/*     alignment degradation (unified-degraded path: arena dst + one       */
/*     final memcpy);                                                     */
/*   - out_source_mosaic_wrap_count: decodes that wrapped the arena source */
/*     region instead of letting Halide upload it.                        */
/*                                                                        */
/* Null-pointer convention (binding lead ruling, execution contract        */
/* "Rulings during execution"): individual out-pointers may be null and    */
/* are then skipped; returns 0 if at least one was filled, -1 only when    */
/* ALL five are null. */
/* ===================================================================== */
int32_t ceyx_debug_zero_copy_capability_counters(
    int32_t *out_zero_copy_path_is_enabled,
    int32_t *out_capability_override_state,
    uint64_t *out_destination_wrap_count,
    uint64_t *out_destination_alignment_degradation_count,
    uint64_t *out_source_mosaic_wrap_count);

/* ===================================================================== */
/* T12.0 -- FROZEN OUTPUT-FORMAT CONTRACT (mem8 v3 campaign, Phase P0,    */
/* Halcyon/docs/logs/2026-09-19/mem8-v3-plan.md "T12.0 -- Freeze the      */
/* yuv420 format contract").                                              */
/*                                                                        */
/* DECLARATIONS ONLY. No behaviour is implemented by T12.0: the kernel    */
/* arm is T12, the converter is T13, the Dart binding is T14 and the      */
/* Halcyon seam is T15a. Those four tasks CONSUME this block; a consumer  */
/* that needs it changed STOPS and reports to the lead rather than        */
/* editing it, because the whole value of freezing it is that four        */
/* consumers can trust one declaration.                                   */
/*                                                                        */
/* R-B: rgba8 is enumerator 0 and remains the C-side DEFAULT, so every    */
/* existing caller and every existing entry point is byte-for-byte        */
/* unchanged. This block is purely ADDITIVE.                              */
/*                                                                        */
/* Enumerators are APPENDED, never renumbered -- the same rule            */
/* CeyxImageFormat (ceyx_encode_api.h) already follows, because the       */
/* values cross an FFI boundary into plugin/lib/src/codec_format.dart's   */
/* CeyxOutputFormat mirror.                                               */
/* ===================================================================== */
enum CeyxOutputFormat {
    /* 4 B/px interleaved RGBA8 -- today's behaviour, the default. */
    kCeyxOutputFormatRgba8 = 0,
    /* Planar 4:2:0, 1.5 B/px average. See the layout contract below. */
    kCeyxOutputFormatYuv420 = 1
};

/* ===================================================================== */
/* DESTINATION MEMORY LAYOUT -- RULED, TIGHTLY PACKED (T12.0 clause 2a,   */
/* rationale in T12.0.1).                                                 */
/*                                                                        */
/* A decode writes into a SINGLE CONTIGUOUS CALLER-OWNED allocation.      */
/* Plane order is Y, Cb, Cr. Each plane is TIGHTLY PACKED: its row stride */
/* EQUALS its plane width, and there is NO inter-plane padding.           */
/*                                                                        */
/*   plane extents:  Y  = w x h                                           */
/*                   Cb = ceil(w/2) x ceil(h/2)                           */
/*                   Cr = ceil(w/2) x ceil(h/2)                           */
/*   plane bases:    Y  = base                                            */
/*                   Cb = base + w*h                                      */
/*                   Cr = base + w*h + ceil(w/2)*ceil(h/2)                */
/*   total bytes:    EXACTLY w*h + 2*(ceil(w/2) * ceil(h/2))              */
/*                                                                        */
/* This is not an observation about Halide's defaults, it is the          */
/* contract: the byte-count formula below is correct ONLY under tight     */
/* packing, and under padded rows it UNDER-ALLOCATES, which is a heap     */
/* overrun rather than a miscount. T12's tests therefore ASSERT the       */
/* returned descriptor's pointers and strides against literals, including */
/* an ODD-dimension case where a naive w/2 differs from ceil(w/2).        */
/*                                                                        */
/* If a backend turns out to be unable to write this layout, that is      */
/* R-O's exception trigger: STOP and report for a user ruling. A          */
/* per-backend layout is forbidden.                                       */
/* ===================================================================== */
typedef struct CeyxYuv420PlaneDescriptor {
    uint32_t struct_size;         /* sizeof(CeyxYuv420PlaneDescriptor) */
    uint8_t *plane_base[3];       /* Y, Cb, Cr -- see plane bases above */
    int32_t  plane_width[3];
    int32_t  plane_height[3];
    int32_t  plane_row_stride[3]; /* == plane_width[i], by contract */
} CeyxYuv420PlaneDescriptor;

/* Bytes a decode of width x height in `output_format` occupies.
 *
 * THIS IS THE CONTRACT'S SIZING FUNCTION (T12.0 clause 2b), not a T14
 * convenience: T12's destination sizing, T13's converter, T14's binding and
 * T15's slot sizing all call it. A consumer that open-codes the arithmetic
 * instead is a DEFECT BY CONTRACT, not a style preference. Its Dart mirror is
 * `ceyxOutputFormatByteCount` (plugin/lib/src/codec_format.dart) and the two
 * must agree exactly.
 *
 *   kCeyxOutputFormatRgba8  -> w*h*4
 *   kCeyxOutputFormatYuv420 -> w*h + 2*(ceil(w/2) * ceil(h/2))
 *
 * Returns the byte count, or -1 for an unknown format / a non-positive width
 * or height. int64_t because w*h*4 overflows int32_t well inside the sensor
 * sizes this pipeline already decodes. */
int64_t ceyx_output_format_byte_count(int32_t output_format, int32_t width,
                                      int32_t height);

/* ===================================================================== */
/* FORMAT-TAKING ENTRY POINTS (T12.0 clause 2).                           */
/*                                                                        */
/* Additive siblings of the format-agnostic entries in ceyx_decode_into.h */
/* (ceyx_probe_output_size / ceyx_decode_into_buffer /                    */
/* ceyx_decode_into_buffer_oriented), which are UNCHANGED and remain      */
/* rgba8. Format selection is a HOST-SIDE choice of which entry to call;  */
/* it is never an Expr inside a kernel (DngRenderGenerator.cpp:641-655).  */
/*                                                                        */
/* `out_planes` may be NULL. When non-NULL and the request succeeded with */
/* kCeyxOutputFormatYuv420, it is filled with the layout described above; */
/* for kCeyxOutputFormatRgba8 it is zeroed (there are no planes).         */
/* Callers set out_planes->struct_size before the call.                   */
/* ===================================================================== */

/* Format-aware sibling of ceyx_probe_output_size. Additionally reports the
 * destination byte requirement for `output_format`, which is exactly
 * ceyx_output_format_byte_count(output_format, *out_width, *out_height).
 * out_byte_count may be NULL. Same return codes as ceyx_probe_output_size. */
int32_t ceyx_probe_output_size_format(const char *file_path, int32_t max_dim,
                                      int32_t output_format,
                                      int32_t *out_width, int32_t *out_height,
                                      int64_t *out_byte_count);

/* Format-aware sibling of ceyx_decode_into_buffer. Identical contract except
 * that the pixels written to dst are in `output_format` and the dst_capacity
 * floor is ceyx_output_format_byte_count(...) rather than w*h*4; a smaller
 * capacity is kCeyxErrDstTooSmall before any pixel work. */
DngResult *ceyx_decode_into_buffer_format(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t output_format, CeyxYuv420PlaneDescriptor *out_planes);

/* Format-aware sibling of ceyx_decode_into_buffer_oriented. Orientation
 * semantics are unchanged (orientation failure is decode failure, never an
 * unoriented fallback); plane extents describe the ORIENTED extent. */
DngResult *ceyx_decode_into_buffer_oriented_format(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t exif_orientation, int32_t output_format,
    CeyxYuv420PlaneDescriptor *out_planes);

/* The single yuv420 -> RGBA8 upconvert implementation (SR-11, T13). Dart must
 * never open-code one. `src` is the tightly-packed layout above; `dst` is
 * w*h*4 interleaved RGBA8 with alpha 255. Conversion follows the plan's §3
 * oracle: full-range BT.601, 2x2 box-average chroma.
 * Returns 0, or kCeyxErrDstTooSmall when dst_capacity < w*h*4, or -1 for a
 * null pointer or a non-positive extent. */
int32_t ceyx_yuv420_to_rgba8(const uint8_t *src, size_t src_capacity,
                             uint8_t *dst, size_t dst_capacity, int32_t width,
                             int32_t height);

/* ===================================================================== */
/* FAILURE ENCODING FOR A STALE DYLIB (T12.0 clause 3 / R-J).             */
/*                                                                        */
/* A library predating this contract does not EXPORT the five symbols     */
/* above. The guarded Dart lookup therefore finds nothing, and the        */
/* binding raises a HARD TYPED FAILURE (CeyxFormatUnsupportedException,   */
/* plugin/lib/src/codec_format.dart) at submit time -- NEVER a null, and  */
/* NEVER a silent rgba8 fallback, which would hand the caller 4 B/px      */
/* bytes it is about to interpret as 1.5 B/px. There is no C-side error   */
/* code for this condition, because the condition is "the symbol is       */
/* absent", which no C call can report.                                   */
/*                                                                        */
/* The exception carries EXACTLY THREE fields (T12.0.2): the requested    */
/* format, the missing symbol, and the ABSOLUTE PATH OF THE LIBRARY       */
/* ACTUALLY LOADED. A fourth (the expected pin digest) is ruled out: it   */
/* is already authoritative in Halcyon's scripts/ceyx_release_pin.json.   */
/* ===================================================================== */

#ifdef __cplusplus
}
#endif
