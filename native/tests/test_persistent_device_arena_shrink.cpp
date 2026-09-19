// test_persistent_device_arena_shrink.cpp — T1 gate (mem8 campaign, SR-1):
// arena idle release down to a lane floor.
//
// AUTHORITATIVE SPEC: Halcyon docs/logs/2026-09-12/mem8-plan.md T1.4/T1.5,
// amended by docs/logs/2026-09-19/mem8-v2-plan.md P1.1 (A1 upper-bound
// restatement with the exercised format recorded, A2 the separate
// volatile_device_bytes counter, A3 the release-dominates-volatile lifecycle
// clause) and read together with docs/logs/2026-09-19/mem8-v3-plan.md P1.A
// (readings carry a ROUTE label, because a fused route legitimately holds no
// Stage-3 region at all).
//
// WHAT THIS GATE IS FOR. raw_persistent_device_arena_shrink_to_lane_floor()
// is the only native path that gives device bytes back while the process keeps
// running. Its hazard is not "does it free memory" — release_all_regions()
// already does that and is unchanged here — but "does it free the RIGHT lanes,
// and does it refuse a lane that is mid-decode". Both of those are what the
// six cases below pin.
//
// PROCEDURE
//   Phase 0  release_all_lanes(), so every byte counted below was allocated by
//            this driver.
//   Phase 1  warm N lanes (default 8) — N worker threads each decode the
//            corpus file once and then PARK, still alive. Parking matters:
//            a lane identity is a thread identity, so S5's "re-warm a lane
//            that was released" is only expressible while the owning threads
//            still exist. A thread that had exited would leave its arena in
//            the map but unreachable for a second decode, and a fresh thread
//            would be a NEW lane, which proves nothing about re-allocation of
//            a RELEASED region.
//   Phase 2  S1/S2 floor-2 shrink, S4 degenerate repeat, S5 re-warm.
//   Phase 3  S3 live-binding refusal on the main thread's own lane.
//   Phase 4  S6 floor-0 releases everything.
//
// NOT-YET-INTEGRATED HANDLING (red-first, predecessor T1.5 criterion 6): until
// raw_persistent_device_arena_shrink_to_lane_floor() and
// ceyx_debug_arena_shrink_counters() exist in the built library this driver
// does not LINK, and that link failure is the accepted RED for S1-S6 — exactly
// as test_persistent_device_arena.cpp:62-69 records for its own first run.
//
// Style follows test_persistent_device_arena.cpp's report()/CHECK() convention.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "HalideRuntime.h"

#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"
#include "raw_persistent_device_arena.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
  std::printf("[ArenaShrink] %s -> %s (%s)\n", name, ok ? "PASS" : "FAIL",
              detail);
  if (!ok) ++failures;
}

#define CHECK(name, cond, detail) report(name, (cond), detail)

struct ShrinkCounters {
  uint64_t shrink_calls = 0;
  uint64_t lanes_released = 0;
  uint64_t lanes_refused = 0;
  uint64_t bytes_released = 0;
  uint64_t resident_lane_count = 0;
  uint64_t volatile_device_bytes = 0;
};

ShrinkCounters read_shrink_counters() {
  ShrinkCounters c;
  const int32_t rc = ceyx_debug_arena_shrink_counters(
      &c.shrink_calls, &c.lanes_released, &c.lanes_refused, &c.bytes_released,
      &c.resident_lane_count, &c.volatile_device_bytes);
  if (rc != 0) {
    std::printf(
        "[ArenaShrink] WARNING: ceyx_debug_arena_shrink_counters returned %d "
        "(non-zero means ALL out-pointers were null; this driver always passes "
        "all six, so a non-zero rc here is a probe defect)\n",
        (int)rc);
  }
  return c;
}

struct AllocCounters {
  uint64_t allocation_count = 0;
  uint64_t growth_reallocation_count = 0;
  uint64_t binding_count = 0;
  uint64_t resident_device_bytes = 0;
  uint64_t live_lane_count = 0;
};

AllocCounters read_alloc_counters() {
  AllocCounters c;
  ceyx_debug_persistent_device_arena_counters(
      &c.allocation_count, &c.growth_reallocation_count, &c.binding_count,
      &c.resident_device_bytes, &c.live_lane_count);
  return c;
}

// EXERCISED FORMAT (v2 A1): every decode this driver issues requests RGBA8
// through raw_pipeline_decode_file_into. At T1's execution time the yuv420
// output arm (T12) does not exist, so `format=rgba8` is the only answer this
// driver can give — it is recorded as PROVENANCE, not as a threshold input:
// the per-lane Stage-3 region is RGB16 under every requested output format.
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
  const RawErrorCode rc = raw_pipeline_decode_file_into(
      path, develop, dst.data(), dst.size(), result);
  return rc == kRawSuccess && result.rgba_ptr == dst.data();
}

// A parked worker lane: decodes on command, then waits. See the PROCEDURE note
// on why the thread must outlive its first decode.
class WorkerLane {
 public:
  WorkerLane(const std::string& path, int index)
      : path_(path), index_(index),
        thread_([this] { run(); }) {}

  void request_decode() {
    {
      std::lock_guard<std::mutex> guard(lock_);
      ++requested_;
    }
    cv_.notify_one();
  }

  void wait_idle() {
    std::unique_lock<std::mutex> guard(lock_);
    cv_done_.wait(guard, [this] { return completed_ == requested_; });
  }

  void stop() {
    {
      std::lock_guard<std::mutex> guard(lock_);
      stop_ = true;
    }
    cv_.notify_one();
    thread_.join();
  }

  bool ok() const { return ok_.load(); }

 private:
  void run() {
    for (;;) {
      std::unique_lock<std::mutex> guard(lock_);
      cv_.wait(guard, [this] { return stop_ || completed_ < requested_; });
      if (stop_) return;
      guard.unlock();
      const bool decoded = decode_one(path_.c_str());
      if (!decoded) {
        ok_.store(false);
        std::printf("[ArenaShrink] worker %d decode FAILED (%s)\n", index_,
                    path_.c_str());
      }
      guard.lock();
      ++completed_;
      guard.unlock();
      cv_done_.notify_all();
    }
  }

  std::string path_;
  int index_ = 0;
  std::mutex lock_;
  std::condition_variable cv_;
  std::condition_variable cv_done_;
  int requested_ = 0;
  int completed_ = 0;
  bool stop_ = false;
  std::atomic<bool> ok_{true};
  std::thread thread_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string path = "image_samples/raw_sample.arw";
  int lane_count = 8;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--lanes" && i + 1 < argc) {
      lane_count = std::atoi(argv[++i]);
    } else {
      path = argument;
    }
  }
  // A gate that can be configured into vacuity is not a gate: the floor is 2,
  // so fewer than 3 lanes cannot distinguish "released the surplus" from
  // "released nothing".
  if (lane_count < 3) {
    std::printf("[ArenaShrink] lanes=%d refused: this gate requires >= 3\n",
                lane_count);
    return 2;
  }

  // PROVENANCE LINE (v2 A1 + v3 P1.A). `format` records which output format
  // the warm-up requested; `route` records which pipeline route produced the
  // resident bytes. Neither is a threshold input — they exist so a reader
  // handed two artifacts can tell a regression from a route change. At T1's
  // execution time the fused route (T20) does not exist, so every byte below
  // belongs to the unfused generic-RAW route and its RGB16 Stage-3 region.
  std::printf("[ArenaShrink] format=rgba8 route=generic_raw_unfused lanes=%d "
              "file=%s\n",
              lane_count, path.c_str());

  // Phase 0 — clean baseline.
  ceyx::raw_persistent_device_arena_release_all_lanes();

  // Phase 1 — warm N lanes on N parked worker threads.
  std::vector<WorkerLane*> workers;
  workers.reserve(static_cast<size_t>(lane_count));
  for (int i = 0; i < lane_count; ++i) {
    workers.push_back(new WorkerLane(path, i));
  }
  for (auto* w : workers) w->request_decode();
  for (auto* w : workers) w->wait_idle();

  bool all_decoded = true;
  for (auto* w : workers) all_decoded = all_decoded && w->ok();
  CHECK("warmup_decodes_succeeded", all_decoded,
        "every warm-up decode must succeed before any shrink assertion means "
        "anything");

  const AllocCounters warm = read_alloc_counters();
  const uint64_t resident_warm = warm.resident_device_bytes;
  const uint64_t per_lane_bytes =
      lane_count > 0 ? resident_warm / static_cast<uint64_t>(lane_count) : 0;
  std::printf(
      "[ArenaShrink] warm: resident_device_bytes=%llu live_lane_count=%llu "
      "per_lane=%llu allocation_count=%llu\n",
      (unsigned long long)resident_warm,
      (unsigned long long)warm.live_lane_count,
      (unsigned long long)per_lane_bytes,
      (unsigned long long)warm.allocation_count);
  // Bound provenance, printed so the artifact carries it without a reader
  // having to open this source file (lead ruling, 2026-09-20). The frame size
  // is PROBED rather than hard-coded, so the line stays true when this gate is
  // pointed at a different file via argv.
  {
    uint32_t pw = 0, ph = 0;
    const double megapixels =
        raw_pipeline_probe_output_size(path.c_str(), 0u, &pw, &ph) ==
                kRawSuccess
            ? (static_cast<double>(pw) * ph) / 1.0e6
            : 0.0;
    std::printf(
        "[ArenaShrink] bound_provenance: plan_constant=194MB@~16MP "
        "measured=%lluMB@%.1fMP self_derived=resident_warm/lanes "
        "(the bound is NOT the plan's constant; see S1's comment)\n",
        (unsigned long long)(per_lane_bytes / (1024ull * 1024ull)),
        megapixels);
  }

  // The whole gate is vacuous without resident bytes to release: on a host
  // with no Metal device every arena call answers nullptr and every number
  // below would be a legitimate zero. Say so rather than printing PASS lines
  // that assert nothing.
  if (resident_warm == 0) {
    std::printf(
        "[ArenaShrink] SKIP-REFUSED: warm-up produced 0 resident device bytes "
        "(no Metal arena on this host). This gate cannot pass vacuously.\n");
    for (auto* w : workers) { w->stop(); delete w; }
    return 3;
  }

  const ShrinkCounters before_s1 = read_shrink_counters();

  // ---- S1: floor-2 shrink releases exactly the lanes in excess of the floor.
  const ceyx::RawArenaShrinkOutcome s1 =
      ceyx::raw_persistent_device_arena_shrink_to_lane_floor(2);
  const AllocCounters after_s1 = read_alloc_counters();
  const ShrinkCounters counters_s1 = read_shrink_counters();
  std::printf(
      "[ArenaShrink] S1: lanes_released=%zu lanes_refused=%zu "
      "bytes_released=%llu resident_after=%llu\n",
      s1.lanes_released, s1.lanes_refused,
      (unsigned long long)s1.bytes_released,
      (unsigned long long)after_s1.resident_device_bytes);

  {
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "expected lanes_released=%d, got %zu",
                  lane_count - 2, s1.lanes_released);
    CHECK("S1_released_lane_count",
          s1.lanes_released == static_cast<size_t>(lane_count - 2), detail);
  }
  {
    // UPPER bound, never an equality (v2 A1): a lane legitimately holding
    // fewer regions must not fail this gate.
    //
    // PROVENANCE OF THE BOUND (lead ruling, 2026-09-20). The predecessor plan
    // states this criterion as a literal `<= 2 x 194 MB`. That constant was
    // derived from a ~16 MP frame; the in-repo corpus file this gate drives
    // is 6048x4024 on the sensor and 6024x4024 = 24.2 MP after crop at the
    // pipeline's output, which measures ~278 MB per lane. Rather than
    // widen the plan's constant — or, worse, choose a corpus file that makes
    // the plan's number come out — the bound is DERIVED from this same run's
    // own warm-up residency (resident_warm / lane_count). That is the
    // criterion's intent stated mechanically: after a floor-2 shrink,
    // residency must have collapsed to at most the floor's worth of lanes.
    // A self-derived bound cannot be gamed by frame choice, and the run
    // passes it at exact equality rather than with slack.
    const uint64_t bound = 2ull * per_lane_bytes;
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "resident_after=%llu must be <= 2 * per_lane = %llu",
                  (unsigned long long)after_s1.resident_device_bytes,
                  (unsigned long long)bound);
    CHECK("S1_residency_at_or_below_two_lanes",
          after_s1.resident_device_bytes <= bound, detail);
  }
  CHECK("S1_bytes_released_matches_residency_drop",
        s1.bytes_released == resident_warm - after_s1.resident_device_bytes,
        "the reported byte figure is read from the arena's own counter, never "
        "inferred");
  CHECK("S1_counters_recorded_the_call",
        counters_s1.shrink_calls == before_s1.shrink_calls + 1 &&
            counters_s1.lanes_released ==
                before_s1.lanes_released + s1.lanes_released,
        "the process-wide shrink counters must move with the call");

  // ---- S2: exactly two lanes still hold regions.
  {
    char detail[128];
    std::snprintf(detail, sizeof(detail), "resident_lane_count=%llu, want 2",
                  (unsigned long long)counters_s1.resident_lane_count);
    CHECK("S2_resident_lane_count_equals_floor",
          counters_s1.resident_lane_count == 2, detail);
  }

  // ---- A2 baseline: with T17 not yet landed the volatile counter reads 0.
  // This is the red-before-green baseline T17 flips.
  {
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "volatile_device_bytes=%llu, want 0 before T17 lands",
                  (unsigned long long)counters_s1.volatile_device_bytes);
    CHECK("A2_volatile_counter_zero_before_T17",
          counters_s1.volatile_device_bytes == 0, detail);
  }

  // ---- S4: degenerate repeat. Nothing to do is SUCCESS, not failure.
  const ceyx::RawArenaShrinkOutcome s4 =
      ceyx::raw_persistent_device_arena_shrink_to_lane_floor(2);
  const AllocCounters after_s4 = read_alloc_counters();
  CHECK("S4_degenerate_call_is_all_zeros",
        s4.lanes_released == 0 && s4.lanes_refused == 0 &&
            s4.bytes_released == 0,
        "a call with <= floor lanes resident is a no-op returning all zeros");
  CHECK("S4_degenerate_call_left_residency_untouched",
        after_s4.resident_device_bytes == after_s1.resident_device_bytes,
        "the below-floor lanes must not be released");

  // ---- S5: re-warm a RELEASED lane. Its regions must re-allocate (visible in
  // the existing allocation counter, never hidden) and must NOT be counted as
  // growth: a released region is gone, so its next bind is a first allocation.
  const AllocCounters before_s5 = read_alloc_counters();
  for (auto* w : workers) w->request_decode();
  for (auto* w : workers) w->wait_idle();
  const AllocCounters after_s5 = read_alloc_counters();
  std::printf(
      "[ArenaShrink] S5: allocation_delta=%llu growth_delta=%llu "
      "resident_after=%llu\n",
      (unsigned long long)(after_s5.allocation_count -
                           before_s5.allocation_count),
      (unsigned long long)(after_s5.growth_reallocation_count -
                           before_s5.growth_reallocation_count),
      (unsigned long long)after_s5.resident_device_bytes);
  CHECK("S5_rewarm_allocations_are_visible",
        after_s5.allocation_count > before_s5.allocation_count,
        "a released region must re-allocate rather than stay unavailable, and "
        "the cost must appear in the existing counter (AC1 clause)");
  CHECK("S5_rewarm_is_not_counted_as_growth",
        after_s5.growth_reallocation_count ==
            before_s5.growth_reallocation_count,
        "same frame size, so no region exceeds an existing one");

  // ---- S3: live-binding refusal (the hazard this task exists to contain).
  // Run on the MAIN thread's own lane, which is distinct from every worker
  // lane because a lane identity is a thread identity.
  ceyx::RawPersistentDeviceArena* main_arena =
      ceyx::raw_persistent_device_arena_for_current_lane();
  if (main_arena == nullptr) {
    CHECK("S3_live_binding_refused", false,
          "the main thread got no arena, so the refusal path cannot be "
          "exercised — this is a gate-procedure failure, not a defect");
  } else {
    halide_buffer_t probe_buffer{};
    const size_t probe_bytes = 1u << 20;
    {
      ceyx::RawDeviceArenaRegionBinding binding(
          main_arena, &probe_buffer,
          ceyx::RawDeviceArenaRegion::kSourceMosaicRegion, probe_bytes);
      const bool bound = static_cast<bool>(binding);
      CHECK("S3_probe_binding_established", bound,
            "S3 needs a live binding on the main lane before it can assert a "
            "refusal");
      const uint64_t lane_bytes_before = main_arena->resident_device_bytes();
      const ceyx::RawArenaShrinkOutcome s3 =
          ceyx::raw_persistent_device_arena_shrink_to_lane_floor(0);
      const uint64_t lane_bytes_after = main_arena->resident_device_bytes();
      std::printf(
          "[ArenaShrink] S3: lanes_refused=%zu lane_bytes_before=%llu "
          "lane_bytes_after=%llu\n",
          s3.lanes_refused, (unsigned long long)lane_bytes_before,
          (unsigned long long)lane_bytes_after);
      CHECK("S3_live_binding_refused", bound && s3.lanes_refused >= 1,
            "a shrink must REFUSE a lane holding a live binding, not defer "
            "silently (lanes_refused == 0 would be the silent-defer defect)");
      CHECK("S3_refused_lane_bytes_unchanged",
            lane_bytes_after == lane_bytes_before,
            "the refused lane's residency must be byte-identical");
    }
    // The binding must still detach cleanly at scope exit; if the shrink had
    // torn the region out from under it this is where it would crash.
    CHECK("S3_binding_detached_cleanly",
          !main_arena->has_live_binding(),
          "the RAII binding must detach at scope exit after a refused shrink");
  }

  // ---- S6: floor 0 releases every quiescent lane.
  const ceyx::RawArenaShrinkOutcome s6 =
      ceyx::raw_persistent_device_arena_shrink_to_lane_floor(0);
  const AllocCounters after_s6 = read_alloc_counters();
  std::printf("[ArenaShrink] S6: lanes_released=%zu bytes_released=%llu "
              "resident_after=%llu\n",
              s6.lanes_released, (unsigned long long)s6.bytes_released,
              (unsigned long long)after_s6.resident_device_bytes);
  CHECK("S6_floor_zero_releases_everything",
        after_s6.resident_device_bytes == 0,
        "floor 0 is legal and must release every quiescent lane");
  {
    const ShrinkCounters final_counters = read_shrink_counters();
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "resident_lane_count=%llu, want 0",
                  (unsigned long long)final_counters.resident_lane_count);
    CHECK("S6_resident_lane_count_zero",
          final_counters.resident_lane_count == 0, detail);
  }

  for (auto* w : workers) { w->stop(); delete w; }

  std::printf("[ArenaShrink] failures=%d\n", failures);
  return failures == 0 ? 0 : 1;
}
