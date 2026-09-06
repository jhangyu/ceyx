// test_raw_diagnostics_freshness.cpp — R6 fix gate.
//
// Bug: ceyx_decode_into_buffer's generic-RAW arm (ceyx_decode_into_ffi.cpp)
// called raw_pipeline_decode_file_into and filled DngResult::decode_ms/
// process_ms from its RawPipelineResult, but never wrote that result into the
// thread-local state raw_last_diagnostics()/raw_last_color_diagnostics() read
// -- only raw_decode_and_process (raw_ffi_api.cpp) did that. So after a
// decode-into call, those two legacy queries kept describing whichever
// raw_decode_and_process call had last run on the thread (or "no decode has
// run" if none ever had): stale by construction, not merely by timing.
//
// Fix: raw_ffi_api.cpp now exposes raw_record_decode_into_diagnostics(),
// which ceyx_decode_into_ffi.cpp calls with its own out.diag/out.color_diag
// right after raw_pipeline_decode_file_into returns (success or failure,
// mirroring raw_decode_and_process's own unconditional write).
//
// This test proves freshness, not just presence: it asserts raw_last_
// diagnostics() reports NOTHING before any call is made on this (fresh
// process's) thread, then that the SOLE decode-into call this test makes is
// what populates it -- by requiring the reported raw_unpack_ms to equal this
// exact call's own DngResult::decode_ms, a real measured duration that no
// leftover/stale write could coincidentally reproduce.
//
// Usage: test_raw_diagnostics_freshness <generic-raw-sample>
// The sample MUST be a generic-RAW file (e.g. a Bayer .ARW) that takes the
// DNG_ENABLE_GENERIC_RAW route -- a .dng has no legacy RAW diagnostics
// channel at all (raw_last_diagnostics is RAW-route-only by design), so it
// cannot exercise what this test checks.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ceyx_decode_into.h"
#include "dng_ffi_api.h"
#include "raw_ffi_api.h"
#include "raw_pipeline_contract.h"  // full RawDecodeDiagnostics definition

static int g_failures = 0;
#define CHECK(cond, ...)                                          \
  do {                                                            \
    if (!(cond)) {                                                \
      std::fprintf(stderr, "[FAIL] %s:%d: ", __FILE__, __LINE__); \
      std::fprintf(stderr, __VA_ARGS__);                          \
      std::fprintf(stderr, "\n");                                 \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

int main(int argc, char **argv) {
  if (argc < 2) {
    // A GAP is a failure, not a note -- an invocation with no sample must not
    // report RC=0 (same rule test_ceyx_decode_into.cpp's main() follows).
    std::fprintf(stderr,
                  "[GAP] no generic-RAW sample given. REPORT THIS.\n");
    return 1;
  }
  const char *path = argv[1];

  RawDecodeDiagnostics before{};
  CHECK(raw_last_diagnostics(&before) == -1,
        "raw_last_diagnostics reported a decode before any FFI call was "
        "made on this thread -- test precondition violated, cannot prove "
        "freshness");

  int32_t w = 0, h = 0;
  const int32_t probe_rc = ceyx_probe_output_size(path, 0, &w, &h);
  CHECK(probe_rc == 0 && w > 0 && h > 0, "probe failed rc=%d for %s",
        probe_rc, path);
  if (probe_rc != 0 || w <= 0 || h <= 0) {
    std::fprintf(stderr, "%s: %d failure(s)\n", argv[0], g_failures);
    return 1;
  }

  const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
  std::vector<uint8_t> buf(need);
  DngResult *result = ceyx_decode_into_buffer(path, 0, buf.data(), need);
  CHECK(result != nullptr, "ceyx_decode_into_buffer returned null");
  if (!result) {
    std::fprintf(stderr, "%s: %d failure(s)\n", argv[0], g_failures);
    return 1;
  }
  CHECK(result->error_code == 0, "decode-into failed, error_code=%d",
        result->error_code);

  RawDecodeDiagnostics diag{};
  const int32_t diag_rc = raw_last_diagnostics(&diag);
  CHECK(diag_rc == 0,
        "raw_last_diagnostics returned %d after a decode-into call -- the "
        "decode-into path did not record diagnostics (the pre-fix defect)",
        diag_rc);

  // The discriminating assertion (see file header): raw_unpack_ms is a real
  // measured duration unique to THIS call. `before` already proved nothing
  // had been recorded prior to it, so this equality can only hold if this
  // exact decode-into call is what wrote it.
  if (diag_rc == 0) {
    CHECK(diag.raw_unpack_ms == result->decode_ms,
          "raw_last_diagnostics().raw_unpack_ms (%f) does not match this "
          "call's own DngResult.decode_ms (%f) -- diagnostics are stale or "
          "attributable to a different call",
          diag.raw_unpack_ms, result->decode_ms);
  }

  dng_free_result(result);

  std::fprintf(stderr, "%s: %d failure(s)\n", argv[0], g_failures);
  return g_failures == 0 ? 0 : 1;
}
