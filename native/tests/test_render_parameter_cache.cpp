// test_render_parameter_cache.cpp — AC5 (plan §9.5) param-cache proof, both
// directions.
//
// R1-T5 (docs/logs/2026-09-11/plan-gpu-copy-elimination.md §9.5). Drives the
// generic RAW route on ONE lane (this process's main thread — a lane is
// single-threaded by construction, plan §5.2) and reads the process-wide
// counters ceyx::render_parameter_uploads_performed() /
// ceyx::render_parameter_cache_hits() (render_parameter_upload_cache.h)
// around four decodes of the SAME image:
//   1. cold                                — first decode, populates all 12
//   2. same settings again                 — expect +0 uploads, +12 hits
//   3. tone_curve_strength changed          — expect +1 upload  (tone_curve
//                                             only, plan §5.4)
//   4. exposure_ev changed                  — expect +2 uploads (exp_ramp +
//                                             tone_curve, plan §5.4)
//
// Each step also hashes the decoded RGBA output (FNV-1a) so a cache bug that
// serves STALE bytes (upload count right, pixels wrong) is caught: step 2's
// hash must equal step 1's, steps 3 and 4 must differ from the immediately
// preceding step.
//
// NULL-LANE / NOT-YET-INTEGRATED HANDLING: as of this writing (R1-T5, written
// against R1-T1's landed header before R1-T2 wires ensure_buffer_uploaded()
// into dng_render_halide.cpp), NO call site invokes ensure_buffer_uploaded()
// yet, so both counters read 0 for every step -- the EXPECTED RED this task's
// acceptance criterion #2 anticipates. This binary still compiles and runs
// against the pre-cache binary; it reports a clearly labelled RED (not a
// crash, not a false PASS) until R1-T2 lands.
//
// Style follows test_raw_auto_exposure.cpp's report()/CHECK() convention.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"
#include "render_parameter_upload_cache.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
  std::printf("[RenderParamCache] %s -> %s (%s)\n", name, ok ? "PASS" : "FAIL",
              detail);
  if (!ok) ++failures;
}

#define CHECK(name, cond, detail) report(name, (cond), detail)

uint64_t fnv1a(const uint8_t* data, size_t len) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ull;
  }
  return h;
}

struct DecodeStepResult {
  bool ok = false;
  uint64_t hash = 0;
  uint64_t uploads_before = 0, uploads_after = 0;
  uint64_t hits_before = 0, hits_after = 0;
};

DecodeStepResult run_decode(const char* path, const RawDevelopParams& develop) {
  DecodeStepResult r;
  r.uploads_before = ceyx::render_parameter_uploads_performed();
  r.hits_before = ceyx::render_parameter_cache_hits();

  uint32_t pw = 0, ph = 0;
  if (raw_pipeline_probe_output_size(path, develop.max_output_long_edge, &pw,
                                      &ph) != kRawSuccess ||
      pw == 0 || ph == 0) {
    return r;
  }
  std::vector<uint8_t> dst(static_cast<size_t>(pw) * ph * 4);
  RawPipelineResult result{};
  const RawErrorCode rc = raw_pipeline_decode_file_into(
      path, develop, dst.data(), dst.size(), result);

  r.uploads_after = ceyx::render_parameter_uploads_performed();
  r.hits_after = ceyx::render_parameter_cache_hits();
  r.ok = (rc == kRawSuccess) && (result.rgba_ptr == dst.data());
  if (r.ok) r.hash = fnv1a(dst.data(), dst.size());
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path =
      (argc > 1) ? argv[1] : "image_samples/raw_sample.arw";

  RawDevelopParams s1{};
  s1.exposure_ev = 0.0f;
  s1.tone_curve_strength = 1.0f;
  s1.output_space = kRawOutputColorSpaceSrgb;
  s1.max_output_long_edge = 0u;
  s1.auto_exposure_mode = kRawAutoExposureOff;  // plan §9.5: single-image
                                                 // steps require a stable
                                                 // reference; AE-on cross-
                                                 // image variance is a
                                                 // different gate (§9.5 note).

  // Step 1: cold.
  DecodeStepResult step1 = run_decode(path, s1);
  CHECK("step1_cold_decode_ok", step1.ok, path);
  const uint64_t step1_uploads_delta = step1.uploads_after - step1.uploads_before;
  const uint64_t step1_hits_delta = step1.hits_after - step1.hits_before;
  std::printf(
      "[RenderParamCache] step1 uploads_delta=%llu hits_delta=%llu hash=0x%016llx\n",
      (unsigned long long)step1_uploads_delta,
      (unsigned long long)step1_hits_delta,
      (unsigned long long)step1.hash);

  // Step 2: same settings, same image.
  DecodeStepResult step2 = run_decode(path, s1);
  CHECK("step2_unchanged_decode_ok", step2.ok, path);
  const uint64_t step2_uploads_delta = step2.uploads_after - step2.uploads_before;
  const uint64_t step2_hits_delta = step2.hits_after - step2.hits_before;
  CHECK("step2_uploads_delta_is_0", step2_uploads_delta == 0,
        "plan §9.5 table row 2");
  CHECK("step2_hits_delta_is_12", step2_hits_delta == 12,
        "plan §9.5 table row 2 — rules out 'cache never ran'");
  CHECK("step2_hash_equals_step1", step2.hash == step1.hash,
        "unchanged settings must reproduce identical pixels");

  // Step 3: tone_curve_strength changed — exactly 1 buffer (tone_curve).
  RawDevelopParams s3 = s1;
  s3.tone_curve_strength = 0.5f;
  DecodeStepResult step3 = run_decode(path, s3);
  CHECK("step3_tone_curve_decode_ok", step3.ok, path);
  const uint64_t step3_uploads_delta = step3.uploads_after - step3.uploads_before;
  const uint64_t step3_hits_delta = step3.hits_after - step3.hits_before;
  std::printf("[RenderParamCache] step3 uploads_delta=%llu hits_delta=%llu hash=0x%016llx\n",
              (unsigned long long)step3_uploads_delta,
              (unsigned long long)step3_hits_delta,
              (unsigned long long)step3.hash);
  CHECK("step3_uploads_delta_is_1", step3_uploads_delta == 1,
        "plan §9.5 table row 3 — tone_curve only, via builder :226-234");
  CHECK("step3_hits_delta_is_11", step3_hits_delta == 11,
        "plan §9.5 table row 3 — the other eleven buffers must hit");
  CHECK("step3_hash_differs_from_step1", step3.hash != step1.hash,
        "changed tone curve must change output pixels");

  // Step 4: exposure_ev changed — exactly 2 buffers (exp_ramp + tone_curve).
  RawDevelopParams s4 = s1;
  s4.exposure_ev = 1.0f;
  DecodeStepResult step4 = run_decode(path, s4);
  CHECK("step4_exposure_decode_ok", step4.ok, path);
  const uint64_t step4_uploads_delta = step4.uploads_after - step4.uploads_before;
  const uint64_t step4_hits_delta = step4.hits_after - step4.hits_before;
  std::printf("[RenderParamCache] step4 uploads_delta=%llu hits_delta=%llu hash=0x%016llx\n",
              (unsigned long long)step4_uploads_delta,
              (unsigned long long)step4_hits_delta,
              (unsigned long long)step4.hash);
  CHECK("step4_uploads_delta_is_2", step4_uploads_delta == 2,
        "plan §9.5 table row 4 — exp_ramp + tone_curve, ruling R-2026-09-11-1");
  CHECK("step4_hits_delta_is_10", step4_hits_delta == 10,
        "plan §9.5 table row 4 — the other ten buffers must hit");
  CHECK("step4_hash_differs_from_step3", step4.hash != step3.hash,
        "changed exposure must change output pixels");

  std::printf("[RenderParamCache] TOTAL failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
