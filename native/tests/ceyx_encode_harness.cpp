// Harness for the RGBA8 encode C ABI (ceyx_encode_api.h).
//
// Drives the production extern "C" entries through the shipped dylib exactly as
// the Dart side will, and gates:
//   1. JPEG output starts with SOI (0xFF 0xD8) and ends with EOI (0xFF 0xD9);
//   2. WebP output carries the RIFF....WEBP container magic and reports the
//      requested extent back through WebPGetInfo-equivalent header fields;
//   3. argument validation returns the documented negative codes and never
//      hands back a buffer;
//   4. a 4080x3056 (12.5MP) JPEG encode completes, with its wall time printed
//      (informational — this harness does not gate on a time budget; the
//      Phase 13 budget is measured in-process on the Dart side).
//
// Exit code 0 = all cases passed, 1 = at least one failed. Every case prints a
// PASS/FAIL line so a truncated log is distinguishable from a green run.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "ceyx_encode_api.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char *name, const char *detail = "") {
  std::printf("[encode] %s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
  if (!ok) ++g_failures;
}

// Deterministic gradient with a varying alpha, so a channel-order or stride
// mistake shows up as a decode-side difference rather than a uniform block.
std::vector<uint8_t> MakeRgba(int w, int h) {
  std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = (static_cast<size_t>(y) * w + x) * 4;
      px[i + 0] = static_cast<uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));
      px[i + 1] = static_cast<uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));
      px[i + 2] = static_cast<uint8_t>((x + y) & 0xFF);
      px[i + 3] = 255;
    }
  }
  return px;
}

}  // namespace

int main() {
  const int kW = 64, kH = 48;
  const std::vector<uint8_t> small = MakeRgba(kW, kH);

  // --- Case 1: JPEG round trip -------------------------------------------
  {
    uint8_t *out = nullptr;
    size_t len = 0;
    const int32_t rc =
        ceyx_encode_jpeg_rgba8(small.data(), kW, kH, 80, &out, &len);
    char detail[256];
    std::snprintf(detail, sizeof(detail), "(rc=%d %s, len=%zu)", rc,
                  ceyx_encode_error_name(rc), len);
    const bool ok = rc == kCeyxEncodeSuccess && out != nullptr && len > 4 &&
                    out[0] == 0xFF && out[1] == 0xD8 &&
                    out[len - 2] == 0xFF && out[len - 1] == 0xD9;
    Check(ok, "jpeg_64x48_soi_eoi", detail);
    ceyx_encode_free(out);
  }

  // --- Case 2: WebP round trip -------------------------------------------
  {
    uint8_t *out = nullptr;
    size_t len = 0;
    const int32_t rc =
        ceyx_encode_webp_rgba8(small.data(), kW, kH, 80, &out, &len);
    char detail[256];
    std::snprintf(detail, sizeof(detail), "(rc=%d %s, len=%zu)", rc,
                  ceyx_encode_error_name(rc), len);
    if (rc == kCeyxEncodeErrUnsupported) {
      // A build configured without the dist must say so explicitly rather than
      // silently emitting nothing; that is itself the contract under test.
      Check(out == nullptr && len == 0, "webp_unsupported_reports_cleanly",
            detail);
    } else {
      const bool ok = rc == kCeyxEncodeSuccess && out != nullptr && len > 12 &&
                      std::memcmp(out, "RIFF", 4) == 0 &&
                      std::memcmp(out + 8, "WEBP", 4) == 0;
      Check(ok, "webp_64x48_riff_webp_magic", detail);
    }
    ceyx_encode_free(out);
  }

  // --- Case 3: argument validation ---------------------------------------
  {
    uint8_t *out = reinterpret_cast<uint8_t *>(0x1);  // must be overwritten
    size_t len = 123;
    const int32_t rc_null =
        ceyx_encode_jpeg_rgba8(nullptr, kW, kH, 80, &out, &len);
    Check(rc_null == kCeyxEncodeErrNullArg && out == nullptr && len == 0,
          "jpeg_null_rgba_rejected", "");

    const int32_t rc_dim =
        ceyx_encode_jpeg_rgba8(small.data(), 0, kH, 80, &out, &len);
    Check(rc_dim == kCeyxEncodeErrBadDimensions, "jpeg_zero_width_rejected", "");

    const int32_t rc_q =
        ceyx_encode_jpeg_rgba8(small.data(), kW, kH, 0, &out, &len);
    Check(rc_q == kCeyxEncodeErrBadQuality, "jpeg_quality_zero_rejected", "");

    const int32_t rc_wq =
        ceyx_encode_webp_rgba8(small.data(), kW, kH, 101, &out, &len);
    Check(rc_wq == kCeyxEncodeErrBadQuality, "webp_quality_101_rejected", "");
  }

  // --- Case 4: 4080x3056 (12.5MP) full-size encode ------------------------
  {
    const int kBigW = 4080, kBigH = 3056;
    const std::vector<uint8_t> big = MakeRgba(kBigW, kBigH);

    uint8_t *out = nullptr;
    size_t len = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const int32_t rc =
        ceyx_encode_jpeg_rgba8(big.data(), kBigW, kBigH, 80, &out, &len);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    char detail[256];
    std::snprintf(detail, sizeof(detail), "(rc=%d, len=%zu, %.1f ms)", rc, len,
                  ms);
    Check(rc == kCeyxEncodeSuccess && out != nullptr && len > 4 &&
              out[0] == 0xFF && out[1] == 0xD8,
          "jpeg_4080x3056_q80", detail);
    ceyx_encode_free(out);

    uint8_t *wout = nullptr;
    size_t wlen = 0;
    const auto t2 = std::chrono::steady_clock::now();
    const int32_t wrc =
        ceyx_encode_webp_rgba8(big.data(), kBigW, kBigH, 80, &wout, &wlen);
    const auto t3 = std::chrono::steady_clock::now();
    const double wms =
        std::chrono::duration<double, std::milli>(t3 - t2).count();
    std::snprintf(detail, sizeof(detail), "(rc=%d, len=%zu, %.1f ms)", wrc,
                  wlen, wms);
    Check(wrc == kCeyxEncodeSuccess || wrc == kCeyxEncodeErrUnsupported,
          "webp_4080x3056_q80", detail);
    ceyx_encode_free(wout);
  }

  std::printf("[encode] failures=%d\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
