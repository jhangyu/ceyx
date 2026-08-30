// HEIC + AVIF round-trip and encode-contract gate, driven through the PUBLIC C
// ABI (the reason dng_ffi_harness exists: an internal-function test proves
// nothing about what Dart actually calls).
//
// Deliberate deviations from the plan's draft, each with its reason:
//
//  1. The 12.5 MP latency measurement is behind `--latency` rather than always
//     on. A 4080x3056 AVIF encode through libaom is minutes of wall clock; a
//     gate that takes minutes stops being run, and the plan is explicit that
//     these numbers are RECORDED, not gated on (spec R-b). The default run is
//     the correctness gate; the measurement is a separate, declared invocation.
//     A skipped measurement PRINTS that it was skipped -- a skipped case and a
//     passing one must never look the same in a log.
//
//  2. EXIF is checked by asserting the exact TIFF block appears byte-identically
//     in the produced file, not by re-reading it with libheif. The codec test
//     targets are given only ${INC_DIR} ${SRC_DIR} by native/cmake/tests.cmake,
//     with no libheif include directory, and that file belongs to another task.
//     A byte-exact search over the container is the stronger check anyway: it
//     cannot be satisfied by a reader and a writer sharing the same bug. An
//     independent libheif reader re-reads it out-of-tree at sign-off.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ceyx_encode_api.h"
#include "ceyx_still_api.h"

static int g_failures = 0;

static void check(bool ok, const char *what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}

static void check_eq(int32_t got, int32_t want, const char *what) {
  if (got != want) {
    std::printf("FAIL %s: got %d, want %d\n", what, got, want);
    ++g_failures;
  } else {
    std::printf("ok   %s == %d\n", what, got);
  }
}

// A deterministic gradient with a non-trivial alpha ramp: a flat colour would
// round-trip through a broken chroma path unnoticed.
static std::vector<uint8_t> MakeSource(int w, int h) {
  std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint8_t *p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = static_cast<uint8_t>((x * 255) / (w - 1));
      p[1] = static_cast<uint8_t>((y * 255) / (h - 1));
      p[2] = static_cast<uint8_t>(((x + y) * 255) / (w + h - 2));
      p[3] = static_cast<uint8_t>(255 - (x * 128) / (w - 1));
    }
  }
  return rgba;
}

// PSNR over the RGB channels only. Alpha is excluded on purpose: HEIC and AVIF
// carry alpha as a SEPARATE auxiliary image with its own quantiser, so folding
// it into one figure measures two codecs' losses as if they were one and makes
// a colour regression hideable behind a clean alpha plane (and vice versa).
static double PsnrRgb(const uint8_t *a, const uint8_t *b, size_t pixels) {
  double se = 0.0;
  for (size_t i = 0; i < pixels; ++i) {
    for (int c = 0; c < 3; ++c) {
      const double d = static_cast<double>(a[i * 4 + c]) -
                       static_cast<double>(b[i * 4 + c]);
      se += d * d;
    }
  }
  const double mse = se / static_cast<double>(pixels * 3);
  if (mse == 0.0) return 1000.0;
  return 10.0 * std::log10((255.0 * 255.0) / mse);
}

static CeyxEncodeOptions Opts(int quality, bool lossless) {
  CeyxEncodeOptions o;
  std::memset(&o, 0, sizeof(o));
  o.struct_size = sizeof(o);
  o.quality = quality;
  o.lossless = lossless ? 1 : 0;
  o.effort = 0;
  return o;
}

// A minimal but real little-endian TIFF/EXIF block: header + one IFD entry
// (Make = "CEYX"). Small enough to inline, structured enough that a reader
// which does not actually parse it will not accidentally pass.
static std::vector<uint8_t> MakeExif() {
  std::vector<uint8_t> e = {
      'I','I', 0x2A,0x00,            // little-endian TIFF magic
      0x08,0x00,0x00,0x00,           // offset to IFD0
      0x01,0x00,                     // 1 entry
      0x0F,0x01,                     // tag 0x010F = Make
      0x02,0x00,                     // type ASCII
      0x05,0x00,0x00,0x00,           // count 5
      0x1A,0x00,0x00,0x00,           // value offset
      0x00,0x00,0x00,0x00,           // next IFD = 0
      'C','E','Y','X','\0'
  };
  return e;
}

static std::vector<uint8_t> ReadFile(const char *path) {
  std::vector<uint8_t> bytes;
  FILE *f = std::fopen(path, "rb");
  if (!f) return bytes;
  uint8_t chunk[65536];
  size_t n = 0;
  while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
    bytes.insert(bytes.end(), chunk, chunk + n);
  }
  std::fclose(f);
  return bytes;
}

static bool Contains(const std::vector<uint8_t> &hay,
                     const std::vector<uint8_t> &needle) {
  if (needle.empty() || hay.size() < needle.size()) return false;
  const size_t last = hay.size() - needle.size();
  for (size_t i = 0; i <= last; ++i) {
    if (std::memcmp(hay.data() + i, needle.data(), needle.size()) == 0) {
      return true;
    }
  }
  return false;
}

static void RoundTrip(int32_t format, const char *label, const char *path) {
  const int w = 64, h = 48;
  const std::vector<uint8_t> src = MakeSource(w, h);
  const std::vector<uint8_t> exif = MakeExif();

  CeyxEncodeOptions opts = Opts(90, false);
  CeyxEncodeMetadata meta;
  std::memset(&meta, 0, sizeof(meta));
  meta.struct_size = sizeof(meta);
  meta.exif = exif.data();
  meta.exif_len = exif.size();

  uint8_t *buf = nullptr;
  size_t len = 0;
  const int32_t rc =
      ceyx_encode_rgba8(format, src.data(), w, h, &opts, &meta, &buf, &len);
  check_eq(rc, kCeyxEncodeSuccess, (std::string(label) + " encode rc").c_str());
  check(buf != nullptr && len > 0, (std::string(label) + " produced bytes").c_str());
  if (rc != kCeyxEncodeSuccess || !buf) return;

  FILE *f = std::fopen(path, "wb");
  if (!f) {
    check(false, (std::string(label) + " open output for write").c_str());
    ceyx_encode_free(buf);
    return;
  }
  std::fwrite(buf, 1, len, f);
  std::fclose(f);
  ceyx_encode_free(buf);

  // EXIF must survive into the container byte-for-byte. libheif prefixes the
  // stored payload with a 4-byte TIFF-header offset, so the block is not at a
  // fixed position -- search for it.
  const std::vector<uint8_t> file_bytes = ReadFile(path);
  check(Contains(file_bytes, exif),
        (std::string(label) + " EXIF block embedded byte-identically").c_str());

  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  const int32_t drc = ceyx_still_decode_rgba(path, kCeyxFormatUnknown, 0, &out);
  check_eq(drc, kCeyxStillSuccess, (std::string(label) + " decode rc").c_str());
  if (drc != kCeyxStillSuccess) return;

  check_eq(static_cast<int32_t>(out.width), w, (std::string(label) + " width").c_str());
  check_eq(static_cast<int32_t>(out.height), h, (std::string(label) + " height").c_str());
  check_eq(static_cast<int32_t>(out.rgba_len), w * h * 4,
           (std::string(label) + " rgba_len").c_str());

  if (out.rgba && out.rgba_len == static_cast<int64_t>(src.size())) {
    const double psnr =
        PsnrRgb(src.data(), out.rgba, static_cast<size_t>(w) * h);
    std::printf("     %s PSNR(RGB) = %.2f dB\n", label, psnr);
    check(psnr >= 35.0, (std::string(label) + " PSNR >= 35 dB").c_str());
    // A lossy codec at q90 that reproduces the source EXACTLY means the buffer
    // being compared is not the decoded one.
    check(psnr < 1000.0,
          (std::string(label) + " PSNR is not a self-comparison").c_str());
  }
  ceyx_still_release(&out);
  // Double release must be safe (heif_api.h:58-60's contract, inherited).
  ceyx_still_release(&out);
}

static void ContractCases() {
  const int w = 8, h = 8;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions good = Opts(90, false);
  uint8_t *buf = reinterpret_cast<uint8_t *>(0x1);  // poison: must be nulled
  size_t len = 99;                                  // poison: must be zeroed

  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, nullptr, w, h, &good, nullptr, &buf, &len),
           kCeyxEncodeErrNullArg, "null rgba -> -401");
  check(buf == nullptr && len == 0, "failure zeroes *out and *out_len");

  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), 0, h, &good, nullptr, &buf, &len),
           kCeyxEncodeErrBadDimensions, "width 0 -> -402");
  check(buf == nullptr && len == 0, "width 0 leaves *out/*out_len zeroed");

  CeyxEncodeOptions bad_q = Opts(0, false);
  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), w, h, &bad_q, nullptr, &buf, &len),
           kCeyxEncodeErrBadQuality, "quality 0 lossy -> -403");
  check(buf == nullptr && len == 0, "bad quality leaves *out/*out_len zeroed");

  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), w, h, nullptr, nullptr, &buf, &len),
           kCeyxEncodeErrBadOptions, "opts NULL -> -408");
  check(buf == nullptr && len == 0, "opts NULL leaves *out/*out_len zeroed");

  CeyxEncodeOptions bad_size = Opts(90, false);
  bad_size.struct_size = sizeof(CeyxEncodeOptions) + 4;
  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), w, h, &bad_size, nullptr, &buf, &len),
           kCeyxEncodeErrBadOptions, "struct_size too large -> -408");

  CeyxEncodeOptions bad_res = Opts(90, false);
  bad_res.reserved0 = 1;
  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), w, h, &bad_res, nullptr, &buf, &len),
           kCeyxEncodeErrBadOptions, "reserved0 != 0 -> -408");

  check_eq(ceyx_encode_rgba8(99, src.data(), w, h, &good, nullptr, &buf, &len),
           kCeyxEncodeErrBadFormat, "format 99 -> -410");
  check(buf == nullptr && len == 0, "bad format leaves *out/*out_len zeroed");

  // kCeyxFormatUnknown (0) is a DECODE-side sentinel meaning "sniff"; as an
  // encode target it is not a format.
  check_eq(ceyx_encode_rgba8(kCeyxFormatUnknown, src.data(), w, h, &good, nullptr, &buf, &len),
           kCeyxEncodeErrBadFormat, "format Unknown -> -410");

  CeyxEncodeOptions lossless = Opts(90, true);
  check_eq(ceyx_encode_rgba8(kCeyxFormatJpeg, src.data(), w, h, &lossless, nullptr, &buf, &len),
           kCeyxEncodeErrLosslessUnsupported, "lossless JPEG -> -411");

  // Metadata pointer/length must agree.
  CeyxEncodeMetadata bad_meta;
  std::memset(&bad_meta, 0, sizeof(bad_meta));
  bad_meta.struct_size = sizeof(bad_meta);
  bad_meta.exif_len = 4;  // non-zero length, NULL pointer
  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), w, h, &good, &bad_meta, &buf, &len),
           kCeyxEncodeErrNullArg, "exif_len without exif -> -401");

  // out / out_len themselves may be NULL: that must be rejected, not crash.
  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), w, h, &good, nullptr, nullptr, &len),
           kCeyxEncodeErrNullArg, "out NULL -> -401");
  check_eq(ceyx_encode_rgba8(kCeyxFormatHeic, src.data(), w, h, &good, nullptr, &buf, nullptr),
           kCeyxEncodeErrNullArg, "out_len NULL -> -401");
}

// Plan Task 7 acceptance: "Encode latency is RECORDED, not tuned". Prints, never
// gates. Opt-in, because a 12.5 MP AVIF encode is minutes of wall clock.
static void RecordLatency() {
  const int w = 4080, h = 3056;  // 12.5 MP, the Halcyon full-frame extent
  const std::vector<uint8_t> src = MakeSource(w, h);
  const struct { int32_t fmt; const char *name; } kCases[] = {
      {kCeyxFormatHeic, "heic"}, {kCeyxFormatAvif, "avif"}};

  for (const auto &c : kCases) {
    CeyxEncodeOptions opts = Opts(90, false);  // effort = 0 (codec default)
    uint8_t *buf = nullptr;
    size_t len = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const int32_t rc =
        ceyx_encode_rgba8(c.fmt, src.data(), w, h, &opts, nullptr, &buf, &len);
    const auto t1 = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("LATENCY %s 4080x3056 q90 effort=0: rc=%d len=%zu %.1f ms\n",
                c.name, rc, len, ms);
    check_eq(rc, kCeyxEncodeSuccess,
             (std::string(c.name) + " 12.5MP encode rc").c_str());
    ceyx_encode_free(buf);
  }
}

int main(int argc, char **argv) {
  const std::string dir = (argc > 1) ? argv[1] : std::string(".");
  bool want_latency = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--latency") == 0) want_latency = true;
  }

  check_eq(ceyx_encode_supports(kCeyxFormatHeic), 1, "supports HEIC");
  check_eq(ceyx_encode_supports(kCeyxFormatAvif), 1, "supports AVIF");
  check_eq(ceyx_encode_supports(kCeyxFormatJpeg), 1, "supports JPEG");
  check_eq(ceyx_encode_supports(99), kCeyxEncodeErrBadFormat, "supports(99) -> -410");

  RoundTrip(kCeyxFormatHeic, "heic", (dir + "/rt_heic.heic").c_str());
  RoundTrip(kCeyxFormatAvif, "avif", (dir + "/rt_avif.avif").c_str());
  ContractCases();

  if (want_latency) {
    RecordLatency();
  } else {
    std::printf("SKIP latency measurement (pass --latency to record it)\n");
  }

  std::printf(g_failures == 0 ? "CODEC_HEIF_OK\n" : "CODEC_HEIF_FAILED (%d)\n",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
