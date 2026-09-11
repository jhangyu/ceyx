#pragma once

// Format-agnostic reusable-buffer decode entries (WP10, AMENDMENT 3).
//
// ONE entry pair covers every route. The caller does not know, and does not
// need to know, whether a path is a DNG or a generic RAW: ceyx_probe_output_size
// and ceyx_decode_into_buffer route internally on raw_probe_file.
//
// WHY THESE ARE ALWAYS PRESENT. The translation unit that defines them
// (src/ffi/ceyx_decode_into_ffi.cpp) is always compiled; only the generic-RAW
// ARM inside it is guarded by DNG_ENABLE_GENERIC_RAW. A build with generic RAW
// compiled out therefore still EXPORTS both symbols, and a RAW input to that
// build gets kCeyxErrFormatUnsupportedInBuild. That is deliberately better than
// omitting the symbols: a Dart guarded lookup that finds nothing ships a
// feature which is silently absent and green everywhere, whereas a specific
// error code is diagnosable.

#include <stddef.h>
#include <stdint.h>

#include "dng_ffi_api.h"  // DngResult, dng_free_result

#ifdef __cplusplus
extern "C" {
#endif

/// Codes for the format-agnostic layer. Deliberately disjoint from BOTH the
/// DNG scale (0..-101, dng_error_codes.h) and the RAW scale (-201..-212,
/// raw_pipeline_contract.h:112-125), so a Dart caller can tell which layer
/// spoke without knowing which route the file took.
enum CeyxDecodeIntoError {
  kCeyxErrDstTooSmall              = -301,
  kCeyxErrFormatUnsupportedInBuild = -302,
  kCeyxErrProbeFailed              = -303,
};

/// Format-agnostic. Probes the OUTPUT extent of a decode WITHOUT decoding.
/// Routes on raw_probe_file: DNG inputs take the DNG metadata probe (parse,
/// no ReadStage1Image); generic-RAW inputs take the LibRaw metadata probe
/// (open_file, no unpack). Both skip the expensive step by construction.
/// Byte requirement for the matching decode is (*out_width)*(*out_height)*4.
/// Returns 0, or a negative code: kCeyxErrDstTooSmall is never returned here;
/// kCeyxErrFormatUnsupportedInBuild means this build has no decoder for that
/// format (generic RAW compiled out).
int32_t ceyx_probe_output_size(const char *file_path, int32_t max_dim,
                               int32_t *out_width, int32_t *out_height);

/// Format-agnostic decode into a CALLER-OWNED buffer.
/// OWNERSHIP: dst is never freed and never retained by the library.
/// On success result->rgba_data == dst (pointer identity is the contract).
/// WP5: the caller no longer needs to clear result->rgba_data before
/// dng_free_result() -- that call frees ONLY the struct now. Clearing it first
/// remains harmless.
/// dst NULL / capacity 0 / capacity < w*h*4 -> kCeyxErrDstTooSmall with
/// width and height FILLED IN and rgba_data NULL, before any pixel work.
/// Returns a heap-allocated DngResult; free with dng_free_result().
DngResult *ceyx_decode_into_buffer(const char *file_path, int32_t max_dim,
                                   uint8_t *dst, size_t dst_capacity);

/// Orientation-aware sibling of ceyx_decode_into_buffer. Identical contract
/// EXCEPT: the pixels written to dst have EXIF `exif_orientation` already
/// applied, and result->width/height are the ORIENTED extent.
/// exif_orientation outside 1..8 is treated as 1 (identity), matching the
/// host's own table; it is never an error.
/// ADDITIVE: older binaries lack this symbol. Callers MUST resolve it
/// defensively and fall back to ceyx_decode_into_buffer + host-side rotation.
///
/// ORIENTATION FAILURE IS DECODE FAILURE. This entry either returns the
/// correctly ORIENTED extent or it fails outright; it never falls back to an
/// unoriented render. On failure the returned DngResult carries one of the
/// orientation error codes (kCeyxOrientErrBadArgs = -401, kCeyxOrientErrOverlap
/// = -402, kCeyxOrientErrKernel = -403; see ceyx_orient.h) and
/// result->width/height are zeroed. The fused kernel writes the oriented
/// pixels (transposing included) straight into dst, so no scratch frame is
/// taken at all.
DngResult *ceyx_decode_into_buffer_oriented(const char *file_path,
                                            int32_t max_dim, uint8_t *dst,
                                            size_t dst_capacity,
                                            int32_t exif_orientation);

/// WP5 (user ruling R3): the AC-2.6 scratch-failure test hook
/// (ceyx_debug_force_scratch_failure) is now INERT. There is no scratch
/// checkout and no degradation arm left for it to force a failure against;
/// its flag has zero readers. The symbol is still exported for ABI stability
/// only -- calling it is a no-op.

/// R4 (gpu-copy-elimination campaign, Round 4): a page-aligned allocator pair
/// for the Dart-side pooled RGBA buffers. `package:ffi`'s `malloc` has no
/// aligned form, and the C2 zero-copy wrap
/// (`ceyxDecodeIntoPrepare`'s `out_destination_is_page_aligned` probe, this
/// TU's .cpp) only ever answers true for a destination whose pointer AND
/// capacity are BOTH multiples of the same page-size constant the arena uses
/// (`kRawDeviceArenaAlignmentBytes` = 16384, `raw_persistent_device_arena.h`).
/// Every buffer `CeyxNativeBufferPool` hands out as a POOLED allocation is
/// meant to come from `ceyx_pool_aligned_alloc` and go back through
/// `ceyx_pool_aligned_free` -- never `malloc`/`free`, since the two allocator
/// families are not interchangeable (`posix_memalign`/`_aligned_malloc`
/// memory must be freed by `free`/`_aligned_free`, not by a mismatched
/// deallocator). Unpooled/oversize/adopted buffers are UNCHANGED by this pair
/// -- they keep using ordinary `malloc`, because an unaligned adopted pointer
/// is legal input to the alignment probe (it just answers false).
///
/// ALWAYS COMPILED (same TU as the rest of this header, no
/// DNG_ENABLE_GENERIC_RAW guard): the alignment constant is a fixed number,
/// not a generic-RAW-only concept, so these two symbols exist in every build
/// configuration.
///
/// `ceyx_pool_aligned_alloc` rounds `byte_count` UP to the next 16384-byte
/// multiple before allocating, so the returned capacity is itself a multiple
/// of 16384 (the second half of the alignment contract). Returns NULL on
/// failure or when `byte_count == 0`.
void *ceyx_pool_aligned_alloc(size_t byte_count);

/// Frees memory obtained from `ceyx_pool_aligned_alloc`. A NULL pointer is a
/// no-op. MUST NOT be called on memory obtained from `malloc`/`calloc`, and
/// `ceyx_pool_aligned_alloc` memory MUST NOT be freed with `free`/`malloc.free`
/// -- the two allocators are platform-specific and not interchangeable
/// (`_aligned_malloc`/`_aligned_free` on Windows, `posix_memalign`/`free`
/// elsewhere).
void ceyx_pool_aligned_free(void *ptr);

#ifdef __cplusplus
}
#endif
