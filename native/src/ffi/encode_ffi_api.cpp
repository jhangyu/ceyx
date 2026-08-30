// C ABI for the RGBA8 -> JPEG / WebP encode route. Mirrors src/ffi/
// heif_ffi_api.cpp's shape: no exception may cross this boundary, every entry
// validates its pointers, and buffer ownership transfers to the caller with an
// explicit release call.
//
// Both encoders hand back a buffer obtained from malloc(), so ceyx_encode_free
// is a single free() for either codec and the allocator stays an
// implementation detail of this file (libwebp allocates with WebPMalloc; its
// buffer is copied out and released here rather than exposing WebPFree across
// the ABI).

#include "ceyx_encode_api.h"

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <jpeglib.h>
#include <jerror.h>

#if CEYX_ENABLE_WEBP
#include <webp/encode.h>
#endif

#if defined(_WIN32)
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

namespace {

// JPEG's dimension fields are 16-bit; anything larger is rejected up front
// rather than wrapping inside libjpeg.
constexpr int32_t kMaxDimension = 65535;

/// Shared argument validation. On failure *out/*out_len are zeroed so a caller
/// that frees unconditionally is safe.
//
// NOTE: this function does NOT and cannot validate that `rgba` actually
// holds width*height*4 readable bytes -- it only has the pointer and the
// claimed dimensions, not the buffer's real allocation size. That length
// contract is entirely the caller's responsibility (see payload_reencoder.dart
// on the Halcyon side, which guards this before calling in).
int32_t ValidateArgs(const uint8_t *rgba, int32_t width, int32_t height,
                     int32_t quality, uint8_t **out, size_t *out_len) {
  if (out) *out = nullptr;
  if (out_len) *out_len = 0;
  if (!rgba || !out || !out_len) return kCeyxEncodeErrNullArg;
  if (width <= 0 || height <= 0 || width > kMaxDimension ||
      height > kMaxDimension) {
    return kCeyxEncodeErrBadDimensions;
  }
  if (quality < 1 || quality > 100) return kCeyxEncodeErrBadQuality;
  return kCeyxEncodeSuccess;
}

// --- libjpeg error handling -------------------------------------------------
// The default error manager calls exit() on a fatal error, which would take the
// host application down. Replace it with a longjmp back into the encoder.
struct JpegErrorMgr {
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
};

void JpegErrorExit(j_common_ptr cinfo) {
  JpegErrorMgr *err = reinterpret_cast<JpegErrorMgr *>(cinfo->err);
  longjmp(err->setjmp_buffer, 1);
}

void JpegSilentOutput(j_common_ptr) {}

}  // namespace

extern "C" {

FFI_EXPORT const char *ceyx_encode_error_name(int32_t code) {
  switch (code) {
    case kCeyxEncodeSuccess: return "kCeyxEncodeSuccess";
    case kCeyxEncodeErrNullArg: return "kCeyxEncodeErrNullArg";
    case kCeyxEncodeErrBadDimensions: return "kCeyxEncodeErrBadDimensions";
    case kCeyxEncodeErrBadQuality: return "kCeyxEncodeErrBadQuality";
    case kCeyxEncodeErrAllocationFailed: return "kCeyxEncodeErrAllocationFailed";
    case kCeyxEncodeErrEncodeFailed: return "kCeyxEncodeErrEncodeFailed";
    case kCeyxEncodeErrUnsupported: return "kCeyxEncodeErrUnsupported";
    case kCeyxEncodeErrUnknownException: return "kCeyxEncodeErrUnknownException";
    default: return "kCeyxEncodeErrUnknown";
  }
}

FFI_EXPORT int32_t ceyx_encode_jpeg_rgba8(const uint8_t *rgba, int32_t width,
                               int32_t height, int32_t quality, uint8_t **out,
                               size_t *out_len) {
  const int32_t bad =
      ValidateArgs(rgba, width, height, quality, out, out_len);
  if (bad != kCeyxEncodeSuccess) return bad;

  struct jpeg_compress_struct cinfo;
  JpegErrorMgr jerr;
  unsigned char *buffer = nullptr;  // allocated by libjpeg via malloc/realloc
  unsigned long buffer_len = 0;
  // volatile: modified after setjmp() and read in the longjmp handler below;
  // per C11 7.13.2.1 a non-volatile automatic's value is indeterminate there,
  // which a register-allocating compiler can turn into free() of garbage.
  // Only reachable on the no-JCS_EXTENSIONS (plain libjpeg) branch.
  uint8_t *volatile row_scratch = nullptr;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = JpegErrorExit;
  jerr.pub.output_message = JpegSilentOutput;
  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_compress(&cinfo);
    if (buffer) free(buffer);
    if (row_scratch) free(row_scratch);
    *out = nullptr;
    *out_len = 0;
    return kCeyxEncodeErrEncodeFailed;
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buffer, &buffer_len);
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
#ifdef JCS_EXTENSIONS
  // libjpeg-turbo consumes the RGBA rows directly; no packing pass, which is
  // most of why this path is ~66x faster than the pure-Dart encoder it
  // replaces.
  cinfo.input_components = 4;
  cinfo.in_color_space = JCS_EXT_RGBA;
#else
  // Plain libjpeg (the Linux find_package(JPEG) branch may resolve one):
  // repack each row to RGB into scratch. Correctness over speed.
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  row_scratch = static_cast<uint8_t *>(malloc(static_cast<size_t>(width) * 3));
  if (!row_scratch) {
    jpeg_destroy_compress(&cinfo);
    if (buffer) free(buffer);
    return kCeyxEncodeErrAllocationFailed;
  }
#endif
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_start_compress(&cinfo, TRUE);

  const size_t stride = static_cast<size_t>(width) * 4;
  while (cinfo.next_scanline < cinfo.image_height) {
    const uint8_t *src = rgba + static_cast<size_t>(cinfo.next_scanline) * stride;
#ifdef JCS_EXTENSIONS
    JSAMPROW row = const_cast<JSAMPROW>(reinterpret_cast<const JSAMPLE *>(src));
#else
    for (int32_t x = 0; x < width; ++x) {
      row_scratch[x * 3 + 0] = src[x * 4 + 0];
      row_scratch[x * 3 + 1] = src[x * 4 + 1];
      row_scratch[x * 3 + 2] = src[x * 4 + 2];
    }
    JSAMPROW row = reinterpret_cast<JSAMPROW>(row_scratch);
#endif
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  if (row_scratch) free(row_scratch);

  if (!buffer || buffer_len == 0) {
    if (buffer) free(buffer);
    return kCeyxEncodeErrEncodeFailed;
  }
  *out = static_cast<uint8_t *>(buffer);
  *out_len = static_cast<size_t>(buffer_len);
  return kCeyxEncodeSuccess;
}

FFI_EXPORT int32_t ceyx_encode_webp_rgba8(const uint8_t *rgba, int32_t width,
                               int32_t height, int32_t quality, uint8_t **out,
                               size_t *out_len) {
  const int32_t bad =
      ValidateArgs(rgba, width, height, quality, out, out_len);
  if (bad != kCeyxEncodeSuccess) return bad;
#if CEYX_ENABLE_WEBP
  uint8_t *webp_buf = nullptr;
  const size_t webp_len =
      WebPEncodeRGBA(rgba, width, height, width * 4,
                     static_cast<float>(quality), &webp_buf);
  if (webp_len == 0 || !webp_buf) {
    if (webp_buf) WebPFree(webp_buf);
    return kCeyxEncodeErrEncodeFailed;
  }
  // Copy out of libwebp's allocator so the caller frees with plain free()
  // through ceyx_encode_free, exactly like the JPEG path.
  uint8_t *owned = static_cast<uint8_t *>(malloc(webp_len));
  if (!owned) {
    WebPFree(webp_buf);
    return kCeyxEncodeErrAllocationFailed;
  }
  memcpy(owned, webp_buf, webp_len);
  WebPFree(webp_buf);
  *out = owned;
  *out_len = webp_len;
  return kCeyxEncodeSuccess;
#else
  return kCeyxEncodeErrUnsupported;
#endif
}

FFI_EXPORT void ceyx_encode_free(uint8_t *buffer) {
  if (buffer) free(buffer);
}

}  // extern "C"
