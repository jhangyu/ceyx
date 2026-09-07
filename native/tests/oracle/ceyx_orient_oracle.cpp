#include "ceyx_orient.h"

#include <algorithm>
#include <cstring>
#include <vector>

// Standalone TU: no Halide/DNG-SDK/LibRaw dependency (spec Task 1 constraint).
// Only <cstdint>/<cstddef> (via the header)/<cstring>/<algorithm> are used;
// <vector> below is used solely for the one-row scratch in case 4 and is a
// standard-library container, not a project dependency.

namespace {

constexpr size_t kTileSize = 32;

// Byte-range overlap test. `a`/`b` are treated as [a, a+la) / [b, b+lb).
inline bool RangesOverlap(const void *a, size_t la, const void *b, size_t lb) {
  const auto pa = reinterpret_cast<uintptr_t>(a);
  const auto pb = reinterpret_cast<uintptr_t>(b);
  return pa < pb + lb && pb < pa + la;
}

inline int32_t NormalizeOrientation(int32_t o) {
  return (o >= 1 && o <= 8) ? o : 1;
}

struct OrientParams {
  int32_t quarter_turns_cw;
  bool mirrored;
};

// Mirrors exif_orientation.dart's exifTransformFor table exactly.
inline OrientParams ParamsFor(int32_t o) {
  switch (o) {
    case 2:
      return {0, true};
    case 3:
      return {2, false};
    case 4:
      return {2, true};
    case 5:
      return {1, true};
    case 6:
      return {1, false};
    case 7:
      return {3, true};
    case 8:
      return {3, false};
    default:
      // 1, and anything normalized to 1.
      return {0, false};
  }
}

inline void ZeroOut(int32_t *out_width, int32_t *out_height) {
  if (out_width) *out_width = 0;
  if (out_height) *out_height = 0;
}

}  // namespace

int32_t ceyx_orientation_transposes(int32_t exif_orientation) {
  const OrientParams p = ParamsFor(NormalizeOrientation(exif_orientation));
  return (p.quarter_turns_cw % 2 == 1) ? 1 : 0;
}

int32_t ceyx_orient_rgba(const uint8_t *src, uint8_t *dst, size_t dst_capacity,
                          int32_t width, int32_t height,
                          int32_t exif_orientation, int32_t *out_width,
                          int32_t *out_height) {
  if (src == nullptr || dst == nullptr || width <= 0 || height <= 0) {
    ZeroOut(out_width, out_height);
    return kCeyxOrientErrBadArgs;
  }

  const size_t w = static_cast<size_t>(width);
  const size_t h = static_cast<size_t>(height);
  const size_t frame_bytes = w * h * 4;

  if (dst_capacity < frame_bytes) {
    ZeroOut(out_width, out_height);
    return kCeyxOrientErrBadArgs;
  }

  const int32_t orientation = NormalizeOrientation(exif_orientation);
  const OrientParams p = ParamsFor(orientation);
  const bool transposes = (p.quarter_turns_cw % 2 == 1);
  const bool same = (src == dst);
  const bool overlap =
      !same && RangesOverlap(src, frame_bytes, dst, frame_bytes);

  // Transposing cases (5-8) require disjoint, non-aliased buffers. Every
  // other overlap (partial, non-identical-pointer overlap on any case, or
  // any overlap at all on a transposing case) is refused before any write;
  // src == dst is legal ONLY for the non-transposing cases (1-4).
  if ((transposes && (same || overlap)) || (!transposes && overlap)) {
    ZeroOut(out_width, out_height);
    return kCeyxOrientErrOverlap;
  }

  const int32_t ow = transposes ? height : width;
  const int32_t oh = transposes ? width : height;

  switch (orientation) {
    case 1: {
      // Identity: no pass at all when src == dst; otherwise a plain copy.
      if (!same) std::memcpy(dst, src, frame_bytes);
      break;
    }
    case 2: {
      // Mirror horizontal: per-row byte-quad reverse. In-place capable.
      if (same) {
        for (size_t y = 0; y < h; ++y) {
          uint8_t *row = dst + y * w * 4;
          for (size_t x = 0; x < w / 2; ++x) {
            uint8_t *a = row + x * 4;
            uint8_t *b = row + (w - 1 - x) * 4;
            uint8_t tmp[4];
            std::memcpy(tmp, a, 4);
            std::memcpy(a, b, 4);
            std::memcpy(b, tmp, 4);
          }
        }
      } else {
        for (size_t y = 0; y < h; ++y) {
          const uint8_t *srow = src + y * w * 4;
          uint8_t *drow = dst + y * w * 4;
          for (size_t x = 0; x < w; ++x) {
            std::memcpy(drow + x * 4, srow + (w - 1 - x) * 4, 4);
          }
        }
      }
      break;
    }
    case 3: {
      // Rotate 180: whole-buffer pixel reverse (swap i with N-1-i).
      // In-place capable.
      const size_t n = w * h;
      if (same) {
        for (size_t i = 0; i < n / 2; ++i) {
          uint8_t *a = dst + i * 4;
          uint8_t *b = dst + (n - 1 - i) * 4;
          uint8_t tmp[4];
          std::memcpy(tmp, a, 4);
          std::memcpy(a, b, 4);
          std::memcpy(b, tmp, 4);
        }
      } else {
        for (size_t i = 0; i < n; ++i) {
          std::memcpy(dst + (n - 1 - i) * 4, src + i * 4, 4);
        }
      }
      break;
    }
    case 4: {
      // Flip vertical (rotate180 then mirror horizontal composes to this):
      // row swap top<->bottom. In-place capable with one row of scratch.
      if (same) {
        std::vector<uint8_t> scratch(w * 4);
        for (size_t y = 0; y < h / 2; ++y) {
          uint8_t *top = dst + y * w * 4;
          uint8_t *bottom = dst + (h - 1 - y) * w * 4;
          std::memcpy(scratch.data(), top, w * 4);
          std::memcpy(top, bottom, w * 4);
          std::memcpy(bottom, scratch.data(), w * 4);
        }
      } else {
        for (size_t y = 0; y < h; ++y) {
          std::memcpy(dst + y * w * 4, src + (h - 1 - y) * w * 4, w * 4);
        }
      }
      break;
    }
    case 5: {
      // Transpose: dst(x,y) = src(y,x). Output dims (ow=h, oh=w).
      for (size_t oy0 = 0; oy0 < w; oy0 += kTileSize) {
        const size_t oy_end = std::min(oy0 + kTileSize, w);
        for (size_t ox0 = 0; ox0 < h; ox0 += kTileSize) {
          const size_t ox_end = std::min(ox0 + kTileSize, h);
          for (size_t oy = oy0; oy < oy_end; ++oy) {
            for (size_t ox = ox0; ox < ox_end; ++ox) {
              const size_t ix = oy;
              const size_t iy = ox;
              std::memcpy(dst + (oy * h + ox) * 4, src + (iy * w + ix) * 4, 4);
            }
          }
        }
      }
      break;
    }
    case 6: {
      // Rotate 90 CW: dst(x,y) = src(y, h-1-x). Output dims (ow=h, oh=w).
      for (size_t oy0 = 0; oy0 < w; oy0 += kTileSize) {
        const size_t oy_end = std::min(oy0 + kTileSize, w);
        for (size_t ox0 = 0; ox0 < h; ox0 += kTileSize) {
          const size_t ox_end = std::min(ox0 + kTileSize, h);
          for (size_t oy = oy0; oy < oy_end; ++oy) {
            for (size_t ox = ox0; ox < ox_end; ++ox) {
              const size_t ix = oy;
              const size_t iy = h - 1 - ox;
              std::memcpy(dst + (oy * h + ox) * 4, src + (iy * w + ix) * 4, 4);
            }
          }
        }
      }
      break;
    }
    case 7: {
      // Transverse (rotate 270 CW then mirror horizontal):
      // dst(x,y) = src(w-1-y, h-1-x). Output dims (ow=h, oh=w).
      for (size_t oy0 = 0; oy0 < w; oy0 += kTileSize) {
        const size_t oy_end = std::min(oy0 + kTileSize, w);
        for (size_t ox0 = 0; ox0 < h; ox0 += kTileSize) {
          const size_t ox_end = std::min(ox0 + kTileSize, h);
          for (size_t oy = oy0; oy < oy_end; ++oy) {
            for (size_t ox = ox0; ox < ox_end; ++ox) {
              const size_t ix = w - 1 - oy;
              const size_t iy = h - 1 - ox;
              std::memcpy(dst + (oy * h + ox) * 4, src + (iy * w + ix) * 4, 4);
            }
          }
        }
      }
      break;
    }
    case 8: {
      // Rotate 270 CW: dst(x,y) = src(w-1-y, x). Output dims (ow=h, oh=w).
      for (size_t oy0 = 0; oy0 < w; oy0 += kTileSize) {
        const size_t oy_end = std::min(oy0 + kTileSize, w);
        for (size_t ox0 = 0; ox0 < h; ox0 += kTileSize) {
          const size_t ox_end = std::min(ox0 + kTileSize, h);
          for (size_t oy = oy0; oy < oy_end; ++oy) {
            for (size_t ox = ox0; ox < ox_end; ++ox) {
              const size_t ix = w - 1 - oy;
              const size_t iy = ox;
              std::memcpy(dst + (oy * h + ox) * 4, src + (iy * w + ix) * 4, 4);
            }
          }
        }
      }
      break;
    }
    default:
      break;
  }

  if (out_width) *out_width = ow;
  if (out_height) *out_height = oh;
  return 0;
}
