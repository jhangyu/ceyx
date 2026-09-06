// test_errmap_dst_too_small.cpp — WP10 regression test for Task #3
// (r5-remediation): the -213(internal)/-301(boundary) error-code mapping at
// ceyx_decode_into_ffi.cpp:154.
//
// SCOPE, READ BEFORE TRUSTING THIS GREEN:
//
// Investigation for this task (r5-errmap-evidence.txt) established that the
// public ceyx_decode_into_buffer entry point can NEVER observe the RAW arm's
// `rc == kRawErrDstTooSmall` branch (ceyx_decode_into_ffi.cpp:145-158) with any
// sample in this repo's corpus, including the Foveon/X3F class
// (sigma_sd_quattro_h_19.x3f) that is the one documented case where probe and
// post-unpack extents could disagree. Phase 2 of ceyx_decode_into_buffer
// (ceyx_decode_into_ffi.cpp:92-96) computes `need` from ceyx_probe_output_size
// and refuses with kCeyxErrDstTooSmall BEFORE Phase 3 ever runs, whenever
// dst_capacity < need. raw_pipeline_probe_output_size deliberately reuses the
// exact same scaledOutputExtent() call the decode branches use (raw_gpu_
// pipeline.cpp:864-867, "probe/decode drift is structural rather than
// test-dependent"), so for every sample in the corpus `need` computed by the
// probe EQUALS the true post-unpack byte count -- there is no capacity window
// in which Phase 2 passes but the RAW pipeline's own makeRgbaCheckout backstop
// (raw_gpu_pipeline.cpp:110-141, anonymous-namespace, not independently
// linkable) still refuses. R4's own wp10n evidence reached the identical
// conclusion ("a sample-based test is unsatisfiable") and used a TEMPORARY
// probe-perturbation seam, removed afterwards, to force the condition once.
// Team-lead ruling (r5, 2026-09-06): do not reintroduce a permanent seam into
// raw_gpu_pipeline.cpp without a user decision. This test does NOT reintroduce
// one, and therefore does NOT exercise ceyx_decode_into_ffi.cpp:154 itself
// through the public API. That branch remains DEAD CODE for the current
// sample corpus -- confirmed by the mutation run recorded in
// r5-errmap-evidence.txt, which reverted the line and observed NO test in
// this repo (including this file and test_ceyx_decode_into.cpp) go red.
//
// WHAT THIS TEST DOES COVER, durably and without any seam:
//
// 1. caseInternalCodeIsReal: raw_pipeline_decode_file_into (raw_gpu_pipeline.h,
//    externally linkable, non-static) is called DIRECTLY with a dst_capacity
//    one byte short of the probed requirement. This bypasses the FFI wrapper's
//    Phase 2 pre-check entirely and lands straight in makeRgbaCheckout's
//    caller_dst_capacity < bytes branch (raw_gpu_pipeline.cpp:138). Asserts the
//    RAW pipeline itself returns kRawErrDstTooSmall (-213) -- i.e. the LHS of
//    the map really is live production code, reachable by direct call. This is
//    real, standing regression coverage with zero injection.
// 2. caseBoundaryNeverLeaksMinus213: calls the PUBLIC ceyx_decode_into_buffer
//    with the same undersized capacity and asserts the caller observes
//    kCeyxErrDstTooSmall (-301) and never the internal kRawErrDstTooSmall
//    (-213). This is the externally-visible half of the contract the map line
//    exists to guarantee, and it is exercised via Phase 2's OWN direct
//    assignment (ceyx_decode_into_ffi.cpp:96) -- a different line than :154,
//    but the same observable promise ("the public boundary never returns
//    -213"). Reverting Phase 2's assignment (a change this test's mutation run
//    does NOT perform, since that line is unconditional and not the one under
//    remediation) would turn this red; reverting :154 specifically does not,
//    for the structural reason above.
//
// Usage: test_errmap_dst_too_small <bayer.dng> <bayer.arw> <xtrans.raf> <x3f>
// Any argument may be omitted; each omission prints [GAP] and fails the run
// (same policy as test_ceyx_decode_into.cpp).

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "ceyx_decode_into.h"
#include "dng_ffi_api.h"
#include "raw_gpu_pipeline.h"

static int g_failures = 0;
#define CHECK(cond, ...)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "[FAIL] %s:%d: ", __FILE__, __LINE__);             \
      std::fprintf(stderr, __VA_ARGS__);                                      \
      std::fprintf(stderr, "\n");                                            \
      ++g_failures;                                                          \
    }                                                                         \
  } while (0)

static bool probe(const char *path, int32_t maxDim, int32_t *w, int32_t *h) {
  const int32_t rc = ceyx_probe_output_size(path, maxDim, w, h);
  CHECK(rc == 0, "probe rc=%d for %s", rc, path);
  return rc == 0 && *w > 0 && *h > 0;
}

// Direct call into the RAW pipeline layer, bypassing ceyx_decode_into_buffer's
// Phase 2 pre-check, so the internal makeRgbaCheckout capacity backstop is the
// ONLY thing standing between an undersized buffer and a decode attempt.
//
// DNG-route samples must NOT be passed here: decodeFileImpl's kRawRouteDng arm
// (raw_gpu_pipeline.cpp:730-753) forwards straight to ceyx_decode_into_buffer
// with the caller's buffer -- i.e. it re-enters the FULL public FFI entry,
// Phase 2 pre-check included, and returns THAT function's kCeyxErrDstTooSmall
// (-301) directly. Discovered empirically: an earlier revision of this test
// included a .dng sample here and got -301 instead of -213, which is not a
// test bug in the assertion but a real routing fact worth documenting. Only
// generic-RAW-route samples (.arw/.raf/.x3f) reach makeRgbaCheckout through
// this call.
static void caseInternalCodeIsReal(const char *path, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, 0, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  if (need < 2) return;  // degenerate extent, nothing to shrink
  std::vector<uint8_t> buf(need - 1, 0xCD);

  RawDevelopParams develop{};
  develop.exposure_ev = 0.0f;
  develop.tone_curve_strength = 1.0f;
  develop.output_space = kRawOutputColorSpaceSrgb;
  develop.max_output_long_edge = 0;

  RawPipelineResult out;
  const RawErrorCode rc = raw_pipeline_decode_file_into(
      path, develop, buf.data(), need - 1, out);
  CHECK(rc == kRawErrDstTooSmall,
        "[%s] direct pipeline call did not report kRawErrDstTooSmall (got %d)",
        label, static_cast<int>(rc));
}

// Public-API view of the same scenario: the caller must see -301, never -213,
// regardless of which internal layer produced the refusal.
static void caseBoundaryNeverLeaksMinus213(const char *path,
                                           const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, 0, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  if (need < 2) return;
  std::vector<uint8_t> buf(need - 1, 0xCD);

  DngResult *r = ceyx_decode_into_buffer(path, 0, buf.data(), need - 1);
  CHECK(r != nullptr, "[%s] null result", label);
  if (!r) return;
  CHECK(r->error_code == kCeyxErrDstTooSmall,
        "[%s] boundary error_code=%d, expected kCeyxErrDstTooSmall(-301)",
        label, r->error_code);
  CHECK(r->error_code != static_cast<int32_t>(kRawErrDstTooSmall),
        "[%s] internal code -213 LEAKED to the public boundary", label);
  dng_free_result(r);
}

int main(int argc, char **argv) {
  struct Sample { const char *path; const char *label; bool dng_route; };
  const Sample samples[] = {
    {argc > 1 ? argv[1] : nullptr, "bayer-dng", true},
    {argc > 2 ? argv[2] : nullptr, "bayer-raw", false},
    {argc > 3 ? argv[3] : nullptr, "xtrans-raw", false},
    {argc > 4 ? argv[4] : nullptr, "linear-rgb-raw-x3f", false},
  };
  for (const auto &s : samples) {
    if (!s.path) {
      std::fprintf(stderr, "[GAP] class=%s not exercised. REPORT THIS.\n",
                   s.label);
      ++g_failures;
      continue;
    }
    // DNG-route samples re-enter the public FFI entry instead of
    // makeRgbaCheckout (see caseInternalCodeIsReal's header comment) -- only
    // run the direct-pipeline probe for generic-RAW samples.
    if (!s.dng_route) caseInternalCodeIsReal(s.path, s.label);
    caseBoundaryNeverLeaksMinus213(s.path, s.label);
  }
  std::fprintf(stderr, "%s: %d failure(s)\n", argv[0], g_failures);
  return g_failures == 0 ? 0 : 1;
}
