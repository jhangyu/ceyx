// JPEG XL encode/decode round-trip harness (Task 9, 2026-08-30 codec
// expansion plan), driven through the PUBLIC C ABI only -- an internal-
// function test would prove nothing about what Dart actually calls.
//
// The check/check_eq/MakeSource/Psnr/Opts/MakeExif/WriteFile helpers below
// are copied VERBATIM from native/tests/test_codec_roundtrip.cpp (Task 8),
// which itself keeps them byte-for-byte identical to
// native/tests/test_codec_heif.cpp, so all three harnesses agree on what they
// measure.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

static double Psnr(const uint8_t *a, const uint8_t *b, size_t n) {
  double se = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    se += d * d;
  }
  const double mse = se / static_cast<double>(n);
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

static void WriteFile(const char *path, const uint8_t *p, size_t n) {
  FILE *f = std::fopen(path, "wb");
  if (!f) {
    check(false, "WriteFile fopen");
    return;
  }
  const size_t wrote = std::fwrite(p, 1, n, f);
  std::fclose(f);
  if (wrote != n) check(false, "WriteFile short write");
}

// EXIF is read back with a reader that is NOT libjxl's encoder. libjxl's Exif
// box takes a 4-byte big-endian TIFF-header-offset PREFIX before the payload;
// omit it and the file encodes, decodes, and carries EXIF that no reader can
// find. A same-library check would not see that. So: parse the ISO-BMFF box
// chain by hand, find the "Exif" box, skip its 4-byte offset field, compare.
//
// This parser is not just trusted to "look right" -- ReadJxlExifBoxRejectsUnprefixedPayload
// below is a negative control proving it actually rejects an unprefixed
// payload rather than trivially passing: if the 4-byte prefix is omitted, the
// first 4 bytes of a real EXIF payload are the TIFF header itself
// ("II*\0" = 0x49492A00 or "MM\0*" = 0x4D4D002A), which this parser reads AS
// tiff_off. Either encoding puts `start` (payload + 4 + tiff_off) at or past
// the end of a small box, so the bounds check at :131 rejects it -- it does
// not accidentally read 0 and pass.
static bool ReadJxlExifBox(const uint8_t *data, size_t n,
                           std::vector<uint8_t> *out) {
  size_t p = 0;
  while (p + 8 <= n) {
    const uint32_t box_size = (uint32_t(data[p]) << 24) | (uint32_t(data[p+1]) << 16) |
                              (uint32_t(data[p+2]) << 8) | uint32_t(data[p+3]);
    const char *type = reinterpret_cast<const char *>(data + p + 4);
    if (box_size < 8 || p + box_size > n) return false;
    if (!std::memcmp(type, "Exif", 4)) {
      // Payload = 4-byte big-endian tiff-header offset, then the EXIF block.
      if (box_size < 12) return false;
      const size_t payload = p + 8;
      const uint32_t tiff_off = (uint32_t(data[payload]) << 24) |
                                (uint32_t(data[payload+1]) << 16) |
                                (uint32_t(data[payload+2]) << 8) |
                                uint32_t(data[payload+3]);
      const size_t start = payload + 4 + tiff_off;
      if (start >= p + box_size) return false;
      out->assign(data + start, data + p + box_size);
      return true;
    }
    p += box_size;
  }
  return false;
}

// The negative control the block comment above refers to: an Exif box whose
// payload is the raw TIFF header with NO 4-byte offset prefix. If
// ReadJxlExifBox trivially trusted its input it would return true with a
// garbage 4-byte-truncated "payload" (the first 4 bytes of the real TIFF
// header consumed as a bogus offset); it must instead reject it.
static void ReadJxlExifBoxRejectsUnprefixedPayload() {
  const uint8_t tiff_le[4] = {0x49, 0x49, 0x2A, 0x00};  // "II*\0", little-endian TIFF
  std::vector<uint8_t> box;
  const uint32_t box_size = 8 + 4;  // header + unprefixed 4-byte payload
  box.push_back(uint8_t(box_size >> 24));
  box.push_back(uint8_t(box_size >> 16));
  box.push_back(uint8_t(box_size >> 8));
  box.push_back(uint8_t(box_size));
  box.insert(box.end(), {'E', 'x', 'i', 'f'});
  box.insert(box.end(), tiff_le, tiff_le + 4);

  std::vector<uint8_t> out;
  check(!ReadJxlExifBox(box.data(), box.size(), &out),
        "ReadJxlExifBox rejects an Exif box with no offset prefix");
}

static void JxlLossless() {
  const int w = 64, h = 48;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(0, /*lossless=*/true);
  uint8_t *buf = nullptr; size_t len = 0;
  check_eq(ceyx_encode_rgba8(kCeyxFormatJxl, src.data(), w, h, &opts, nullptr, &buf, &len),
           kCeyxEncodeSuccess, "jxl lossless encode");
  if (!buf) return;
  WriteFile("rt_jxl_ll.jxl", buf, len);
  ceyx_encode_free(buf);

  CeyxStillResult out; std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba("rt_jxl_ll.jxl", kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, "jxl lossless decode");
  if (out.error_code != kCeyxStillSuccess || !out.rgba) return;
  check(out.rgba_len == static_cast<int64_t>(src.size()) &&
        std::memcmp(src.data(), out.rgba, src.size()) == 0,
        "jxl lossless is BYTE-EXACT incl. alpha");
  ceyx_still_release(&out);
}

static void JxlLossy() {
  // Lossy PSNR, same shape as the other two harnesses, format kCeyxFormatJxl.
  const int w = 256, h = 192;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(90, /*lossless=*/false);
  uint8_t *buf = nullptr; size_t len = 0;
  check_eq(ceyx_encode_rgba8(kCeyxFormatJxl, src.data(), w, h, &opts, nullptr, &buf, &len),
           kCeyxEncodeSuccess, "jxl lossy encode q90");
  if (!buf) return;
  WriteFile("rt_jxl_q90.jxl", buf, len);
  ceyx_encode_free(buf);

  CeyxStillResult out; std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba("rt_jxl_q90.jxl", kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, "jxl lossy decode q90");
  if (out.error_code != kCeyxStillSuccess || !out.rgba) return;
  check(out.rgba_len == static_cast<int64_t>(src.size()), "jxl lossy rgba_len matches source");
  if (out.rgba_len == static_cast<int64_t>(src.size())) {
    const double psnr = Psnr(src.data(), out.rgba, src.size());
    std::printf("     jxl lossy q90 PSNR = %.2f dB\n", psnr);
    check(psnr >= 35.0, "jxl lossy PSNR >= 35 dB at quality 90");
  }
  ceyx_still_release(&out);
}

static void JxlExif() {
  const int w = 32, h = 32;
  const std::vector<uint8_t> src = MakeSource(w, h);
  const std::vector<uint8_t> exif = MakeExif();
  CeyxEncodeOptions opts = Opts(90, false);
  CeyxEncodeMetadata meta; std::memset(&meta, 0, sizeof(meta));
  meta.struct_size = sizeof(meta);
  meta.exif = exif.data(); meta.exif_len = exif.size();

  uint8_t *buf = nullptr; size_t len = 0;
  check_eq(ceyx_encode_rgba8(kCeyxFormatJxl, src.data(), w, h, &opts, &meta, &buf, &len),
           kCeyxEncodeSuccess, "jxl exif encode");
  if (!buf) return;
  std::vector<uint8_t> got;
  const bool found = ReadJxlExifBox(buf, len, &got);
  check(found, "jxl Exif box found by an INDEPENDENT parser");
  check(found && got.size() >= exif.size() &&
        std::memcmp(got.data(), exif.data(), exif.size()) == 0,
        "jxl EXIF bytes survive with the correct tiff-offset prefix");
  WriteFile("rt_jxl_exif.jxl", buf, len);
  ceyx_encode_free(buf);

  CeyxStillResult out; std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba("rt_jxl_exif.jxl", kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, "jxl still decodes AFTER exif embed");
  ceyx_still_release(&out);
}

// The probe must NOT decode pixels: it runs before every preview. A probe that
// silently full-decodes is correct-looking and ruinous on a 4000x3000 file.
static void JxlProbeIsCheap() {
  const int w = 4000, h = 3000;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(0, true);
  uint8_t *buf = nullptr; size_t len = 0;
  if (ceyx_encode_rgba8(kCeyxFormatJxl, src.data(), w, h, &opts, nullptr, &buf, &len)
      != kCeyxEncodeSuccess) { check(false, "jxl large encode for probe test"); return; }
  WriteFile("rt_jxl_big.jxl", buf, len);
  ceyx_encode_free(buf);

  uint32_t pw = 0, ph = 0; int32_t po = 0;
  const clock_t t0 = std::clock();
  check_eq(ceyx_still_probe("rt_jxl_big.jxl", kCeyxFormatJxl, &pw, &ph, &po),
           kCeyxStillSuccess, "jxl probe rc");
  const double ms = 1000.0 * double(std::clock() - t0) / CLOCKS_PER_SEC;
  check_eq(static_cast<int32_t>(pw), w, "jxl probe width");
  check_eq(static_cast<int32_t>(ph), h, "jxl probe height");
  std::printf("     jxl probe took %.1f ms\n", ms);
  // Wall time is a PROXY for "did not decode pixels" -- named as such rather
  // than pretending it measures allocation. A full 12 MP decode cannot finish
  // in 50 ms; a header parse cannot exceed it.
  check(ms < 50.0, "jxl probe is header-only (proxy: < 50 ms)");
}

static void JxlSizedDecode() {
  uint32_t pw = 0, ph = 0; int32_t po = 0;
  (void)ceyx_still_probe("rt_jxl_big.jxl", kCeyxFormatJxl, &pw, &ph, &po);
  CeyxStillResult out; std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba("rt_jxl_big.jxl", kCeyxFormatJxl, 512, &out),
           kCeyxStillSuccess, "jxl sized decode rc");
  check(out.width <= 512 && out.height <= 512, "jxl sized decode caps long edge");
  // 4000x3000 -> long edge 512 means 512x384; allow one pixel of rounding.
  check(std::abs(int(out.height) - 384) <= 1, "jxl sized decode preserves aspect");
  check(out.rgba_len == static_cast<int64_t>(out.width) * out.height * 4,
        "jxl sized rgba_len matches its own extent");
  ceyx_still_release(&out);
}

// Encode latency is RECORDED, not tuned (plan spec R-b): print wall-clock ms
// for a 12.5 MP JXL encode at effort=0, lossy and lossless. These numbers feed
// a later, separate decision about per-format defaults; no default changes
// here on a guess.
static void JxlEncodeLatency() {
  // 12.5 MP: e.g. 4083 x 3062 ~= 12.5M pixels (close to the plan's 12.5 MP
  // reference frame size used elsewhere in this codec-expansion plan).
  const int w = 4083, h = 3062;
  const std::vector<uint8_t> src = MakeSource(w, h);

  {
    CeyxEncodeOptions opts = Opts(90, /*lossless=*/false);
    uint8_t *buf = nullptr; size_t len = 0;
    const clock_t t0 = std::clock();
    const int32_t rc = ceyx_encode_rgba8(kCeyxFormatJxl, src.data(), w, h, &opts, nullptr, &buf, &len);
    const double ms = 1000.0 * double(std::clock() - t0) / CLOCKS_PER_SEC;
    check_eq(rc, kCeyxEncodeSuccess, "jxl 12.5MP lossy effort=0 encode rc");
    std::printf("     jxl 12.5MP LOSSY  effort=0 encode: %.1f ms (%zu bytes)\n", ms, len);
    if (buf) ceyx_encode_free(buf);
  }
  {
    CeyxEncodeOptions opts = Opts(0, /*lossless=*/true);
    uint8_t *buf = nullptr; size_t len = 0;
    const clock_t t0 = std::clock();
    const int32_t rc = ceyx_encode_rgba8(kCeyxFormatJxl, src.data(), w, h, &opts, nullptr, &buf, &len);
    const double ms = 1000.0 * double(std::clock() - t0) / CLOCKS_PER_SEC;
    check_eq(rc, kCeyxEncodeSuccess, "jxl 12.5MP lossless effort=0 encode rc");
    std::printf("     jxl 12.5MP LOSSLESS effort=0 encode: %.1f ms (%zu bytes)\n", ms, len);
    if (buf) ceyx_encode_free(buf);
  }
}

int main() {
  check_eq(ceyx_encode_supports(kCeyxFormatJxl), 1, "supports JXL encode");
  check_eq(ceyx_still_decode_supports(kCeyxFormatJxl), 1, "supports JXL decode");
  ReadJxlExifBoxRejectsUnprefixedPayload();
  JxlLossless();
  JxlLossy();
  JxlExif();
  JxlProbeIsCheap();
  JxlSizedDecode();
  JxlEncodeLatency();
  std::printf(g_failures == 0 ? "CODEC_JXL_OK\n" : "CODEC_JXL_FAILED (%d)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
