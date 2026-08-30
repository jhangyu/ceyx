// WebP encode (lossy + lossless + EXIF mux) and still decode.
//
// Wrapper-only: libwebpmux.a and libwebpdemux.a are ALREADY in the vendored
// dist. -DWEBP_BUILD_WEBPMUX=OFF (fetch_libwebp_dist.sh:87) disables the
// command-line TOOL, not the library -- which is why WebP metadata support
// needs no new dependency, only this file and a link line.
//
// These are INTERNAL impls (still_codec_internal.h); the exported entries live
// in src/ffi/encode_ffi_api.cpp and src/ffi/still_ffi_api.cpp, so nothing here
// carries FFI_EXPORT.

#include "ffi/still_codec_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if CEYX_ENABLE_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#include <webp/mux.h>
#endif

namespace {

#if CEYX_ENABLE_WEBP

// Reads a whole file. Returns empty on any failure; the caller maps that to
// kCeyxStillErrOpenFailed.
std::vector<uint8_t> ReadAll(const char *path) {
  std::vector<uint8_t> bytes;
  FILE *f = std::fopen(path, "rb");
  if (!f) return bytes;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n > 0) {
    bytes.resize(static_cast<size_t>(n));
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) bytes.clear();
  }
  std::fclose(f);
  return bytes;
}

// Copies `n` bytes into a malloc'd buffer so ceyx_encode_free stays free().
int32_t AdoptBuffer(const uint8_t *src, size_t n, uint8_t **out, size_t *out_len) {
  if (!src || n == 0) return kCeyxEncodeErrEncodeFailed;
  auto *owned = static_cast<uint8_t *>(std::malloc(n));
  if (!owned) return kCeyxEncodeErrAllocationFailed;
  std::memcpy(owned, src, n);
  *out = owned;
  *out_len = n;
  return kCeyxEncodeSuccess;
}

#endif  // CEYX_ENABLE_WEBP

}  // namespace

extern "C" int32_t ceyx_webp_encode_impl(const uint8_t *rgba,
                                         int32_t width, int32_t height,
                                         const CeyxEncodeOptions *opts,
                                         const CeyxEncodeMetadata *meta,
                                         uint8_t **out, size_t *out_len) {
#if !CEYX_ENABLE_WEBP
  (void)rgba; (void)width; (void)height; (void)opts; (void)meta;
  (void)out; (void)out_len;
  return kCeyxEncodeErrUnsupported;
#else
  if (!rgba || !opts || !out || !out_len) return kCeyxEncodeErrNullArg;
  *out = nullptr;
  *out_len = 0;
  if (width <= 0 || height <= 0) return kCeyxEncodeErrBadDimensions;

  // FAST PATH, byte-compatibility guarantee: lossy with default effort and no
  // metadata must produce EXACTLY what ceyx_encode_webp_rgba8 has always
  // produced (encode_ffi_api.cpp:176-206). Routing it through WebPConfig would
  // change the output bytes even at the same nominal quality.
  const bool wants_metadata = meta && ((meta->exif && meta->exif_len) ||
                                       (meta->xmp && meta->xmp_len) ||
                                       (meta->icc && meta->icc_len));
  if (!opts->lossless && opts->effort == 0 && !wants_metadata) {
    return ceyx_encode_webp_rgba8(rgba, width, height, opts->quality, out, out_len);
  }

  WebPConfig config;
  if (!WebPConfigInit(&config)) return kCeyxEncodeErrEncodeFailed;
  if (opts->lossless) {
    // The lossless preset level is libwebp's 0..9 effort knob; the ABI's
    // `effort` is 0 (=default) or 1..10. Level 6 is the default here because it
    // is libwebp's own -z default. Output is mathematically exact at any level;
    // only speed and size change.
    int level = opts->effort > 0 ? opts->effort : 6;
    if (level > 9) level = 9;
    if (!WebPConfigLosslessPreset(&config, level)) {
      return kCeyxEncodeErrEncodeFailed;
    }
    config.lossless = 1;
    config.exact = 1;  // preserve RGB under fully-transparent pixels
  } else {
    config.lossless = 0;
    config.quality = static_cast<float>(opts->quality);
    if (opts->effort > 0) {
      // libwebp's method is 0..6; the ABI's effort is 1..10.
      config.method = (opts->effort * 6) / 10;
    }
  }
  if (!WebPValidateConfig(&config)) return kCeyxEncodeErrBadOptions;

  WebPPicture pic;
  if (!WebPPictureInit(&pic)) return kCeyxEncodeErrEncodeFailed;
  pic.use_argb = 1;              // required for the lossless coder
  pic.width = width;
  pic.height = height;
  if (!WebPPictureImportRGBA(&pic, rgba, width * 4)) {
    WebPPictureFree(&pic);
    return kCeyxEncodeErrAllocationFailed;
  }

  WebPMemoryWriter writer;
  WebPMemoryWriterInit(&writer);
  pic.writer = WebPMemoryWrite;
  pic.custom_ptr = &writer;
  const int ok = WebPEncode(&config, &pic);
  WebPPictureFree(&pic);
  if (!ok || writer.size == 0) {
    WebPMemoryWriterClear(&writer);
    return kCeyxEncodeErrEncodeFailed;
  }

  if (!wants_metadata) {
    const int32_t rc = AdoptBuffer(writer.mem, writer.size, out, out_len);
    WebPMemoryWriterClear(&writer);
    return rc;
  }

  // --- metadata mux ------------------------------------------------------
  WebPData image_data = {writer.mem, writer.size};
  WebPMux *mux = WebPMuxNew();
  if (!mux) {
    WebPMemoryWriterClear(&writer);
    return kCeyxEncodeErrAllocationFailed;
  }
  int32_t rc = kCeyxEncodeSuccess;
  if (WebPMuxSetImage(mux, &image_data, 1) != WEBP_MUX_OK) {
    rc = kCeyxEncodeErrEncodeFailed;
  }
  // The chunk fourccs are the container's, not ours: "EXIF", "XMP " (note the
  // trailing space -- a fourcc is exactly four bytes) and "ICCP".
  struct ChunkSpec { const char *fourcc; const uint8_t *p; size_t n; };
  const ChunkSpec chunks[] = {
      {"EXIF", meta->exif, meta->exif_len},
      {"XMP ", meta->xmp,  meta->xmp_len},
      {"ICCP", meta->icc,  meta->icc_len},
  };
  for (const auto &c : chunks) {
    if (rc != kCeyxEncodeSuccess || !c.p || c.n == 0) continue;
    WebPData d = {c.p, c.n};
    if (WebPMuxSetChunk(mux, c.fourcc, &d, 1) != WEBP_MUX_OK) {
      rc = kCeyxEncodeErrMetadataRejected;
    }
  }
  WebPData assembled = {nullptr, 0};
  if (rc == kCeyxEncodeSuccess &&
      WebPMuxAssemble(mux, &assembled) != WEBP_MUX_OK) {
    rc = kCeyxEncodeErrMetadataRejected;
  }
  if (rc == kCeyxEncodeSuccess) {
    rc = AdoptBuffer(assembled.bytes, assembled.size, out, out_len);
  }
  WebPDataClear(&assembled);
  WebPMuxDelete(mux);
  WebPMemoryWriterClear(&writer);
  return rc;
#endif
}

extern "C" int32_t ceyx_webp_probe_impl(const char *path, uint32_t *w, uint32_t *h) {
#if !CEYX_ENABLE_WEBP
  (void)path; (void)w; (void)h;
  return kCeyxStillErrUnsupported;
#else
  if (!path || !*path || !w || !h) return kCeyxStillErrNullPath;
  const std::vector<uint8_t> bytes = ReadAll(path);
  if (bytes.empty()) return kCeyxStillErrOpenFailed;
  int iw = 0, ih = 0;
  if (!WebPGetInfo(bytes.data(), bytes.size(), &iw, &ih)) {
    return kCeyxStillErrDecodeFailed;
  }
  if (iw <= 0 || ih <= 0) return kCeyxStillErrMetadataInvalid;
  *w = static_cast<uint32_t>(iw);
  *h = static_cast<uint32_t>(ih);
  return kCeyxStillSuccess;
#endif
}

extern "C" int32_t ceyx_webp_decode_impl(const char *path, int32_t max_dim,
                                         CeyxStillResult *out) {
#if !CEYX_ENABLE_WEBP
  (void)path; (void)max_dim; (void)out;
  return kCeyxStillErrUnsupported;
#else
  if (!path || !*path || !out) return kCeyxStillErrNullPath;
  const std::vector<uint8_t> bytes = ReadAll(path);
  if (bytes.empty()) return kCeyxStillErrOpenFailed;

  WebPDecoderConfig cfg;
  if (!WebPInitDecoderConfig(&cfg)) return kCeyxStillErrDecodeFailed;
  if (WebPGetFeatures(bytes.data(), bytes.size(), &cfg.input) != VP8_STATUS_OK) {
    return kCeyxStillErrDecodeFailed;
  }
  // An animated WebP decodes to its FIRST frame and returns success: still
  // images only, by design (ceyx_still_api.h). No error code signals this.
  int tw = cfg.input.width, th = cfg.input.height;
  if (tw <= 0 || th <= 0) return kCeyxStillErrMetadataInvalid;
  if (max_dim > 0 && (tw > max_dim || th > max_dim)) {
    if (tw >= th) { th = (th * max_dim + tw / 2) / tw; tw = max_dim; }
    else          { tw = (tw * max_dim + th / 2) / th; th = max_dim; }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    cfg.options.use_scaling = 1;
    cfg.options.scaled_width = tw;
    cfg.options.scaled_height = th;
  }
  const size_t n = static_cast<size_t>(tw) * static_cast<size_t>(th) * 4;
  auto *buf = static_cast<uint8_t *>(std::malloc(n));
  if (!buf) return kCeyxStillErrAllocationFailed;

  cfg.output.colorspace = MODE_RGBA;
  cfg.output.is_external_memory = 1;
  cfg.output.u.RGBA.rgba = buf;
  cfg.output.u.RGBA.stride = tw * 4;
  cfg.output.u.RGBA.size = n;
  const VP8StatusCode st = WebPDecode(bytes.data(), bytes.size(), &cfg);
  WebPFreeDecBuffer(&cfg.output);
  if (st != VP8_STATUS_OK) {
    std::free(buf);
    return st == VP8_STATUS_OUT_OF_MEMORY ? kCeyxStillErrAllocationFailed
                                          : kCeyxStillErrDecodeFailed;
  }
  out->error_code = kCeyxStillSuccess;
  out->width = static_cast<uint32_t>(tw);
  out->height = static_cast<uint32_t>(th);
  out->orientation = 1;
  out->rgba = buf;
  out->rgba_len = static_cast<int64_t>(n);
  return kCeyxStillSuccess;
#endif
}
