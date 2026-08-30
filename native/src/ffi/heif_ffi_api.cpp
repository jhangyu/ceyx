// C ABI for the HEIF route. Mirrors src/ffi/dng_ffi_api.cpp's shape: no
// exception may cross this boundary, every entry validates its pointers, and
// buffer ownership transfers to the caller with an explicit release call.

#include "heif_api.h"

#include <cstdlib>
#include <cstring>

#include "heif_decode.h"
#include "heif_error_codes.h"

// Windows exports come ONLY from __declspec(dllexport): there is no
// CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS and no .def file in this tree, so a
// definition without it compiles, links, produces a correctly-sized DLL and
// then fails at the consumer's Dart lookupFunction. macOS and Linux export
// these by default visibility, which is why the omission was invisible until
// the HEIF route was enabled on Windows. Same macro and same placement as
// src/ffi/dng_ffi_api.cpp and src/ffi/encode_ffi_api.cpp (f2bf987).
#if defined(_WIN32)
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

extern "C" {

FFI_EXPORT const char *heif_error_name(int32_t code) {
  switch (code) {
    case kHeifSuccess: return "kHeifSuccess";
    case kHeifErrNullPath: return "kHeifErrNullPath";
    case kHeifErrOpenFailed: return "kHeifErrOpenFailed";
    case kHeifErrNoPrimaryItem: return "kHeifErrNoPrimaryItem";
    case kHeifErrUnsupportedCodec: return "kHeifErrUnsupportedCodec";
    case kHeifErrDecodeFailed: return "kHeifErrDecodeFailed";
    case kHeifErrColorConversion: return "kHeifErrColorConversion";
    case kHeifErrAllocationFailed: return "kHeifErrAllocationFailed";
    case kHeifErrSizeOverflow: return "kHeifErrSizeOverflow";
    case kHeifErrMetadataInvalid: return "kHeifErrMetadataInvalid";
    case kHeifErrUnknownException: return "kHeifErrUnknownException";
    default: return "kHeifErrUnknown";
  }
}

FFI_EXPORT int32_t heif_probe(const char *path, uint32_t *width, uint32_t *height,
                              int32_t *orientation) {
  if (!path || !path[0] || !width || !height || !orientation) {
    return kHeifErrNullPath;
  }
  try {
    return heifProbePrimary(path, width, height, orientation);
  } catch (...) {
    return kHeifErrUnknownException;
  }
}

FFI_EXPORT int32_t heif_decode_rgba(const char *path, int32_t max_dim, HeifResult *out) {
  if (!out) return kHeifErrNullPath;
  // Fully overwritten, never partially: a caller reading width/height after a
  // failure must see zeroes, not whatever was on its stack.
  std::memset(out, 0, sizeof(HeifResult));
  if (!path || !path[0]) {
    out->error_code = kHeifErrNullPath;
    return kHeifErrNullPath;
  }
  try {
    uint8_t *rgba = nullptr;
    int64_t len = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    int32_t orientation = 1;
    const int32_t rc =
        heifDecodePrimaryRgba(path, max_dim, &rgba, &len, &w, &h, &orientation);
    if (rc != kHeifSuccess) {
      if (rgba) std::free(rgba);
      out->error_code = rc;
      return rc;
    }
    // The invariant _imageFromPixels asserts on the Dart side
    // (decoded_rgba_image_provider.dart:59-67), enforced here so a violation
    // can never reach Dart at all.
    if (len != static_cast<int64_t>(w) * static_cast<int64_t>(h) * 4) {
      std::free(rgba);
      out->error_code = kHeifErrMetadataInvalid;
      return kHeifErrMetadataInvalid;
    }
    out->error_code = kHeifSuccess;
    out->width = w;
    out->height = h;
    out->orientation = orientation;
    out->rgba = rgba;
    out->rgba_len = len;
    return kHeifSuccess;
  } catch (...) {
    out->error_code = kHeifErrUnknownException;
    return kHeifErrUnknownException;
  }
}

FFI_EXPORT void heif_release(HeifResult *r) {
  if (!r) return;
  if (r->rgba) std::free(r->rgba);
  // Zeroing (not just freeing) is what makes a double release safe: the second
  // call sees a null pointer.
  std::memset(r, 0, sizeof(HeifResult));
}

}  // extern "C"
