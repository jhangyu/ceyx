// The still-decode C ABI: one surface, internal routing. heif_* stays exported
// and unchanged forever (heif_api.h); this is purely additive.

#include "ceyx_still_api.h"
#include "still_codec_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#if DNG_ENABLE_HEIF
#include "heif_api.h"
#endif

#if defined(_WIN32)
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

// The HEIC/AVIF arms call into heif_encode.cpp (plan Task 7). Until that TU is
// in the tree, cmake/encode.cmake leaves CEYX_HAS_HEIF_STILL_DECODE at 0 and
// the arms return "unsupported" rather than producing an undefined symbol at
// dylib link time. The definition is computed from the file's existence, so it
// flips the moment Task 7 lands and the build is reconfigured.
#ifndef CEYX_HAS_HEIF_STILL_DECODE
#define CEYX_HAS_HEIF_STILL_DECODE 0
#endif
#define CEYX_HEIF_STILL_ROUTE (DNG_ENABLE_HEIF && CEYX_HAS_HEIF_STILL_DECODE)

// JPEG XL (plan Task 9) needs no local stub here: src/jxl_codec.cpp defines
// ceyx_jxl_probe_impl/ceyx_jxl_decode_impl unconditionally and answers
// "unsupported" internally (#if !CEYX_ENABLE_JXL) when the dist is absent,
// per the absence-degrades rule -- the dispatch arms below call straight
// through to it in every configuration.

namespace {

// Sniff by MAGIC BYTES, never by extension: Halcyon routes by extension but
// passes kCeyxFormatUnknown, so a mislabelled file must still work.
int32_t SniffFormat(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (!f) return kCeyxFormatUnknown;
  uint8_t hdr[32] = {0};
  const size_t n = std::fread(hdr, 1, sizeof(hdr), f);
  std::fclose(f);
  if (n < 12) return kCeyxFormatUnknown;

  if (hdr[0] == 0xFF && hdr[1] == 0xD8 && hdr[2] == 0xFF) return kCeyxFormatJpeg;
  if (!std::memcmp(hdr, "RIFF", 4) && !std::memcmp(hdr + 8, "WEBP", 4)) {
    return kCeyxFormatWebp;
  }
  // JPEG XL: the bare codestream signature and the ISO-BMFF container variant.
  if (hdr[0] == 0xFF && hdr[1] == 0x0A) return kCeyxFormatJxl;
  if (!std::memcmp(hdr, "\x00\x00\x00\x0C\x4A\x58\x4C\x20\x0D\x0A\x87\x0A", 12)) {
    return kCeyxFormatJxl;
  }
  // ISO-BMFF: bytes 4..8 are "ftyp"; the brand at 8..12 separates AVIF from
  // the HEIC family. Both route to libheif, so the distinction is only for the
  // capability check.
  if (!std::memcmp(hdr + 4, "ftyp", 4)) {
    if (!std::memcmp(hdr + 8, "avif", 4) || !std::memcmp(hdr + 8, "avis", 4)) {
      return kCeyxFormatAvif;
    }
    return kCeyxFormatHeic;
  }
  return kCeyxFormatUnknown;
}

}  // namespace

extern "C" {

FFI_EXPORT const char *ceyx_still_error_name(int32_t code) {
  switch (code) {
    case kCeyxStillSuccess: return "kCeyxStillSuccess";
    case kCeyxStillErrNullPath: return "kCeyxStillErrNullPath";
    case kCeyxStillErrOpenFailed: return "kCeyxStillErrOpenFailed";
    case kCeyxStillErrBadFormat: return "kCeyxStillErrBadFormat";
    case kCeyxStillErrUnsupported: return "kCeyxStillErrUnsupported";
    case kCeyxStillErrNoPrimaryItem: return "kCeyxStillErrNoPrimaryItem";
    case kCeyxStillErrDecodeFailed: return "kCeyxStillErrDecodeFailed";
    case kCeyxStillErrColorConversion: return "kCeyxStillErrColorConversion";
    case kCeyxStillErrAllocationFailed: return "kCeyxStillErrAllocationFailed";
    case kCeyxStillErrSizeOverflow: return "kCeyxStillErrSizeOverflow";
    case kCeyxStillErrMetadataInvalid: return "kCeyxStillErrMetadataInvalid";
    case kCeyxStillErrUnknownException: return "kCeyxStillErrUnknownException";
    default: return "kCeyxStillErrUnknown";
  }
}

FFI_EXPORT int32_t ceyx_still_decode_supports(int32_t format) {
  switch (format) {
    case kCeyxFormatWebp: return CEYX_ENABLE_WEBP ? 1 : 0;
    case kCeyxFormatHeic:
    case kCeyxFormatAvif: return CEYX_HEIF_STILL_ROUTE ? 1 : 0;
    case kCeyxFormatJxl:  return CEYX_ENABLE_JXL ? 1 : 0;
    // JPEG: 0 ON PURPOSE, deviating from the plan's literal `return 1`. This
    // surface has no libjpeg decode arm -- a JPEG path handed to
    // ceyx_still_decode_rgba returns kCeyxStillErrOpenFailed -- and this query
    // is the load-bearing truth source for the runtime-capability model (user
    // ruling Q4). Answering 1 while the decode fails is exactly the lie that
    // would poison the Dart side's capability mapping. Halcyon imports JPEG
    // through the Flutter engine, so nothing calls this arm today; if a libjpeg
    // arm is ever added below, flip this back to 1 in the same change.
    case kCeyxFormatJpeg: return 0;
    default: return kCeyxStillErrBadFormat;
  }
}

FFI_EXPORT int32_t ceyx_still_probe(const char *path, int32_t format_hint,
                                    uint32_t *width, uint32_t *height,
                                    int32_t *orientation) {
  if (!path || !*path || !width || !height || !orientation) {
    return kCeyxStillErrNullPath;
  }
  if (format_hint < kCeyxFormatUnknown || format_hint > kCeyxFormatJxl) {
    return kCeyxStillErrBadFormat;
  }
  try {
    const int32_t fmt =
        (format_hint == kCeyxFormatUnknown) ? SniffFormat(path) : format_hint;
    uint32_t w = 0, h = 0;
    int32_t rc;
    switch (fmt) {
      case kCeyxFormatWebp: rc = ceyx_webp_probe_impl(path, &w, &h); break;
      case kCeyxFormatJxl:  rc = ceyx_jxl_probe_impl(path, &w, &h);  break;
      case kCeyxFormatHeic:
      case kCeyxFormatAvif: {
#if CEYX_HEIF_STILL_ROUTE
        int32_t o = 1;
        rc = MapHeifToStillError(heif_probe(path, &w, &h, &o));
#else
        rc = kCeyxStillErrUnsupported;
#endif
        break;
      }
      default: rc = kCeyxStillErrOpenFailed; break;
    }
    if (rc != kCeyxStillSuccess) return rc;   // out-params left UNTOUCHED
    *width = w;
    *height = h;
    *orientation = 1;   // transforms are applied at decode; see the header
    return kCeyxStillSuccess;
  } catch (...) {
    return kCeyxStillErrUnknownException;
  }
}

FFI_EXPORT int32_t ceyx_still_decode_rgba(const char *path, int32_t format_hint,
                                          int32_t max_dim, CeyxStillResult *out) {
  if (!out) return kCeyxStillErrNullPath;
  std::memset(out, 0, sizeof(*out));
  if (!path || !*path) {
    out->error_code = kCeyxStillErrNullPath;
    return out->error_code;
  }
  if (format_hint < kCeyxFormatUnknown || format_hint > kCeyxFormatJxl) {
    out->error_code = kCeyxStillErrBadFormat;
    return out->error_code;
  }
  try {
    const int32_t fmt =
        (format_hint == kCeyxFormatUnknown) ? SniffFormat(path) : format_hint;
    int32_t rc;
    switch (fmt) {
      case kCeyxFormatWebp: rc = ceyx_webp_decode_impl(path, max_dim, out); break;
      case kCeyxFormatJxl:  rc = ceyx_jxl_decode_impl(path, max_dim, out);  break;
      case kCeyxFormatHeic:
      case kCeyxFormatAvif:
#if CEYX_HEIF_STILL_ROUTE
        rc = ceyx_heif_still_decode_impl(path, max_dim, out);
#else
        rc = kCeyxStillErrUnsupported;
#endif
        break;
      default: rc = kCeyxStillErrOpenFailed; break;
    }
    if (rc != kCeyxStillSuccess) {
      std::memset(out, 0, sizeof(*out));
      out->error_code = rc;
    }
    return out->error_code;
  } catch (...) {
    std::memset(out, 0, sizeof(*out));
    out->error_code = kCeyxStillErrUnknownException;
    return out->error_code;
  }
}

FFI_EXPORT void ceyx_still_release(CeyxStillResult *r) {
  if (!r) return;
  if (r->rgba) std::free(r->rgba);
  std::memset(r, 0, sizeof(*r));   // makes a second call a no-op, per contract
}

}  // extern "C"
