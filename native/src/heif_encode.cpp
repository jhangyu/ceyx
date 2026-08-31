// HEIC and AVIF encode, plus the still-decode delegation for both.
//
// Both formats are the SAME libheif container path; the only difference is the
// compression format passed to heif_context_get_encoder_for_format
// (heif_encoding.h:103-106). AVIF DECODE needs no code here at all beyond
// delegation -- it is AV1 in the ISO-BMFF container heif_decode.cpp already
// parses, and the only reason .avif did not work before is the
// WITH_AOM_DECODER=OFF flag the dist script now flips.
//
// Output is copied into a malloc'd buffer so ceyx_encode_free stays a plain
// free() and libheif's allocator remains an implementation detail -- exactly
// the shape encode_ffi_api.cpp:191-199 uses for libwebp.
//
// This TU is compiled on BOTH branches of cmake/heif.cmake. With
// DNG_ENABLE_HEIF=0 it still defines every symbol declared in
// still_codec_internal.h, returning kCeyxEncodeErrUnsupported /
// kCeyxStillErrUnsupported, so a codec excluded from a build degrades into a
// defined error rather than a missing symbol at the caller's lookupFunction.

#include "ffi/still_codec_internal.h"

#include <climits>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "heif_error_codes.h"

#if DNG_ENABLE_HEIF
#include <libheif/heif.h>

#include "heif_decode.h"  // the existing decode implementation
#endif

namespace {

#if DNG_ENABLE_HEIF

// libheif's built-in codec registry is initialised on first use. With three
// codecs compiled in (de265, kvazaar, aom) and ceyx_encode_api.h:12-14
// promising concurrent calls are safe, initialisation must happen exactly once.
// A function-local static is guaranteed thread-safe since C++11; a bare global
// is not.
//
// The matching heif_deinit() is deliberately never called: heif_library.h:141
// states it must not run after exit(), which is precisely when a static
// destructor would fire. Leaking the registry once per process is the
// documented-safe choice.
void EnsureHeifInit() {
  static const bool initialised = [] {
    heif_init(nullptr);
    return true;
  }();
  (void)initialised;
}

struct HeifBufferSink {
  std::vector<uint8_t> bytes;
};

heif_error HeifWrite(heif_context *, const void *data, size_t size,
                     void *userdata) {
  auto *sink = static_cast<HeifBufferSink *>(userdata);
  const auto *p = static_cast<const uint8_t *>(data);
  sink->bytes.insert(sink->bytes.end(), p, p + size);
  return heif_error{heif_error_Ok, heif_suberror_Unspecified, "ok"};
}

// RAII so every early return releases libheif's objects. A goto-cleanup chain
// here would be six labels deep and is exactly where leaks hide.
struct HeifCtx {
  heif_context *ctx = heif_context_alloc();
  heif_image *img = nullptr;
  heif_encoder *enc = nullptr;
  heif_image_handle *handle = nullptr;

  HeifCtx() = default;
  HeifCtx(const HeifCtx &) = delete;
  HeifCtx &operator=(const HeifCtx &) = delete;

  ~HeifCtx() {
    if (handle) heif_image_handle_release(handle);
    if (enc) heif_encoder_release(enc);
    if (img) heif_image_release(img);
    if (ctx) heif_context_free(ctx);
  }
};

#endif  // DNG_ENABLE_HEIF

}  // namespace

extern "C" int32_t ceyx_heif_encode_impl(int32_t format,
                                         const uint8_t *rgba,
                                         int32_t width, int32_t height,
                                         const CeyxEncodeOptions *opts,
                                         const CeyxEncodeMetadata *meta,
                                         uint8_t **out, size_t *out_len) {
#if !DNG_ENABLE_HEIF
  (void)format; (void)rgba; (void)width; (void)height;
  (void)opts; (void)meta; (void)out; (void)out_len;
  return kCeyxEncodeErrUnsupported;
#else
  // Arguments are already validated by the dispatcher; this function owns only
  // the codec interaction. *out/*out_len were zeroed there and are written only
  // on the single success path at the bottom, so every early return below
  // leaves them zeroed by construction rather than by remembering to.
  try {
    EnsureHeifInit();
    HeifCtx h;
    if (!h.ctx) return kCeyxEncodeErrAllocationFailed;

    heif_error err = heif_image_create(width, height, heif_colorspace_RGB,
                                       heif_chroma_interleaved_RGBA, &h.img);
    if (err.code != heif_error_Ok || !h.img) return kCeyxEncodeErrAllocationFailed;

    err = heif_image_add_plane(h.img, heif_channel_interleaved, width, height, 8);
    if (err.code != heif_error_Ok) return kCeyxEncodeErrAllocationFailed;

    // get_plane2, not the deprecated get_plane: heif_image.h:268-277 warns that
    // the int-typed stride of the old entry overflows when multiplied by the
    // height of a large image, which is exactly the arithmetic below.
    size_t dst_stride = 0;
    uint8_t *plane =
        heif_image_get_plane2(h.img, heif_channel_interleaved, &dst_stride);
    if (!plane) return kCeyxEncodeErrAllocationFailed;

    // Row-by-row: libheif's plane stride is its own choice and is frequently
    // not width*4. A single memcpy of the whole buffer would corrupt every row
    // after the first whenever it differs.
    const size_t src_stride = static_cast<size_t>(width) * 4;
    if (dst_stride < src_stride) return kCeyxEncodeErrAllocationFailed;
    for (int y = 0; y < height; ++y) {
      std::memcpy(plane + static_cast<size_t>(y) * dst_stride,
                  rgba + static_cast<size_t>(y) * src_stride, src_stride);
    }

    const heif_compression_format compression =
        (format == kCeyxFormatAvif) ? heif_compression_AV1 : heif_compression_HEVC;
    err = heif_context_get_encoder_for_format(h.ctx, compression, &h.enc);
    if (err.code != heif_error_Ok || !h.enc) {
      // The dist was built without this encoder. Distinct from a bad frame.
      return kCeyxEncodeErrUnsupported;
    }

    if (opts->lossless) {
      err = heif_encoder_set_lossless(h.enc, 1);
      if (err.code != heif_error_Ok) return kCeyxEncodeErrLosslessUnsupported;
    } else {
      err = heif_encoder_set_lossy_quality(h.enc, opts->quality);
      if (err.code != heif_error_Ok) return kCeyxEncodeErrEncodeFailed;
    }

    err = heif_context_encode_image(h.ctx, h.img, h.enc, nullptr, &h.handle);
    if (err.code != heif_error_Ok || !h.handle) return kCeyxEncodeErrEncodeFailed;

    // heif_context_add_exif_metadata / _add_XMP_metadata take an `int` size
    // (heif_metadata.h:96, :102). Reject an oversized block explicitly instead
    // of letting the narrowing cast wrap into a negative length -- a silently
    // truncated metadata block is worse than a refused one.
    if (meta && meta->exif && meta->exif_len > 0) {
      if (meta->exif_len > static_cast<size_t>(INT_MAX)) {
        return kCeyxEncodeErrMetadataRejected;
      }
      err = heif_context_add_exif_metadata(h.ctx, h.handle, meta->exif,
                                           static_cast<int>(meta->exif_len));
      if (err.code != heif_error_Ok) return kCeyxEncodeErrMetadataRejected;
    }
    if (meta && meta->xmp && meta->xmp_len > 0) {
      if (meta->xmp_len > static_cast<size_t>(INT_MAX)) {
        return kCeyxEncodeErrMetadataRejected;
      }
      err = heif_context_add_XMP_metadata(h.ctx, h.handle, meta->xmp,
                                          static_cast<int>(meta->xmp_len));
      if (err.code != heif_error_Ok) return kCeyxEncodeErrMetadataRejected;
    }

    // heif_context_write takes a callback struct, NOT a path
    // (heif_context.h:319-336) -- which is what lets this ABI stay in-memory.
    HeifBufferSink sink;
    heif_writer writer;
    std::memset(&writer, 0, sizeof(writer));
    writer.writer_api_version = 1;
    writer.write = HeifWrite;
    err = heif_context_write(h.ctx, &writer, &sink);
    if (err.code != heif_error_Ok || sink.bytes.empty()) {
      return kCeyxEncodeErrEncodeFailed;
    }

    auto *owned = static_cast<uint8_t *>(std::malloc(sink.bytes.size()));
    if (!owned) return kCeyxEncodeErrAllocationFailed;
    std::memcpy(owned, sink.bytes.data(), sink.bytes.size());
    *out = owned;
    *out_len = sink.bytes.size();
    return kCeyxEncodeSuccess;
  } catch (...) {
    return kCeyxEncodeErrUnknownException;
  }
#endif
}

// Runtime codec queries -- the fix for the parity-matrix §2 defect.
//
// ceyx_still_decode_supports / ceyx_encode_supports used to answer HEIC and
// AVIF through the SAME route-level flag, so they could not distinguish
// "libheif is linked" from "libheif was built WITH the AV1 codec". A platform
// with DNG_ENABLE_HEIF=ON but WITH_AOM_*=OFF -- exactly the Windows dist
// before 2026-08-31, and the macOS *committed* dylib still today -- reported
// AVIF == 1 and then failed on every real AVIF file, with the failure visible
// only as a runtime libheif error.
//
// libheif answers this itself. heif_have_decoder_for_format /
// heif_have_encoder_for_format report on the codecs actually compiled into the
// loaded libheif, which is the exact distinction that was missing.
//
// Guarded on DNG_ENABLE_HEIF so a HEIF-less build still DEFINES both helpers
// (returning 0) and references no libheif symbol -- the same
// degrade-to-a-defined-answer rule cmake/heif.cmake applies by listing this TU
// on both branches of its if/else.
namespace {
#if DNG_ENABLE_HEIF
bool MapFormatToCompression(int32_t format, heif_compression_format *out) {
  switch (format) {
    case kCeyxFormatHeic: *out = heif_compression_HEVC; return true;
    case kCeyxFormatAvif: *out = heif_compression_AV1;  return true;
    default: return false;   // WebP/JXL/JPEG do not travel the HEIF route
  }
}
#endif
}  // namespace

__attribute__((visibility("hidden")))
extern "C" int32_t CeyxHeifHasDecoderFor(int32_t format) {
#if DNG_ENABLE_HEIF
  heif_compression_format compression;
  if (!MapFormatToCompression(format, &compression)) return 0;
  return heif_have_decoder_for_format(compression) ? 1 : 0;
#else
  (void)format;
  return 0;
#endif
}

__attribute__((visibility("hidden")))
extern "C" int32_t CeyxHeifHasEncoderFor(int32_t format) {
#if DNG_ENABLE_HEIF
  heif_compression_format compression;
  if (!MapFormatToCompression(format, &compression)) return 0;
  return heif_have_encoder_for_format(compression) ? 1 : 0;
#else
  (void)format;
  return 0;
#endif
}

/* Maps a HeifErrorCode (-301..-310) onto the CeyxStillErrorCode (-501..-511)
 * scale. NOT static: still_ffi_api.cpp calls it from ceyx_still_probe, which is
 * why still_codec_internal.h declares it. */
extern "C" int32_t ceyx_map_heif_to_still_error(int32_t heif_code) {
  switch (heif_code) {
    case kHeifSuccess:              return kCeyxStillSuccess;
    case kHeifErrNullPath:          return kCeyxStillErrNullPath;
    case kHeifErrOpenFailed:        return kCeyxStillErrOpenFailed;
    case kHeifErrNoPrimaryItem:     return kCeyxStillErrNoPrimaryItem;
    case kHeifErrUnsupportedCodec:  return kCeyxStillErrUnsupported;
    case kHeifErrDecodeFailed:      return kCeyxStillErrDecodeFailed;
    case kHeifErrColorConversion:   return kCeyxStillErrColorConversion;
    case kHeifErrAllocationFailed:  return kCeyxStillErrAllocationFailed;
    case kHeifErrSizeOverflow:      return kCeyxStillErrSizeOverflow;
    case kHeifErrMetadataInvalid:   return kCeyxStillErrMetadataInvalid;
    case kHeifErrUnknownException:  return kCeyxStillErrUnknownException;
    // A code this build does not recognise must not leak a -30x value onto the
    // -50x surface: callers of ceyx_still_* must never see a HeifErrorCode.
    default:                        return kCeyxStillErrDecodeFailed;
  }
}

extern "C" int32_t ceyx_heif_still_decode_impl(const char *path, int32_t max_dim,
                                               CeyxStillResult *out) {
#if !DNG_ENABLE_HEIF
  (void)path; (void)max_dim;
  if (out) std::memset(out, 0, sizeof(*out));
  if (out) out->error_code = kCeyxStillErrUnsupported;
  return kCeyxStillErrUnsupported;
#else
  // Delegate to the EXISTING implementation (heif_decode.h:11-14). AVIF needs no
  // separate path: it is AV1 in the same container.
  if (!out) return kCeyxStillErrNullPath;
  std::memset(out, 0, sizeof(*out));

  uint8_t *rgba = nullptr;
  int64_t rgba_len = 0;
  uint32_t w = 0, hgt = 0;
  int32_t orientation = 1;
  const int32_t rc =
      heifDecodePrimaryRgba(path, max_dim, &rgba, &rgba_len, &w, &hgt,
                            &orientation);
  if (rc != kHeifSuccess) {
    // Map the -301 scale onto the -501 scale; the two are disjoint by design
    // and callers of this surface must never see a HeifErrorCode.
    out->error_code = ceyx_map_heif_to_still_error(rc);
    return out->error_code;
  }
  out->error_code = kCeyxStillSuccess;
  out->width = w;
  out->height = hgt;
  out->orientation = orientation;
  out->rgba = rgba;  // ownership transfers; freed by ceyx_still_release
  out->rgba_len = rgba_len;
  return kCeyxStillSuccess;
#endif
}
