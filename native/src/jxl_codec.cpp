// JPEG XL encode and decode via libjxl.
//
// THE EXIF TRAP: libjxl's "Exif" box payload is a 4-byte BIG-ENDIAN
// tiff-header-offset field followed by the EXIF block. Omitting the prefix
// produces a file that encodes cleanly, decodes cleanly, and carries metadata
// no reader can locate -- a green build with a silently dead feature. That is
// why test_codec_jxl.cpp parses the box with its own reader rather than
// asking libjxl whether the write succeeded.

#include "ffi/still_codec_internal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if CEYX_ENABLE_JXL
#include <jxl/decode.h>
#include <jxl/decode_cxx.h>
#include <jxl/encode.h>
#include <jxl/encode_cxx.h>
#include <jxl/resizable_parallel_runner.h>
#include <jxl/thread_parallel_runner.h>
#include <jxl/thread_parallel_runner_cxx.h>
#endif

namespace {

#if CEYX_ENABLE_JXL

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

// Bounded worker count: the caller may already be on a Dart worker isolate, so
// grabbing every core here would oversubscribe the machine during an
// interactive export.
size_t WorkerCount() {
  const size_t n = JxlThreadParallelRunnerDefaultNumWorkerThreads();
  return n > 4 ? 4 : (n ? n : 1);
}

// libjxl's Exif box wants a 4-byte big-endian offset from the end of that
// field to the TIFF header. Our payload starts AT the TIFF header, so the
// offset is 0 -- but the four bytes must still be present.
std::vector<uint8_t> WrapExifForJxl(const uint8_t *exif, size_t n) {
  std::vector<uint8_t> box;
  box.reserve(4 + n);
  box.insert(box.end(), {0x00, 0x00, 0x00, 0x00});
  box.insert(box.end(), exif, exif + n);
  return box;
}

#endif  // CEYX_ENABLE_JXL

}  // namespace

extern "C" int32_t ceyx_jxl_encode_impl(const uint8_t *rgba,
                                        int32_t width, int32_t height,
                                        const CeyxEncodeOptions *opts,
                                        const CeyxEncodeMetadata *meta,
                                        uint8_t **out, size_t *out_len) {
#if !CEYX_ENABLE_JXL
  (void)rgba; (void)width; (void)height; (void)opts; (void)meta;
  (void)out; (void)out_len;
  return kCeyxEncodeErrUnsupported;
#else
  try {
    // Declaration order = REVERSE destruction order: the runner must outlive
    // the encoder that holds a pointer to it (JxlEncoderSetParallelRunner
    // below), so it is declared FIRST here -- destructing it before `enc`
    // would leave `enc` (and any in-flight worker callback into it) holding a
    // dangling runner pointer during its own teardown. Hardening, no known
    // defect: nothing in this function's current control flow triggers it,
    // since every return happens with the runner already idle.
    auto runner = JxlThreadParallelRunnerMake(nullptr, WorkerCount());
    JxlEncoderPtr enc = JxlEncoderMake(nullptr);
    if (!enc) return kCeyxEncodeErrAllocationFailed;
    if (JxlEncoderSetParallelRunner(enc.get(), JxlThreadParallelRunner,
                                    runner.get()) != JXL_ENC_SUCCESS) {
      return kCeyxEncodeErrEncodeFailed;
    }

    // Boxes must be enabled BEFORE the basic info is set, and only when
    // metadata is actually being written -- enabling them unconditionally adds
    // container overhead to every file.
    const bool want_exif = meta && meta->exif && meta->exif_len > 0;
    const bool want_xmp = meta && meta->xmp && meta->xmp_len > 0;
    if (want_exif || want_xmp) {
      if (JxlEncoderUseBoxes(enc.get()) != JXL_ENC_SUCCESS) {
        return kCeyxEncodeErrMetadataRejected;
      }
    }

    JxlBasicInfo info;
    JxlEncoderInitBasicInfo(&info);
    info.xsize = static_cast<uint32_t>(width);
    info.ysize = static_cast<uint32_t>(height);
    info.bits_per_sample = 8;
    info.exponent_bits_per_sample = 0;
    info.num_color_channels = 3;
    info.num_extra_channels = 1;
    info.alpha_bits = 8;
    info.alpha_exponent_bits = 0;
    // uses_original_profile must be TRUE for lossless: with the XYB transform
    // enabled, "lossless" is lossless in XYB space, not in the caller's RGB.
    info.uses_original_profile = opts->lossless ? JXL_TRUE : JXL_FALSE;
    if (JxlEncoderSetBasicInfo(enc.get(), &info) != JXL_ENC_SUCCESS) {
      return kCeyxEncodeErrEncodeFailed;
    }

    JxlColorEncoding color;
    JxlColorEncodingSetToSRGB(&color, /*is_gray=*/JXL_FALSE);
    if (JxlEncoderSetColorEncoding(enc.get(), &color) != JXL_ENC_SUCCESS) {
      return kCeyxEncodeErrEncodeFailed;
    }

    JxlEncoderFrameSettings *fs =
        JxlEncoderFrameSettingsCreate(enc.get(), nullptr);
    if (!fs) return kCeyxEncodeErrAllocationFailed;
    if (opts->effort > 0) {
      // libjxl effort is 1..9; the ABI's is 1..10.
      const int e = opts->effort > 9 ? 9 : opts->effort;
      JxlEncoderFrameSettingsSetOption(fs, JXL_ENC_FRAME_SETTING_EFFORT, e);
    }
    if (opts->lossless) {
      if (JxlEncoderSetFrameDistance(fs, 0.0f) != JXL_ENC_SUCCESS ||
          JxlEncoderSetFrameLossless(fs, JXL_TRUE) != JXL_ENC_SUCCESS) {
        return kCeyxEncodeErrLosslessUnsupported;
      }
    } else {
      const float distance = JxlEncoderDistanceFromQuality(
          static_cast<float>(opts->quality));
      if (JxlEncoderSetFrameDistance(fs, distance) != JXL_ENC_SUCCESS) {
        return kCeyxEncodeErrEncodeFailed;
      }
    }

    const JxlPixelFormat fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
    const size_t nbytes = static_cast<size_t>(width) * height * 4;
    if (JxlEncoderAddImageFrame(fs, &fmt, rgba, nbytes) != JXL_ENC_SUCCESS) {
      return kCeyxEncodeErrEncodeFailed;
    }

    if (want_exif) {
      const std::vector<uint8_t> box = WrapExifForJxl(meta->exif, meta->exif_len);
      if (JxlEncoderAddBox(enc.get(), "Exif", box.data(), box.size(),
                           JXL_FALSE) != JXL_ENC_SUCCESS) {
        return kCeyxEncodeErrMetadataRejected;
      }
    }
    if (want_xmp) {
      if (JxlEncoderAddBox(enc.get(), "xml ", meta->xmp, meta->xmp_len,
                           JXL_FALSE) != JXL_ENC_SUCCESS) {
        return kCeyxEncodeErrMetadataRejected;
      }
    }
    JxlEncoderCloseInput(enc.get());

    std::vector<uint8_t> compressed(1 << 16);
    uint8_t *next = compressed.data();
    size_t avail = compressed.size();
    JxlEncoderStatus st = JXL_ENC_NEED_MORE_OUTPUT;
    while (st == JXL_ENC_NEED_MORE_OUTPUT) {
      st = JxlEncoderProcessOutput(enc.get(), &next, &avail);
      if (st == JXL_ENC_NEED_MORE_OUTPUT) {
        const size_t used = compressed.size() - avail;
        compressed.resize(compressed.size() * 2);
        next = compressed.data() + used;
        avail = compressed.size() - used;
      }
    }
    if (st != JXL_ENC_SUCCESS) return kCeyxEncodeErrEncodeFailed;
    const size_t total = compressed.size() - avail;
    if (total == 0) return kCeyxEncodeErrEncodeFailed;

    auto *owned = static_cast<uint8_t *>(std::malloc(total));
    if (!owned) return kCeyxEncodeErrAllocationFailed;
    std::memcpy(owned, compressed.data(), total);
    *out = owned;
    *out_len = total;
    return kCeyxEncodeSuccess;
  } catch (...) {
    return kCeyxEncodeErrUnknownException;
  }
#endif
}

extern "C" int32_t ceyx_jxl_probe_impl(const char *path, uint32_t *w, uint32_t *h) {
#if !CEYX_ENABLE_JXL
  (void)path; (void)w; (void)h;
  return kCeyxStillErrUnsupported;
#else
  try {
    const std::vector<uint8_t> bytes = ReadAll(path);
    if (bytes.empty()) return kCeyxStillErrOpenFailed;
    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    if (!dec) return kCeyxStillErrAllocationFailed;
    // BASIC_INFO only: subscribing to FULL_IMAGE here is what would turn a
    // cheap probe into a full decode on every preview.
    if (JxlDecoderSubscribeEvents(dec.get(), JXL_DEC_BASIC_INFO) != JXL_DEC_SUCCESS) {
      return kCeyxStillErrDecodeFailed;
    }
    JxlDecoderSetInput(dec.get(), bytes.data(), bytes.size());
    JxlDecoderCloseInput(dec.get());
    const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
    if (st != JXL_DEC_BASIC_INFO) return kCeyxStillErrDecodeFailed;
    JxlBasicInfo info;
    if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) {
      return kCeyxStillErrDecodeFailed;
    }
    if (info.xsize == 0 || info.ysize == 0) return kCeyxStillErrMetadataInvalid;
    *w = info.xsize;
    *h = info.ysize;
    return kCeyxStillSuccess;
  } catch (...) {
    return kCeyxStillErrUnknownException;
  }
#endif
}

extern "C" int32_t ceyx_jxl_decode_impl(const char *path, int32_t max_dim,
                                        CeyxStillResult *out) {
#if !CEYX_ENABLE_JXL
  (void)path; (void)max_dim; (void)out;
  return kCeyxStillErrUnsupported;
#else
  try {
    const std::vector<uint8_t> bytes = ReadAll(path);
    if (bytes.empty()) return kCeyxStillErrOpenFailed;
    // Declaration order = reverse destruction order, same reasoning as the
    // encoder above: the runner must outlive `dec`, which holds a pointer to
    // it via JxlDecoderSetParallelRunner. Hardening, no known defect.
    auto runner = JxlThreadParallelRunnerMake(nullptr, WorkerCount());
    JxlDecoderPtr dec = JxlDecoderMake(nullptr);
    if (!dec) return kCeyxStillErrAllocationFailed;
    JxlDecoderSetParallelRunner(dec.get(), JxlThreadParallelRunner, runner.get());
    if (JxlDecoderSubscribeEvents(dec.get(),
            JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS) {
      return kCeyxStillErrDecodeFailed;
    }
    JxlDecoderSetInput(dec.get(), bytes.data(), bytes.size());
    JxlDecoderCloseInput(dec.get());

    JxlBasicInfo info;
    std::memset(&info, 0, sizeof(info));
    std::vector<uint8_t> pixels;
    const JxlPixelFormat fmt = {4, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};

    for (;;) {
      const JxlDecoderStatus st = JxlDecoderProcessInput(dec.get());
      if (st == JXL_DEC_ERROR) return kCeyxStillErrDecodeFailed;
      if (st == JXL_DEC_NEED_MORE_INPUT) {
        // The full file is already in `bytes` (JxlDecoderCloseInput was
        // called above), so this is a genuine failure, not a "feed more"
        // request. libjxl still requires a matching ReleaseInput before the
        // decoder (and its parallel runner) are torn down -- omitting it
        // leaves the decoder believing input is still owned by the caller
        // mid-operation. dec/runner are RAII-wrapped and destruct on this
        // return, so the release must happen here, before that happens.
        JxlDecoderReleaseInput(dec.get());
        return kCeyxStillErrDecodeFailed;
      }
      if (st == JXL_DEC_BASIC_INFO) {
        if (JxlDecoderGetBasicInfo(dec.get(), &info) != JXL_DEC_SUCCESS) {
          return kCeyxStillErrDecodeFailed;
        }
        if (info.xsize == 0 || info.ysize == 0) return kCeyxStillErrMetadataInvalid;
        // Overflow guard before any allocation sized by these values.
        const uint64_t n = uint64_t(info.xsize) * info.ysize * 4ull;
        if (n > uint64_t(INT64_MAX)) return kCeyxStillErrSizeOverflow;
        continue;
      }
      if (st == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
        size_t need = 0;
        if (JxlDecoderImageOutBufferSize(dec.get(), &fmt, &need) != JXL_DEC_SUCCESS) {
          return kCeyxStillErrDecodeFailed;
        }
        pixels.resize(need);
        if (JxlDecoderSetImageOutBuffer(dec.get(), &fmt, pixels.data(),
                                        pixels.size()) != JXL_DEC_SUCCESS) {
          return kCeyxStillErrDecodeFailed;
        }
        continue;
      }
      if (st == JXL_DEC_FULL_IMAGE) break;   // first frame only, by design
      if (st == JXL_DEC_SUCCESS) break;
    }
    if (pixels.empty()) return kCeyxStillErrDecodeFailed;

    uint32_t fw = info.xsize, fh = info.ysize;
    std::vector<uint8_t> scaled;
    if (max_dim > 0 && (fw > uint32_t(max_dim) || fh > uint32_t(max_dim))) {
      // fw/fh come from the file's basic-info header -- attacker-controlled,
      // up to the size-overflow ceiling checked above (uint64_t(INT64_MAX)),
      // not bounded by max_dim. Every index computed FROM them (tw/th sizing,
      // and the sy0/sy1/sx0/sx1 source-window bounds below) is done in
      // uint64_t so a large fw/fh cannot silently wrap a uint32 intermediate
      // before the result is narrowed back to the pixel-index width the loop
      // actually needs (tw/th themselves are capped by max_dim, an int32_t,
      // so narrowing THOSE back to uint32_t is safe).
      uint32_t tw, th;
      if (fw >= fh) {
        tw = uint32_t(max_dim);
        th = uint32_t((uint64_t(fh) * tw + uint64_t(fw) / 2) / fw);
      } else {
        th = uint32_t(max_dim);
        tw = uint32_t((uint64_t(fw) * th + uint64_t(fh) / 2) / fh);
      }
      if (tw < 1) tw = 1;
      if (th < 1) th = 1;
      // Box filter. libjxl exposes no arbitrary output scaler, so the sizing
      // contract is honoured here rather than pretended at.
      scaled.resize(size_t(tw) * th * 4);
      for (uint32_t y = 0; y < th; ++y) {
        const uint32_t sy0 = uint32_t((uint64_t(y) * fh) / th);
        const uint32_t sy1 = uint32_t((uint64_t(y + 1) * fh + th - 1) / th);
        for (uint32_t x = 0; x < tw; ++x) {
          const uint32_t sx0 = uint32_t((uint64_t(x) * fw) / tw);
          const uint32_t sx1 = uint32_t((uint64_t(x + 1) * fw + tw - 1) / tw);
          uint32_t acc[4] = {0, 0, 0, 0}, cnt = 0;
          for (uint32_t sy = sy0; sy < sy1 && sy < fh; ++sy) {
            for (uint32_t sx = sx0; sx < sx1 && sx < fw; ++sx) {
              const uint8_t *p = &pixels[(size_t(sy) * fw + sx) * 4];
              for (int c = 0; c < 4; ++c) acc[c] += p[c];
              ++cnt;
            }
          }
          uint8_t *d = &scaled[(size_t(y) * tw + x) * 4];
          for (int c = 0; c < 4; ++c) d[c] = uint8_t(cnt ? acc[c] / cnt : 0);
        }
      }
      fw = tw; fh = th;
      pixels.swap(scaled);
    }

    const size_t n = size_t(fw) * fh * 4;
    auto *buf = static_cast<uint8_t *>(std::malloc(n));
    if (!buf) return kCeyxStillErrAllocationFailed;
    std::memcpy(buf, pixels.data(), n);
    out->error_code = kCeyxStillSuccess;
    out->width = fw;
    out->height = fh;
    out->orientation = 1;
    out->rgba = buf;
    out->rgba_len = static_cast<int64_t>(n);
    return kCeyxStillSuccess;
  } catch (...) {
    return kCeyxStillErrUnknownException;
  }
#endif
}
