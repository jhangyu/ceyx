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

// The yuv420 entry below consumes EXACTLY the destination layout a yuv420
// decode writes, so both the layout contract and its sizing function are taken
// from their single definitions rather than restated here (raw_ffi_api.h:436-487
// rules that open-coding the arithmetic is a defect by contract).
#include "raw_ffi_api.h"
#include "ceyx_yuv420_oracle.h"

#include "still_codec_internal.h"

#include <csetjmp>
#include <cstddef>  // offsetof
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
    case kCeyxEncodeErrBadOptions: return "kCeyxEncodeErrBadOptions";
    case kCeyxEncodeErrMetadataRejected: return "kCeyxEncodeErrMetadataRejected";
    case kCeyxEncodeErrBadFormat: return "kCeyxEncodeErrBadFormat";
    case kCeyxEncodeErrLosslessUnsupported: return "kCeyxEncodeErrLosslessUnsupported";
    case kCeyxEncodeErrBadBufferSize: return "kCeyxEncodeErrBadBufferSize";
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

// Planar 4:2:0 -> JPEG, via libjpeg's raw-data path. See the contract comment
// on the declaration in ceyx_encode_api.h.
//
// WHY THIS IS NOT "rgba8 with a conversion in front": jpeg_write_raw_data
// bypasses BOTH jccolor (RGB->YCbCr) and jcsample (chroma downsample). The
// decoder already produced its planes with libjpeg's own coefficients and its
// own 2x2 box average (ceyx_yuv420_oracle.h, whose provenance is jccolor.c /
// jcsample.c on this tree), so feeding them in raw reproduces what the rgba8
// entry would have computed from the same frame -- it does not approximate it.
//
// PADDING: doc/libjpeg.txt:2866-2933 requires whole iMCU rows and a per-
// component row length of width_in_blocks*DCTSIZE. Those fields only exist
// after jpeg_start_compress, so the scratch bands are sized from them rather
// than from a hand-derived ceil(). Rows/columns past the valid extent replicate
// the last valid sample (the "usually ... replicating the last column and/or
// row" the doc names), which is why an odd width or height never overreads
// `src` and never needs the caller to pre-pad.
//
// ONE implementation, no platform branch: this body is identical on every
// target (the FFI_EXPORT macro above is the Windows export attribute, not a
// second code path), as required by the repo's no-divergence rule.
FFI_EXPORT int32_t ceyx_encode_jpeg_yuv420(const uint8_t *src,
                                           size_t src_capacity, int32_t width,
                                           int32_t height, int32_t quality,
                                           uint8_t **out, size_t *out_len) {
  const int32_t bad = ValidateArgs(src, width, height, quality, out, out_len);
  if (bad != kCeyxEncodeSuccess) return bad;

  // Unlike the rgba8 entry (see ValidateArgs' note), this one is handed the
  // real allocation size, so the length contract is CHECKED here instead of
  // being delegated to the caller.
  const int64_t required =
      ceyx_output_format_byte_count(kCeyxOutputFormatYuv420, width, height);
  if (required <= 0 || src_capacity < static_cast<size_t>(required)) {
    return kCeyxEncodeErrBadBufferSize;
  }

  const int32_t chroma_w = ceyx::yuv420::chroma_extent(width);
  const int32_t chroma_h = ceyx::yuv420::chroma_extent(height);
  const uint8_t *const plane_y = src;
  const uint8_t *const plane_cb =
      src + static_cast<size_t>(width) * static_cast<size_t>(height);
  const uint8_t *const plane_cr =
      plane_cb + static_cast<size_t>(chroma_w) * static_cast<size_t>(chroma_h);

  struct jpeg_compress_struct cinfo;
  JpegErrorMgr jerr;
  unsigned char *buffer = nullptr;  // allocated by libjpeg via malloc/realloc
  unsigned long buffer_len = 0;
  // volatile for the same C11 7.13.2.1 reason the rgba8 entry documents: these
  // are assigned after setjmp() and freed inside the longjmp handler.
  uint8_t *volatile band_y = nullptr;
  uint8_t *volatile band_cb = nullptr;
  uint8_t *volatile band_cr = nullptr;

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = JpegErrorExit;
  jerr.pub.output_message = JpegSilentOutput;
  if (setjmp(jerr.setjmp_buffer)) {
    jpeg_destroy_compress(&cinfo);
    if (buffer) free(buffer);
    if (band_y) free(band_y);
    if (band_cb) free(band_cb);
    if (band_cr) free(band_cr);
    *out = nullptr;
    *out_len = 0;
    return kCeyxEncodeErrEncodeFailed;
  }

  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &buffer, &buffer_len);
  cinfo.image_width = static_cast<JDIMENSION>(width);
  cinfo.image_height = static_cast<JDIMENSION>(height);
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_YCbCr;
  jpeg_set_defaults(&cinfo);
  // Explicit, not inherited: doc/libjpeg.txt:2882-2889 warns that with colour
  // conversion bypassed, in_color_space is ignored and the sampling factors
  // describe the data being supplied, so both are stated rather than assumed.
  jpeg_set_colorspace(&cinfo, JCS_YCbCr);
  jpeg_set_quality(&cinfo, quality, TRUE);
  cinfo.raw_data_in = TRUE;
  cinfo.comp_info[0].h_samp_factor = 2;
  cinfo.comp_info[0].v_samp_factor = 2;
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = 1;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = 1;
  jpeg_start_compress(&cinfo, TRUE);

  // Rows libjpeg will READ per component, now that start_compress has filled
  // width_in_blocks (doc/libjpeg.txt:2904-2913).
  const size_t y_pad_w =
      static_cast<size_t>(cinfo.comp_info[0].width_in_blocks) * DCTSIZE;
  const size_t c_pad_w =
      static_cast<size_t>(cinfo.comp_info[1].width_in_blocks) * DCTSIZE;
  const int kYBandRows = 2 * DCTSIZE;  // max_v_samp_factor * DCTSIZE
  const int kCBandRows = DCTSIZE;

  band_y = static_cast<uint8_t *>(malloc(y_pad_w * kYBandRows));
  band_cb = static_cast<uint8_t *>(malloc(c_pad_w * kCBandRows));
  band_cr = static_cast<uint8_t *>(malloc(c_pad_w * kCBandRows));
  if (!band_y || !band_cb || !band_cr) {
    jpeg_destroy_compress(&cinfo);
    if (buffer) free(buffer);
    if (band_y) free(band_y);
    if (band_cb) free(band_cb);
    if (band_cr) free(band_cr);
    return kCeyxEncodeErrAllocationFailed;
  }

  // Copies one source row into a padded scratch row, replicating the last
  // valid column across the pad. `src_row` is already clamped by the caller,
  // which is what makes the bottom pad a row replication.
  const auto fill_row = [](uint8_t *dst, const uint8_t *plane, int32_t row,
                           int32_t valid_w, size_t padded_w) {
    memcpy(dst, plane + static_cast<size_t>(row) * valid_w,
           static_cast<size_t>(valid_w));
    for (size_t x = static_cast<size_t>(valid_w); x < padded_w; ++x) {
      dst[x] = dst[valid_w - 1];
    }
  };

  JSAMPROW y_rows[2 * DCTSIZE];
  JSAMPROW cb_rows[DCTSIZE];
  JSAMPROW cr_rows[DCTSIZE];
  JSAMPARRAY planes[3] = {y_rows, cb_rows, cr_rows};

  uint8_t *const by = band_y;
  uint8_t *const bcb = band_cb;
  uint8_t *const bcr = band_cr;

  int mcu_row = 0;
  while (cinfo.next_scanline < cinfo.image_height) {
    for (int r = 0; r < kYBandRows; ++r) {
      int32_t sy = mcu_row * kYBandRows + r;
      if (sy > height - 1) sy = height - 1;
      uint8_t *dst = by + static_cast<size_t>(r) * y_pad_w;
      fill_row(dst, plane_y, sy, width, y_pad_w);
      y_rows[r] = reinterpret_cast<JSAMPROW>(dst);
    }
    for (int r = 0; r < kCBandRows; ++r) {
      int32_t sy = mcu_row * kCBandRows + r;
      if (sy > chroma_h - 1) sy = chroma_h - 1;
      uint8_t *dst_cb = bcb + static_cast<size_t>(r) * c_pad_w;
      uint8_t *dst_cr = bcr + static_cast<size_t>(r) * c_pad_w;
      fill_row(dst_cb, plane_cb, sy, chroma_w, c_pad_w);
      fill_row(dst_cr, plane_cr, sy, chroma_w, c_pad_w);
      cb_rows[r] = reinterpret_cast<JSAMPROW>(dst_cb);
      cr_rows[r] = reinterpret_cast<JSAMPROW>(dst_cr);
    }
    jpeg_write_raw_data(&cinfo, planes, static_cast<JDIMENSION>(kYBandRows));
    ++mcu_row;
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  free(by);
  free(bcb);
  free(bcr);

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

// --- Generic multi-format encode surface (2026-08-30, codec expansion) -------
//
// Appended below the two original entries, which are left byte-for-byte
// unchanged: Halcyon already ships against them, and their output is pinned by
// a SHA-256 captured before this change.

namespace {

// Shared validation for the generic entry. Deliberately stricter than
// ValidateArgs (:46-57): it also enforces the struct_size handshake and the
// metadata pointer/length agreement.
//
// struct_size handshake, precisely: `opts->struct_size` is a promise about
// how many bytes of *opts the CALLER actually allocated -- a caller built
// against an older, smaller CeyxEncodeOptions may pass a struct that
// physically does not contain the `effort`/`reserved0` bytes at all. Reading
// those fields through `opts` when struct_size doesn't cover their offset is
// an out-of-bounds read on the caller's memory, not just "reads a stale
// default" -- the zero-the-whole-struct-first convention documented in
// ceyx_encode_api.h is the caller's responsibility, not something this ABI
// boundary may assume for a security-relevant read. So every field read
// beyond the minimum guaranteed prefix (struct_size + quality, the smallest
// shape this API has ever shipped) is gated on struct_size and defaulted
// otherwise; `*safe_opts` comes back fully populated (struct_size forced to
// sizeof(CeyxEncodeOptions)) so downstream codec arms can dereference every
// field of it unconditionally without repeating this audit.
int32_t ValidateGeneric(int32_t format, const uint8_t *rgba,
                        int32_t width, int32_t height,
                        const CeyxEncodeOptions *opts,
                        const CeyxEncodeMetadata *meta,
                        uint8_t **out, size_t *out_len,
                        CeyxEncodeOptions *safe_opts,
                        CeyxEncodeMetadata *safe_meta) {
  if (out) *out = nullptr;
  if (out_len) *out_len = 0;
  if (!rgba || !out || !out_len) return kCeyxEncodeErrNullArg;

  // kCeyxFormatUnknown (0) is a DECODE-side "sniff by content" sentinel; as an
  // encode target it is not a format, hence the lower bound is Jpeg, not
  // Unknown (ceyx_encode_api.h:83-90).
  if (format < kCeyxFormatJpeg || format > kCeyxFormatJxl) {
    return kCeyxEncodeErrBadFormat;
  }
  if (width <= 0 || height <= 0 || width > kMaxDimension ||
      height > kMaxDimension) {
    return kCeyxEncodeErrBadDimensions;
  }
  if (!opts) return kCeyxEncodeErrBadOptions;
  // struct_size handshake: larger than this build knows is a caller from the
  // future and is rejected; smaller is accepted and the missing tail defaults.
  if (opts->struct_size > sizeof(CeyxEncodeOptions)) return kCeyxEncodeErrBadOptions;
  if (opts->struct_size < offsetof(CeyxEncodeOptions, quality) + sizeof(opts->quality)) {
    return kCeyxEncodeErrBadOptions;
  }

  // Only dereference lossless/effort/reserved0 when struct_size proves the
  // caller's allocation actually extends that far; otherwise default.
  const bool has_lossless =
      opts->struct_size >= offsetof(CeyxEncodeOptions, lossless) + sizeof(opts->lossless);
  const bool has_effort =
      opts->struct_size >= offsetof(CeyxEncodeOptions, effort) + sizeof(opts->effort);
  const bool has_reserved0 =
      opts->struct_size >= offsetof(CeyxEncodeOptions, reserved0) + sizeof(opts->reserved0);
  const int32_t lossless = has_lossless ? opts->lossless : 0;
  const int32_t effort = has_effort ? opts->effort : 0;

  if (has_reserved0 && opts->reserved0 != 0) return kCeyxEncodeErrBadOptions;
  if (effort < 0 || effort > 10) return kCeyxEncodeErrBadOptions;
  if (!lossless && (opts->quality < 1 || opts->quality > 100)) {
    return kCeyxEncodeErrBadQuality;
  }
  if (lossless && format == kCeyxFormatJpeg) {
    return kCeyxEncodeErrLosslessUnsupported;
  }
  // Same OOB-read pattern as opts above, on the sibling struct: a caller
  // built against an older, smaller CeyxEncodeMetadata may not physically own
  // the xmp/icc bytes at all. Each pointer/length pair is only dereferenced
  // once struct_size proves it's covered; an uncovered pair is treated as
  // "not provided" (nullptr, 0) rather than read.
  if (meta) {
    if (meta->struct_size > sizeof(CeyxEncodeMetadata)) return kCeyxEncodeErrBadOptions;
    const bool has_exif =
        meta->struct_size >= offsetof(CeyxEncodeMetadata, exif_len) + sizeof(meta->exif_len);
    const bool has_xmp =
        meta->struct_size >= offsetof(CeyxEncodeMetadata, xmp_len) + sizeof(meta->xmp_len);
    const bool has_icc =
        meta->struct_size >= offsetof(CeyxEncodeMetadata, icc_len) + sizeof(meta->icc_len);
    const uint8_t *exif = has_exif ? meta->exif : nullptr;
    const size_t exif_len = has_exif ? meta->exif_len : 0;
    const uint8_t *xmp = has_xmp ? meta->xmp : nullptr;
    const size_t xmp_len = has_xmp ? meta->xmp_len : 0;
    const uint8_t *icc = has_icc ? meta->icc : nullptr;
    const size_t icc_len = has_icc ? meta->icc_len : 0;
    if ((exif == nullptr) != (exif_len == 0)) return kCeyxEncodeErrNullArg;
    if ((xmp == nullptr) != (xmp_len == 0)) return kCeyxEncodeErrNullArg;
    if ((icc == nullptr) != (icc_len == 0)) return kCeyxEncodeErrNullArg;
    if (safe_meta) {
      safe_meta->struct_size = sizeof(CeyxEncodeMetadata);
      safe_meta->exif = exif;
      safe_meta->exif_len = exif_len;
      safe_meta->xmp = xmp;
      safe_meta->xmp_len = xmp_len;
      safe_meta->icc = icc;
      safe_meta->icc_len = icc_len;
    }
  } else if (safe_meta) {
    std::memset(safe_meta, 0, sizeof(*safe_meta));
    safe_meta->struct_size = sizeof(CeyxEncodeMetadata);
  }

  if (safe_opts) {
    safe_opts->struct_size = sizeof(CeyxEncodeOptions);
    safe_opts->quality = opts->quality;
    safe_opts->lossless = lossless;
    safe_opts->effort = effort;
    safe_opts->reserved0 = 0;
  }
  return kCeyxEncodeSuccess;
}

}  // namespace

extern "C" {

FFI_EXPORT int32_t ceyx_encode_supports(int32_t format) {
  switch (format) {
    case kCeyxFormatJpeg: return 1;                 // libjpeg-turbo always linked
    case kCeyxFormatWebp: return CEYX_ENABLE_WEBP ? 1 : 0;
    case kCeyxFormatHeic:
    case kCeyxFormatAvif:
      // See the matching arm in still_ffi_api.cpp: route != codec.
      return CeyxHeifHasEncoderFor(format);
    case kCeyxFormatJxl:  return CEYX_ENABLE_JXL ? 1 : 0;
    default: return kCeyxEncodeErrBadFormat;
  }
}

FFI_EXPORT int32_t ceyx_encode_rgba8(int32_t format,
                                     const uint8_t *rgba,
                                     int32_t width, int32_t height,
                                     const CeyxEncodeOptions *opts,
                                     const CeyxEncodeMetadata *meta,
                                     uint8_t **out, size_t *out_len) {
  // safe_opts is fully populated by ValidateGeneric (struct_size forced to
  // sizeof(CeyxEncodeOptions), lossless/effort/reserved0 defaulted to 0 when
  // the caller's struct_size didn't cover them) -- every codec arm below
  // dereferences &safe_opts, never the caller-owned `opts`, so none of them
  // needs to repeat the struct_size audit.
  CeyxEncodeOptions safe_opts;
  CeyxEncodeMetadata safe_meta;
  std::memset(&safe_opts, 0, sizeof(safe_opts));
  std::memset(&safe_meta, 0, sizeof(safe_meta));
  const int32_t bad = ValidateGeneric(format, rgba, width, height, opts, meta,
                                      out, out_len, &safe_opts, &safe_meta);
  if (bad != kCeyxEncodeSuccess) return bad;
  // meta was optional (NULL means "embed nothing"); preserve that for the
  // codec arms below rather than always handing them a non-NULL &safe_meta.
  const CeyxEncodeMetadata *meta_arg = meta ? &safe_meta : nullptr;

  switch (format) {
    case kCeyxFormatJpeg:
      // EXIF for JPEG is attached by the caller (Halcyon re-encodes with
      // package:image); this entry ignores meta for JPEG rather than silently
      // pretending to embed it.
      return ceyx_encode_jpeg_rgba8(rgba, width, height, safe_opts.quality, out, out_len);
    case kCeyxFormatWebp:
#if CEYX_ENABLE_WEBP
      return ceyx_webp_encode_impl(rgba, width, height, &safe_opts, meta_arg, out, out_len);
#else
      return kCeyxEncodeErrUnsupported;
#endif
    case kCeyxFormatHeic:
    case kCeyxFormatAvif:
      return ceyx_heif_encode_impl(format, rgba, width, height, &safe_opts, meta_arg, out, out_len);
    case kCeyxFormatJxl:
#if CEYX_ENABLE_JXL
      return ceyx_jxl_encode_impl(rgba, width, height, &safe_opts, meta_arg, out, out_len);
#else
      return kCeyxEncodeErrUnsupported;
#endif
    default:
      return kCeyxEncodeErrBadFormat;  // unreachable; validated above
  }
}

}  // extern "C"
