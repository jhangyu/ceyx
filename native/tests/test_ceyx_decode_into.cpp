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
  // WP5: the process-global pool counter this case used to sample is gone with
  // the pool. What it was really asserting -- "a refusal did not hand out a
  // buffer" -- is now checked directly below as rgba_data == nullptr on each
  // refusal, which is a local, per-call fact rather than a global delta.
  DngResult *s = ceyx_decode_into_buffer(path, 0, buf.data(), need - 1);
  CHECK(s && s->error_code == kCeyxErrDstTooSmall, "[%s] short not refused",
        label);
  CHECK(s && s->rgba_data == nullptr, "[%s] short returned a pointer", label);
  CHECK(s && s->width == w && s->height == h,
        "[%s] refusal must report the extent", label);
  if (s) dng_free_result(s);

  DngResult *n = ceyx_decode_into_buffer(path, 0, nullptr, need);
  CHECK(n && n->error_code == kCeyxErrDstTooSmall, "[%s] null dst", label);
  CHECK(n && n->rgba_data == nullptr, "[%s] null dst returned a pointer", label);
  if (n) dng_free_result(n);

  DngResult *z = ceyx_decode_into_buffer(path, 0, buf.data(), 0);
  CHECK(z && z->error_code == kCeyxErrDstTooSmall, "[%s] zero cap", label);
  CHECK(z && z->rgba_data == nullptr, "[%s] zero cap returned a pointer", label);
  if (z) dng_free_result(z);

}

// AC15.4 / AC15.5 — pointer identity, no ownership taken, failure leaves the
// caller's bytes alone. Run per route: this is where BOTH checkout guards are
// exercised, the DNG one on a .dng and the RAW one on an .arw/.raf.
static void caseOwnership(const char *path, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, 0, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> buf(need, 0xAB);

  DngResult *r = ceyx_decode_into_buffer(path, 0, buf.data(), need);
  CHECK(r != nullptr, "[%s] null result", label);
  if (!r) return;
  CHECK(r->error_code == 0, "[%s] error=%d", label, r->error_code);
  CHECK(r->rgba_data == buf.data(),
        "[%s] rgba_data must BE the caller buffer (%p vs %p)", label,
        static_cast<void *>(r->rgba_data), static_cast<void *>(buf.data()));
  // WP5: "no pool buffer was checked out" is now expressed by the pointer
  // identity above -- if the decode had allocated its own output, rgba_data
  // would not BE buf.data(). That is a stronger statement than the old counter
  // delta, which could not tell "never allocated" from "allocated and returned".
  r->rgba_data = nullptr;
  dng_free_result(r);
}

static void caseFailureLeavesBufferAlone(const char *good, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(good, 0, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> buf(need, 0xAB);
  DngResult *r = ceyx_decode_into_buffer("/nonexistent/broken.raw", 0,
                                         buf.data(), need);
  CHECK(r && r->error_code != 0, "[%s] corrupt reported success", label);
  if (r) dng_free_result(r);
  CHECK(buf[0] == 0xAB, "[%s] caller buffer clobbered on failure", label);
  // WP5: the old counter delta here asked "did a failure path release dst into
  // the pool?". With the pool and every release arm deleted, no code path can;
  // the surviving observable is that the caller's bytes are untouched, above.
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
// (nonexistent path, undersized dst).
//
// WP5: the scratch this case accounted for no longer exists -- the fused kernel
// writes oriented pixels straight into dst -- and neither does the pool counter
// it used. What survives, and is what the case is really for, is that 50
// successes and 50 failures in a row all keep writing into the CALLER's buffer
// and none of them corrupts the next call. That is asserted per iteration now
// rather than as one global delta at the end, which also localises a failure to
// the iteration that caused it.
static void caseOrientedScratchAccounting(const char *path, const char *label) {
  int32_t w = 0, h = 0;
  if (!probe(path, kOrientMaxDim, &w, &h)) return;
  const size_t need = static_cast<size_t>(w) * h * 4;
  std::vector<uint8_t> buf(need);

  // Orientations chosen so the loop alternates transposing (6, 8, 5, 7) with
  // non-transposing (1, 3, 2, 4): a regression on either arm shows up.
  const int32_t cycle[] = {1, 6, 3, 8, 2, 5, 4, 7, 6, 1};
  int successOwnershipViolations = 0;
  for (int i = 0; i < 50; ++i) {
    DngResult *r = ceyx_decode_into_buffer_oriented(
        path, kOrientMaxDim, buf.data(), need, cycle[i % 10]);
    if (r) {
      if (r->error_code == 0 && r->rgba_data != buf.data())
        ++successOwnershipViolations;
      r->rgba_data = nullptr;
      dng_free_result(r);
    }
  }
  CHECK(successOwnershipViolations == 0,
        "[%s] AC-2.5 %d of 50 oriented successes did not write into the "
        "caller's buffer", label, successOwnershipViolations);

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
    CHECK(b && b->rgba_data == nullptr,
          "[%s] AC-2.5 refusal returned a pointer", label);
    if (b) dng_free_result(b);
  }
}

// AC-2.6, RETIRED BY R-19 (productionization plan §3 Task 4 "NAMED BEHAVIOUR
// CHANGE"): the fused GPU path never checks out scratch for an oriented
// decode — the kernel writes the oriented pixels straight into the caller's
// buffer, transposing included — so there was nothing left to degrade.
// WP5 (user ruling R3) deleted the scratch-failure hook this case used to
// flip. The REGRESSION VALUE was never in the flip: it is the
// extent-swap assertion, i.e. that two independent oriented decodes of the same
// file both report the correctly ORIENTED (swapped) extent rather than a silent
// unoriented fallback. That assertion is kept verbatim, as a control/forced
// PAIR, so a regression back to the old degradation behaviour still fails here.
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

  DngResult *r = ceyx_decode_into_buffer_oriented(path, kOrientMaxDim,
                                                  forced.data(), need, 6);

  CHECK(r != nullptr, "[%s] R-19 null result", label);
  if (!r) return;
  CHECK(r->error_code == 0,
        "[%s] R-19 second oriented decode must not fail (error=%d)", label,
        r->error_code);
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
                                            const char *raw_path,
                                            const char *upstream_fail_path,
                                            const char *label) {
  // (a) overlapping src/dst.
  {
    std::fprintf(stderr, "[%s] case=a (direct overlap) setup\n", label);
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
        /*ctx=*/nullptr, /*exif_orientation=*/1);
    CHECK(!ok, "[%s] B-1a overlapping call unexpectedly succeeded", label);
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kOverlap,
          "[%s] B-1a expected kOverlap, got %d", label,
          static_cast<int>(dngRenderStage4LastFailureReason()));
  }

  // (b) null dst -> false, reason stays kNone (no over-claiming).
  {
    std::fprintf(stderr, "[%s] case=b (null dst) setup\n", label);
    std::vector<uint16_t> src(4 * 4 * 4, 0);
    RenderParams params{};
    const bool ok = runRenderStage4HalideAot(
        src.data(), 4, 4, 3, 0, 0, 0, 1.0f, 4, 4, params,
        /*dst=*/nullptr, /*ctx=*/nullptr,
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
    std::fprintf(stderr, "[%s] case=c (staleness reset, DNG) setup\n", label);
    std::vector<uint16_t> buf(4 * 4 * 4, 0);
    RenderParams params{};
    const bool overlap_ok = runRenderStage4HalideAot(
        buf.data(), 4, 4, 3, 0, 0, 0, 1.0f, 4, 4, params,
        reinterpret_cast<uint8_t *>(buf.data()), nullptr, 1);
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

  // (e) V-1 close: repeat the staleness half (c) with the RAW route (.arw),
  // not just DNG. Proves the RAW route also dispatches Stage4 on the CALLER
  // thread — thread_local is the right carrier for it too. If this fails,
  // that is a structural finding (thread_local wrong for RAW), not something
  // to patch around here.
  if (raw_path) {
    std::fprintf(stderr, "[%s] case=e (staleness reset, RAW) setup\n", label);
    std::vector<uint16_t> buf(4 * 4 * 4, 0);
    RenderParams params{};
    const bool overlap_ok = runRenderStage4HalideAot(
        buf.data(), 4, 4, 3, 0, 0, 0, 1.0f, 4, 4, params,
        reinterpret_cast<uint8_t *>(buf.data()), nullptr, 1);
    CHECK(!overlap_ok, "[%s] B-1e RAW setup overlap call unexpectedly succeeded",
          label);
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kOverlap,
          "[%s] B-1e RAW setup expected kOverlap", label);

    int32_t w = 0, h = 0;
    if (probe(raw_path, kOrientMaxDim, &w, &h)) {
      const size_t need = static_cast<size_t>(w) * h * 4;
      std::vector<uint8_t> dst(need);
      DngResult *r = ceyx_decode_into_buffer_oriented(raw_path, kOrientMaxDim,
                                                       dst.data(), need, 1);
      CHECK(r && r->error_code == 0,
            "[%s] B-1e RAW reset-half decode failed (error=%d)", label,
            r ? r->error_code : -1);
      if (r) { r->rgba_data = nullptr; dng_free_result(r); }
      CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kNone,
            "[%s] B-1e RAW route did not reset the reason to kNone after a "
            "successful decode — thread_local may be the wrong carrier for "
            "the RAW route (STRUCTURAL, do not patch around), got %d",
            label, static_cast<int>(dngRenderStage4LastFailureReason()));
    }
  }

  // (f) PIN, not a B-2 regression case (reviewer correction, S-3): this
  // case CANNOT fail for "B-2" — dst_capacity=1 fails inside
  // ceyxDecodeIntoPrepare, which returns 31 lines ABOVE the
  // dngRenderStage4ResetFailureReason() call and the mapper; deleting the
  // reset entirely would not turn this case red, so it proves nothing about
  // the reset fix itself. What it DOES pin, and is worth keeping for: the
  // mapper (ceyxMapStage4FailureReason) is structurally unreachable from a
  // prepare-level refusal path, regardless of what stale reason sits on this
  // thread. If a future edit hoists or widens the mapper's call site so it
  // starts covering prepare-level failures too, this is the regression that
  // would first go red. The REAL composed B-2 closure — stale kOverlap
  // reaching all the way through phase 3 to a pre-Stage4 failure — is case
  // (d) below, using raw_sample.arw.trunc.raw.
  if (good_path) {
    std::fprintf(stderr, "[%s] case=f (prepare-refusal mapper-unreachable pin) setup\n",
                 label);
    std::vector<uint16_t> buf(4 * 4 * 4, 0);
    RenderParams params{};
    const bool overlap_ok = runRenderStage4HalideAot(
        buf.data(), 4, 4, 3, 0, 0, 0, 1.0f, 4, 4, params,
        reinterpret_cast<uint8_t *>(buf.data()), nullptr, 1);
    CHECK(!overlap_ok, "[%s] B-1f setup overlap call unexpectedly succeeded",
          label);
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kOverlap,
          "[%s] B-1f setup expected kOverlap", label);

    uint8_t tiny_dst[1] = {0xAB};
    DngResult *r = ceyx_decode_into_buffer_oriented(good_path, kOrientMaxDim,
                                                     tiny_dst, 1, 6);
    CHECK(r != nullptr, "[%s] B-1f null result", label);
    if (r) {
      CHECK(r->error_code == kCeyxErrDstTooSmall,
            "[%s] B-1f PIN VIOLATED: expected kCeyxErrDstTooSmall (%d), got "
            "%d — the mapper became reachable from a prepare-level refusal",
            label, kCeyxErrDstTooSmall, r->error_code);
      dng_free_result(r);
    }
  }

  // (d) THE composed B-2 closure: a decode that fails UPSTREAM of Stage4 —
  // after ceyxDecodeIntoPrepare succeeds (so the reset-before-phase3 line
  // actually runs, unlike (f) above) but before the runner is ever reached —
  // must NOT have its generic error code clobbered by a STALE reason left by
  // an earlier decode on this thread. Setup: force a stale kOverlap via the
  // direct runner call (as in (a)/(c)), THEN decode a file that is malformed
  // enough to fail during unpack/adapter-build (never reaching Stage4) but
  // still parses far enough for ceyxDecodeIntoPrepare's metadata-only probe
  // to succeed. Expected fixture: image_samples/raw_corpus/raw_sample.arw.
  // trunc.raw (reviewer-verified: reaches phase 3 with the stale kOverlap
  // still set, fails pre-Stage4, generic code survives).
  if (upstream_fail_path) {
    std::fprintf(stderr, "[%s] case=d (composed B-2: stale overlap + upstream-of-Stage4 failure) setup\n",
                 label);
    std::vector<uint16_t> buf(4 * 4 * 4, 0);
    RenderParams params{};
    const bool overlap_ok = runRenderStage4HalideAot(
        buf.data(), 4, 4, 3, 0, 0, 0, 1.0f, 4, 4, params,
        reinterpret_cast<uint8_t *>(buf.data()), nullptr, 1);
    CHECK(!overlap_ok, "[%s] B-1d setup overlap call unexpectedly succeeded",
          label);
    CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kOverlap,
          "[%s] B-1d setup expected kOverlap", label);

    // Deliberately NOT the shared probe() helper: that always CHECKs rc==0,
    // and this fixture is EXPECTED to possibly fail even the probe (see the
    // else-branch note below) — that must not itself register as a failure.
    int32_t w = 0, h = 0;
    const bool probe_ok =
        ceyx_probe_output_size(upstream_fail_path, kOrientMaxDim, &w, &h) ==
            0 &&
        w > 0 && h > 0;
    if (!probe_ok) {
      // The fixture doesn't pass even the metadata probe, so
      // ceyxDecodeIntoPrepare itself refuses before the reset line is ever
      // reached — that path is unaffected by B-2 by construction (the reset
      // is inside the fused branch, downstream of prepare) and this subcase
      // has nothing further to prove with this fixture. Documented rather
      // than silently skipped.
      std::fprintf(stderr,
                   "[%s] B-1d note: %s fails the pre-decode probe too, so "
                   "the upstream-of-Stage4 case is not exercised by this "
                   "fixture (prepare-level failures never reach the reset "
                   "line at all).\n",
                   label, upstream_fail_path);
    } else {
      const size_t need = static_cast<size_t>(w) * h * 4;
      std::vector<uint8_t> dst(need);
      DngResult *r = ceyx_decode_into_buffer_oriented(
          upstream_fail_path, kOrientMaxDim, dst.data(), need, 6);
      CHECK(r != nullptr, "[%s] B-1d null result", label);
      if (r) {
        CHECK(r->error_code != 0,
              "[%s] B-1d malformed fixture unexpectedly decoded successfully",
              label);
        CHECK(r->error_code != kCeyxOrientErrOverlap &&
                  r->error_code != kCeyxOrientErrKernel,
              "[%s] B-1d generic error %d was CLOBBERED by the stale "
              "reason (B-2 regression)",
              label, r->error_code);
        CHECK(dngRenderStage4LastFailureReason() == Stage4FailureReason::kNone,
              "[%s] B-1d reason not reset to kNone by an upstream-of-Stage4 "
              "failure, got %d",
              label, static_cast<int>(dngRenderStage4LastFailureReason()));
        dng_free_result(r);
      }
    }
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

  // B-1/B-2 red-state proof is route-agnostic (it drives the shared
  // low-level runner directly), so it runs once, not once per sample class.
  // argv[1] (DNG) and argv[3] (.arw, RAW) cover the staleness/reset halves
  // on both routes (case c, case e); argv[6], if given, is the composed B-2
  // closure fixture for case (d) — reviewer-recommended:
  // image_samples/raw_corpus/raw_sample.arw.trunc.raw (passes the metadata
  // probe, then fails before Stage4 with the stale kOverlap still set).
  // Optional: without argv[6] this one case is documented-not-exercised
  // rather than skipped silently.
  {
    const char *dng_any = argc > 1 ? argv[1] : nullptr;
    const char *raw_any = argc > 3 ? argv[3] : nullptr;
    const char *upstream_fail = argc > 6 ? argv[6] : nullptr;
    caseStage4FailureReasonRedState(dng_any, raw_any, upstream_fail,
                                    "b1-redstate");
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
