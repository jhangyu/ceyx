// test_persistent_device_arena.cpp — AC1 (plan §9.1) zero per-frame device
// allocation gate, R2-T4 (docs/logs/2026-09-11/plan-gpu-copy-elimination.md
// §9.1, §8.3, §8.4).
//
// Drives the generic RAW route on ONE lane (this process's main thread — a
// lane is single-threaded by construction, plan §2.1/§8.2) over the 3-file
// in-repo corpus (image_samples/raw_sample.arw,
// image_samples/raw_corpus/fuji_xt3.raf, fuji_xt5.raf) and reads the five
// process-wide counters via ceyx_debug_persistent_device_arena_counters()
// (native/include/raw_ffi_api.h, R2-T1).
//
// Procedure (plan §9.1, binding):
//   0. raw_persistent_device_arena_release_all_lanes() — clean baseline.
//   1. Probe each corpus file's output pixel count
//      (raw_pipeline_probe_output_size) and pick the LARGEST as the warmup
//      frame. Actual-size sizing (ruled R-2026-09-11-2, plan §3.1) means a
//      late large frame legitimately reallocates a region — the warmup MUST
//      be the largest frame so the measured window is genuinely post-growth.
//   2. Decode 1 = warmup (the largest frame). Read all five counters
//      immediately after.
//   3. Decodes 2..20 (19 more) cycle through the remaining corpus order
//      (round-robin over all three files, largest included, since it is
//      already resident and re-decoding it must NOT grow anything). Read all
//      five counters again after decode 20.
//
// Four required assertions (plan §9.1, all mandatory, none may stand alone —
// plan §8.4 "arena never used" row):
//   1. allocation_count after warmup (decode 1) = non-zero.
//   2. allocation_count delta across decodes 2..20 = exactly 0.
//   3. growth_reallocation_count delta across decodes 2..20 = 0.
//   4. binding_count after decode 20 = non-zero AND >= 3 * 20 (three regions
//      bound per decode, 20 decodes total).
// Additionally: the arena log (stdout, captured by the artifact script) must
// show no `event=binding_failure` line — this driver does not parse its own
// stdout for that; the artifact command pipes this binary's output through a
// grep check (see docs/logs/2026-09-11/verify/ac1-c1-allocation-counter.txt).
//
// NOT-YET-INTEGRATED HANDLING: as of this writing (R2-T4, written against the
// plan's fixed §2.6 API ahead of R2-T1/T2/T3 landing), this driver will not
// link until raw_persistent_device_arena_release_all_lanes() and
// ceyx_debug_persistent_device_arena_counters() exist in the built dylib.
// That is the expected RED (plan §8.4 row 5: "the instrument reproduces the
// known-bad/not-yet-built state first") and is recorded as such in the
// artifact, exactly as test_render_parameter_cache.cpp's RUN 1 recorded a
// missing-symbol link failure before R1-T1/T2 landed.
//
// Style follows test_render_parameter_cache.cpp's report()/CHECK() convention.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"
#include "raw_persistent_device_arena.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
  std::printf("[PersistentDeviceArena] %s -> %s (%s)\n", name,
              ok ? "PASS" : "FAIL", detail);
  if (!ok) ++failures;
}

#define CHECK(name, cond, detail) report(name, (cond), detail)

struct ArenaCounters {
  uint64_t allocation_count = 0;
  uint64_t growth_reallocation_count = 0;
  uint64_t binding_count = 0;
  uint64_t resident_device_bytes = 0;
  uint64_t live_lane_count = 0;
};

ArenaCounters read_counters() {
  ArenaCounters c;
  const int32_t rc = ceyx_debug_persistent_device_arena_counters(
      &c.allocation_count, &c.growth_reallocation_count, &c.binding_count,
      &c.resident_device_bytes, &c.live_lane_count);
  if (rc != 0) {
    std::printf(
        "[PersistentDeviceArena] WARNING: "
        "ceyx_debug_persistent_device_arena_counters returned %d "
        "(non-zero means all out-pointers were null; this driver always "
        "passes all five, so a non-zero rc here is a probe defect)\n",
        (int)rc);
  }
  return c;
}

bool decode_one(const char* path) {
  RawDevelopParams develop{};
  develop.exposure_ev = 0.0f;
  develop.tone_curve_strength = 1.0f;
  develop.output_space = kRawOutputColorSpaceSrgb;
  develop.max_output_long_edge = 0u;
  develop.auto_exposure_mode = kRawAutoExposureOff;

  uint32_t pw = 0, ph = 0;
  if (raw_pipeline_probe_output_size(path, develop.max_output_long_edge, &pw,
                                      &ph) != kRawSuccess ||
      pw == 0 || ph == 0) {
    return false;
  }
  std::vector<uint8_t> dst(static_cast<size_t>(pw) * ph * 4);
  RawPipelineResult result{};
  const RawErrorCode rc = raw_pipeline_decode_file_into(path, develop,
                                                         dst.data(),
                                                         dst.size(), result);
  return rc == kRawSuccess && result.rgba_ptr == dst.data();
}

// Returns 0 on probe failure so a broken corpus entry sorts last rather than
// silently winning the "largest" comparison.
uint64_t probe_pixel_count(const char* path) {
  RawDevelopParams develop{};
  develop.max_output_long_edge = 0u;
  uint32_t pw = 0, ph = 0;
  if (raw_pipeline_probe_output_size(path, develop.max_output_long_edge, &pw,
                                      &ph) != kRawSuccess) {
    return 0;
  }
  return static_cast<uint64_t>(pw) * static_cast<uint64_t>(ph);
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> corpus = {
      "image_samples/raw_sample.arw",
      "image_samples/raw_corpus/fuji_xt3.raf",
      "image_samples/raw_corpus/fuji_xt5.raf",
  };
  if (argc > 1) {
    corpus.clear();
    for (int i = 1; i < argc; ++i) corpus.push_back(argv[i]);
  }
  if (corpus.size() < 1) {
    std::printf("[PersistentDeviceArena] no corpus files given\n");
    return 1;
  }

  // Determine the largest frame — required warmup, plan §9.1.
  size_t largest_index = 0;
  uint64_t largest_pixels = 0;
  for (size_t i = 0; i < corpus.size(); ++i) {
    const uint64_t pixels = probe_pixel_count(corpus[i].c_str());
    std::printf("[PersistentDeviceArena] corpus[%zu]=%s pixels=%llu\n", i,
                corpus[i].c_str(), (unsigned long long)pixels);
    if (pixels > largest_pixels) {
      largest_pixels = pixels;
      largest_index = i;
    }
  }
  CHECK("largest_frame_determined", largest_pixels > 0,
        "at least one corpus file must probe successfully");
  std::printf(
      "[PersistentDeviceArena] warmup file = corpus[%zu] (%s), pixels=%llu\n",
      largest_index, corpus[largest_index].c_str(),
      (unsigned long long)largest_pixels);

  // Step 0: clean baseline.
  ceyx::raw_persistent_device_arena_release_all_lanes();

  // Step 1/2: warmup = decode 1 = the largest frame.
  const bool warmup_ok = decode_one(corpus[largest_index].c_str());
  CHECK("warmup_decode_ok", warmup_ok, corpus[largest_index].c_str());
  const ArenaCounters after_warmup = read_counters();
  std::printf(
      "[PersistentDeviceArena] after warmup: allocation_count=%llu "
      "growth_reallocation_count=%llu binding_count=%llu "
      "resident_device_bytes=%llu live_lane_count=%llu\n",
      (unsigned long long)after_warmup.allocation_count,
      (unsigned long long)after_warmup.growth_reallocation_count,
      (unsigned long long)after_warmup.binding_count,
      (unsigned long long)after_warmup.resident_device_bytes,
      (unsigned long long)after_warmup.live_lane_count);

  // Required condition 1.
  CHECK("condition1_allocation_count_nonzero_after_warmup",
        after_warmup.allocation_count != 0,
        "plan §9.1 condition 1 — the arena's own creation");

  // Step 3: decodes 2..20 (19 more), round-robin over the full corpus order
  // (largest included — re-decoding it must not grow anything).
  const int kTotalDecodes = 20;
  bool all_measured_ok = true;
  for (int n = 2; n <= kTotalDecodes; ++n) {
    const std::string& path = corpus[(n - 1) % corpus.size()];
    const bool ok = decode_one(path.c_str());
    if (!ok) {
      std::printf("[PersistentDeviceArena] decode %d FAILED (%s)\n", n,
                  path.c_str());
      all_measured_ok = false;
    }
  }
  CHECK("decodes_2_through_20_ok", all_measured_ok,
        "all 19 measured-window decodes must succeed for the counters to be "
        "meaningful");

  const ArenaCounters after_20 = read_counters();
  std::printf(
      "[PersistentDeviceArena] after decode 20: allocation_count=%llu "
      "growth_reallocation_count=%llu binding_count=%llu "
      "resident_device_bytes=%llu live_lane_count=%llu\n",
      (unsigned long long)after_20.allocation_count,
      (unsigned long long)after_20.growth_reallocation_count,
      (unsigned long long)after_20.binding_count,
      (unsigned long long)after_20.resident_device_bytes,
      (unsigned long long)after_20.live_lane_count);

  const int64_t allocation_delta =
      static_cast<int64_t>(after_20.allocation_count) -
      static_cast<int64_t>(after_warmup.allocation_count);
  const int64_t growth_delta =
      static_cast<int64_t>(after_20.growth_reallocation_count) -
      static_cast<int64_t>(after_warmup.growth_reallocation_count);
  std::printf(
      "[PersistentDeviceArena] window(2..20) allocation_delta=%lld "
      "growth_delta=%lld binding_count_total=%llu\n",
      (long long)allocation_delta, (long long)growth_delta,
      (unsigned long long)after_20.binding_count);

  // Required condition 2.
  CHECK("condition2_allocation_delta_is_exactly_0", allocation_delta == 0,
        "plan §9.1 condition 2 — steady state, zero new device allocations "
        "across the measured window");

  // Required condition 3.
  CHECK("condition3_growth_delta_is_0", growth_delta == 0,
        "plan §9.1 condition 3 — largest frame already resident from "
        "warmup, so no region should grow in the measured window; a "
        "non-zero value here means either the warmup did not cover the "
        "largest frame (gate-procedure error) or a real growth defect");

  // Required condition 4.
  const uint64_t min_expected_bindings =
      3ull * static_cast<uint64_t>(kTotalDecodes);
  CHECK("condition4_binding_count_nonzero", after_20.binding_count != 0,
        "plan §9.1 condition 4 — distinguishes 'zero allocations because it "
        "works' from 'zero allocations because the arena is absent' (§8.4)");
  CHECK("condition4_binding_count_at_least_3x20",
        after_20.binding_count >= min_expected_bindings,
        "plan §9.1 condition 4 — three regions bound per decode, 20 decodes");

  std::printf("[PersistentDeviceArena] TOTAL failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
