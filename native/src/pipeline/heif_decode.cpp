// HEIC/HEIF decode through libheif + libde265 (dynamically linked; see
// native/third_party/heif-dist/PROVENANCE.md for versions, hashes and the
// LGPL-3 relinking rationale).
//
// This TU holds everything that touches libheif's API. The C ABI wrapper in
// src/ffi/heif_ffi_api.cpp holds no libheif types, so a build with
// DNG_ENABLE_HEIF=OFF drops both files and the dylib has no libheif
// dependency at all.

#include "heif_decode.h"

#include <libheif/heif.h>

#include <cstdlib>
#include <cstring>

#include "heif_error_codes.h"

namespace {

// Mirrors kDecodedPixelBudgetBytes on the Dart side
// (lib/services/image_pipeline/dng_decode_contract.dart). The Dart loader
// checks it first via heif_probe; this is the second line for direct native
// callers such as the H1 gate, and the only line for a file whose probe was
// waved through.
constexpr int64_t kHeifMaxRgbaBytes = 1500000000;

struct ContextGuard {
  heif_context *ctx = nullptr;
  ~ContextGuard() {
    if (ctx) heif_context_free(ctx);
  }
};

struct HandleGuard {
  heif_image_handle *handle = nullptr;
  ~HandleGuard() {
    if (handle) heif_image_handle_release(handle);
  }
};

struct ImageGuard {
  heif_image *image = nullptr;
  ~ImageGuard() {
    if (image) heif_image_release(image);
  }
};

struct OptionsGuard {
  heif_decoding_options *options = nullptr;
  ~OptionsGuard() {
    if (options) heif_decoding_options_free(options);
  }
};

// Opens the file and takes the primary (`pitm`) image handle.
int32_t openPrimary(const char *path, ContextGuard &ctx, HandleGuard &handle) {
  ctx.ctx = heif_context_alloc();
  if (!ctx.ctx) return kHeifErrAllocationFailed;

  heif_error err = heif_context_read_from_file(ctx.ctx, path, nullptr);
  if (err.code != heif_error_Ok) return kHeifErrOpenFailed;

  err = heif_context_get_primary_image_handle(ctx.ctx, &handle.handle);
  if (err.code != heif_error_Ok || !handle.handle) return kHeifErrNoPrimaryItem;

  return kHeifSuccess;
}

// libheif's own error taxonomy, mapped onto ours. Only the distinctions the
// app can act on are preserved; everything else collapses to
// kHeifErrDecodeFailed.
int32_t mapDecodeError(const heif_error &err) {
  if (err.code == heif_error_Unsupported_filetype ||
      err.code == heif_error_Unsupported_feature) {
    return kHeifErrUnsupportedCodec;
  }
  if (err.code == heif_error_Memory_allocation_error) {
    return kHeifErrAllocationFailed;
  }
  if (err.code == heif_error_Color_profile_does_not_exist) {
    return kHeifErrColorConversion;
  }
  return kHeifErrDecodeFailed;
}

}  // namespace

int32_t heifProbePrimary(const char *path, uint32_t *width, uint32_t *height,
                         int32_t *orientation) {
  ContextGuard ctx;
  HandleGuard handle;
  const int32_t opened = openPrimary(path, ctx, handle);
  if (opened != kHeifSuccess) return opened;

  // POST-transform extent: libheif accounts for the item's irot/imir
  // properties here, which is the same geometry the decode below produces.
  const int w = heif_image_handle_get_width(handle.handle);
  const int h = heif_image_handle_get_height(handle.handle);
  if (w <= 0 || h <= 0) return kHeifErrMetadataInvalid;

  *width = static_cast<uint32_t>(w);
  *height = static_cast<uint32_t>(h);
  // Always 1: the pixels are delivered display-ready (see heif_api.h).
  *orientation = 1;
  return kHeifSuccess;
}

int32_t heifDecodePrimaryRgba(const char *path, int32_t max_dim,
                              uint8_t **out_rgba, int64_t *out_len,
                              uint32_t *out_width, uint32_t *out_height,
                              int32_t *out_orientation) {
  ContextGuard ctx;
  HandleGuard handle;
  const int32_t opened = openPrimary(path, ctx, handle);
  if (opened != kHeifSuccess) return opened;

  OptionsGuard options;
  options.options = heif_decoding_options_alloc();
  if (!options.options) return kHeifErrAllocationFailed;
  // 10/12-bit HEIC (iPhone HDR) must arrive as 8-bit: everything downstream of
  // DecodedRgba is RGBA8 (spec section 11 parks 16-bit display precision).
  options.options->convert_hdr_to_8bit = 1;
  // ignore_transformations is left at its default 0 on purpose: libheif
  // applies irot/imir, which is what makes the reported orientation 1.

  ImageGuard decoded;
  heif_error err =
      heif_decode_image(handle.handle, &decoded.image, heif_colorspace_RGB,
                        heif_chroma_interleaved_RGBA, options.options);
  if (err.code != heif_error_Ok || !decoded.image) return mapDecodeError(err);

  ImageGuard scaled;
  heif_image *frame = decoded.image;
  int width = heif_image_get_width(frame, heif_channel_interleaved);
  int height = heif_image_get_height(frame, heif_channel_interleaved);
  if (width <= 0 || height <= 0) return kHeifErrMetadataInvalid;

  if (max_dim > 0 && (width > max_dim || height > max_dim)) {
    // Long edge to max_dim, aspect preserved, both sides at least 1.
    const double scale = static_cast<double>(max_dim) /
                         static_cast<double>(width >= height ? width : height);
    int target_w = static_cast<int>(width * scale + 0.5);
    int target_h = static_cast<int>(height * scale + 0.5);
    if (target_w < 1) target_w = 1;
    if (target_h < 1) target_h = 1;
    err = heif_image_scale_image(frame, &scaled.image, target_w, target_h,
                                 nullptr);
    if (err.code != heif_error_Ok || !scaled.image) return mapDecodeError(err);
    frame = scaled.image;
    width = heif_image_get_width(frame, heif_channel_interleaved);
    height = heif_image_get_height(frame, heif_channel_interleaved);
    if (width <= 0 || height <= 0) return kHeifErrMetadataInvalid;
  }

  const int64_t needed =
      static_cast<int64_t>(width) * static_cast<int64_t>(height) * 4;
  if (needed > kHeifMaxRgbaBytes) return kHeifErrSizeOverflow;

  int stride = 0;
  const uint8_t *plane =
      heif_image_get_plane_readonly(frame, heif_channel_interleaved, &stride);
  if (!plane || stride <= 0) return kHeifErrColorConversion;

  uint8_t *buffer =
      static_cast<uint8_t *>(std::malloc(static_cast<size_t>(needed)));
  if (!buffer) return kHeifErrAllocationFailed;

  // Row-by-row, never one memcpy: libheif's plane stride is padded for SIMD
  // and is routinely larger than width*4. Copying `needed` bytes in one go
  // would shear the image and read past the plane on the last row.
  const size_t row_bytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    std::memcpy(buffer + static_cast<size_t>(y) * row_bytes,
                plane + static_cast<size_t>(y) * static_cast<size_t>(stride),
                row_bytes);
  }

  *out_rgba = buffer;
  *out_len = needed;
  *out_width = static_cast<uint32_t>(width);
  *out_height = static_cast<uint32_t>(height);
  *out_orientation = 1;
  return kHeifSuccess;
}
