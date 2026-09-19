// raw_corpus_ev_gate.cpp — T-V3 corpus EV-equality gate
// (docs/logs/2026-09-19/spec-cpu-levers.md §6.6 T-V3 AC 3).
//
// The shipped corpus hash gate (raw_corpus_hash_baseline.cpp:52) decodes with
// auto_exposure_mode = kRawAutoExposureOff, so it exercises NONE of the
// auto-exposure estimator. This gate is the actual coverage for T-V3: it
// decodes every corpus entry with auto-exposure ON and prints the resulting
// auto_exposure_ev as its RAW 32-BIT PATTERN, never as %.7f text — a decimal
// rendering can hide a low-bit difference, and no epsilon is authorised for
// this lever at any point.
//
// One line per file:
//   EV path=<path> rc=<n> ae_status=<n> ev_bits=0x<8 hex> ev=<%.9g>
// Lines are compared byte-for-byte between the before-arm and the after-arm by
// native/scripts/tmp/t-v3_ev_gate.py. Exit code is 0 when every file produced a
// line (a decode FAILURE is still a line — the malformed corpus entries are
// expected to keep their error codes, and a changed error code must fail the
// comparison rather than abort the run).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"

namespace {

uint32_t floatBits(float f) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s [--repeat N] <raw_file>...\n", argv[0]);
    return 2;
  }

  // --repeat N decodes each file N times and prints, per decode, the
  // estimator's own wall time from RawTimingDiagnostics::auto_exposure_ms
  // (raw_ffi_api.h:119 — the same quantity T-O1 bracketed). This is the AC 8
  // w1 latency instrument; it changes nothing about the EV lines above it.
  int repeat = 1;
  std::vector<const char*> paths;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
      repeat = std::atoi(argv[++i]);
      if (repeat < 1) repeat = 1;
      continue;
    }
    paths.push_back(argv[i]);
  }

  for (size_t pi = 0; pi < paths.size(); ++pi) {
    const char* path = paths[pi];
    RawDevelopParams develop{};
    develop.exposure_ev = 0.0f;
    develop.tone_curve_strength = 1.0f;
    develop.output_space = kRawOutputColorSpaceSrgb;
    develop.max_output_long_edge = 0u;
    develop.auto_exposure_mode = kRawAutoExposureOn;  // the whole point of this gate

    uint32_t pw = 0, ph = 0;
    RawErrorCode probe_rc =
        raw_pipeline_probe_output_size(path, develop.max_output_long_edge, &pw, &ph);
    if (probe_rc != kRawSuccess || pw == 0 || ph == 0) {
      std::printf("EV path=%s rc=%d ae_status=0 ev_bits=0x00000000 ev=0\n", path,
                  static_cast<int>(probe_rc));
      std::fflush(stdout);
      continue;
    }

    std::vector<uint8_t> dst(static_cast<size_t>(pw) * ph * 4);
    for (int iteration = 0; iteration < repeat; ++iteration) {
      RawPipelineResult result{};
      RawErrorCode rc = raw_pipeline_decode_file_into(path, develop, dst.data(), dst.size(),
                                                      result);
      if (iteration == 0) {
        std::printf("EV path=%s rc=%d ae_status=%u ev_bits=0x%08x ev=%.9g\n", path,
                    static_cast<int>(rc), result.color_diag.auto_exposure_status,
                    floatBits(result.color_diag.auto_exposure_ev),
                    static_cast<double>(result.color_diag.auto_exposure_ev));
      }
      if (repeat > 1) {
        // One line per decode, iteration index included so the pre-declared
        // "iteration 0 is cold" exclusion can be applied by the reader rather
        // than silently inside the instrument.
        std::printf("TIMING path=%s iter=%d rc=%d auto_exposure_ms=%.6f "
                    "estimator_ms=%.6f ev_bits=0x%08x\n",
                    path, iteration, static_cast<int>(rc), result.timing.auto_exposure_ms,
                    result.color_diag.auto_exposure_estimator_ms,
                    floatBits(result.color_diag.auto_exposure_ev));
      }
      std::fflush(stdout);
    }
  }
  return 0;
}
