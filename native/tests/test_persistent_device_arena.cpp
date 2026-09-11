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
//   1. PER-REGION REQUIREMENT PROFILE (round-2 review S3). Output pixel count
//      is NOT a sufficient warmup selector: only two of the three regions are
//      sized by output pixels (kStageThreeInterleavedRgb16Region = pixels*6,
//      kDestinationRgba8Region = pixels*4). The third,
//      kSourceMosaicRegion, is sized by the decoder's mosaic
//      row_stride_bytes * mosaic height (raw_gpu_pipeline.cpp's
//      src_arena_binding), which is NOT a function of the output pixel count —
//      a file with fewer output pixels can legitimately need the larger mosaic
//      region. Picking the warmup by output pixels could therefore leave the
//      mosaic region to grow inside the measured window, and condition 3 would
//      fire on a GATE-PROCEDURE error while looking exactly like a defect.
//      So this driver MEASURES each file's per-region byte requirement before
//      the gate proper: release all lanes, decode the file alone, and read
//      resident_device_bytes — which for a single lane with all three regions
//      resident is mosaic + pixels*6 + pixels*4, so
//        mosaic_bytes = resident_device_bytes - pixels*10
//      with pixels taken from raw_pipeline_probe_output_size. The full table is
//      printed, so ANY growth event in the measured window is attributable to
//      warmup choice versus defect by reading the artifact alone.
//      The warmup is then the set of files attaining the per-region maxima:
//      one file when a single file dominates all three regions (the historical
//      procedure, unchanged in that case), otherwise the two or three files
//      needed to cover every region's maximum. Actual-size sizing (ruled
//      R-2026-09-11-2, plan §3.1) means a late larger frame legitimately
//      reallocates a region — the warmup must cover every region's maximum so
//      the measured window is genuinely post-growth.
//   2. Warmup = the selected file(s), decoded once each. Read all five
//      counters immediately after.
//   3. 19 measured decodes cycle through the corpus order
//      (round-robin over all three files, largest included, since it is
//      already resident and re-decoding it must NOT grow anything). Read all
//      five counters again after decode 20.
//
// Four required assertions (plan §9.1, all mandatory, none may stand alone —
// plan §8.4 "arena never used" row):
//   1. allocation_count after warmup = non-zero.
//   2. allocation_count delta across the 19 measured decodes = exactly 0.
//   3. growth_reallocation_count delta across the 19 measured decodes = 0.
//   4. binding_count at the end = non-zero AND >= 3 * (warmup_decodes + 19)
//      (three regions bound per decode). The warmup decode count is printed so
//      the bound is reproducible from the artifact.
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

// Per-file, per-region byte requirement (round-2 review S3). `mosaic_bytes` is
// MEASURED (resident total minus the two pixel-derived regions), the other two
// are DERIVED from the probed output pixel count exactly as the binding sites
// compute them.
struct RegionRequirements {
  uint64_t pixels = 0;
  uint64_t mosaic_bytes = 0;
  uint64_t stage_three_bytes = 0;
  uint64_t destination_bytes = 0;
  uint64_t resident_total_bytes = 0;
  bool measured = false;
};

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

// Isolates one file on a clean arena and reads back what it actually required.
// Every call starts from release_all_lanes(), so resident_device_bytes after
// the decode belongs to this file alone.
RegionRequirements profile_region_requirements(const char* path) {
  RegionRequirements r;
  r.pixels = probe_pixel_count(path);
  ceyx::raw_persistent_device_arena_release_all_lanes();
  if (!decode_one(path)) return r;
  const ArenaCounters after = read_counters();
  r.resident_total_bytes = after.resident_device_bytes;
  r.stage_three_bytes = r.pixels * 6ull;
  r.destination_bytes = r.pixels * 4ull;
  const uint64_t pixel_derived = r.stage_three_bytes + r.destination_bytes;
  // A resident total below the pixel-derived pair means the arena did not hold
  // all three regions for this decode (no Metal device, lane ceiling, bind
  // failure). Leave mosaic_bytes at 0 and measured=false so the selection below
  // falls back to the pixel-count ordering rather than trusting a nonsense
  // subtraction.
  if (r.resident_total_bytes >= pixel_derived && r.pixels > 0) {
    r.mosaic_bytes = r.resident_total_bytes - pixel_derived;
    r.measured = true;
  }
  return r;
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

  // Step 1: per-region requirement profile (round-2 review S3). Output pixels
  // size only two of the three regions; the mosaic region is measured.
  std::vector<RegionRequirements> requirements;
  requirements.reserve(corpus.size());
  bool any_probe_ok = false;
  for (size_t i = 0; i < corpus.size(); ++i) {
    RegionRequirements r = profile_region_requirements(corpus[i].c_str());
    std::printf(
        "[PersistentDeviceArena] requirements corpus[%zu]=%s pixels=%llu "
        "mosaic_bytes=%llu stage3_bytes=%llu destination_bytes=%llu "
        "resident_total_bytes=%llu measured=%d\n",
        i, corpus[i].c_str(), (unsigned long long)r.pixels,
        (unsigned long long)r.mosaic_bytes,
        (unsigned long long)r.stage_three_bytes,
        (unsigned long long)r.destination_bytes,
        (unsigned long long)r.resident_total_bytes, r.measured ? 1 : 0);
    if (r.pixels > 0) any_probe_ok = true;
    requirements.push_back(r);
  }
  CHECK("region_requirements_profiled", any_probe_ok,
        "at least one corpus file must probe successfully");

  // Warmup selection: the file attaining the maximum of EACH region, deduped.
  // With a single dominating file this collapses to the historical one-file
  // warmup; otherwise it is the minimal covering set.
  std::vector<size_t> warmup_indices;
  const char* kRegionNames[3] = {"source_mosaic", "stage3_rgb16",
                                 "destination_rgba8"};
  for (int region = 0; region < 3; ++region) {
    size_t best_index = 0;
    uint64_t best_bytes = 0;
    for (size_t i = 0; i < requirements.size(); ++i) {
      const RegionRequirements& r = requirements[i];
      const uint64_t bytes = region == 0   ? r.mosaic_bytes
                             : region == 1 ? r.stage_three_bytes
                                           : r.destination_bytes;
      if (bytes > best_bytes) {
        best_bytes = bytes;
        best_index = i;
      }
    }
    std::printf(
        "[PersistentDeviceArena] region %s max = corpus[%zu] (%s) bytes=%llu\n",
        kRegionNames[region], best_index, corpus[best_index].c_str(),
        (unsigned long long)best_bytes);
    bool already = false;
    for (size_t chosen : warmup_indices) {
      if (chosen == best_index) already = true;
    }
    if (!already) warmup_indices.push_back(best_index);
  }
  if (warmup_indices.empty()) warmup_indices.push_back(0);
  std::printf("[PersistentDeviceArena] warmup covers %zu file(s):",
              warmup_indices.size());
  for (size_t index : warmup_indices) {
    std::printf(" corpus[%zu]=%s", index, corpus[index].c_str());
  }
  std::printf("\n");

  // Step 0: clean baseline (the profile above left one lane's regions live).
  ceyx::raw_persistent_device_arena_release_all_lanes();

  // Step 2: warmup = the covering set, one decode each.
  bool warmup_ok = true;
  for (size_t index : warmup_indices) {
    if (!decode_one(corpus[index].c_str())) {
      std::printf("[PersistentDeviceArena] warmup decode FAILED (%s)\n",
                  corpus[index].c_str());
      warmup_ok = false;
    }
  }
  CHECK("warmup_decode_ok", warmup_ok,
        "every warmup file must decode for the measured window to be "
        "post-growth");
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

  // Step 3: the 19 measured decodes, round-robin over the full corpus order
  // (warmup files included — re-decoding them must not grow anything).
  const int kMeasuredDecodes = 19;
  const int kTotalDecodes =
      static_cast<int>(warmup_indices.size()) + kMeasuredDecodes;
  bool all_measured_ok = true;
  for (int n = 1; n <= kMeasuredDecodes; ++n) {
    const std::string& path = corpus[static_cast<size_t>(n) % corpus.size()];
    const bool ok = decode_one(path.c_str());
    if (!ok) {
      std::printf("[PersistentDeviceArena] decode %d FAILED (%s)\n", n,
                  path.c_str());
      all_measured_ok = false;
    }
  }
  CHECK("measured_window_decodes_ok", all_measured_ok,
        "all 19 measured-window decodes must succeed for the counters to be "
        "meaningful");

  const ArenaCounters after_20 = read_counters();
  std::printf(
      "[PersistentDeviceArena] after measured window (warmup_decodes=%zu "
      "measured_decodes=%d total=%d): allocation_count=%llu "
      "growth_reallocation_count=%llu binding_count=%llu "
      "resident_device_bytes=%llu live_lane_count=%llu\n",
      warmup_indices.size(), kMeasuredDecodes, kTotalDecodes,
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
      "[PersistentDeviceArena] measured window allocation_delta=%lld "
      "growth_delta=%lld binding_count_total=%llu\n",
      (long long)allocation_delta, (long long)growth_delta,
      (unsigned long long)after_20.binding_count);

  // Required condition 2.
  CHECK("condition2_allocation_delta_is_exactly_0", allocation_delta == 0,
        "plan §9.1 condition 2 — steady state, zero new device allocations "
        "across the measured window");

  // Required condition 3.
  CHECK("condition3_growth_delta_is_0", growth_delta == 0,
        "plan §9.1 condition 3 — every region's maximum is already resident "
        "from the warmup covering set, so no region should grow in the "
        "measured window; a non-zero value here is attributable by reading "
        "the per-region requirement table printed above (a region whose "
        "maximum the warmup did not cover = gate-procedure error; a region "
        "growing past a covered maximum = real defect)");

  // Required condition 4.
  const uint64_t min_expected_bindings =
      3ull * static_cast<uint64_t>(kTotalDecodes);
  CHECK("condition4_binding_count_nonzero", after_20.binding_count != 0,
        "plan §9.1 condition 4 — distinguishes 'zero allocations because it "
        "works' from 'zero allocations because the arena is absent' (§8.4)");
  std::printf(
      "[PersistentDeviceArena] condition4 bound: 3 * %d total decodes = %llu\n",
      kTotalDecodes, (unsigned long long)min_expected_bindings);
  CHECK("condition4_binding_count_at_least_3_per_decode",
        after_20.binding_count >= min_expected_bindings,
        "plan §9.1 condition 4 — three regions bound per decode, "
        "warmup + measured decodes total");

  std::printf("[PersistentDeviceArena] TOTAL failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
