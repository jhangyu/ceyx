// Generic RAW C ABI. Reuses the FROZEN DngResult layout, so Dart bindings need
// no struct change (spec section 12.2). error_code carries a RawErrorCode,
// whose values (<= -201) are disjoint from DngErrorCode.
#include <cstdio>
#include <cstdlib>

#include "dng_pipeline.h"
#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"

// C2 zero-copy capability gate accessors (plan §4.1.4), defined by
// impl-capability-opus in dng_metal_context.h/.cpp -- included for the
// enabled/override half of ceyx_debug_zero_copy_capability_counters below.
#include "dng_metal_context.h"

// R3-T4: the three C2 wrap/degradation counters (plan §4.5). Declared here
// rather than defined here -- they are incremented at the call sites that
// actually perform the wrap (raw_gpu_pipeline.cpp for the source mosaic
// wrap, dng_render_halide.cpp for the destination wrap / alignment
// degradation), which are outside this task's file ownership this round.
// Forward-declared so this probe compiles and links against whichever
// translation unit ends up defining them; names and namespace are FROZEN by
// plan §4.5 and must not be renamed at either end.
namespace ceyx {
uint64_t zero_copy_destination_wrap_count();
uint64_t zero_copy_destination_alignment_degradation_count();
uint64_t zero_copy_source_mosaic_wrap_count();
}  // namespace ceyx

// Same export decoration as src/dng_ffi_api.cpp, so this entry survives any
// future visibility tightening on the dylib.
#if defined(_WIN32)
#define RAW_FFI_EXPORT __declspec(dllexport)
#else
#define RAW_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

// Round 2 Task 2.4 acceptance bullet: RawDecodeDiagnostics is explicitly NOT
// modified by this task (fixed Dart-visible layout, spec section 12). This
// pins its pre-change size so a future accidental edit anywhere in this
// header chain fails the build instead of silently reflowing the Dart FFI
// struct. Measured via native/scripts/tmp/round2_sizeof_probe.cpp before
// this assert was written (RC=0, see round2_syms.txt).
static_assert(sizeof(RawDecodeDiagnostics) == 64,
              "RawDecodeDiagnostics size changed -- this struct is Dart-visible "
              "and frozen for Round 2 Task 2.4 (spec section 12)");

namespace {
thread_local RawDecodeDiagnostics g_last_diagnostics{};
// Round 2 Task 2.4. Mirrors g_last_diagnostics's lifecycle: reset to a
// not-yet-decoded sentinel at translation-unit init, overwritten at the end
// of every decode that records diagnostics. g_have_color_diagnostics is the
// sentinel (rather than reusing RawDecodeDiagnostics::frontend the way
// raw_last_diagnostics does) because struct_size is always non-zero here by
// construction, so it cannot double as "nothing recorded yet".
thread_local RawColorDiagnostics g_last_color_diagnostics{};
thread_local bool g_have_color_diagnostics = false;

// C4 (plan §6.4/§6.7): mirrors g_last_color_diagnostics's lifecycle exactly,
// including the separate boolean sentinel -- struct_size is never zero on a
// recorded decode, so it cannot double as "nothing recorded yet" either.
thread_local RawTimingDiagnostics g_last_timing_diagnostics{};
thread_local bool g_have_timing_diagnostics = false;
}

extern "C" {

RAW_FFI_EXPORT int32_t raw_last_diagnostics(RawDecodeDiagnostics* out) {
    if (!out) return -1;
    if (g_last_diagnostics.frontend == kRawFrontendUnknown) return -1;
    *out = g_last_diagnostics;
    return 0;
}

RAW_FFI_EXPORT int32_t raw_last_color_diagnostics(RawColorDiagnostics* out) {
    if (!out) return -1;
    if (!g_have_color_diagnostics) return -1;
    *out = g_last_color_diagnostics;
    return 0;
}

RAW_FFI_EXPORT int32_t raw_last_timing_diagnostics(RawTimingDiagnostics* out) {
    if (!out) return -1;
    if (!g_have_timing_diagnostics) return -1;
    *out = g_last_timing_diagnostics;
    return 0;
}

// WP5: the SOLE writer of the thread-local diagnostics state below. It used
// to share that duty with the allocating RAW C ABI entry, which is deleted --
// so the decode-into path (ceyx_decode_into_ffi.cpp) is now the only producer,
// which is the intended end state, not an accident. Not RAW_FFI_EXPORT'd --
// internal, same-binary call only (see raw_ffi_api.h).
void raw_record_decode_into_diagnostics(
    const RawDecodeDiagnostics* diag,
    const RawColorPipelineDiagnostics* color_diag) {
    if (!diag) return;
    g_last_diagnostics = *diag;
    if (color_diag) {
        RawColorDiagnostics converted{};
        converted.struct_size = static_cast<uint32_t>(sizeof(RawColorDiagnostics));
        converted.auto_exposure_ev = color_diag->auto_exposure_ev;
        converted.auto_exposure_status = color_diag->auto_exposure_status;
        converted.vendor_curve_applied = color_diag->vendor_curve_applied;
        converted.matrix_route = color_diag->matrix_route;
        converted.clamped_mask = 0;
        std::snprintf(converted.reason, sizeof(converted.reason), "%s",
                      color_diag->auto_exposure_reason);
        g_last_color_diagnostics = converted;
        g_have_color_diagnostics = true;
    }
}

// C4 (plan §6.4/§6.7): sole writer of the timing thread-local state, called
// unconditionally (success or failure) from the decode-INTO entry point
// alongside raw_record_decode_into_diagnostics. Not RAW_FFI_EXPORT'd --
// internal, same-binary call only (see raw_ffi_api.h).
void raw_record_decode_timing_diagnostics(const RawTimingDiagnostics* timing) {
    if (!timing) return;
    RawTimingDiagnostics recorded = *timing;
    recorded.struct_size = static_cast<uint32_t>(sizeof(RawTimingDiagnostics));
    g_last_timing_diagnostics = recorded;
    g_have_timing_diagnostics = true;
}

// ---------------------------------------------------------------------------
// R3-T4 -- C2 zero-copy capability-gate probe (plan §4.5). Debug/probe
// surface only (see raw_ffi_api.h's contract comment): not Dart-visible,
// nothing added to DngResult. State fields report the CURRENT gate state
// (not a delta); the three counters are process-wide totals the gates read
// as deltas across decodes, exactly like the arena/cache probes above.
//
// Null-pointer convention (execution contract "Rulings during execution"):
// any out-pointer may be null and is then skipped; -1 only when all five
// are null.
// ---------------------------------------------------------------------------

RAW_FFI_EXPORT int32_t ceyx_debug_zero_copy_capability_counters(
    int32_t* out_zero_copy_path_is_enabled,
    int32_t* out_capability_override_state,
    uint64_t* out_destination_wrap_count,
    uint64_t* out_destination_alignment_degradation_count,
    uint64_t* out_source_mosaic_wrap_count) {
    if (!out_zero_copy_path_is_enabled && !out_capability_override_state &&
        !out_destination_wrap_count && !out_destination_alignment_degradation_count &&
        !out_source_mosaic_wrap_count) {
        return -1;
    }
    if (out_zero_copy_path_is_enabled || out_capability_override_state) {
        const ceyx::ZeroCopyCapabilityStateSnapshot snapshot =
            ceyx::zero_copy_capability_state_snapshot();
        if (out_zero_copy_path_is_enabled) {
            *out_zero_copy_path_is_enabled = snapshot.zero_copy_path_is_enabled ? 1 : 0;
        }
        if (out_capability_override_state) {
            *out_capability_override_state =
                static_cast<int32_t>(snapshot.capability_override_state);
        }
    }
    if (out_destination_wrap_count) {
        *out_destination_wrap_count = ceyx::zero_copy_destination_wrap_count();
    }
    if (out_destination_alignment_degradation_count) {
        *out_destination_alignment_degradation_count =
            ceyx::zero_copy_destination_alignment_degradation_count();
    }
    if (out_source_mosaic_wrap_count) {
        *out_source_mosaic_wrap_count = ceyx::zero_copy_source_mosaic_wrap_count();
    }
    return 0;
}

}  // extern "C"
