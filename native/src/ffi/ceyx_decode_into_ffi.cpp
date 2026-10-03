// Format-agnostic reusable-buffer entries. ALWAYS compiled: this TU is in
// NATIVE_SOURCES via the GLOB_RECURSE at cmake/pipeline.cmake:90 and is named
// in none of the EXCLUDE filters. The generic-RAW arm is guarded by
// DNG_ENABLE_GENERIC_RAW, which reaches this TU through libraw_vendored's
// INTERFACE definition (cmake/generic_raw.cmake, libraw_vendored target) — so in an
// OFF build the SYMBOLS still exist and a RAW input gets a specific error,
// rather than the Dart lookup finding nothing and the whole feature going
// silently missing.
#include "ceyx_decode_into.h"
#include "decode_into.h"  // T5b: body lives in src/pipeline/decode_into.cpp

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "ceyx_orient.h"
// mem8 v3 T12: the frozen output-format contract (CeyxOutputFormat, the plane
// descriptor, the format-taking entry declarations) and THE sizing arithmetic.
// Included UNCONDITIONALLY, unlike the DNG_ENABLE_GENERIC_RAW block below,
// because the format entries must EXIST in every build for the same reason
// this file's header records for its format-agnostic siblings: a symbol that
// is absent in some builds ships a feature that is silently missing, whereas a
// specific error code is diagnosable.
#include "ceyx_output_format_size.h"
#include "dng_error_codes.h"
#include "dng_ffi_api.h"
#include "dng_pipeline.h"
#include "dng_render_params.h"
#include "raw_ffi_api.h"
#include "raw_file_router.h"
#if defined(DNG_ENABLE_GENERIC_RAW)
#include "raw_gpu_pipeline.h"
#include "raw_persistent_device_arena.h"  // R3-T4: kRawDeviceArenaAlignmentBytes
#endif

#include "ceyx_ffi_export.h"
#if defined(_WIN32)
#include <malloc.h>  // _aligned_malloc / _aligned_free
#endif

#if defined(__APPLE__)
#include <malloc/malloc.h>  // malloc_zone_pressure_relief
#endif

#if defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>  // malloc_trim
#endif

namespace {
// R4 (gpu-copy-elimination campaign): the ONE physical alignment constant the
// pool allocator, the arena (raw_persistent_device_arena.h) and the
// ceyx::decodeIntoPrepare probe below all agree on. Kept as a private literal
// here (rather than including the arena header unconditionally) so this pair
// stays compiled in every build configuration, including
// DNG_ENABLE_GENERIC_RAW=OFF; the static_assert further down is what keeps it
// from drifting out of sync with the arena's own constant whenever that
// header IS available in this TU.
constexpr size_t kCeyxPoolAlignmentBytes = 16384;
}  // namespace

#if defined(DNG_ENABLE_GENERIC_RAW)
static_assert(kCeyxPoolAlignmentBytes == ceyx::kRawDeviceArenaAlignmentBytes,
              "the pool allocator's alignment must match the arena's and the "
              "ceyx::decodeIntoPrepare alignment probe's — one physical "
              "constant, checked here whenever this TU has visibility into "
              "the arena header");
#endif

extern "C" {

// R4: page-aligned allocator pair for CeyxNativeBufferPool's POOLED
// allocations (see ceyx_decode_into.h for the full contract). `byte_count` is
// rounded UP to a kCeyxPoolAlignmentBytes multiple so the returned capacity
// satisfies the alignment probe's capacity half as well as its pointer half.
CEYX_FFI_EXPORT void *ceyx_pool_aligned_alloc(size_t byte_count) {
  if (byte_count == 0) return nullptr;
  const size_t rounded =
      ((byte_count + kCeyxPoolAlignmentBytes - 1) / kCeyxPoolAlignmentBytes) *
      kCeyxPoolAlignmentBytes;
#if defined(_WIN32)
  return _aligned_malloc(rounded, kCeyxPoolAlignmentBytes);
#else
  void *ptr = nullptr;
  if (posix_memalign(&ptr, kCeyxPoolAlignmentBytes, rounded) != 0) {
    return nullptr;
  }
  return ptr;
#endif
}

CEYX_FFI_EXPORT void ceyx_pool_aligned_free(void *ptr) {
  if (!ptr) return;
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
}

}  // extern "C"

extern "C" {

CEYX_FFI_EXPORT int32_t ceyx_probe_output_size(const char *file_path,
                                               int32_t max_dim,
                                               int32_t *out_width,
                                               int32_t *out_height) {
  return ceyx::probeOutputSize(file_path, max_dim, out_width, out_height);
}

}  // extern "C"

// Blocker B-1 fix (round reviewer): §1.6 promises -402/-403 out of the fused
// oriented path, but every runner refusal site returns a lossy `bool`, so the
// caller only ever sees the generic code the pipeline layer already reports
// (e.g. kDngErrStage4Failed / kRawErrKernelFailed). dngRenderStage4LastFailureReason()
// (dng_render_params.h) is the per-call-thread-local channel the bridge owner
// added to recover the reason: valid to read ONLY directly after a runner/
// pipeline call returned failure, and always kNone after a success.
//
// CRITICAL: kNone on failure is LEGITIMATE (bad args, scratch allocation, a
// non-orientation SDK refusal) — the existing generic error_code must be kept
// as-is in that case. Mapping kNone to -402/-403 would re-introduce exactly
// the over-claiming bug this channel exists to prevent.
static void ceyxMapStage4FailureReason(DngResult *result) {
  if (result->error_code == 0) return;   // success: nothing to override
  switch (dngRenderStage4LastFailureReason()) {
    case Stage4FailureReason::kOverlap:
      result->error_code = kCeyxOrientErrOverlap;   // -402
      return;
    case Stage4FailureReason::kKernel:
      result->error_code = kCeyxOrientErrKernel;    // -403
      return;
    case Stage4FailureReason::kNone:
    default:
      return;   // keep the existing generic code
  }
}

// WP5 (user ruling R3): the AC-2.6 scratch-failure test hook and its flag are
// DELETED. The degradation arm they existed to exercise no longer exists -- the
// fused kernel writes oriented pixels straight into the caller's buffer, so no
// scratch is taken -- which left the flag written by its setter and read by
// nothing.

extern "C" {

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer(const char *file_path,
                                                   int32_t max_dim,
                                                   uint8_t *dst,
                                                   size_t dst_capacity) {
  return ceyx::decodeIntoBuffer(file_path, max_dim, dst, dst_capacity);
}

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer_oriented(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t exif_orientation) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  bool destination_is_page_aligned = false;
  if (!ceyx::decodeIntoPrepare(file_path, max_dim, dst, dst_capacity,
                             /*output_format=*/kCeyxOutputFormatRgba8, &route,
                             result, &destination_is_page_aligned)) {
    return result;
  }

  // Fused path (Task 9: the only path now, on every platform). The kernel
  // writes oriented pixels straight into the caller's buffer. No scratch, no
  // second pass, no degradation arm — the transposing in-place impossibility
  // that motivated them no longer exists.
  //
  // R-19 (named behaviour change, plan §3 Task 4): post-fusion there is no
  // successful-but-unoriented return value any more. Previously a scratch
  // shortage under memory pressure still produced a viewable photo
  // (unoriented, with the host rotating). Now an orientation failure IS a
  // decode failure — whatever the Stage4 kernel failure is (result->error_code
  // set by the pipeline, rgba_data left null). This is the accepted
  // consequence of D1, not an oversight.
  // B-2 fix (round reviewer, fix cycle 2): reset the reason to kNone
  // IMMEDIATELY before phase 3, not just rely on the runner resetting it on
  // its own next call. Without this reset here, a failure that never reaches
  // Stage4 at all — file-not-found, parse failure, OpcodeList2 failure, or
  // the RAW pre-runner dst-too-small backstop in makeRgbaCheckout — would
  // read whatever reason a PRIOR decode on this thread left behind and
  // clobber the real, correct error code with a stale -402/-403. The runner
  // itself resets at its own entry (dng_render_halide.cpp), which is enough
  // ONLY when the runner actually runs; this call may return failure without
  // ever reaching it.
  dngRenderStage4ResetFailureReason();
  ceyx::decodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       exif_orientation, destination_is_page_aligned,
                       /*output_format=*/kCeyxOutputFormatRgba8, result);
  if (result->error_code != 0) {
    // The read below is honest ONLY because of the reset immediately above:
    // many phase-3 failures (bad file, parse, OpcodeList2, the RAW
    // pre-runner capacity backstop) return without Stage4 ever executing, so
    // the runner's own reset-on-entry never fires on this call. Without the
    // explicit reset here this read could observe a STALE reason left by an
    // earlier decode on this thread and overwrite a correct generic error
    // (e.g. kCeyxErrDstTooSmall) with a wrong -402/-403 (B-2).
    ceyxMapStage4FailureReason(result);
    return result;
  }
  result->rgba_data = dst;   // pipeline already reported the ORIENTED extent
  return result;
}

// ===========================================================================
// mem8 v3 T12 milestone 3 — the FORMAT-TAKING entries of the frozen T12.0
// contract (raw_ffi_api.h). Additive siblings of the three above, which are
// UNCHANGED and remain rgba8.
//
// They live beside those siblings rather than in the FFI file the plan's file
// table guessed at, because the value of the extraction above is that the
// route probe, the extent probe and the capacity refusal exist ONCE. A second
// copy of that sequence in another translation unit is precisely the drift the
// comment on ceyx::decodeIntoPrepare exists to prevent -- so the format entries
// call the SAME two helpers, with the format as one more argument.
// ===========================================================================

// Fills the caller's plane descriptor for a SUCCESSFUL decode. Zeroed for
// rgba8 (there are no planes), which is the contract's own wording.
//
// struct_size is the caller's declared sizeof. It is honoured rather than
// assumed: a caller built against a future, larger descriptor must not have
// this build write past what it allocated, and a caller built against a
// smaller one must not be handed fields it has no room for. A mismatch is not
// an error here -- the fields this build knows are written only if they fit.
static void ceyxFillYuv420PlaneDescriptor(CeyxYuv420PlaneDescriptor *out_planes,
                                          int32_t output_format, uint8_t *dst,
                                          int32_t width, int32_t height) {
  if (!out_planes) return;
  const uint32_t declared = out_planes->struct_size;
  if (declared < sizeof(CeyxYuv420PlaneDescriptor)) return;
  *out_planes = CeyxYuv420PlaneDescriptor{};
  out_planes->struct_size = declared;
  if (output_format != kCeyxOutputFormatYuv420) return;  // rgba8: stays zeroed

  // The frozen layout, spelled from the same oracle the kernel and the sizing
  // function use. Nothing here re-derives ceil(w/2).
  const int32_t chroma_w = ceyx::yuv420::chroma_extent(width);
  const int32_t chroma_h = ceyx::yuv420::chroma_extent(height);
  const size_t luma_bytes = static_cast<size_t>(width) * height;
  const size_t chroma_bytes = static_cast<size_t>(chroma_w) * chroma_h;

  out_planes->plane_base[0] = dst;
  out_planes->plane_base[1] = dst + luma_bytes;
  out_planes->plane_base[2] = dst + luma_bytes + chroma_bytes;
  out_planes->plane_width[0] = width;
  out_planes->plane_width[1] = chroma_w;
  out_planes->plane_width[2] = chroma_w;
  out_planes->plane_height[0] = height;
  out_planes->plane_height[1] = chroma_h;
  out_planes->plane_height[2] = chroma_h;
  // Row stride EQUALS plane width, by contract -- not by observation of what
  // Halide happened to produce. The kernel asserts the same packing on its own
  // side (dim(0).set_stride(1)), so the two agree by construction.
  for (int i = 0; i < 3; ++i) {
    out_planes->plane_row_stride[i] = out_planes->plane_width[i];
  }
}

CEYX_FFI_EXPORT int32_t ceyx_probe_output_size_format(
    const char *file_path, int32_t max_dim, int32_t output_format,
    int32_t *out_width, int32_t *out_height, int64_t *out_byte_count) {
  if (out_byte_count) *out_byte_count = 0;
  const int32_t rc =
      ceyx_probe_output_size(file_path, max_dim, out_width, out_height);
  if (rc != 0) return rc;
  // By construction this is exactly
  // ceyx_output_format_byte_count(output_format, *out_width, *out_height) --
  // the contract's wording -- because it IS that call.
  const int64_t bytes = ceyx::output_format_byte_count(
      output_format, *out_width, *out_height);
  if (bytes < 0) return kCeyxErrFormatUnsupportedInBuild;
  if (out_byte_count) *out_byte_count = bytes;
  return 0;
}

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer_format(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t output_format, CeyxYuv420PlaneDescriptor *out_planes) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  bool destination_is_page_aligned = false;
  if (!ceyx::decodeIntoPrepare(file_path, max_dim, dst, dst_capacity,
                             output_format, &route, result,
                             &destination_is_page_aligned)) {
    return result;
  }
  ceyx::decodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       /*exif_orientation=*/1, destination_is_page_aligned,
                       output_format, result);
  // Descriptor only on SUCCESS: a failed decode wrote no planes, and handing
  // back plane pointers into a buffer nothing filled invites the caller to
  // read uninitialised memory as an image.
  if (result->error_code == 0) {
    ceyxFillYuv420PlaneDescriptor(out_planes, output_format, dst,
                                  result->width, result->height);
  }
  return result;
}

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer_oriented_format(
    const char *file_path, int32_t max_dim, uint8_t *dst, size_t dst_capacity,
    int32_t exif_orientation, int32_t output_format,
    CeyxYuv420PlaneDescriptor *out_planes) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  bool destination_is_page_aligned = false;
  if (!ceyx::decodeIntoPrepare(file_path, max_dim, dst, dst_capacity,
                             output_format, &route, result,
                             &destination_is_page_aligned)) {
    return result;
  }
  // Same B-2 reset as the rgba8 oriented entry above, for the same reason: a
  // phase-3 failure that never reaches Stage-4 must not read a stale reason.
  dngRenderStage4ResetFailureReason();
  ceyx::decodeIntoPhase3(file_path, max_dim, route, dst, dst_capacity,
                       exif_orientation, destination_is_page_aligned,
                       output_format, result);
  if (result->error_code != 0) {
    ceyxMapStage4FailureReason(result);
    return result;
  }
  result->rgba_data = dst;
  // result->width/height are the ORIENTED extent here, which is what the
  // contract says the plane extents describe -- so the descriptor is built
  // from them and never from the unoriented probe.
  ceyxFillYuv420PlaneDescriptor(out_planes, output_format, dst, result->width,
                                result->height);
  return result;
}

}  // extern "C"

// Deliberate absence, recorded so it is not "fixed" later: there is NO
// failure-path release in this file, and there is nothing left that could need
// one. `dst` is the CALLER's buffer on every path, and WP5 deleted the RGBA
// output pool, so there is no pool to release anything to and no library-owned
// buffer to give back. Freeing dst here would corrupt a Dart-owned address.
//
// This paragraph used to carry an exception: the oriented entry's transposing
// arm checked a SCRATCH frame out of that pool and did release it. Both the
// scratch checkout and the pool are gone -- the fused kernel writes the
// oriented pixels, transposing included, directly into dst -- so the rule is
// now unqualified.
