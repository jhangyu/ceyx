// Generic RAW C ABI. Reuses the FROZEN DngResult layout, so Dart bindings need
// no struct change (spec section 12.2). error_code carries a RawErrorCode,
// whose values (<= -201) are disjoint from DngErrorCode.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ceyx_decode_into.h"        // kCeyxErrDstTooSmall (the shared code)
#include "ceyx_output_format_size.h"
#include "dng_pipeline.h"
#include "dng_render_params.h"       // T12 Y5: the Stage-4 scratch accessors
#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"

// C2 zero-copy capability gate accessors (plan §4.1.4), defined by
// impl-capability-opus in dng_metal_context.h/.cpp -- included for the
// enabled/override half of ceyx_debug_zero_copy_capability_counters below.
#include "dng_metal_context.h"

// R4-T4 nit N1: the three C2 wrap/degradation counters (plan §4.5) are
// already declared by the dng_metal_context.h include above -- this file
// used to re-declare them here too, which is a silent-drift surface (any
// future rename in dng_metal_context.h would not fail this translation unit
// until link time). Removed; the include is the single declaration point.
// Names and namespace remain FROZEN by plan §4.5.

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
    // R4-T4 B2 fix (round-4 review): honour out->struct_size instead of a
    // blind `*out = ...` -- a caller built against an older, smaller
    // RawTimingDiagnostics (e.g. before source_mosaic_copy_milliseconds was
    // appended) sets out->struct_size to ITS sizeof before calling; writing
    // this binary's full (larger) struct into that smaller caller-owned
    // buffer would overrun it. Copying only min(caller's struct_size, this
    // binary's sizeof) is what makes the "struct_size-versioned" framing in
    // the header comment actually true for this cross-boundary getter, and
    // it must be done at raw byte granularity (not struct assignment) since
    // the copied length can now be smaller than sizeof(RawTimingDiagnostics).
    // Round-4 parking-lot P-5 follow-up: struct_size is a caller-supplied
    // INPUT (header comment above raw_last_timing_diagnostics), not merely
    // an output field. A caller that zero-initialises `out` and forgets to
    // set struct_size passes 0 here; without this guard that yields
    // copy_bytes == 0 below (an all-zero struct silently returned with
    // RC == 0), which is exactly the "caller bug, not a valid signal" case
    // the header already documents but this function did not previously
    // enforce. Anything smaller than sizeof(uint32_t) can't even hold the
    // struct_size field this function writes back on success, so it is
    // rejected the same way.
    const uint32_t caller_struct_size = out->struct_size;
    if (caller_struct_size < sizeof(uint32_t)) return -1;
    const size_t copy_bytes = std::min<size_t>(
        caller_struct_size, sizeof(RawTimingDiagnostics));
    std::memcpy(out, &g_last_timing_diagnostics, copy_bytes);
    out->struct_size = static_cast<uint32_t>(sizeof(RawTimingDiagnostics));
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

// ---------------------------------------------------------------------------
// mem8 v3 T12 (Y5) — the Stage-4 RGBA8 destination-scratch probe. A forward to
// the pipeline's accessors; the accounting itself lives at the sole writer.
// ---------------------------------------------------------------------------
RAW_FFI_EXPORT int32_t ceyx_debug_stage4_rgba_scratch_counters(
    uint64_t* out_last_decode_bytes, uint64_t* out_high_water_bytes) {
    if (!out_last_decode_bytes && !out_high_water_bytes) return -1;
    if (out_last_decode_bytes) {
        *out_last_decode_bytes = runRenderStage4LastRgbaScratchBytes();
    }
    if (out_high_water_bytes) {
        *out_high_water_bytes = runRenderStage4RgbaScratchHighWaterBytes();
    }
    return 0;
}

// ---------------------------------------------------------------------------
// mem8 v3 T12 — the FROZEN contract's sizing function (raw_ffi_api.h, T12.0
// clause 2b). A one-line forward on purpose: the arithmetic itself is inline in
// ceyx_output_format_size.h so the pipeline's internal sizing (Stage-4's
// destination views, the RGBA checkout in raw_gpu_pipeline.cpp) and this
// exported entry cannot drift apart. Chroma extents come from the oracle's
// chroma_extent (ceil, not truncation) — the odd-dimension case is exactly
// where a naive w/2 under-allocates, which is a heap overrun, not a miscount.
// ---------------------------------------------------------------------------
RAW_FFI_EXPORT int64_t ceyx_output_format_byte_count(int32_t output_format,
                                                      int32_t width,
                                                      int32_t height) {
    return ceyx::output_format_byte_count(output_format, width, height);
}

// ---------------------------------------------------------------------------
// mem8 v3 T12 milestone 3 / SR-11 — THE yuv420 -> RGBA8 upconvert. Dart must
// never open-code one, which is the whole reason this is an export.
//
// Every coefficient and every rounding term comes from ceyx_yuv420_oracle.h,
// whose inverse half is transcribed from the libjpeg-turbo vendored on this
// tree (jdcolor.c:236-250 + jdcolext.c:61-65, provenance in the header). Not
// one numeric literal appears below: if a coefficient is ever wrong it is
// wrong in ONE place, for the encoder, the two kernels and this converter
// alike.
//
// CHROMA UPSAMPLING IS NEAREST-NEIGHBOUR (plane_index >> 1), the box filter's
// inverse. libjpeg's decoder offers a fancy triangular upsample too, but the
// contract this implements is the 2x2 box average's counterpart -- a smarter
// upsampler here would make a decode->upconvert round trip disagree with the
// oracle by more than rounding, which is exactly the bound Y4/Y7 assert on.
// ---------------------------------------------------------------------------
RAW_FFI_EXPORT int32_t ceyx_yuv420_to_rgba8(const uint8_t *src,
                                             size_t src_capacity, uint8_t *dst,
                                             size_t dst_capacity, int32_t width,
                                             int32_t height) {
    if (!src || !dst || width <= 0 || height <= 0) return -1;
    const int64_t need_src = ceyx::output_format_byte_count(1, width, height);
    const int64_t need_dst = ceyx::output_format_byte_count(0, width, height);
    if (need_src < 0 || need_dst < 0) return -1;
    // The SOURCE shortfall is a null/extent-class argument error (-1), not
    // kCeyxErrDstTooSmall: that code names a DESTINATION that cannot hold the
    // result, and reporting it for a truncated input would send the caller off
    // to grow the wrong buffer.
    if (src_capacity < static_cast<size_t>(need_src)) return -1;
    if (dst_capacity < static_cast<size_t>(need_dst)) return kCeyxErrDstTooSmall;

    const int32_t chroma_w = ceyx::yuv420::chroma_extent(width);
    const int32_t chroma_h = ceyx::yuv420::chroma_extent(height);
    const uint8_t *y_plane = src;
    const uint8_t *cb_plane = y_plane + static_cast<size_t>(width) * height;
    const uint8_t *cr_plane =
        cb_plane + static_cast<size_t>(chroma_w) * chroma_h;

    for (int32_t y = 0; y < height; ++y) {
        const int32_t chroma_row = y >> 1;   // never out of range: ceil(h/2)
        const uint8_t *y_row = y_plane + static_cast<size_t>(y) * width;
        const uint8_t *cb_row =
            cb_plane + static_cast<size_t>(chroma_row) * chroma_w;
        const uint8_t *cr_row =
            cr_plane + static_cast<size_t>(chroma_row) * chroma_w;
        uint8_t *out_row = dst + static_cast<size_t>(y) * width * 4;
        for (int32_t x = 0; x < width; ++x) {
            const int32_t chroma_col = x >> 1;
            uint8_t r = 0, g = 0, b = 0;
            ceyx::yuv420::rgb_from_ycbcr(y_row[x], cb_row[chroma_col],
                                          cr_row[chroma_col], &r, &g, &b);
            out_row[x * 4 + 0] = r;
            out_row[x * 4 + 1] = g;
            out_row[x * 4 + 2] = b;
            out_row[x * 4 + 3] = 255;
        }
    }
    return 0;
}

}  // extern "C"
