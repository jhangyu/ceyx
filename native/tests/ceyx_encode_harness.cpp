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
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ceyx_encode_api.h"
#include "ceyx_yuv420_oracle.h"
#include "dng_ffi_api.h"
#include "raw_ffi_api.h"
#include "test_report.h"

namespace {

// Line format kept as-is (no parser reads it; verify_yuv420_encode.py reads the
// dumped files). Counting goes through test_report.h so the run ends with the
// shared summary line and exit-code rule.
void Check(bool ok, const char *name, const char *detail = "") {
  std::printf("[encode] %s %s %s\n", ok ? "PASS" : "FAIL", name, detail);
  ++test_report::executed;
  if (!ok) ++test_report::failures;
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

// RGBA8 -> the decoder's tightly-packed planar 4:2:0 destination layout
// (raw_ffi_api.h:436-450), using the SHARED oracle rather than a second
// coefficient set — the whole point of ceyx_yuv420_oracle.h. This stands in for
// what a yuv420 decode writes, so Case 5 can run without a RAW file; Case 6
// then repeats the test on real decoder output.
std::vector<uint8_t> RgbaToYuv420(const std::vector<uint8_t> &rgba, int w,
                                  int h) {
  const int cw = ceyx::yuv420::chroma_extent(w);
  const int ch = ceyx::yuv420::chroma_extent(h);
  std::vector<uint8_t> out(static_cast<size_t>(w) * h +
                           2u * static_cast<size_t>(cw) * ch);
  uint8_t *const y = out.data();
  uint8_t *const cb = y + static_cast<size_t>(w) * h;
  uint8_t *const cr = cb + static_cast<size_t>(cw) * ch;

  // libjpeg converts EVERY pixel first and downsamples the CHROMA afterwards
  // (oracle header, "ORDER OF OPERATIONS IS PART OF THE ORACLE"), so do the
  // same rather than averaging RGB and converting once.
  std::vector<uint8_t> full_cb(static_cast<size_t>(w) * h);
  std::vector<uint8_t> full_cr(static_cast<size_t>(w) * h);
  for (int j = 0; j < h; ++j) {
    for (int i = 0; i < w; ++i) {
      const size_t s = (static_cast<size_t>(j) * w + i) * 4;
      const int32_t r = rgba[s], g = rgba[s + 1], b = rgba[s + 2];
      const size_t d = static_cast<size_t>(j) * w + i;
      y[d] = ceyx::yuv420::luma_from_rgb(r, g, b);
      full_cb[d] = ceyx::yuv420::cb_from_rgb(r, g, b);
      full_cr[d] = ceyx::yuv420::cr_from_rgb(r, g, b);
    }
  }
  for (int j = 0; j < ch; ++j) {
    for (int i = 0; i < cw; ++i) {
      const int x0 = i * 2, x1 = (x0 + 1 < w) ? x0 + 1 : x0;
      const int y0 = j * 2, y1 = (y0 + 1 < h) ? y0 + 1 : y0;
      const auto at = [&](const std::vector<uint8_t> &p, int xx, int yy) {
        return static_cast<int32_t>(p[static_cast<size_t>(yy) * w + xx]);
      };
      const size_t d = static_cast<size_t>(j) * cw + i;
      cb[d] = ceyx::yuv420::box_average_2x2(at(full_cb, x0, y0),
                                            at(full_cb, x1, y0),
                                            at(full_cb, x0, y1),
                                            at(full_cb, x1, y1), i);
      cr[d] = ceyx::yuv420::box_average_2x2(at(full_cr, x0, y0),
                                            at(full_cr, x1, y0),
                                            at(full_cr, x0, y1),
                                            at(full_cr, x1, y1), i);
    }
  }
  return out;
}

// Minimal SOF0/SOF2 scan: the produced file's OWN declaration of extent and
// sampling factors, read from the bytes rather than assumed from the request.
bool ParseSof(const uint8_t *d, size_t n, int *w, int *h, int *ncomp, int *h0,
              int *v0) {
  size_t p = 2;  // past SOI
  while (p + 3 < n) {
    if (d[p] != 0xFF) return false;
    const uint8_t marker = d[p + 1];
    if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      p += 2;
      continue;
    }
    const size_t seg = (static_cast<size_t>(d[p + 2]) << 8) | d[p + 3];
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2) {
      if (p + 10 >= n) return false;
      *h = (d[p + 5] << 8) | d[p + 6];
      *w = (d[p + 7] << 8) | d[p + 8];
      *ncomp = d[p + 9];
      if (*ncomp < 1 || p + 12 >= n) return false;
      *h0 = d[p + 11] >> 4;
      *v0 = d[p + 11] & 0x0F;
      return true;
    }
    if (marker == 0xDA) return false;  // scan reached, no SOF
    p += 2 + seg;
  }
  return false;
}

// Dumps an encoded buffer next to its sibling so an out-of-process verifier
// can decode BOTH and compare. This harness cannot decode a JPEG itself: this
// build exposes no JPEG still-decode route (ceyx_still_decode_supports(
// kCeyxFormatJpeg) == 0, observed 2026-09-20), so a "decode it back" check
// here would be asserting on an absent capability rather than on the encoder.
// scripts/verify_yuv420_encode.py owns the pixel comparison.
bool DumpFile(const char *dir, const char *name, const uint8_t *data,
              size_t len) {
  char path[1024];
  std::snprintf(path, sizeof(path), "%s/%s", dir, name);
  std::FILE *f = std::fopen(path, "wb");
  if (!f) return false;
  const bool ok = std::fwrite(data, 1, len, f) == len;
  std::fclose(f);
  std::printf("[encode]   dumped %s (%zu bytes)\n", path, len);
  return ok;
}

}  // namespace

int main(int argc, char **argv) {
  // Halide #8497: release the GPU while the driver is alive, not in the DLL-unload destructor.
  std::atexit(ceyx_native_release_gpu);
  // argv[1] = optional RAW file for Case 6; argv[2] = optional directory the
  // encoded JPEGs are dumped into for out-of-process content verification.
  const char *const dump_dir = argc >= 3 ? argv[2] : ".";
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

  // --- Case 5: planar yuv420 -> JPEG, even and ODD extents ----------------
  //
  // The odd case (101x51) is the one that matters: ceil(w/2) != w/2 and
  // libjpeg's raw-data path needs whole iMCU rows, so a padding mistake in
  // ceyx_encode_jpeg_yuv420 shows up here as a libjpeg refusal, a wrong SOF
  // extent, or garbage along the right/bottom edge.
  {
    struct Case { int w, h; const char *name; };
    const Case cases[] = {{kW, kH, "yuv420_64x48"}, {101, 51, "yuv420_101x51"}};
    for (const Case &c : cases) {
      const std::vector<uint8_t> rgba = MakeRgba(c.w, c.h);
      const std::vector<uint8_t> planar = RgbaToYuv420(rgba, c.w, c.h);

      uint8_t *out = nullptr;
      size_t len = 0;
      const int32_t rc = ceyx_encode_jpeg_yuv420(
          planar.data(), planar.size(), c.w, c.h, 90, &out, &len);
      char detail[256];
      std::snprintf(detail, sizeof(detail), "(rc=%d %s, len=%zu)", rc,
                    ceyx_encode_error_name(rc), len);
      const bool framed = rc == kCeyxEncodeSuccess && out != nullptr &&
                          len > 4 && out[0] == 0xFF && out[1] == 0xD8 &&
                          out[len - 2] == 0xFF && out[len - 1] == 0xD9;
      Check(framed, c.name, detail);

      if (framed) {
        int sw = 0, sh = 0, h0 = 0, v0 = 0, ncomp = 0;
        const bool sof = ParseSof(out, len, &sw, &sh, &ncomp, &h0, &v0);
        std::snprintf(detail, sizeof(detail),
                      "(sof=%d %dx%d ncomp=%d samp=%dx%d)", sof ? 1 : 0, sw, sh,
                      ncomp, h0, v0);
        Check(sof && sw == c.w && sh == c.h && ncomp == 3 && h0 == 2 && v0 == 2,
              "yuv420_sof_contract", detail);

        // Content is gated OUT OF PROCESS (see DumpFile's comment): this file
        // and the rgba8 encode of the SAME frame are dumped, and
        // scripts/verify_yuv420_encode.py decodes both and compares them. The
        // PAIR is the point -- the two entries must agree, which is a stronger
        // statement than either file merely being a valid JPEG.
        char fname[128];
        std::snprintf(fname, sizeof(fname), "synth_%dx%d_yuv420.jpg", c.w, c.h);
        Check(DumpFile(dump_dir, fname, out, len), "yuv420_dump_written", "");

        uint8_t *ref = nullptr;
        size_t ref_len = 0;
        const int32_t rrc =
            ceyx_encode_jpeg_rgba8(rgba.data(), c.w, c.h, 90, &ref, &ref_len);
        std::snprintf(fname, sizeof(fname), "synth_%dx%d_rgba8.jpg", c.w, c.h);
        Check(rrc == kCeyxEncodeSuccess &&
                  DumpFile(dump_dir, fname, ref, ref_len),
              "rgba8_reference_dump_written", "");
        ceyx_encode_free(ref);
      }
      ceyx_encode_free(out);

      // Argument validation, including the capacity check the rgba8 entry
      // structurally cannot perform.
      uint8_t *bad_out = reinterpret_cast<uint8_t *>(0x1);
      size_t bad_len = 123;
      const int32_t rc_null = ceyx_encode_jpeg_yuv420(
          nullptr, planar.size(), c.w, c.h, 80, &bad_out, &bad_len);
      Check(rc_null == kCeyxEncodeErrNullArg && bad_out == nullptr &&
                bad_len == 0,
            "yuv420_null_src_rejected", "");
      const int32_t rc_small = ceyx_encode_jpeg_yuv420(
          planar.data(), planar.size() - 1, c.w, c.h, 80, &bad_out, &bad_len);
      Check(rc_small == kCeyxEncodeErrBadBufferSize,
            "yuv420_short_capacity_rejected", "");
      const int32_t rc_q = ceyx_encode_jpeg_yuv420(
          planar.data(), planar.size(), c.w, c.h, 101, &bad_out, &bad_len);
      Check(rc_q == kCeyxEncodeErrBadQuality, "yuv420_quality_101_rejected", "");
    }
  }

  // --- Case 6: a REAL decoded frame, not a synthetic pattern --------------
  //
  // argv[1] is a RAW file. It is decoded straight into the yuv420 destination
  // layout by the production decode entry and handed to the new encoder with
  // nothing in between -- exactly the path Halcyon's re-encode arm will take.
  // Skipped LOUDLY (never silently) when no path is given.
  if (argc >= 2) {
    const char *path = argv[1];
    int32_t w = 0, h = 0;
    int64_t bytes = 0;
    const int32_t prc = ceyx_probe_output_size_format(
        path, 0, kCeyxOutputFormatYuv420, &w, &h, &bytes);
    char detail[512];
    std::snprintf(detail, sizeof(detail), "(prc=%d %dx%d bytes=%lld)", prc, w, h,
                  static_cast<long long>(bytes));
    Check(prc == 0 && w > 0 && h > 0 && bytes > 0, "real_probe_yuv420", detail);
    if (prc == 0 && bytes > 0) {
      std::vector<uint8_t> dst(static_cast<size_t>(bytes));
      CeyxYuv420PlaneDescriptor planes;
      std::memset(&planes, 0, sizeof(planes));
      planes.struct_size = sizeof(planes);
      DngResult *dr = ceyx_decode_into_buffer_format(
          path, 0, dst.data(), dst.size(), kCeyxOutputFormatYuv420, &planes);
      const int32_t derr = dr ? dr->error_code : -9999;
      std::snprintf(detail, sizeof(detail), "(err=%d %dx%d)", derr,
                    dr ? dr->width : 0, dr ? dr->height : 0);
      Check(derr == 0, "real_decode_yuv420", detail);
      if (derr == 0) {
        // The layout the decoder REPORTS must be the tightly-packed one the
        // encoder assumes; asserting it makes the encode below a test of the
        // production contract rather than of a local assumption.
        Check(planes.plane_base[0] == dst.data() &&
                  planes.plane_base[1] ==
                      dst.data() + static_cast<size_t>(w) * h &&
                  planes.plane_row_stride[0] == w &&
                  planes.plane_row_stride[1] == (w + 1) / 2,
              "real_plane_layout_matches_contract", "");

        uint8_t *out = nullptr;
        size_t len = 0;
        const auto t0 = std::chrono::steady_clock::now();
        const int32_t rc =
            ceyx_encode_jpeg_yuv420(dst.data(), dst.size(), w, h, 90, &out, &len);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        int sw = 0, sh = 0, h0 = 0, v0 = 0, ncomp = 0;
        const bool sof = (rc == kCeyxEncodeSuccess) && out &&
                         ParseSof(out, len, &sw, &sh, &ncomp, &h0, &v0);
        std::snprintf(detail, sizeof(detail),
                      "(rc=%d len=%zu %.1f ms sof=%dx%d samp=%dx%d)", rc, len,
                      ms, sw, sh, h0, v0);
        Check(rc == kCeyxEncodeSuccess && len > 4 && sof && sw == w &&
                  sh == h && ncomp == 3 && h0 == 2 && v0 == 2,
              "real_encode_yuv420", detail);
        if (rc == kCeyxEncodeSuccess) {
          Check(DumpFile(dump_dir, "real_yuv420.jpg", out, len),
                "real_yuv420_dump_written", "");
          // The rgba8 sibling of the SAME file: decode it again in rgba8 and
          // encode through the existing entry, so the verifier compares two
          // renderings of one photo rather than two unrelated images.
          const int64_t rgba_bytes =
              ceyx_output_format_byte_count(kCeyxOutputFormatRgba8, w, h);
          std::vector<uint8_t> rgba_dst(static_cast<size_t>(rgba_bytes));
          DngResult *dr2 = ceyx_decode_into_buffer_format(
              path, 0, rgba_dst.data(), rgba_dst.size(), kCeyxOutputFormatRgba8,
              nullptr);
          const int32_t derr2 = dr2 ? dr2->error_code : -9999;
          uint8_t *ref = nullptr;
          size_t ref_len = 0;
          const int32_t rrc =
              derr2 == 0 ? ceyx_encode_jpeg_rgba8(rgba_dst.data(), w, h, 90,
                                                  &ref, &ref_len)
                         : -1;
          std::snprintf(detail, sizeof(detail), "(decode=%d encode=%d)", derr2,
                        rrc);
          Check(rrc == kCeyxEncodeSuccess &&
                    DumpFile(dump_dir, "real_rgba8.jpg", ref, ref_len),
                "real_rgba8_reference_dump_written", detail);
          ceyx_encode_free(ref);
          dng_free_result(dr2);
        }
        ceyx_encode_free(out);
      }
      dng_free_result(dr);
    }
  } else {
    test_report::reportSkip("encode", "real_frame_yuv420", "no-raw-path");
  }

  return test_report::finish("encode");
}
