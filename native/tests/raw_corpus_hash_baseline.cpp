// raw_corpus_hash_baseline.cpp — R1-T5 (plan §9.4/AC4) baseline red run
// driver.
//
// Decodes each RAW file given on argv through raw_pipeline_decode_file_into
// (the same generic-RAW entry probe_concurrent_raw.cpp uses) at default
// settings (RawDevelopParams{} — exposure_ev=0, tone_curve_strength default
// per its C++ default-member-initialisers, output_space=sRGB, full
// resolution) and prints one line per file:
//   HASH path=<path> w=<W> h=<H> fnv1a=0x<16 hex digits> rc=<n>
// Exit code is the count of failed decodes (0 == every file decoded).
//
// This is the reference-hash producer for AC4 (plan §9.4) run 1 — captured
// against the PRE-CHANGE binary, before any Slice A/B/C code lands, per
// plan §9.0's "instrument validated by reproducing the baseline, not by
// reporting success" discipline (§8.3 item 4 / §8.4 row 5).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"

namespace {

uint64_t fnv1a(const uint8_t* data, size_t len) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ull;
  }
  return h;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <raw_file>...\n", argv[0]);
    return 2;
  }

  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    const char* path = argv[i];
    RawDevelopParams develop{};
    develop.exposure_ev = 0.0f;
    develop.tone_curve_strength = 1.0f;
    develop.output_space = kRawOutputColorSpaceSrgb;
    develop.max_output_long_edge = 0u;
    develop.auto_exposure_mode = kRawAutoExposureOff;  // stable baseline hash

    uint32_t pw = 0, ph = 0;
    RawErrorCode probe_rc =
        raw_pipeline_probe_output_size(path, develop.max_output_long_edge, &pw, &ph);
    if (probe_rc != kRawSuccess || pw == 0 || ph == 0) {
      std::printf("HASH path=%s w=0 h=0 fnv1a=0x0000000000000000 rc=%d\n",
                  path, static_cast<int>(probe_rc));
      ++failures;
      continue;
    }

    std::vector<uint8_t> dst(static_cast<size_t>(pw) * ph * 4);
    RawPipelineResult result{};
    RawErrorCode rc = raw_pipeline_decode_file_into(path, develop, dst.data(),
                                                     dst.size(), result);
    if (rc != kRawSuccess || result.rgba_ptr != dst.data()) {
      std::printf("HASH path=%s w=%u h=%u fnv1a=0x0000000000000000 rc=%d\n",
                  path, pw, ph, static_cast<int>(rc));
      ++failures;
      continue;
    }

    const uint64_t h = fnv1a(dst.data(), dst.size());
    std::printf("HASH path=%s w=%u h=%u fnv1a=0x%016llx rc=0\n", path, pw, ph,
                (unsigned long long)h);
  }
  return failures;
}
