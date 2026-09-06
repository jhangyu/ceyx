// Task 1 (native-rotation spec, native-rotation-contract.md): correctness
// coverage for ceyx_orient_rgba / ceyx_orientation_transposes against an
// INDEPENDENT naive per-pixel reference implementation written in this file.
// The reference deliberately shares no code with native/src/ffi/ceyx_orient.cpp
// or with native/tests/probe_strided_output.cpp's cpuReferenceOrient.
//
// Semantics under test mirror Halcyon's
// lib/services/image_pipeline/exif_orientation.dart exifTransformFor table:
// quarter-turns-clockwise applied first, then an optional horizontal mirror.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ceyx_orient.h"

namespace {

int g_failures = 0;

void Report(const char *name, bool ok, const char *detail) {
  std::printf("[CeyxOrient] %s %s -> %s\n", name, detail, ok ? "PASS" : "FAIL");
  if (!ok) ++g_failures;
}

// --- independent naive per-pixel reference --------------------------------
//
// Written directly from the EXIF orientation definitions (quarter-turns CW,
// then horizontal mirror), NOT by transcribing ceyx_orient.cpp's tiled/
// in-place implementation. Always allocates a fresh output buffer; never
// operates in place, so it cannot share any aliasing logic with the
// production code either.
struct NaiveResult {
  std::vector<uint8_t> pixels;
  int32_t width = 0;
  int32_t height = 0;
};

// Rotates (x, y) in a width x height image by `quarterTurnsCw` quarter turns
// clockwise, returning the SOURCE coordinate for a given DESTINATION
// coordinate in the rotated image, plus the rotated image's dimensions.
NaiveResult NaiveOrient(const uint8_t *src, int32_t width, int32_t height,
                        int32_t orientation) {
  int32_t o = orientation;
  if (o < 1 || o > 8) o = 1;

  // (quarterTurnsCw, mirrored) exactly as exif_orientation.dart's table.
  int32_t quarter_turns = 0;
  bool mirrored = false;
  switch (o) {
    case 2: quarter_turns = 0; mirrored = true; break;
    case 3: quarter_turns = 2; mirrored = false; break;
    case 4: quarter_turns = 2; mirrored = true; break;
    case 5: quarter_turns = 1; mirrored = true; break;
    case 6: quarter_turns = 1; mirrored = false; break;
    case 7: quarter_turns = 3; mirrored = true; break;
    case 8: quarter_turns = 3; mirrored = false; break;
    default: quarter_turns = 0; mirrored = false; break;
  }

  const int32_t w = width;
  const int32_t h = height;

  // Step 1: rotate quarter_turns times, 90 CW each time, computed pixel by
  // pixel with a fresh buffer per rotation (deliberately not clever).
  std::vector<uint8_t> cur(src, src + static_cast<size_t>(w) * h * 4);
  int32_t cw = w, ch = h;
  for (int32_t t = 0; t < quarter_turns; ++t) {
    const int32_t nw = ch;
    const int32_t nh = cw;
    std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
    for (int32_t y = 0; y < nh; ++y) {
      for (int32_t x = 0; x < nw; ++x) {
        // Rotating a (cw x ch) image 90 deg CW: destination (x,y) came from
        // source (y, ch-1-x).
        const int32_t sx = y;
        const int32_t sy = ch - 1 - x;
        const size_t di = (static_cast<size_t>(y) * nw + x) * 4;
        const size_t si = (static_cast<size_t>(sy) * cw + sx) * 4;
        std::memcpy(&next[di], &cur[si], 4);
      }
    }
    cur = std::move(next);
    cw = nw;
    ch = nh;
  }

  // Step 2: mirror horizontally, if requested.
  if (mirrored) {
    std::vector<uint8_t> next(cur.size());
    for (int32_t y = 0; y < ch; ++y) {
      for (int32_t x = 0; x < cw; ++x) {
        const int32_t sx = cw - 1 - x;
        const size_t di = (static_cast<size_t>(y) * cw + x) * 4;
        const size_t si = (static_cast<size_t>(y) * cw + sx) * 4;
        std::memcpy(&next[di], &cur[si], 4);
      }
    }
    cur = std::move(next);
  }

  NaiveResult result;
  result.pixels = std::move(cur);
  result.width = cw;
  result.height = ch;
  return result;
}

std::vector<uint8_t> MakeGradient(int32_t width, int32_t height) {
  std::vector<uint8_t> buf(static_cast<size_t>(width) * height * 4);
  for (int32_t y = 0; y < height; ++y) {
    for (int32_t x = 0; x < width; ++x) {
      const size_t i = (static_cast<size_t>(y) * width + x) * 4;
      buf[i + 0] = static_cast<uint8_t>((x * 37 + y * 91) & 0xFF);
      buf[i + 1] = static_cast<uint8_t>((x * 13 + y * 5) & 0xFF);
      buf[i + 2] = static_cast<uint8_t>((x * 251 + y * 17) & 0xFF);
      buf[i + 3] = static_cast<uint8_t>((x + y * 3) & 0xFF);
    }
  }
  return buf;
}

// AC-1.1 / AC-1.2: one named case per orientation, output equal to the
// independent naive reference, at multiple sizes.
bool CheckOneOrientation(int32_t orientation, int32_t width, int32_t height,
                          const char *sizeLabel) {
  const std::vector<uint8_t> src = MakeGradient(width, height);
  const NaiveResult expected = NaiveOrient(src.data(), width, height, orientation);

  const size_t cap = static_cast<size_t>(width) * height * 4;
  std::vector<uint8_t> dst(cap, 0xAA);
  int32_t ow = -1, oh = -1;
  const int32_t rc = ceyx_orient_rgba(src.data(), dst.data(), cap, width, height,
                                      orientation, &ow, &oh);

  char label[128];
  std::snprintf(label, sizeof(label), "orientation=%d size=%s", orientation, sizeLabel);

  bool ok = (rc == 0);
  ok = ok && (ow == expected.width) && (oh == expected.height);
  ok = ok && (dst == expected.pixels);
  Report("PerOrientationCorrectness", ok, label);
  return ok;
}

bool CheckOneOrientationInPlace(int32_t orientation, int32_t width, int32_t height,
                                const char *sizeLabel) {
  // Only meaningful for non-transposing orientations (1-4); the production
  // function refuses in-place aliasing for 5-8.
  const std::vector<uint8_t> src = MakeGradient(width, height);
  const NaiveResult expected = NaiveOrient(src.data(), width, height, orientation);

  std::vector<uint8_t> buf = src;
  int32_t ow = -1, oh = -1;
  const int32_t rc = ceyx_orient_rgba(buf.data(), buf.data(), buf.size(), width,
                                      height, orientation, &ow, &oh);

  char label[128];
  std::snprintf(label, sizeof(label), "in-place orientation=%d size=%s", orientation,
                sizeLabel);

  bool ok = (rc == 0) && (ow == expected.width) && (oh == expected.height) &&
            (buf == expected.pixels);
  Report("InPlaceCorrectness", ok, label);
  return ok;
}

// AC-1.4: orientation N then its algebraic inverse returns the original.
int32_t InverseOf(int32_t orientation) {
  switch (orientation) {
    case 6: return 8;
    case 8: return 6;
    default: return orientation;  // 1,2,3,4,5,7 are all self-inverse
  }
}

bool CheckRoundTrip(int32_t orientation, int32_t width, int32_t height,
                    const char *sizeLabel) {
  const std::vector<uint8_t> src = MakeGradient(width, height);

  const size_t cap1 = static_cast<size_t>(width) * height * 4;
  std::vector<uint8_t> stage1(cap1, 0);
  int32_t w1 = -1, h1 = -1;
  int32_t rc1 = ceyx_orient_rgba(src.data(), stage1.data(), cap1, width, height,
                                 orientation, &w1, &h1);

  const int32_t inv = InverseOf(orientation);
  const size_t cap2 = static_cast<size_t>(w1) * h1 * 4;
  std::vector<uint8_t> stage2(cap2, 0);
  int32_t w2 = -1, h2 = -1;
  int32_t rc2 = ceyx_orient_rgba(stage1.data(), stage2.data(), cap2, w1, h1, inv,
                                 &w2, &h2);

  char label[128];
  std::snprintf(label, sizeof(label), "orientation=%d size=%s", orientation, sizeLabel);

  bool ok = (rc1 == 0) && (rc2 == 0) && (w2 == width) && (h2 == height) &&
            (stage2 == src);
  Report("RoundTrip", ok, label);
  return ok;
}

bool CheckTransposesPredicate() {
  bool ok = true;
  for (int32_t v = -3; v <= 12; ++v) {
    const int32_t expected = (v == 5 || v == 6 || v == 7 || v == 8) ? 1 : 0;
    const int32_t got = ceyx_orientation_transposes(v);
    if (got != expected) {
      char detail[64];
      std::snprintf(detail, sizeof(detail), "v=%d expected=%d got=%d", v, expected, got);
      Report("TransposesPredicate", false, detail);
      ok = false;
    }
  }
  if (ok) Report("TransposesPredicate", true, "[-3,12] all match");
  return ok;
}

bool CheckOverlapRefusal() {
  const int32_t width = 8, height = 8;
  std::vector<uint8_t> buf = MakeGradient(width, height);
  const std::vector<uint8_t> saved = buf;

  int32_t ow = 999, oh = 999;
  const int32_t rc = ceyx_orient_rgba(buf.data(), buf.data(), buf.size(), width,
                                      height, 6, &ow, &oh);

  const bool ok = (rc == kCeyxOrientErrOverlap) && (buf == saved) && (ow == 0) &&
                  (oh == 0);
  Report("OverlapRefusal", ok, "orientation=6 src==dst");
  return ok;
}

bool CheckBadArgsRefusal() {
  bool ok = true;

  {
    std::vector<uint8_t> dst(4 * 4 * 4);
    int32_t ow = 999, oh = 999;
    const int32_t rc = ceyx_orient_rgba(nullptr, dst.data(), dst.size(), 4, 4, 1,
                                        &ow, &oh);
    const bool one_ok = (rc == kCeyxOrientErrBadArgs) && (ow == 0) && (oh == 0);
    Report("BadArgsRefusal", one_ok, "null src");
    ok = ok && one_ok;
  }
  {
    std::vector<uint8_t> src(4 * 4 * 4);
    int32_t ow = 999, oh = 999;
    const int32_t rc = ceyx_orient_rgba(src.data(), nullptr, 4 * 4 * 4, 4, 4, 1,
                                        &ow, &oh);
    const bool one_ok = (rc == kCeyxOrientErrBadArgs) && (ow == 0) && (oh == 0);
    Report("BadArgsRefusal", one_ok, "null dst");
    ok = ok && one_ok;
  }
  {
    std::vector<uint8_t> src(4 * 4 * 4), dst(4 * 4 * 4);
    int32_t ow = 999, oh = 999;
    const int32_t rc = ceyx_orient_rgba(src.data(), dst.data(), dst.size(), 0, 4, 1,
                                        &ow, &oh);
    const bool one_ok = (rc == kCeyxOrientErrBadArgs) && (ow == 0) && (oh == 0);
    Report("BadArgsRefusal", one_ok, "width=0");
    ok = ok && one_ok;
  }
  {
    std::vector<uint8_t> src(4 * 4 * 4), dst(4 * 4 * 4);
    int32_t ow = 999, oh = 999;
    const int32_t rc = ceyx_orient_rgba(src.data(), dst.data(), dst.size(), 4, -1, 1,
                                        &ow, &oh);
    const bool one_ok = (rc == kCeyxOrientErrBadArgs) && (ow == 0) && (oh == 0);
    Report("BadArgsRefusal", one_ok, "height=-1");
    ok = ok && one_ok;
  }
  {
    std::vector<uint8_t> src(4 * 4 * 4), dst(4 * 4 * 4 - 1);
    int32_t ow = 999, oh = 999;
    const int32_t rc = ceyx_orient_rgba(src.data(), dst.data(), dst.size(), 4, 4, 1,
                                        &ow, &oh);
    const bool one_ok = (rc == kCeyxOrientErrBadArgs) && (ow == 0) && (oh == 0);
    Report("BadArgsRefusal", one_ok, "dst_capacity too small");
    ok = ok && one_ok;
  }
  return ok;
}

}  // namespace

int main() {
  // AC-1.1/AC-1.2: 8 named orientation cases at a baseline square size.
  for (int32_t o = 1; o <= 8; ++o) {
    CheckOneOrientation(o, 16, 16, "16x16");
  }

  // In-place variants for the non-transposing orientations (1-4).
  for (int32_t o = 1; o <= 4; ++o) {
    CheckOneOrientationInPlace(o, 16, 16, "16x16");
  }

  // AC-1.3: odd / degenerate extents, all 8 orientations each.
  const struct { int32_t w, h; const char *label; } kOddSizes[] = {
      {3, 2, "3x2"}, {2, 3, "2x3"}, {1, 7, "1x7"}, {7, 1, "7x1"}, {5, 5, "5x5"},
  };
  for (const auto &sz : kOddSizes) {
    for (int32_t o = 1; o <= 8; ++o) {
      CheckOneOrientation(o, sz.w, sz.h, sz.label);
    }
  }

  // AC-1.4: round-trip N then inverse(N), all 8 values, at an oblong size
  // (so a transpose bug that only shows on square images can't hide).
  for (int32_t o = 1; o <= 8; ++o) {
    CheckRoundTrip(o, 5, 7, "5x7");
  }

  // AC-1.5: ceyx_orientation_transposes over [-3, 12].
  CheckTransposesPredicate();

  // AC-1.6: overlap refusal (transposing case, src==dst) leaves the buffer
  // byte-identical to a saved copy and zeroes the output extent.
  CheckOverlapRefusal();

  // AC-2.x-adjacent: bad-args refusals, all before any write, all zeroing
  // the output extent. (Kept alongside Task 1's own tests since this TU
  // owns ceyx_orient_rgba's argument-validation contract.)
  CheckBadArgsRefusal();

  if (g_failures == 0) {
    std::printf("[CeyxOrient] All tests passed! (0 failures)\n");
    return 0;
  }
  std::printf("[CeyxOrient] %d failure(s)\n", g_failures);
  return 1;
}
