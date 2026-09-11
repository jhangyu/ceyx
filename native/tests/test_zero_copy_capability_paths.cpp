// test_zero_copy_capability_paths.cpp — R3-T4 gate driver for C2's three
// destination paths (plan §4.3 "unified-wrapped / unified-degraded /
// fallback", §9.4 AC4 runs 3-4, §9.6 AC6).
//
// WHY THIS EXISTS: AC4/AC6 require bit-exact output AND the right counter
// attribution on the two paths a machine with default settings and an
// aligned Dart-pool buffer never takes on its own — the alignment-degraded
// path needs a DELIBERATELY misaligned caller buffer, and the fallback path
// needs the capability gate forced off. Plan §9.4: "Path 2 [degraded] is the
// one most likely to be skipped ... its absence is invisible." This driver
// makes both reachable and asserts on the probe counters, not on which
// branch we assume ran.
//
// PROCEDURE
//   Phase 1 (reference): decode every corpus file once with the gate at its
//     natural (probed) state and a NORMALLY allocated (heap, presumptively
//     aligned-or-not — irrelevant, this run is not path-asserted) caller
//     buffer. This is the baseline hash list every other run must equal.
//   Phase 2 (forced fallback, AC6 / AC4 run 4): every corpus file decoded
//     again with ceyx::set_zero_copy_capability_override_for_testing(kForcedOff)
//     in effect. Asserts zero_copy_path_is_enabled()==false,
//     destination_wrap_count delta==0, source_mosaic_wrap_count delta==0,
//     hashes == phase-1 reference.
//   Phase 3 (alignment-degraded, AC4 run 3 / AC6 second case): override
//     cleared back to natural, every corpus file decoded into a
//     DELIBERATELY misaligned destination buffer (heap pointer + 1 byte, so
//     it is never a multiple of kRawDeviceArenaAlignmentBytes regardless of
//     what the allocator happens to return). Asserts
//     destination_alignment_degradation_count delta == decode count,
//     destination_wrap_count delta == 0, hashes == phase-1 reference.
//   Phase 4 (unified-wrapped, AC4 run 2 / the POSITIVE case): override
//     cleared back to natural, every corpus file decoded into a genuinely
//     page-aligned destination buffer (posix_memalign to
//     kRawDeviceArenaAlignmentBytes, capacity ALSO rounded up to a page
//     multiple so both halves of the §4.3 alignment check agree), with
//     develop.caller_destination_is_page_aligned set true to match. Without
//     this phase every other assertion in this file is negative-space only
//     (fallback/degraded took no wrap) -- nothing proves the wrapped path is
//     EVER reachable, so a build where the wrap silently always refuses
//     would pass every check above. Asserts destination_wrap_count delta ==
//     decode count, source_mosaic_wrap_count delta == decode count,
//     alignment_degradation delta == 0, hashes == phase-1 reference, and (by
//     capturing stderr for this phase) no
//     `zerocopy|ev=destination_wrap_refused` line was emitted.
//
// Deliberately NOT duplicated here: the multi-lane / under-load repeat is
// test_concurrent_raw_decode.cpp, reused as-is per this task's instructions.
//
// Usage: test_zero_copy_capability_paths [<raw_file>...]
// Exit 0 iff every assertion passed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "dng_metal_context.h"
#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"
#include "raw_persistent_device_arena.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
  std::printf("[ZeroCopyCapabilityPaths] %s -> %s (%s)\n", name,
              ok ? "PASS" : "FAIL", detail);
  if (!ok) ++failures;
}

#define CHECK(name, cond, detail) report(name, (cond), detail)

// FNV-1a 64, same construction as test_concurrent_raw_decode.cpp so hashes
// from either driver are directly comparable if ever cross-checked.
uint64_t hash_bytes(const uint8_t* data, size_t byte_count) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < byte_count; ++i) {
    h ^= static_cast<uint64_t>(data[i]);
    h *= 1099511628211ull;
  }
  return h;
}

RawDevelopParams develop_params(bool destination_is_page_aligned = false) {
  RawDevelopParams develop{};
  develop.exposure_ev = 0.0f;
  develop.tone_curve_strength = 1.0f;
  develop.output_space = kRawOutputColorSpaceSrgb;
  develop.max_output_long_edge = 0u;
  develop.auto_exposure_mode = kRawAutoExposureOff;
  // Phase 4 (positive case): this driver calls raw_pipeline_decode_file_into
  // directly, bypassing ceyxDecodeIntoPrepare's FFI-level probe, so the flag
  // is set here by hand -- the caller is responsible for making sure the
  // ACTUAL buffer it passes agrees (see the aligned allocation in phase 4
  // below). Every other phase leaves this false, matching the FFI probe's
  // answer for a normally-allocated or deliberately-misaligned buffer.
  develop.caller_destination_is_page_aligned = destination_is_page_aligned;
  return develop;
}

struct DecodeOutcome {
  bool ok = false;
  uint64_t hash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Decodes into a caller-supplied buffer (never allocated here), so the
// caller controls alignment. `dst_capacity` must already be >= the probed
// extent's byte count; the alignment-degraded phase over-allocates on
// purpose so a +1 offset still fits.
DecodeOutcome decode_and_hash_into(const char* path, uint8_t* dst,
                                   size_t dst_capacity,
                                   bool destination_is_page_aligned = false) {
  DecodeOutcome outcome;
  const RawDevelopParams develop = develop_params(destination_is_page_aligned);
  RawPipelineResult result{};
  const RawErrorCode rc =
      raw_pipeline_decode_file_into(path, develop, dst, dst_capacity, result);
  if (rc != kRawSuccess || result.rgba_ptr == nullptr) return outcome;
  outcome.width = result.width;
  outcome.height = result.height;
  outcome.hash = hash_bytes(
      dst, static_cast<size_t>(result.width) * result.height * 4u);
  outcome.ok = true;
  return outcome;
}

// Round `value` up to the next multiple of kRawDeviceArenaAlignmentBytes.
size_t round_up_to_page(size_t value) {
  const size_t page = ceyx::kRawDeviceArenaAlignmentBytes;
  return ((value + page - 1) / page) * page;
}

// RAII stderr capture, scoped to phase 4's decode loop only -- everything
// this test itself prints goes through stdout (std::printf), so redirecting
// stderr cannot swallow this driver's own PASS/FAIL lines. Captures to a
// process-unique temp file (mkstemp) rather than a fixed name, so a
// concurrent run of this same binary (there is none in this campaign's
// single-occupancy rule, but the pattern should not silently corrupt if that
// ever changes) cannot collide.
class StderrCapture {
 public:
  StderrCapture() {
    char path_template[] = "/tmp/ceyx_zero_copy_stderr_capture_XXXXXX";
    fd_ = mkstemp(path_template);
    path_ = path_template;
    if (fd_ < 0) return;
    std::fflush(stderr);
    saved_stderr_fd_ = dup(fileno(stderr));
    dup2(fd_, fileno(stderr));
  }
  ~StderrCapture() {
    if (saved_stderr_fd_ >= 0) {
      std::fflush(stderr);
      dup2(saved_stderr_fd_, fileno(stderr));
      close(saved_stderr_fd_);
    }
    if (fd_ >= 0) close(fd_);
    if (!path_.empty()) std::remove(path_.c_str());
  }
  // Reads the captured file back and reports whether `needle` appears
  // ANCHORED at the start of some line -- never a bare substring match
  // (the 2026-08-28 self-collision family: this driver's OWN prints must
  // never be able to satisfy this check).
  bool any_line_starts_with(const char* needle) const {
    std::fflush(stderr);
    FILE* f = std::fopen(path_.c_str(), "r");
    if (!f) return false;
    const size_t needle_len = std::strlen(needle);
    std::string line;
    bool found = false;
    int c;
    while ((c = std::fgetc(f)) != EOF) {
      if (c == '\n') {
        if (line.compare(0, needle_len, needle) == 0) found = true;
        line.clear();
      } else {
        line.push_back(static_cast<char>(c));
      }
    }
    if (!line.empty() && line.compare(0, needle_len, needle) == 0) {
      found = true;
    }
    std::fclose(f);
    return found;
  }

 private:
  int fd_ = -1;
  int saved_stderr_fd_ = -1;
  std::string path_;
};

struct ZeroCopyCounters {
  int32_t path_is_enabled = 0;
  int32_t override_state = 0;
  uint64_t destination_wrap_count = 0;
  uint64_t destination_alignment_degradation_count = 0;
  uint64_t source_mosaic_wrap_count = 0;
};

ZeroCopyCounters read_zero_copy_counters() {
  ZeroCopyCounters c;
  const int32_t rc = ceyx_debug_zero_copy_capability_counters(
      &c.path_is_enabled, &c.override_state, &c.destination_wrap_count,
      &c.destination_alignment_degradation_count,
      &c.source_mosaic_wrap_count);
  if (rc != 0) {
    std::printf(
        "[ZeroCopyCapabilityPaths] WARNING: zero-copy counter probe "
        "returned %d (this driver passes all five out-pointers, so a "
        "non-zero rc is a probe defect, not an expected null-skip)\n",
        (int)rc);
  }
  return c;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> corpus;
  for (int i = 1; i < argc; ++i) corpus.push_back(argv[i]);
  if (corpus.empty()) {
    corpus = {
        "image_samples/raw_sample.arw",
        "image_samples/raw_corpus/fuji_xt3.raf",
        "image_samples/raw_corpus/fuji_xt5.raf",
    };
  }
  std::printf("[ZeroCopyCapabilityPaths] files=%zu\n", corpus.size());

  // ---------------------------------------------------------------------
  // Phase 1: reference hashes at the gate's natural state.
  // ---------------------------------------------------------------------
  ceyx::set_zero_copy_capability_override_for_testing(
      ceyx::ZeroCopyCapabilityOverrideState::kNone);
  std::vector<uint64_t> reference_hashes(corpus.size(), 0);
  bool reference_ok = true;
  for (size_t i = 0; i < corpus.size(); ++i) {
    uint32_t probe_width = 0, probe_height = 0;
    if (raw_pipeline_probe_output_size(corpus[i].c_str(), 0, &probe_width,
                                       &probe_height) != kRawSuccess ||
        probe_width == 0 || probe_height == 0) {
      std::printf("[ZeroCopyCapabilityPaths] probe FAILED (%s)\n",
                  corpus[i].c_str());
      reference_ok = false;
      continue;
    }
    const size_t capacity =
        static_cast<size_t>(probe_width) * probe_height * 4u;
    std::vector<uint8_t> destination(capacity);
    const DecodeOutcome outcome =
        decode_and_hash_into(corpus[i].c_str(), destination.data(), capacity);
    if (!outcome.ok) {
      std::printf("[ZeroCopyCapabilityPaths] reference decode FAILED (%s)\n",
                  corpus[i].c_str());
      reference_ok = false;
      continue;
    }
    reference_hashes[i] = outcome.hash;
    std::printf(
        "[ZeroCopyCapabilityPaths] reference corpus[%zu]=%s %ux%u "
        "hash=%016llx\n",
        i, corpus[i].c_str(), outcome.width, outcome.height,
        (unsigned long long)outcome.hash);
  }
  CHECK("reference_decodes_ok", reference_ok,
        "every corpus file must decode at the gate's natural state before "
        "the forced/degraded runs below can be judged against it");
  if (!reference_ok) {
    std::printf("[ZeroCopyCapabilityPaths] TOTAL failures=%d\n", failures);
    return 1;
  }

  // ---------------------------------------------------------------------
  // Phase 2: forced fallback (AC6 / AC4 run 4).
  // ---------------------------------------------------------------------
  ceyx::set_zero_copy_capability_override_for_testing(
      ceyx::ZeroCopyCapabilityOverrideState::kForcedOff);
  const ZeroCopyCounters fallback_baseline = read_zero_copy_counters();
  bool fallback_ok = true;
  for (size_t i = 0; i < corpus.size(); ++i) {
    uint32_t probe_width = 0, probe_height = 0;
    if (raw_pipeline_probe_output_size(corpus[i].c_str(), 0, &probe_width,
                                       &probe_height) != kRawSuccess) {
      fallback_ok = false;
      continue;
    }
    const size_t capacity =
        static_cast<size_t>(probe_width) * probe_height * 4u;
    std::vector<uint8_t> destination(capacity);
    const DecodeOutcome outcome =
        decode_and_hash_into(corpus[i].c_str(), destination.data(), capacity);
    if (!outcome.ok || outcome.hash != reference_hashes[i]) {
      std::printf(
          "[ZeroCopyCapabilityPaths] forced-fallback MISMATCH (%s) ok=%d "
          "got=%016llx want=%016llx\n",
          corpus[i].c_str(), outcome.ok,
          (unsigned long long)outcome.hash,
          (unsigned long long)reference_hashes[i]);
      fallback_ok = false;
    }
  }
  const ZeroCopyCounters fallback_after = read_zero_copy_counters();
  CHECK("forced_fallback_hashes_match_reference", fallback_ok,
        "AC6: every decode succeeds through the copy-based fallback with "
        "output identical to the AC4 baseline list");
  CHECK("forced_fallback_gate_reports_disabled",
        fallback_after.path_is_enabled == 0 &&
            fallback_after.override_state ==
                static_cast<int32_t>(
                    ceyx::ZeroCopyCapabilityOverrideState::kForcedOff),
        "zero_copy_path_is_enabled() must read false and the override state "
        "must read forced_off through the SAME probe the gates read (the "
        "gate must be genuinely off, not merely reported off)");
  CHECK("forced_fallback_took_no_destination_wrap",
        fallback_after.destination_wrap_count ==
            fallback_baseline.destination_wrap_count,
        "zero_copy_destination_wrap_count delta must be 0 -- a run that "
        "silently kept taking the unified-wrapped path would still report "
        "three PASSes here without this counter check (plan §9.4)");
  CHECK("forced_fallback_took_no_source_mosaic_wrap",
        fallback_after.source_mosaic_wrap_count ==
            fallback_baseline.source_mosaic_wrap_count,
        "the gate being off must also stop the input-side wrap, not only "
        "the output-side one");

  // ---------------------------------------------------------------------
  // Phase 3: alignment-degraded path (AC4 run 3 / AC6 second case).
  // ---------------------------------------------------------------------
  ceyx::set_zero_copy_capability_override_for_testing(
      ceyx::ZeroCopyCapabilityOverrideState::kNone);
  const ZeroCopyCounters degraded_baseline = read_zero_copy_counters();
  bool degraded_ok = true;
  int degraded_decode_count = 0;
  for (size_t i = 0; i < corpus.size(); ++i) {
    uint32_t probe_width = 0, probe_height = 0;
    if (raw_pipeline_probe_output_size(corpus[i].c_str(), 0, &probe_width,
                                       &probe_height) != kRawSuccess) {
      degraded_ok = false;
      continue;
    }
    const size_t need = static_cast<size_t>(probe_width) * probe_height * 4u;
    // Deliberately misaligned: allocate one extra byte and hand back
    // (base + 1). A heap allocator MAY return a page-aligned base, but
    // base+1 is never a multiple of kRawDeviceArenaAlignmentBytes (16384),
    // so this buffer fails the §4.3 alignment check unconditionally,
    // regardless of what the allocator happens to do on this machine.
    std::vector<uint8_t> backing(need + 1);
    uint8_t* misaligned_dst = backing.data() + 1;
    CHECK("misaligned_probe_buffer_is_actually_misaligned",
          (reinterpret_cast<uintptr_t>(misaligned_dst) %
           ceyx::kRawDeviceArenaAlignmentBytes) != 0,
          "self-check: if this ever fires the buffer happens to be aligned "
          "and phase 3 is not exercising the degraded path it claims to");
    const DecodeOutcome outcome =
        decode_and_hash_into(corpus[i].c_str(), misaligned_dst, need);
    if (!outcome.ok || outcome.hash != reference_hashes[i]) {
      std::printf(
          "[ZeroCopyCapabilityPaths] alignment-degraded MISMATCH (%s) ok=%d "
          "got=%016llx want=%016llx\n",
          corpus[i].c_str(), outcome.ok,
          (unsigned long long)outcome.hash,
          (unsigned long long)reference_hashes[i]);
      degraded_ok = false;
    } else {
      ++degraded_decode_count;
    }
  }
  const ZeroCopyCounters degraded_after = read_zero_copy_counters();
  CHECK("alignment_degraded_hashes_match_reference", degraded_ok,
        "the unified-degraded path (arena dst + one final copy) must be "
        "bit-exact with the AC4 baseline list");
  CHECK("alignment_degraded_count_equals_decode_count",
        degraded_after.destination_alignment_degradation_count -
                degraded_baseline.destination_alignment_degradation_count ==
            static_cast<uint64_t>(degraded_decode_count),
        "every misaligned-destination decode must be attributed to the "
        "degradation counter -- this is the path AC4/AC6 name as most "
        "likely to be silently skipped");
  CHECK("alignment_degraded_took_no_destination_wrap",
        degraded_after.destination_wrap_count ==
            degraded_baseline.destination_wrap_count,
        "the degraded path must not ALSO report a true wrap for the same "
        "decode -- the two counters are mutually exclusive per decode");

  // ---------------------------------------------------------------------
  // Phase 4: unified-wrapped, the POSITIVE case (AC4 run 2). Without this
  // phase every assertion above is negative-space only -- nothing proves the
  // wrapped path is EVER taken, so a build where the wrap silently always
  // refuses would pass every check so far (team-lead spot-check finding).
  // ---------------------------------------------------------------------
  ceyx::set_zero_copy_capability_override_for_testing(
      ceyx::ZeroCopyCapabilityOverrideState::kNone);
  const ZeroCopyCounters wrapped_baseline = read_zero_copy_counters();
  bool wrapped_ok = true;
  int wrapped_decode_count = 0;
  bool wrap_refused_seen = false;
  {
    StderrCapture capture;
    for (size_t i = 0; i < corpus.size(); ++i) {
      uint32_t probe_width = 0, probe_height = 0;
      if (raw_pipeline_probe_output_size(corpus[i].c_str(), 0, &probe_width,
                                         &probe_height) != kRawSuccess) {
        wrapped_ok = false;
        continue;
      }
      const size_t need =
          static_cast<size_t>(probe_width) * probe_height * 4u;
      const size_t aligned_capacity = round_up_to_page(need);
      void* aligned_ptr = nullptr;
      if (posix_memalign(&aligned_ptr, ceyx::kRawDeviceArenaAlignmentBytes,
                         aligned_capacity) != 0 ||
          aligned_ptr == nullptr) {
        std::printf(
            "[ZeroCopyCapabilityPaths] posix_memalign FAILED for %s\n",
            corpus[i].c_str());
        wrapped_ok = false;
        continue;
      }
      uint8_t* aligned_dst = static_cast<uint8_t*>(aligned_ptr);
      CHECK("wrapped_probe_buffer_is_actually_aligned",
            (reinterpret_cast<uintptr_t>(aligned_dst) %
                 ceyx::kRawDeviceArenaAlignmentBytes ==
             0) &&
                (aligned_capacity % ceyx::kRawDeviceArenaAlignmentBytes ==
                 0),
            "self-check: posix_memalign must satisfy both halves of the "
            "§4.3 alignment check (pointer AND capacity), or this phase "
            "is not testing what it claims to");
      const DecodeOutcome outcome = decode_and_hash_into(
          corpus[i].c_str(), aligned_dst, aligned_capacity,
          /*destination_is_page_aligned=*/true);
      if (!outcome.ok || outcome.hash != reference_hashes[i]) {
        std::printf(
            "[ZeroCopyCapabilityPaths] unified-wrapped MISMATCH (%s) ok=%d "
            "got=%016llx want=%016llx\n",
            corpus[i].c_str(), outcome.ok,
            (unsigned long long)outcome.hash,
            (unsigned long long)reference_hashes[i]);
        wrapped_ok = false;
      } else {
        ++wrapped_decode_count;
      }
      std::free(aligned_ptr);
    }
    wrap_refused_seen =
        capture.any_line_starts_with("zerocopy|ev=destination_wrap_refused");
  }
  const ZeroCopyCounters wrapped_after = read_zero_copy_counters();
  CHECK("unified_wrapped_hashes_match_reference", wrapped_ok,
        "AC4 run 2: the true zero-copy destination path must be bit-exact "
        "with the baseline list");

  // Diagnostic strengthening (team-lead authorization, R3-T4 gate-gap
  // follow-up): print the ACTUAL observed deltas on the two counter checks
  // below, not just the pass/fail verdict, so a red run reports numbers
  // instead of forcing a re-run with a debugger.
  const int64_t wrap_delta = static_cast<int64_t>(
                                 wrapped_after.destination_wrap_count) -
                             static_cast<int64_t>(
                                 wrapped_baseline.destination_wrap_count);
  const int64_t degradation_delta =
      static_cast<int64_t>(
          wrapped_after.destination_alignment_degradation_count) -
      static_cast<int64_t>(
          wrapped_baseline.destination_alignment_degradation_count);
  char wrap_count_detail[192];
  std::snprintf(wrap_count_detail, sizeof(wrap_count_detail),
               "POSITIVE proof the wrap succeeded on every decode of this "
               "phase -- without this assertion a build that silently "
               "never wraps would still pass every other check in this "
               "file; observed: wrap_delta=%lld decode_count=%d",
               (long long)wrap_delta, wrapped_decode_count);
  CHECK("unified_wrapped_destination_wrap_count_equals_decode_count",
        wrap_delta == static_cast<int64_t>(wrapped_decode_count),
        wrap_count_detail);
  CHECK("unified_wrapped_source_mosaic_wrap_count_equals_decode_count",
        wrapped_after.source_mosaic_wrap_count -
                wrapped_baseline.source_mosaic_wrap_count ==
            static_cast<uint64_t>(wrapped_decode_count),
        "the input-side wrap must ALSO fire on the unified-wrapped path");
  char degradation_detail[192];
  std::snprintf(degradation_detail, sizeof(degradation_detail),
               "a genuinely aligned buffer must never be attributed to the "
               "degradation counter -- the two are mutually exclusive per "
               "decode; observed: degradation_delta=%lld decode_count=%d",
               (long long)degradation_delta, wrapped_decode_count);
  CHECK("unified_wrapped_took_no_alignment_degradation",
        degradation_delta == 0, degradation_detail);
  CHECK("unified_wrapped_no_destination_wrap_refused_logged",
        !wrap_refused_seen,
        "no zerocopy|ev=destination_wrap_refused line (grep anchored to "
        "line start, never the bare token) may appear while every decode "
        "in this phase supplied a genuinely page-aligned buffer");

  std::printf("[ZeroCopyCapabilityPaths] TOTAL failures=%d\n", failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}
