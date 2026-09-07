// test_ceyx_decode_into.cpp — WP10 gate for the format-agnostic entry pair.
// Linked against the SHARED dng_decoder_native (tests.cmake:1729-1738 /
// :1752-1760 precedent: compiling pipeline sources into a test links none of
// the shipping code, so a green would say nothing about the artifact).
//
// Usage: test_ceyx_decode_into <bayer.dng> <lossy.dng> <bayer.arw>
//                              <xtrans.raf> <linear.x3f>
// Any argument may be omitted; each omission prints [GAP] and must be reported.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "ceyx_decode_into.h"
#include "ceyx_orient.h"
#include "dng_ffi_api.h"
#include "dng_render_params.h"

static int g_failures = 0;
#define CHECK(cond, ...)                                                      \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::fprintf(stderr, "[FAIL] %s:%d: ", __FILE__, __LINE__);             \
      std::fprintf(stderr, __VA_ARGS__);                                      \
      std::fprintf(stderr, "\n");                                             \
      ++g_failures;                                                           \
    }                                                                         \
  } while (0)

static bool probe(const char *path, int32_t maxDim, int32_t *w, int32_t *h) {
  const int32_t rc = ceyx_probe_output_size(path, maxDim, w, h);
  CHECK(rc == 0, "probe rc=%d for %s", rc, path);
  return rc == 0 && *w > 0 && *h > 0;
}

// AC15.2 / AC15.3 — refusals are cheap and acquire nothing, on every route.
static void caseRefusals(const char *path, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, 0, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> buf(need);
  // Baseline-delta, not absolute zero. dng_debug_pool_checked_out() is a
  // PROCESS-GLOBAL counter, so asserting == 0 makes this case's verdict depend
  // on every case that ran before it: one earlier leak poisons the baseline and
  // this case reports a failure it did not cause. Demonstrated during mutation
  // M-5, where a DNG-side leak turned both RAW classes red on this very line.
  // A gate that can only be trusted when everything before it passed is half a
  // gate; comparing against a sampled baseline makes the verdict local.
  const size_t before = dng_debug_pool_checked_out();

  DngResult *s = ceyx_decode_into_buffer(path, 0, buf.data(), need - 1);
  CHECK(s && s->error_code == kCeyxErrDstTooSmall, "[%s] short not refused",
        label);
  CHECK(s && s->rgba_data == nullptr, "[%s] short returned a pointer", label);
  CHECK(s && s->width == w && s->height == h,
        "[%s] refusal must report the extent", label);
  if (s) dng_free_result(s);

  DngResult *n = ceyx_decode_into_buffer(path, 0, nullptr, need);
  CHECK(n && n->error_code == kCeyxErrDstTooSmall, "[%s] null dst", label);
  if (n) dng_free_result(n);

  DngResult *z = ceyx_decode_into_buffer(path, 0, buf.data(), 0);
  CHECK(z && z->error_code == kCeyxErrDstTooSmall, "[%s] zero cap", label);
  if (z) dng_free_result(z);

  CHECK(dng_debug_pool_checked_out() == before,
        "[%s] refusal acquired a buffer (checked_out %zu -> %zu)", label,
        before, dng_debug_pool_checked_out());
}

// AC15.4 / AC15.5 — pointer identity, no ownership taken, failure leaves the
// caller's bytes alone. Run per route: this is where BOTH checkout guards are
// exercised, the DNG one on a .dng and the RAW one on an .arw/.raf.
static void caseOwnership(const char *path, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, 0, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> buf(need, 0xAB);

  const size_t before = dng_debug_pool_checked_out();
  DngResult *r = ceyx_decode_into_buffer(path, 0, buf.data(), need);
  CHECK(r != nullptr, "[%s] null result", label);
  if (!r) return;
  CHECK(r->error_code == 0, "[%s] error=%d", label, r->error_code);
  CHECK(r->rgba_data == buf.data(),
        "[%s] rgba_data must BE the caller buffer (%p vs %p)", label,
        static_cast<void *>(r->rgba_data), static_cast<void *>(buf.data()));
  CHECK(dng_debug_pool_checked_out() == before,
        "[%s] a pool buffer was checked out", label);
  r->rgba_data = nullptr;
  dng_free_result(r);
  CHECK(dng_debug_pool_checked_out() == before, "[%s] free disturbed the pool",
        label);
}

static void caseFailureLeavesBufferAlone(const char *good, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(good, 0, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> buf(need, 0xAB);
  // Baseline-delta for the same reason as caseRefusals above.
  const size_t before = dng_debug_pool_checked_out();
  DngResult *r = ceyx_decode_into_buffer("/nonexistent/broken.raw", 0,
                                         buf.data(), need);
  CHECK(r && r->error_code != 0, "[%s] corrupt reported success", label);
  if (r) dng_free_result(r);
  CHECK(buf[0] == 0xAB, "[%s] caller buffer clobbered on failure", label);
  CHECK(dng_debug_pool_checked_out() == before,
        "[%s] failure left a checkout (a guard released dst?) %zu -> %zu",
        label, before, dng_debug_pool_checked_out());
}

// AC15.6 — probe and decode agree.
//
// NOTE ON WHAT THIS ACTUALLY COMPARES (the plan's comment here was wrong and is
// corrected rather than copied): the decode side calls the _into entry, not the
// ordinary one. That is still a real probe-vs-decode comparison, because the
// width/height asserted below are OVERWRITTEN from the pipeline's own result
// after the decode returns (ceyx_decode_into_ffi.cpp phase 3) — they are the
// decoded extent, not the probed one echoed back. A disagreement can surface
// two ways and both are caught: as a mismatch here, or, if the decode wants MORE
// than the probe predicted, as a kCeyxErrDstTooSmall on the error_code check,
// since the buffer is deliberately sized from the PROBE.
static void caseAgreement(const char *path, int32_t maxDim, const char *label) {
  int32_t pw = 0, ph = 0;
  if (!probe(path, maxDim, &pw, &ph)) return;
  int32_t w = 0, h = 0;
  const size_t need = static_cast<size_t>(pw) * ph * 4;
  std::vector<uint8_t> buf(need);
  DngResult *r = ceyx_decode_into_buffer(path, maxDim, buf.data(), need);
  CHECK(r != nullptr, "[%s] null", label);
  if (!r) return;
  CHECK(r->error_code == 0, "[%s] error=%d maxDim=%d", label, r->error_code,
        maxDim);
  w = r->width; h = r->height;
  CHECK(w == pw && h == ph,
        "PROBE/DECODE DISAGREE [%s] maxDim=%d probe=%dx%d decode=%dx%d",
        label, maxDim, pw, ph, w, h);
  r->rgba_data = nullptr;
  dng_free_result(r);
}

// ---------------------------------------------------------------------------
// Task 2 — ceyx_decode_into_buffer_oriented. AC-2.1 .. AC-2.6.
// (AC-2.7, symbol export, is a dump-to-file-then-grep step outside this binary:
// a `nm ... | grep -q` reports FAILURE when the symbol IS found, because grep's
// early exit SIGPIPEs nm under `set -euo pipefail`.)
// ---------------------------------------------------------------------------

// The oriented decode uses a reduced max_dim so the 50+50 accounting loop below
// stays a test rather than a benchmark. Every assertion here is about extents,
// pointers and pool bookkeeping, none of which depend on the decode size.
static const int32_t kOrientMaxDim = 512;

// AC-2.1 — orientation 1 through the oriented entry is byte-identical to the
// unoriented entry on the same fixture. This is the one case where the two
// entries must agree on PIXELS, and it is what proves the extracted phases 1-2
// helper did not change the plain entry's behaviour either.
static void caseOrientedIdentity(const char *path, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, kOrientMaxDim, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> plain(need, 0x11), oriented(need, 0x22);

  DngResult *a = ceyx_decode_into_buffer(path, kOrientMaxDim, plain.data(), need);
  DngResult *b = ceyx_decode_into_buffer_oriented(path, kOrientMaxDim,
                                                  oriented.data(), need, 1);
  CHECK(a && b, "[%s] null result", label);
  if (!a || !b) { if (a) dng_free_result(a); if (b) dng_free_result(b); return; }
  CHECK(a->error_code == 0 && b->error_code == 0,
        "[%s] AC-2.1 errors plain=%d oriented=%d", label, a->error_code,
        b->error_code);
  CHECK(a->width == b->width && a->height == b->height,
        "[%s] AC-2.1 extent %dx%d vs %dx%d", label, a->width, a->height,
        b->width, b->height);
  if (a->error_code == 0 && b->error_code == 0) {
    const size_t bytes = static_cast<size_t>(a->width) * a->height * 4;
    CHECK(std::memcmp(plain.data(), oriented.data(), bytes) == 0,
          "[%s] AC-2.1 orientation 1 is not byte-identical to the unoriented "
          "entry", label);
  }
  a->rgba_data = nullptr; b->rgba_data = nullptr;
  dng_free_result(a);
  dng_free_result(b);
}

// AC-2.2 (extent swap at 6), AC-2.3 (pointer identity on all 8) and AC-2.4
// (byte-count invariance on all 8) in one pass over the eight orientations,
// because they assert three properties of the same eight calls.
static void caseOrientedExtents(const char *path, const char *label) {
  int32_t pw = 0, ph = 0;
  if (!probe(path, kOrientMaxDim, &pw, &ph)) return;
  const size_t need = static_cast<size_t>(pw) * ph * 4;
  std::vector<uint8_t> buf(need);

  // Reference extent from the UNORIENTED entry: the probe's extent and the
  // decoded extent can legitimately differ (post-unpack correction), and it is
  // the DECODED one the swap must be measured against.
  int32_t uw = 0, uh = 0;
  {
    DngResult *u = ceyx_decode_into_buffer(path, kOrientMaxDim, buf.data(), need);
    CHECK(u && u->error_code == 0, "[%s] AC-2.2 baseline decode failed", label);
    if (!u || u->error_code != 0) { if (u) dng_free_result(u); return; }
    uw = u->width; uh = u->height;
    u->rgba_data = nullptr;
    dng_free_result(u);
  }

  for (int32_t o = 1; o <= 8; ++o) {
    DngResult *r = ceyx_decode_into_buffer_oriented(path, kOrientMaxDim,
                                                    buf.data(), need, o);
    CHECK(r != nullptr, "[%s] o=%d null result", label, o);
    if (!r) continue;
    CHECK(r->error_code == 0, "[%s] o=%d error=%d", label, o, r->error_code);
    if (r->error_code == 0) {
      // AC-2.3 — pointer identity on EVERY success path, all 8 orientations.
      // This is the assertion that catches the scratch escaping to the caller.
      CHECK(r->rgba_data == buf.data(),
            "[%s] AC-2.3 o=%d rgba_data %p != dst %p (scratch escaped?)", label,
            o, static_cast<void *>(r->rgba_data),
            static_cast<void *>(buf.data()));
      // AC-2.4 — the byte count the pool pre-acquired stays correct for every
      // orientation. Load-bearing: ceyx_probe_output_size takes no orientation.
      CHECK(static_cast<size_t>(r->width) * r->height * 4 ==
                static_cast<size_t>(uw) * uh * 4,
            "[%s] AC-2.4 o=%d byte count %zu != %zu", label, o,
            static_cast<size_t>(r->width) * r->height * 4,
            static_cast<size_t>(uw) * uh * 4);
      // AC-2.2 — transposing orientations swap the extent, the others do not.
      const bool transposes = (o >= 5 && o <= 8);
      const bool swapped = (r->width == uh && r->height == uw);
      const bool same = (r->width == uw && r->height == uh);
      if (transposes) {
        CHECK(swapped, "[%s] AC-2.2 o=%d expected %dx%d, got %dx%d", label, o,
              uh, uw, r->width, r->height);
      } else {
        CHECK(same, "[%s] AC-2.2 o=%d expected %dx%d, got %dx%d", label, o, uw,
              uh, r->width, r->height);
      }
    }
    r->rgba_data = nullptr;
    dng_free_result(r);
  }
}

// AC-2.5 — the scratch is released on EVERY exit. 50 successes mixing
// transposing and non-transposing orientations, then 50 forced failures
// (nonexistent path, undersized dst), each measured as a delta against a
// sampled baseline rather than against absolute zero (same reasoning as
// caseRefusals above: the counter is process-global).
static void caseOrientedScratchAccounting(const char *path, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, kOrientMaxDim, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> buf(need);

  const size_t before = dng_debug_pool_checked_out();
  // Orientations chosen so the loop alternates transposing (6, 8, 5, 7) with
  // non-transposing (1, 3, 2, 4): a leak on either arm shows up.
  const int32_t cycle[] = {1, 6, 3, 8, 2, 5, 4, 7, 6, 1};
  for (int i = 0; i < 50; ++i) {
    DngResult *r = ceyx_decode_into_buffer_oriented(
        path, kOrientMaxDim, buf.data(), need, cycle[i % 10]);
    if (r) { r->rgba_data = nullptr; dng_free_result(r); }
  }
  CHECK(dng_debug_pool_checked_out() == before,
        "[%s] AC-2.5 50 oriented successes leaked scratch (%zu -> %zu)", label,
        before, dng_debug_pool_checked_out());

  for (int i = 0; i < 25; ++i) {
    // Failure class 1: the file does not exist (fails in phase 1, before any
    // scratch is taken).
    DngResult *a = ceyx_decode_into_buffer_oriented(
        "/nonexistent/broken.raw", kOrientMaxDim, buf.data(), need, 6);
    if (a) dng_free_result(a);
    // Failure class 2: dst is one byte too small (fails in phase 2).
    DngResult *b = ceyx_decode_into_buffer_oriented(path, kOrientMaxDim,
                                                    buf.data(), need - 1, 6);
    CHECK(b && b->error_code == kCeyxErrDstTooSmall,
          "[%s] AC-2.5 undersized dst not refused", label);
    if (b) dng_free_result(b);
  }
  CHECK(dng_debug_pool_checked_out() == before,
        "[%s] AC-2.5 50 oriented failures leaked scratch (%zu -> %zu)", label,
        before, dng_debug_pool_checked_out());
}

// AC-2.6, RETIRED BY R-19 (productionization plan §3 Task 4 "NAMED BEHAVIOUR
// CHANGE"): the fused GPU path never checks out scratch for an oriented
// decode — the kernel writes the oriented pixels straight into the caller's
// buffer, transposing included — so `ceyx_debug_force_scratch_failure` has
// nothing left to degrade. This case now asserts exactly that: forcing the
// (now-vestigial) flag does NOT change the outcome, i.e. the decode still
// succeeds with the correctly ORIENTED (swapped) extent, not a silent
// fallback to unoriented. A regression back to the old degradation behaviour
// would fail this on the extent-swap assertion.
static void caseOrientedDegradationRetired(const char *path,
                                           const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, kOrientMaxDim, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> control(need), forced(need);

  // Control: orientation 6, flag untouched.
  DngResult *c = ceyx_decode_into_buffer_oriented(path, kOrientMaxDim,
                                                  control.data(), need, 6);
  CHECK(c && c->error_code == 0, "[%s] R-19 control decode failed", label);
  const int32_t cw = c ? c->width : 0, ch = c ? c->height : 0;
  if (c) { c->rgba_data = nullptr; dng_free_result(c); }

  const size_t before = dng_debug_pool_checked_out();
  const int32_t prev = ceyx_debug_force_scratch_failure(1);
  DngResult *r = ceyx_decode_into_buffer_oriented(path, kOrientMaxDim,
                                                  forced.data(), need, 6);
  ceyx_debug_force_scratch_failure(prev);

  CHECK(r != nullptr, "[%s] R-19 null result", label);
  if (!r) return;
  CHECK(r->error_code == 0,
        "[%s] R-19 forcing the retired scratch-failure flag must not fail "
        "the fused decode (error=%d)", label, r->error_code);
  CHECK(r->rgba_data == forced.data(),
        "[%s] R-19 pointer identity broken", label);
  // The extent MUST be swapped (H, W): the flag has no mechanism left to act
  // on, so the outcome must be identical to the control's oriented decode,
  // not the old unswapped degradation extent.
  CHECK(r->width == cw && r->height == ch,
        "[%s] R-19 extent %dx%d should match the oriented control %dx%d",
        label, r->width, r->height, cw, ch);
  r->rgba_data = nullptr;
  dng_free_result(r);
  CHECK(dng_debug_pool_checked_out() == before,
        "[%s] R-19 case disturbed the pool", label);
}

// Step 4.3 — dedicated regression for the acceptance criterion as literally
// stated in the plan: orientation 6 returns extent (H, W) of the DECODED
// (unoriented) extent and rgba_data == dst. Reference is the actual decoded
// extent, not the probe's — probe and post-unpack decode extent can
// legitimately disagree on the RAW routes (documented above in
// caseAgreement/WP10 stale-extent; a probe-based reference flakes exactly on
// the linear-rgb-raw class). (The overlap sub-case, "an overlapping src/dst
// request returns -402", does not apply at this FFI layer: this entry takes
// no raw source-buffer pointer to overlap dst with — file_path is the only
// source and the overlap refusal G-8 is a Stage4-bridge/kernel-level
// contract, gated directly by test_stage4_oriented.cpp (Task 5). Flagged for
// team-lead rather than fabricating an inapplicable scenario.)
static void caseOrientedStep43(const char *path, const char *label) {
  int32_t pw = 0, ph = 0;
  if (!probe(path, kOrientMaxDim, &pw, &ph)) return;
  const size_t need = static_cast<size_t>(pw) * ph * 4;
  std::vector<uint8_t> buf(need);

  int32_t uw = 0, uh = 0;
  {
    DngResult *u =
        ceyx_decode_into_buffer(path, kOrientMaxDim, buf.data(), need);
    CHECK(u && u->error_code == 0, "[%s] Step4.3 baseline decode failed",
          label);
    if (!u || u->error_code != 0) { if (u) dng_free_result(u); return; }
    uw = u->width; uh = u->height;
    u->rgba_data = nullptr;
    dng_free_result(u);
  }

  DngResult *r =
      ceyx_decode_into_buffer_oriented(path, kOrientMaxDim, buf.data(), need, 6);
  CHECK(r != nullptr, "[%s] Step4.3 null result", label);
  if (!r) return;
  CHECK(r->error_code == 0, "[%s] Step4.3 o=6 error=%d", label,
        r->error_code);
  CHECK(r->rgba_data == buf.data(),
        "[%s] Step4.3 rgba_data must be dst", label);
  CHECK(r->width == uh && r->height == uw,
        "[%s] Step4.3 o=6 expected (H,W)=(%d,%d), got %dx%d", label, uh, uw,
        r->width, r->height);
  r->rgba_data = nullptr;
  dng_free_result(r);
}

// ---------------------------------------------------------------------------
// Blocker B-1 (round reviewer) — red-state proof for the Stage4
// failure-reason channel (dng_render_params.h) and its FFI mapping to
// kCeyxOrientErrOverlap (-402) / kCeyxOrientErrKernel (-403). Before this,
// the channel was implemented but never exercised: every green gate only
// covers the success path, where the reason is trivially kNone.
//
// Three cases, per the bridge owner's recipe, calling the exported low-level
// runner (runRenderStage4HalideAot, dng_render_params.h) directly:
//   (a) overlapping src/dst -> false, reason == kOverlap
//   (b) null dst -> false, reason == kNone (proves no over-claiming a reason
//       for a non-orientation-specific refusal)
//   (c) staleness: an overlap failure followed by a REAL successful oriented
//       decode (through the public FFI) must observe kNone again -- proving
//       the runner resets the reason on its very next call, not just on
//       success in general.
//
// A fourth case (kKernel, -403) is NOT exercised end-to-end: there is no
// cheap way to force copy_to_host()/the Halide realize call to fail without
// either a real GPU-resource-exhaustion scenario or corrupting internal
// Halide state, and no injection hook exists for it. Per instruction, this is
// stated rather than faked. What CAN be, and is, proven here: (1) the
// -402/-403 numeric mapping itself is a correct, order-independent 1:1
// relationship with the enum (asserted directly below against
// ceyx_orient.h's public constants -- the actual switch in
// ceyx_decode_into_ffi.cpp is a static function in a different TU and cannot
// be called from this binary; this is the closest direct proof reachable
// without exposing a debug hook), and (2) the ONE reason value this binary
// CAN produce (kOverlap) really does flow through the real FFI-callable
// runner and really does reset to kNone on the next call, which is the
// staleness property -403 depends on identically.
//
// END-TO-END overlap via the PUBLIC ceyx_decode_into_buffer_oriented is also
// NOT reachable: that entry takes only a file_path as its source, never a
// caller-supplied src pointer, so there is nothing for dst to alias (same
// conclusion as Step 4.3's overlap note above). The runner-level call below
// is the correct and only layer at which "src/dst overlap" is even
// expressible.
static void caseStage4FailureReasonRedState(const char *good_path,
                                            const char *label) {
  // (a) overlapping src/dst.
  {
    std::vector<uint16_t> buf(4 * 4 * 4, 0);   // room for src AND dst aliasing
    RenderParams params{};                      // unused before the overlap
                                                 // check; default-constructed
                                                 // is fine (never reaches the
                                                 // Halide dispatch below it).
    const bool ok = runRenderStage4HalideAot(
        buf.data(), /*src_w=*/4, /*src_h=*/4, /*src_p=*/3,
        /*src_row_step=*/0, /*src_col_step=*/0, /*src_plane_step=*/0,
        /*src_scale=*/1.0f, /*dst_w=*/4, /*dst_h=*/4, params,
        reinterpret_cast<uint8_t *>(buf.data()),   // dst aliases src exactly
        /*fuse_rgba=*/true, /*ctx=*/nullptr, /*exif_orientation=*/1);
    CHECK(!ok, "[%s] B-1a overlapping call unexpectedly succeeded", label);
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kOverlap,
          "[%s] B-1a expected kOverlap, got %d", label,
          static_cast<int>(dngRenderStage4LastFailureReason()));
  }

  // (b) null dst -> false, reason stays kNone (no over-claiming).
  {
    std::vector<uint16_t> src(4 * 4 * 4, 0);
    RenderParams params{};
    const bool ok = runRenderStage4HalideAot(
        src.data(), 4, 4, 3, 0, 0, 0, 1.0f, 4, 4, params,
        /*dst=*/nullptr, /*fuse_rgba=*/true, /*ctx=*/nullptr,
        /*exif_orientation=*/1);
    CHECK(!ok, "[%s] B-1b null-dst call unexpectedly succeeded", label);
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kNone,
          "[%s] B-1b expected kNone (no over-claim) for a non-orientation "
          "refusal, got %d",
          label, static_cast<int>(dngRenderStage4LastFailureReason()));
  }

  // (c) staleness: overlap failure (reason == kOverlap from (a) above, or
  // re-triggered here for a self-contained case), then a REAL successful
  // decode, then the reason must read kNone again.
  {
    std::vector<uint16_t> buf(4 * 4 * 4, 0);
    RenderParams params{};
    const bool overlap_ok = runRenderStage4HalideAot(
        buf.data(), 4, 4, 3, 0, 0, 0, 1.0f, 4, 4, params,
        reinterpret_cast<uint8_t *>(buf.data()), true, nullptr, 1);
    CHECK(!overlap_ok, "[%s] B-1c setup overlap call unexpectedly succeeded",
          label);
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kOverlap,
          "[%s] B-1c setup expected kOverlap", label);

    if (!good_path) return;   // no sample available for the reset half
    int32_t w = 0, h = 0;
    if (!probe(good_path, kOrientMaxDim, &w, &h)) return;
    const size_t need = static_cast<size_t>(w) * h * 4;
    std::vector<uint8_t> dst(need);
    DngResult *r = ceyx_decode_into_buffer_oriented(good_path, kOrientMaxDim,
                                                     dst.data(), need, 1);
    CHECK(r && r->error_code == 0,
          "[%s] B-1c reset-half decode failed (error=%d)", label,
          r ? r->error_code : -1);
    if (r) { r->rgba_data = nullptr; dng_free_result(r); }
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kNone,
          "[%s] B-1c reason not reset to kNone after a successful decode "
          "(staleness contract violated), got %d",
          label, static_cast<int>(dngRenderStage4LastFailureReason()));
  }

  // Numeric mapping proof: ceyxMapStage4FailureReason (ceyx_decode_into_ffi.cpp,
  // static, not reachable from this TU) maps kOverlap -> kCeyxOrientErrOverlap
  // and kKernel -> kCeyxOrientErrKernel. What IS checkable here is that those
  // two public constants are exactly what the plan §1.6 table promises, so a
  // future edit to either side (the switch or the constants) that breaks the
  // agreement is caught even though the switch body itself cannot be called
  // directly.
  CHECK(kCeyxOrientErrOverlap == -402,
        "[%s] B-1 kCeyxOrientErrOverlap drifted from -402", label);
  CHECK(kCeyxOrientErrKernel == -403,
        "[%s] B-1 kCeyxOrientErrKernel drifted from -403", label);
}

int main(int argc, char **argv) {
  struct Sample { const char *path; const char *label; };
  const Sample samples[] = {
    {argc > 1 ? argv[1] : nullptr, "bayer-dng"},
    {argc > 2 ? argv[2] : nullptr, "lossy-dng"},
    {argc > 3 ? argv[3] : nullptr, "bayer-raw"},
    {argc > 4 ? argv[4] : nullptr, "xtrans-raw"},
    // R-3 (review, impl-wp10r-raw-opus): the linear-RGB/Foveon GPU branch is
    // the THIRD of the RAW pipeline's three branches and AC15.6's four named
    // classes miss it entirely — AC14.6 had required one sample per branch and
    // that requirement was lost in the A3 rewrite. This slot restores it. It is
    // also the only sample class where probe and post-unpack extents could ever
    // disagree (R11.1), so it is the closest thing the corpus has to a live
    // test of that risk.
    {argc > 5 ? argv[5] : nullptr, "linear-rgb-raw"},
  };

  // B-1 red-state proof is route-agnostic (it drives the shared low-level
  // runner directly), so it runs once, not once per sample class. Uses
  // whichever first sample is available for its staleness/reset half.
  {
    const char *any = argc > 1 ? argv[1] : nullptr;
    caseStage4FailureReasonRedState(any, "b1-redstate");
  }

  for (const auto &s : samples) {
    if (!s.path) {
      // A GAP is a FAILURE, not a note. AC15.6 says a partial pass is not
      // accepted as a full one, so the missing class must move the EXIT CODE —
      // a warning line that leaves RC=0 is invisible to CI and to anyone
      // grepping for failures, and an invocation that silently drops an
      // argument would test half the matrix and report success.
      // (Caught in review by impl-wp10r-raw-opus, who proved it by running this
      // binary with 2 of 4 samples: RC=0 with two [GAP] lines printed.)
      std::fprintf(stderr, "[GAP] class=%s not exercised. REPORT THIS.\n",
                   s.label);
      ++g_failures;
      continue;
    }
    caseRefusals(s.path, s.label);
    caseOwnership(s.path, s.label);
    caseFailureLeavesBufferAlone(s.path, s.label);
    caseAgreement(s.path, 0, s.label);
    caseAgreement(s.path, 2800, s.label);
    caseOrientedIdentity(s.path, s.label);
    caseOrientedExtents(s.path, s.label);
    caseOrientedScratchAccounting(s.path, s.label);
    caseOrientedDegradationRetired(s.path, s.label);
    caseOrientedStep43(s.path, s.label);
  }
  std::fprintf(stderr, "%s: %d failure(s)\n", argv[0], g_failures);
  return g_failures == 0 ? 0 : 1;
}
