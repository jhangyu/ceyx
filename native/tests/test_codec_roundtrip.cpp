// WebP round-trip and still-decode-dispatcher gate (plan Task 8), driven
// through the PUBLIC C ABI only -- an internal-function test would prove
// nothing about what Dart actually calls.
//
// The check/check_eq/MakeSource/Psnr/Opts/MakeExif helpers are kept
// byte-for-byte identical to native/tests/test_codec_heif.cpp so the two
// harnesses cannot drift in what "PSNR" means.

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

// Deliberately NOT libwebp's own demux reader: a mux bug shared by libwebp's
// writer and reader would be invisible to a same-library check. Walks the RIFF
// container: 12-byte "RIFF....WEBP" header, then (fourcc, u32 size, payload,
// pad-to-even) chunks until the fourcc is "EXIF".
static bool ReadEmbeddedExif(const uint8_t *buf, size_t len,
                             std::vector<uint8_t> *outp) {
  if (len < 12 || std::memcmp(buf, "RIFF", 4) != 0 ||
      std::memcmp(buf + 8, "WEBP", 4) != 0) {
    return false;
  }
  size_t off = 12;
  while (off + 8 <= len) {
    const uint8_t *fourcc = buf + off;
    const uint32_t size = static_cast<uint32_t>(buf[off + 4]) |
                          (static_cast<uint32_t>(buf[off + 5]) << 8) |
                          (static_cast<uint32_t>(buf[off + 6]) << 16) |
                          (static_cast<uint32_t>(buf[off + 7]) << 24);
    const size_t payload = off + 8;
    if (size > len || payload + size > len) return false;
    if (std::memcmp(fourcc, "EXIF", 4) == 0) {
      outp->assign(buf + payload, buf + payload + size);
      return true;
    }
    off = payload + size + (size & 1);
  }
  return false;
}

// Lossless is BYTE-EXACT, not PSNR. A lossless codec that loses one byte is
// broken, and a 60 dB PSNR threshold would happily pass it.
static void LosslessRoundTrip(int32_t format, const char *label, const char *path) {
  const int w = 64, h = 48;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(0, /*lossless=*/true);

  uint8_t *buf = nullptr;
  size_t len = 0;
  check_eq(ceyx_encode_rgba8(format, src.data(), w, h, &opts, nullptr, &buf, &len),
           kCeyxEncodeSuccess, (std::string(label) + " lossless encode").c_str());
  if (!buf) return;
  WriteFile(path, buf, len);
  ceyx_encode_free(buf);

  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba(path, kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, (std::string(label) + " lossless decode").c_str());
  if (out.error_code != kCeyxStillSuccess || !out.rgba) return;

  check_eq(static_cast<int32_t>(out.rgba_len), w * h * 4,
           (std::string(label) + " lossless rgba_len").c_str());
  const bool exact = out.rgba_len == static_cast<int64_t>(src.size()) &&
                     std::memcmp(src.data(), out.rgba, src.size()) == 0;
  check(exact, (std::string(label) + " lossless is BYTE-EXACT incl. alpha").c_str());
  if (!exact) {
    // Name the first divergence: "not byte-exact" alone does not distinguish a
    // one-pixel bug from a channel-order bug.
    for (size_t i = 0; i < src.size(); ++i) {
      if (src[i] != out.rgba[i]) {
        std::printf("     first diff at byte %zu (px %zu ch %zu): src=%u got=%u\n",
                    i, i / 4, i % 4, src[i], out.rgba[i]);
        break;
      }
    }
  }
  ceyx_still_release(&out);
}

// EXIF must survive, and the file must STILL DECODE afterwards: a mux step
// that corrupts the container is the realistic failure mode, and an
// EXIF-only assertion would not see it.
static void ExifRoundTrip(int32_t format, const char *label, const char *path) {
  const int w = 32, h = 32;
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
  check_eq(ceyx_encode_rgba8(format, src.data(), w, h, &opts, &meta, &buf, &len),
           kCeyxEncodeSuccess, (std::string(label) + " exif encode").c_str());
  if (!buf) return;
  WriteFile(path, buf, len);

  // Independent read-back: locate the EXIF payload in the produced container
  // and compare it to what went in.
  std::vector<uint8_t> got;
  const bool found = ReadEmbeddedExif(buf, len, &got);
  check(found, (std::string(label) + " EXIF chunk present").c_str());
  check(found && got.size() >= exif.size() &&
        std::memcmp(got.data(), exif.data(), exif.size()) == 0,
        (std::string(label) + " EXIF bytes survive round-trip").c_str());
  ceyx_encode_free(buf);

  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba(path, kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess,
           (std::string(label) + " still decodes AFTER exif embed").c_str());
  ceyx_still_release(&out);
}

static void NoMetadataCase(const char *path) {
  const int w = 16, h = 16;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(90, false);
  uint8_t *buf = nullptr;
  size_t len = 0;
  check_eq(ceyx_encode_rgba8(kCeyxFormatWebp, src.data(), w, h, &opts, nullptr,
                             &buf, &len),
           kCeyxEncodeSuccess, "webp meta=NULL encode");
  if (!buf) return;
  std::vector<uint8_t> got;
  check(!ReadEmbeddedExif(buf, len, &got), "meta=NULL produces NO exif chunk");
  WriteFile(path, buf, len);
  ceyx_encode_free(buf);
  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba(path, kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, "meta=NULL file still decodes");
  ceyx_still_release(&out);
}

// Content sniffing must not depend on the extension: Halcyon routes by
// extension but passes kCeyxFormatUnknown, so a mislabelled file must work.
static void SniffCase(const char *webp_path, const char *renamed) {
  std::rename(webp_path, renamed);
  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba(renamed, kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, "sniff: .bin containing WebP decodes");
  ceyx_still_release(&out);
}

// max_dim is a REQUEST, not a guarantee (ceyx_still_api.h). The test asserts
// the caller-visible contract: the long edge is capped and the result is read
// back, never assumed.
static void SizedDecodeCase(const char *path) {
  const int w = 200, h = 100;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(90, false);
  uint8_t *buf = nullptr;
  size_t len = 0;
  if (ceyx_encode_rgba8(kCeyxFormatWebp, src.data(), w, h, &opts, nullptr,
                        &buf, &len) != kCeyxEncodeSuccess) return;
  WriteFile(path, buf, len);
  ceyx_encode_free(buf);

  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba(path, kCeyxFormatWebp, 50, &out),
           kCeyxStillSuccess, "sized decode rc");
  check(out.width <= 50 && out.height <= 50, "sized decode caps the long edge");
  check(out.rgba_len == static_cast<int64_t>(out.width) * out.height * 4,
        "sized decode rgba_len matches its OWN reported extent");
  ceyx_still_release(&out);
}

// Lossy fidelity, for parity with the HEIF harness.
static void LossyRoundTrip(int32_t format, const char *label, const char *path) {
  const int w = 64, h = 48;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(90, false);

  uint8_t *buf = nullptr;
  size_t len = 0;
  check_eq(ceyx_encode_rgba8(format, src.data(), w, h, &opts, nullptr, &buf, &len),
           kCeyxEncodeSuccess, (std::string(label) + " lossy encode").c_str());
  if (!buf) return;
  WriteFile(path, buf, len);
  ceyx_encode_free(buf);

  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba(path, kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, (std::string(label) + " lossy decode").c_str());
  if (out.error_code != kCeyxStillSuccess || !out.rgba) return;
  check_eq(static_cast<int32_t>(out.width), w,
           (std::string(label) + " lossy width").c_str());
  check_eq(static_cast<int32_t>(out.height), h,
           (std::string(label) + " lossy height").c_str());
  const double psnr = Psnr(src.data(), out.rgba,
                           out.rgba_len == static_cast<int64_t>(src.size())
                               ? src.size() : 0);
  std::printf("     %s lossy PSNR = %.2f dB\n", label, psnr);
  check(out.rgba_len == static_cast<int64_t>(src.size()) && psnr >= 35.0,
        (std::string(label) + " lossy PSNR >= 35 dB @ q90").c_str());
  ceyx_still_release(&out);
}

// A second release on an already-released struct must be a no-op, not a
// double free. Run under -fsanitize=address to make that assertion real.
static void DoubleReleaseCase(const char *path) {
  CeyxStillResult out;
  std::memset(&out, 0, sizeof(out));
  check_eq(ceyx_still_decode_rgba(path, kCeyxFormatUnknown, 0, &out),
           kCeyxStillSuccess, "double-release: decode");
  ceyx_still_release(&out);
  ceyx_still_release(&out);   // must be a no-op
  check(out.rgba == nullptr && out.rgba_len == 0,
        "double release leaves the struct zeroed");
  ceyx_still_release(nullptr);  // NULL-safe
}

// B1 (round-2 review, still_ffi_api.cpp SniffFormat): scan compatible_brands
// when the major brand isn't directly recognised.
//
// IMPORTANT, stated honestly: in THIS codebase the Heic and Avif arms of both
// ceyx_still_probe and ceyx_still_decode_rgba call the exact same
// heif_probe()/heif_decode function -- `format` is not read inside that
// branch at all (only ceyx_heif_encode_impl's ENCODE path branches on it, to
// pick the HEVC vs AV1 encoder). And the pre-fix SniffFormat's fallback for
// ANY unrecognised ftyp major brand was `return kCeyxFormatHeic`, never
// kCeyxFormatUnknown -- so for a real, fully-payloaded ISO-BMFF file, decode
// already succeeded before this fix regardless of which family it landed in.
// That means the "hint-less decode fails" symptom described for B1 does not
// reproduce as a black-box-observable failure against this codebase, and no
// RED assertion is possible at the ceyx_still_probe/decode return-code level
// for a fully valid file -- confirmed by hand: git-show of the pre-fix
// SniffFormat (commit caade3c) has the same `return kCeyxFormatHeic;`
// catch-all this build already had.
//
// What this test DOES verify, and is a genuine regression guard: a file
// whose major brand is a real, spec-legal brand this build does not match
// directly ("avio", "mif2" -- both used by real AVIF/HEIC image-sequence
// files in the wild) but whose compatible_brands correctly lists "avif" or
// "heic" still decodes successfully end-to-end via hint-less sniffing, byte-
// identical to a normally-labelled file. This guards against a FUTURE
// regression where the Heic/Avif catch-all is removed (since it is no longer
// load-bearing for correctness once the compatible_brands scan is trusted)
// and only the scan is left to do the routing.
static void FtypCompatibleBrandSniffCase(const char *avif_path,
                                         const char *heic_path) {
  const int w = 24, h = 24;
  const std::vector<uint8_t> src = MakeSource(w, h);
  CeyxEncodeOptions opts = Opts(90, false);

  auto encode_and_patch = [&](int32_t format, const char *path,
                              const char *unrecognised_major) -> bool {
    uint8_t *buf = nullptr;
    size_t len = 0;
    if (ceyx_encode_rgba8(format, src.data(), w, h, &opts, nullptr, &buf, &len) !=
        kCeyxEncodeSuccess) {
      return false;
    }
    // Real libheif output: [0..4) box_size BE, [4..8) "ftyp",
    // [8..12) major_brand, [12..16) minor_version, [16..) compatible_brands
    // (already contains "mif1 avif miaf" / "mif1 heic miaf" per libheif's
    // own writer -- verified by inspection of a real encoded file's first 32
    // bytes). Overwriting ONLY the major_brand bytes with a brand this
    // build's direct-match lists do NOT contain forces routing through the
    // compatible_brands scan path added by this fix, while leaving the box
    // size and every other byte (including the actual compressed image data)
    // untouched.
    check(len > 16 && !std::memcmp(buf + 4, "ftyp", 4),
          "encoded output has a real ftyp box to patch");
    std::memcpy(buf + 8, unrecognised_major, 4);
    WriteFile(path, buf, len);
    ceyx_encode_free(buf);
    return true;
  };

  if (encode_and_patch(kCeyxFormatAvif, avif_path, "avio")) {
    CeyxStillResult out;
    std::memset(&out, 0, sizeof(out));
    check_eq(ceyx_still_decode_rgba(avif_path, kCeyxFormatUnknown, 0, &out),
             kCeyxStillSuccess,
             "ftyp major=avio (unrecognised) compatible=avif still sniffs+decodes");
    ceyx_still_release(&out);
  }
  if (encode_and_patch(kCeyxFormatHeic, heic_path, "mif2")) {
    CeyxStillResult out;
    std::memset(&out, 0, sizeof(out));
    check_eq(ceyx_still_decode_rgba(heic_path, kCeyxFormatUnknown, 0, &out),
             kCeyxStillSuccess,
             "ftyp major=mif2 (unrecognised) compatible=heic still sniffs+decodes");
    ceyx_still_release(&out);
  }
}

int main(int argc, char **argv) {
  const std::string d = (argc > 1) ? argv[1] : std::string(".");
  check_eq(ceyx_encode_supports(kCeyxFormatWebp), 1, "supports WebP encode");
  check_eq(ceyx_still_decode_supports(kCeyxFormatWebp), 1, "supports WebP decode");

  LosslessRoundTrip(kCeyxFormatWebp, "webp", (d + "/rt_webp_ll.webp").c_str());
  ExifRoundTrip(kCeyxFormatWebp, "webp", (d + "/rt_webp_exif.webp").c_str());
  NoMetadataCase((d + "/rt_webp_nometa.webp").c_str());
  SizedDecodeCase((d + "/rt_webp_sized.webp").c_str());
  DoubleReleaseCase((d + "/rt_webp_sized.webp").c_str());
  LossyRoundTrip(kCeyxFormatWebp, "webp", (d + "/rt_webp_lossy.webp").c_str());
  SniffCase((d + "/rt_webp_nometa.webp").c_str(), (d + "/rt_sniff.bin").c_str());
  FtypCompatibleBrandSniffCase((d + "/rt_ftyp_avio.bin").c_str(),
                               (d + "/rt_ftyp_mif2.bin").c_str());

  std::printf(g_failures == 0 ? "CODEC_ROUNDTRIP_OK\n"
                              : "CODEC_ROUNDTRIP_FAILED (%d)\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
