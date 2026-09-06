// Format-agnostic reusable-buffer entries. ALWAYS compiled: this TU is in
// NATIVE_SOURCES via the GLOB_RECURSE at cmake/pipeline.cmake:90 and is named
// in none of the EXCLUDE filters. The generic-RAW arm is guarded by
// DNG_ENABLE_GENERIC_RAW, which reaches this TU through libraw_vendored's
// INTERFACE definition (cmake/tests.cmake:1372, linked at :1381) — so in an
// OFF build the SYMBOLS still exist and a RAW input gets a specific error,
// rather than the Dart lookup finding nothing and the whole feature going
// silently missing.
#include "ceyx_decode_into.h"

#include <cstdlib>

#include "dng_error_codes.h"
#include "dng_ffi_api.h"
#include "dng_pipeline.h"
#include "raw_file_router.h"
#if defined(DNG_ENABLE_GENERIC_RAW)
#include "raw_gpu_pipeline.h"
#endif

#if defined(_WIN32)
#define CEYX_FFI_EXPORT __declspec(dllexport)
#else
#define CEYX_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

extern "C" {

CEYX_FFI_EXPORT int32_t ceyx_probe_output_size(const char *file_path,
                                               int32_t max_dim,
                                               int32_t *out_width,
                                               int32_t *out_height) {
  // Zero the out-params FIRST, so every failure return below leaves them 0
  // without each path having to remember to.
  if (out_width) *out_width = 0;
  if (out_height) *out_height = 0;
  if (!out_width || !out_height) return kDngErrNullPath;

  RawRoute route = kRawRouteUnknown;
  const RawErrorCode prc = raw_probe_file(file_path, &route);
  if (prc != kRawSuccess) return static_cast<int32_t>(prc);

  if (route == kRawRouteDng) {
    DngPipelineResult probe;
    if (!dng_pipeline_probe_output_size(file_path, max_dim, probe)) {
      return probe.error_code != 0 ? probe.error_code : kCeyxErrProbeFailed;
    }
    *out_width = static_cast<int32_t>(probe.width);
    *out_height = static_cast<int32_t>(probe.height);
    return 0;
  }

#if defined(DNG_ENABLE_GENERIC_RAW)
  uint32_t w = 0, h = 0;
  const RawErrorCode rc = raw_pipeline_probe_output_size(
      file_path, max_dim > 0 ? static_cast<uint32_t>(max_dim) : 0u, &w, &h);
  if (rc != kRawSuccess) return static_cast<int32_t>(rc);
  *out_width = static_cast<int32_t>(w);
  *out_height = static_cast<int32_t>(h);
  return 0;
#else
  return kCeyxErrFormatUnsupportedInBuild;
#endif
}

CEYX_FFI_EXPORT DngResult *ceyx_decode_into_buffer(const char *file_path,
                                                   int32_t max_dim,
                                                   uint8_t *dst,
                                                   size_t dst_capacity) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result) return nullptr;

  RawRoute route = kRawRouteUnknown;
  const RawErrorCode prc = raw_probe_file(file_path, &route);
  if (prc != kRawSuccess) {
    result->error_code = static_cast<int32_t>(prc);
    return result;
  }

  // Phase 1: metadata-only extent, on whichever route owns this file. Neither
  // probe touches pixels, so a stale caller-side prediction is refused below
  // for the price of a header parse.
  int32_t w = 0, h = 0;
  const int32_t probe_rc = ceyx_probe_output_size(file_path, max_dim, &w, &h);
  if (probe_rc != 0) {
    result->error_code = probe_rc;
    return result;
  }
  result->width = w;
  result->height = h;

  // Phase 2: the capacity decision, before any pixel work on either route.
  // The extent stays filled in on refusal: that is what lets the caller
  // re-acquire an EXACT slot and retry once, rather than guessing again.
  const size_t need = static_cast<size_t>(w) * h * 4;
  if (!dst || dst_capacity < need) {
    result->error_code = kCeyxErrDstTooSmall;
    return result;
  }

  // Phase 3: decode through the route's caller-buffer sibling, each of which
  // binds dst AFTER its own internal result reset (A3.2).
  if (route == kRawRouteDng) {
    DngPipelineResult pipeline;
    if (!dng_pipeline_decode_to_rgb_into(file_path, max_dim, dst, dst_capacity,
                                         pipeline)) {
      result->error_code = pipeline.error_code;
      result->decode_ms = pipeline.decode_ms;
      result->process_ms = pipeline.process_ms;
      return result;
    }
    if (!pipeline.rgba_ptr) {
      // fuse_rgba_output=false leaves RGB8 in the buffer, which is not the
      // layout this entry advertises. Refuse loudly rather than hand back
      // pixels in a shape the caller will misread.
      result->error_code = kDngErrRgbaAllocFailed;
      return result;
    }
    result->rgba_data = pipeline.rgba_ptr;    // == dst, by construction
    result->width = static_cast<int32_t>(pipeline.width);
    result->height = static_cast<int32_t>(pipeline.height);
    result->decode_ms = pipeline.decode_ms;
    result->process_ms = pipeline.process_ms;
    return result;
  }

#if defined(DNG_ENABLE_GENERIC_RAW)
  RawDevelopParams develop{};
  develop.exposure_ev = 0.0f;
  develop.tone_curve_strength = 1.0f;
  develop.output_space = kRawOutputColorSpaceSrgb;
  develop.max_output_long_edge =
      max_dim > 0 ? static_cast<uint32_t>(max_dim) : 0u;

  RawPipelineResult out;
  const RawErrorCode rc =
      raw_pipeline_decode_file_into(file_path, develop, dst, dst_capacity, out);
  result->decode_ms = out.diag.raw_unpack_ms;
  result->process_ms = out.diag.gpu_process_ms;
  if (rc != kRawSuccess) {
    // WP10 boundary map (lead ruling, 2026-09-06). kRawErrDstTooSmall (-213) is
    // INTERNAL: it is what makeRgbaCheckout's POST-UNPACK capacity backstop
    // returns. That backstop is a genuinely different event from the pre-decode
    // refusal above — the pre-check runs before either pipeline is entered,
    // whereas some formats only reveal their true extent during unpack (the
    // X3F/Foveon raw_pitch case, libraw_frontend.cpp:238-243), so the shortfall
    // cannot be known until after. Keeping -213 internal lets the backstop name
    // its own failure precisely instead of borrowing a wrong code; mapping it
    // here keeps A3.4's "one too-small code for one entry pair" true of the
    // PUBLIC surface, which is what Dart sees.
    // width/height are already filled in from the probe above, so the ruling's
    // "with width/height filled in" condition holds on this path too.
    result->error_code = (rc == kRawErrDstTooSmall)
                             ? static_cast<int32_t>(kCeyxErrDstTooSmall)
                             : static_cast<int32_t>(rc);
    // WP10 stale-extent fix. The probe's extent was written above, but on a
    // post-unpack refusal it is the extent that JUST PROVED INSUFFICIENT —
    // handing it back makes the caller re-acquire the same failing size, so the
    // retry can never advance and a bounded retry becomes "photo will not open".
    // The pipeline now publishes the TRUE post-unpack extent before its early
    // return (makeRgbaCheckout), so prefer it whenever it is populated.
    if (out.width > 0) {
      if (out.height > 0) {
        result->width = static_cast<int32_t>(out.width);
        result->height = static_cast<int32_t>(out.height);
      }
    }
    return result;
  }
  result->rgba_data = out.rgba_ptr;           // == dst, by construction
  result->width = static_cast<int32_t>(out.width);
  result->height = static_cast<int32_t>(out.height);
  return result;
#else
  result->error_code = kCeyxErrFormatUnsupportedInBuild;
  return result;
#endif
}

}  // extern "C"

// Deliberate absence, recorded so it is not "fixed" later: there is NO
// failure-path release here. raw_decode_and_process has an
// `else if (out.rgba_ptr) { dng_rgba_output_release(...) }` arm
// (raw_ffi_api.cpp:84-87) because ITS buffer is pool-owned. With a caller
// buffer there is nothing pool-owned to give back, and releasing dst into the
// pool would hand a Dart-owned address to the next decode — the exact
// corruption the borrowing guards exist to prevent, and one the pool would
// absorb silently (RgbaOutputPool::release logs unknown pointers as a no-op).
