/**
 * test_heif_color.cpp — H1 known-answer colour gate for the HEIF route.
 *
 * Why this exists (spec section 7.5): the S4 CFA gate validates the RAW
 * demosaic/white-balance/colour-transform pipeline from a Bayer sample. HEIC
 * decoding is YUV 4:2:0 -> RGB with an NCLX/ICC colour description and shares
 * no code with it, so S4 says nothing about HEIC. A YUV range or
 * matrix-coefficient mistake (full vs limited range, BT.601 vs BT.709)
 * produces an image that is obviously THERE and subtly wrong — a smoke test
 * passes it and a reference comparison catches it.
 *
 * Usage:
 *   test_heif_color <sample.heic> <reference.rgba> [--max-mae N]
 *
 * Output contract (deliberately identical in shape to test_cfa_color's):
 * one "[HEIF COLOR] ... PASS|FAIL|SKIP" line; exit 0 ONLY on PASS.
 * A missing fixture prints SKIP and exits non-zero — a gate that skips
 * silently produces a report indistinguishable from a full run.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "heif_api.h"

namespace {

struct Reference {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

// Sidecar layout, written by native/tests/data/png_to_rgba.py:
//   magic "H1RGBA\0\0" | uint32 width | uint32 height | RGBA8 pixels
bool loadReference(const char *path, Reference *out, const char **why) {
  std::FILE *fh = std::fopen(path, "rb");
  if (!fh) {
    *why = "reference file not found";
    return false;
  }
  char magic[8] = {0};
  uint32_t header[2] = {0, 0};
  bool ok = std::fread(magic, 1, 8, fh) == 8 &&
            std::memcmp(magic, "H1RGBA\0\0", 8) == 0 &&
            std::fread(header, sizeof(uint32_t), 2, fh) == 2;
  if (!ok) {
    std::fclose(fh);
    *why = "reference is not an H1RGBA sidecar";
    return false;
  }
  out->width = header[0];
  out->height = header[1];
  const size_t expected =
      static_cast<size_t>(out->width) * out->height * 4;
  out->rgba.resize(expected);
  const size_t read = std::fread(out->rgba.data(), 1, expected, fh);
  std::fclose(fh);
  if (read != expected) {
    *why = "reference is truncated";
    return false;
  }
  return true;
}

// Mean absolute difference over R, G and B. Alpha is ignored: HEIC has no
// alpha here and both sides synthesise 255.
double meanAbsoluteError(const uint8_t *a, const uint8_t *b, size_t pixels) {
  double sum = 0.0;
  for (size_t i = 0; i < pixels; ++i) {
    const size_t o = i * 4;
    sum += std::fabs(static_cast<double>(a[o + 0]) - b[o + 0]);
    sum += std::fabs(static_cast<double>(a[o + 1]) - b[o + 1]);
    sum += std::fabs(static_cast<double>(a[o + 2]) - b[o + 2]);
  }
  return pixels == 0 ? 0.0 : sum / (static_cast<double>(pixels) * 3.0);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: test_heif_color <sample.heic> <reference.rgba> "
                 "[--max-mae N]\n");
    return 2;
  }
  const char *heicPath = argv[1];
  const char *refPath = argv[2];
  double maxMae = 2.0;  // spec section 7.5: MAE <= 2/255

  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--max-mae") == 0 && i + 1 < argc) {
      maxMae = std::atof(argv[++i]);
    } else {
      std::fprintf(stderr, "[HEIF COLOR] FAIL: unknown argument %s\n", argv[i]);
      return 2;
    }
  }

  Reference reference;
  const char *why = "";
  if (!loadReference(refPath, &reference, &why)) {
    // SKIP, and still non-zero: an absent fixture must not read as a pass.
    std::printf("[HEIF COLOR] SKIP: %s (%s)\n", refPath, why);
    return 1;
  }

  HeifResult result;
  const int32_t rc = heif_decode_rgba(heicPath, 0, &result);
  if (rc != 0 || !result.rgba) {
    std::printf("[HEIF COLOR] FAIL: decode error_code=%d for %s\n", rc,
                heicPath);
    heif_release(&result);
    return 1;
  }

  if (result.width != reference.width || result.height != reference.height) {
    // Not a resize-and-compare: a dimension mismatch means the container
    // transform (irot/imir) was handled differently from ImageIO, which is a
    // real defect in the heif_api.h orientation contract.
    std::printf("[HEIF COLOR] FAIL: size %ux%u != reference %ux%u for %s\n",
                result.width, result.height, reference.width, reference.height,
                heicPath);
    heif_release(&result);
    return 1;
  }

  const size_t pixels =
      static_cast<size_t>(result.width) * static_cast<size_t>(result.height);
  const double mae =
      meanAbsoluteError(result.rgba, reference.rgba.data(), pixels);
  const bool pass = mae <= maxMae;

  std::printf("[HEIF COLOR] file=%s size=%ux%u pixels=%zu MAE=%.4f max=%.4f "
              "[%s]\n",
              heicPath, result.width, result.height, pixels, mae, maxMae,
              pass ? "PASS" : "FAIL");

  heif_release(&result);
  return pass ? 0 : 1;
}
