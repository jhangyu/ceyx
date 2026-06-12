#include "dng_ffi_api.h"
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_info.h>
#include <dng_exceptions.h>
#include <iostream>

#include "dng_pipeline_v2.h"

#if defined(_WIN32)
#define FFI_EXPORT __declspec(dllexport)
#else
#define FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

// ---------------------------------------------------------------------------
// NEON RGB->RGBA helper (ARM64) with scalar fallback for other platforms.
// 16-pixel NEON lane: vld3q_u8 deinterleaves 3-channel RGB; vst4q_u8 writes
// 4-channel RGBA with alpha hard-coded to 255.
// Ownership contract: caller allocates `rgba` with new[]; this function only
// fills it.  Do NOT change the allocation strategy without updating ABI and
// Dart bindings.
// ---------------------------------------------------------------------------
#if defined(__aarch64__)
#include <arm_neon.h>

static void rgb_to_rgba_neon(const uint8_t* rgb, uint8_t* rgba,
                              size_t pixel_count) {
    const uint8x16_t alpha = vdupq_n_u8(255);
    size_t i = 0;
    const size_t vec_end = (pixel_count / 16) * 16;
    for (; i < vec_end; i += 16) {
        uint8x16x3_t src = vld3q_u8(rgb + i * 3);
        uint8x16x4_t dst;
        dst.val[0] = src.val[0];
        dst.val[1] = src.val[1];
        dst.val[2] = src.val[2];
        dst.val[3] = alpha;
        vst4q_u8(rgba + i * 4, dst);
    }
    for (; i < pixel_count; ++i) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
}

#else  // non-ARM64 scalar fallback

static void rgb_to_rgba_neon(const uint8_t* rgb, uint8_t* rgba,
                              size_t pixel_count) {
    for (size_t i = 0; i < pixel_count; ++i) {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = 255;
    }
}

#endif  // __aarch64__
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// W7-A: process-scoped RGBA output pool.
//
// `dng_decode_and_process` previously did `new uint8_t[pixelCount*4]` (~96 MB
// for 24 MP) on every decode.  On warm repeats this re-faults thousands of
// fresh pages.  The pool hands out reusable, uninitialised storage and only
// grows when a larger frame arrives — every byte is overwritten by the
// planar->RGBA fill before it is read, so skipping zero-init is safe (mirrors
// Stage4ScratchPool in dng_render_halide.cpp).
//
// Zero ABI impact: the DngResult struct is untouched (Gotcha #78 static_asserts
// stay green).  Pool ownership is tracked via a side-channel map keyed on the
// raw pointer.  The three release entry points (dng_free_result /
// dng_free_rgba_buffer / dng_free_halide_buffer) consult the pool first and
// only fall back to delete[] for non-pool buffers, so existing callers — incl.
// the zero-copy NativeFinalizer path — keep working without changes.
// ---------------------------------------------------------------------------
namespace {

class RgbaPool {
public:
    // Hands out a buffer of at least `count` bytes. Ownership stays with the
    // pool (tracked in checked_out_); reclaim via release().
    uint8_t* acquire(size_t count) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < free_.size(); ++i) {
            if (free_[i].cap >= count) {
                uint8_t* p = free_[i].buf.release();
                size_t cap = free_[i].cap;
                free_.erase(free_.begin() + static_cast<std::ptrdiff_t>(i));
                checked_out_.emplace(p, cap);
                return p;
            }
        }
        size_t cap = count;
        uint8_t* p = new uint8_t[cap];  // no value-init -> no memset
        checked_out_.emplace(p, cap);
        return p;
    }

    // Returns true if `ptr` was pool-owned (now reclaimed); false if it is not
    // a pool buffer and the caller should delete[] it.
    bool release(uint8_t* ptr) {
        if (!ptr)
            return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = checked_out_.find(ptr);
        if (it == checked_out_.end())
            return false;
        size_t cap = it->second;
        checked_out_.erase(it);
        free_.push_back({std::unique_ptr<uint8_t[]>(ptr), cap});
        return true;
    }

private:
    struct Slot {
        std::unique_ptr<uint8_t[]> buf;
        size_t cap;
    };
    std::mutex mutex_;
    std::vector<Slot> free_;
    std::unordered_map<uint8_t*, size_t> checked_out_;
};

RgbaPool& rgbaPool() {
    static RgbaPool instance;
    return instance;
}

}  // namespace
// ---------------------------------------------------------------------------

extern "C" {

FFI_EXPORT DngResult *dng_decode_and_process(const char *file_path) {
  DngResult *result =
      static_cast<DngResult *>(std::calloc(1, sizeof(DngResult)));
  if (!result)
    return nullptr;

  DngPipelineV2Result pipeline;
  if (!dng_pipeline_v2_decode_to_rgb(file_path, pipeline)) {
    result->error_code = pipeline.error_code;
    result->decode_ms = pipeline.decode_ms;
    result->process_ms = pipeline.process_ms;
    std::cerr << "[FFI] Pipeline v2 failed: " << result->error_code << "\n";
    return result;
  }

  const size_t pixelCount =
      static_cast<size_t>(pipeline.width) * static_cast<size_t>(pipeline.height);
  // 24MP RGBA: ~96 MB. W7-A: pool-backed (RgbaPool) to avoid a full re-fault
  // of the buffer on every warm decode. Ownership stays with the pool and is
  // reclaimed by dng_free_result() / dng_free_rgba_buffer() /
  // dng_free_halide_buffer() (which consult the pool before delete[]). No ABI
  // change — DngResult layout is untouched.
  uint8_t *rgba = rgbaPool().acquire(pixelCount * 4);
  rgb_to_rgba_neon(pipeline.rgb_ptr, rgba, pixelCount);

  result->rgba_data = rgba;
  result->width = static_cast<int32_t>(pipeline.width);
  result->height = static_cast<int32_t>(pipeline.height);
  result->error_code = 0;
  result->decode_ms = pipeline.decode_ms;
  result->process_ms = pipeline.process_ms;

  // [FFI] Success stderr removed (W6-5 / TD-23): timing fields on result
  // struct are sufficient; always-on stderr polluted Xcode console and
  // CI timing parsers. Re-enable via DiagnosticConfig in the future if
  // a dedicated debug channel is needed.
  return result;
}

FFI_EXPORT int32_t dng_decoder_warmup_for_size(int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    return -1;
  }
  return dng_pipeline_v2_warmup_for_size(width, height) ? 0 : -2;
}

FFI_EXPORT int dng_extract_preview_jpeg(const char *filePath, uint8_t **outBuffer,
                                      int *outSize) {
  if (!filePath || !outBuffer || !outSize)
    return 5; // INVALID_ARGUMENT

  try {
    dng_host host;
    dng_file_stream stream(filePath);
    dng_info info;
    info.Parse(host, stream);
    info.PostParse(host);

    int bestPreviewIfd = -1;
    uint32 bestPreviewWidth = 0;
    for (uint32 i = 0; i < info.fIFDCount; ++i) {
      const dng_ifd &ifd = *info.fIFD[i];
      if (ifd.fCompression == 7 && ifd.fPhotometricInterpretation == 6 &&
          ifd.fNewSubFileType == 1 && ifd.fImageWidth > bestPreviewWidth) {
        bestPreviewWidth = ifd.fImageWidth;
        bestPreviewIfd = static_cast<int>(i);
      }
    }

    if (bestPreviewIfd < 0)
      return 1;

    const dng_ifd &ifd = *info.fIFD[bestPreviewIfd];
    const uint64 offset = ifd.fTileOffset[0];
    const uint32 byteCount = ifd.fTileByteCount[0];
    if (byteCount == 0)
      return 1;

    *outSize = static_cast<int>(byteCount);
    *outBuffer = new uint8_t[*outSize];
    stream.SetReadPosition(offset);
    stream.Get(*outBuffer, byteCount);
    return 0;
  } catch (const dng_exception &e) {
    return e.ErrorCode();
  } catch (...) {
    return -100;
  }
}

FFI_EXPORT void dng_free_buffer(uint8_t *buffer) {
  if (buffer)
    delete[] buffer;
}

FFI_EXPORT void dng_free_result(DngResult *result) {
  if (!result)
    return;
  // Frees rgba_data when non-NULL, then frees the struct itself.
  // Zero-copy callers MUST clear result->rgba_data before calling this
  // (see _decodeZeroCopy in dng_decoder_service.dart) to avoid double-free.
  // W7-A: rgba_data is pool-backed; reclaim via the pool, only delete[] if it
  // was not a pool buffer.
  if (result->rgba_data) {
    if (!rgbaPool().release(result->rgba_data)) {
      delete[] result->rgba_data;
    }
    result->rgba_data = nullptr;
  }
  std::free(result);
}

FFI_EXPORT void dng_free_rgba_buffer(void *ptr) {
  if (!ptr)
    return;
  // W7-A: reclaim to the pool when applicable, else delete[].
  uint8_t *p = static_cast<uint8_t *>(ptr);
  if (!rgbaPool().release(p)) {
    delete[] p;
  }
}

FFI_EXPORT void dng_free_halide_buffer(void *ptr) {
  dng_free_rgba_buffer(ptr);
}

} // extern "C"
