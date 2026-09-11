// test_concurrent_raw_decode.cpp — multi-lane RAW concurrency gate
// (Round 2.5 parking-lot item "multi-lane RAW concurrency case needed before
// Slice C's per-lane isolation claims"; plan
// docs/logs/2026-09-11/plan-gpu-copy-elimination.md §8.2 hazards 1-2, §2.1,
// §3.3).
//
// WHY THIS EXISTS: every AC1/AC4 gate so far drives the generic RAW route on
// ONE lane (the process main thread). The persistent device arena's entire
// safety argument, however, is per-lane isolation: one arena per lane keyed on
// pthread_self(), 1:1 with the sticky per-thread Metal queue
// (dng_metal_context.cpp), so a region can never be written by a kernel on one
// queue while being read on another (plan §8.2 hazard 1). A single-threaded
// gate cannot observe that property AT ALL — it is trivially true with one
// lane. This driver is the evidence C2's under-load AC4 repeat builds on.
//
// PROCEDURE
//   Phase 1 (serial reference): decode every corpus file once on the main
//     thread and hash the RGBA output (FNV-1a 64). These hashes are the
//     single-lane reference the concurrent outputs must equal bit-for-bit.
//   Phase 2 (reset): raw_persistent_device_arena_release_all_lanes(), then
//     read the counter baseline. Every assertion below is on a DELTA across
//     the concurrent phase, because the five counters are process-wide totals
//     and phase 1 already moved them.
//   Phase 3 (concurrent): N threads (default 4, >= 4 required by the parking
//     lot item), each decoding every corpus file `repeat` times into its OWN
//     malloc'd destination buffer, hashing each output and comparing to the
//     phase-1 reference. The main thread does NOT decode during this phase, so
//     the live lane count at the end must be exactly N.
//
// ASSERTIONS (all mandatory)
//   (a) every concurrent decode succeeded, wrote into the caller's buffer, and
//       its hash equals the single-threaded reference for that file;
//   (b) live_lane_count == N — one lane per decoding thread. Lower means
//       threads shared a lane (per-lane isolation is not what the arena is
//       doing) or some thread got no arena at all; higher means lane identity
//       is not thread identity;
//   (c) allocation delta == 3 * N — each of the N lanes allocated its three
//       regions exactly once from the clean baseline. This is the mechanical
//       form of "the arena is per-lane": a shared arena would allocate 3 in
//       total, an absent arena 0;
//   (d) binding delta >= 3 * total concurrent decodes — three regions bound
//       per decode. A binding_failure lowers this, so it is the in-binary
//       proxy for the parking lot's "zero binding_failure events"; the
//       artifact additionally greps this binary's output for
//       `event=binding_failure`, the direct evidence (same convention as the
//       AC1 artifact). NOTE: no string this driver PRINTS may contain that
//       token — RUN 1 proved the grep otherwise matches the driver's own PASS
//       message and reports a hit on a healthy run.
//   Growth reallocations are PRINTED, not asserted: with actual-size sizing
//   (ruling R-2026-09-11-2) a lane that meets a larger frame later in its own
//   sequence legitimately grows a region, and the thread interleaving decides
//   who meets what first.
//
// Usage:
//   test_concurrent_raw_decode [--threads N] [--repeat R] [<raw_file>...]
// Defaults: N=4, R=2, the 3-file in-repo corpus used by the AC1 gate.
// Exit 0 iff every assertion passed.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "raw_ffi_api.h"
#include "raw_gpu_pipeline.h"
#include "raw_persistent_device_arena.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
  std::printf("[ConcurrentRawDecode] %s -> %s (%s)\n", name,
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
        "[ConcurrentRawDecode] WARNING: counter probe returned %d (non-zero "
        "means every out-pointer was null; this driver passes all five, so a "
        "non-zero rc is a probe defect)\n",
        (int)rc);
  }
  return c;
}

// FNV-1a 64. Chosen over a checksum because a transposition of two decodes'
// outputs (the corruption shape this gate hunts) must change the digest.
uint64_t hash_bytes(const uint8_t* data, size_t byte_count) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < byte_count; ++i) {
    h ^= static_cast<uint64_t>(data[i]);
    h *= 1099511628211ull;
  }
  return h;
}

RawDevelopParams develop_params() {
  RawDevelopParams develop{};
  develop.exposure_ev = 0.0f;
  develop.tone_curve_strength = 1.0f;
  develop.output_space = kRawOutputColorSpaceSrgb;
  develop.max_output_long_edge = 0u;
  develop.auto_exposure_mode = kRawAutoExposureOff;
  return develop;
}

struct DecodeOutcome {
  bool ok = false;
  uint64_t hash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool wrote_into_caller_buffer = false;
};

// One full generic-RAW decode into a buffer this function owns, hashed and
// freed before returning — so a thread's peak footprint is one frame, and the
// only thing that escapes is the digest.
DecodeOutcome decode_and_hash(const char* path) {
  DecodeOutcome outcome;
  const RawDevelopParams develop = develop_params();
  uint32_t probe_width = 0, probe_height = 0;
  if (raw_pipeline_probe_output_size(path, develop.max_output_long_edge,
                                     &probe_width, &probe_height) !=
          kRawSuccess ||
      probe_width == 0 || probe_height == 0) {
    return outcome;
  }
  const size_t capacity =
      static_cast<size_t>(probe_width) * probe_height * 4u;
  std::vector<uint8_t> destination(capacity);
  RawPipelineResult result{};
  const RawErrorCode rc = raw_pipeline_decode_file_into(
      path, develop, destination.data(), destination.size(), result);
  if (rc != kRawSuccess || result.rgba_ptr == nullptr) return outcome;
  outcome.wrote_into_caller_buffer = (result.rgba_ptr == destination.data());
  outcome.width = result.width;
  outcome.height = result.height;
  outcome.hash = hash_bytes(destination.data(),
                            static_cast<size_t>(result.width) * result.height *
                                4u);
  outcome.ok = true;
  return outcome;
}

}  // namespace

int main(int argc, char** argv) {
  int thread_count = 4;
  int repeat = 2;
  std::vector<std::string> corpus;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--threads" && i + 1 < argc) {
      thread_count = std::atoi(argv[++i]);
    } else if (argument == "--repeat" && i + 1 < argc) {
      repeat = std::atoi(argv[++i]);
    } else {
      corpus.push_back(argument);
    }
  }
  if (corpus.empty()) {
    corpus = {
        "image_samples/raw_sample.arw",
        "image_samples/raw_corpus/fuji_xt3.raf",
        "image_samples/raw_corpus/fuji_xt5.raf",
    };
  }
  // The parking-lot item specifies N >= 4; a smaller value would not exercise
  // multi-lane behaviour meaningfully, so it is refused rather than silently
  // accepted (a gate that can be configured into vacuity is not a gate).
  if (thread_count < 4) {
    std::printf(
        "[ConcurrentRawDecode] threads=%d refused: this gate requires at "
        "least 4 concurrent lanes\n",
        thread_count);
    return 2;
  }
  if (repeat < 1) return 2;
  if (static_cast<size_t>(thread_count) >
      ceyx::kRawDeviceArenaMaximumLaneCount) {
    std::printf(
        "[ConcurrentRawDecode] threads=%d exceeds the arena lane ceiling "
        "(%zu); lanes above the ceiling correctly get NO arena, which would "
        "make the live-lane assertion below fail for a non-defect reason\n",
        thread_count, ceyx::kRawDeviceArenaMaximumLaneCount);
    return 2;
  }
  std::printf("[ConcurrentRawDecode] threads=%d repeat=%d files=%zu\n",
              thread_count, repeat, corpus.size());

  // Phase 1: single-lane reference on the main thread.
  std::vector<uint64_t> reference_hashes(corpus.size(), 0);
  bool reference_ok = true;
  for (size_t i = 0; i < corpus.size(); ++i) {
    const DecodeOutcome outcome = decode_and_hash(corpus[i].c_str());
    if (!outcome.ok) {
      std::printf("[ConcurrentRawDecode] reference decode FAILED (%s)\n",
                  corpus[i].c_str());
      reference_ok = false;
      continue;
    }
    reference_hashes[i] = outcome.hash;
    std::printf(
        "[ConcurrentRawDecode] reference corpus[%zu]=%s %ux%u hash=%016llx\n",
        i, corpus[i].c_str(), outcome.width, outcome.height,
        (unsigned long long)outcome.hash);
  }
  CHECK("reference_decodes_ok", reference_ok,
        "every corpus file must decode single-threaded before the concurrent "
        "hashes mean anything");
  if (!reference_ok) {
    std::printf("[ConcurrentRawDecode] TOTAL failures=%d\n", failures);
    return 1;
  }

  // Phase 2: clean lane baseline, then the counter baseline for the deltas.
  ceyx::raw_persistent_device_arena_release_all_lanes();
  const ArenaCounters baseline = read_counters();
  std::printf(
      "[ConcurrentRawDecode] baseline: allocation_count=%llu "
      "growth_reallocation_count=%llu binding_count=%llu "
      "resident_device_bytes=%llu live_lane_count=%llu\n",
      (unsigned long long)baseline.allocation_count,
      (unsigned long long)baseline.growth_reallocation_count,
      (unsigned long long)baseline.binding_count,
      (unsigned long long)baseline.resident_device_bytes,
      (unsigned long long)baseline.live_lane_count);
  CHECK("baseline_live_lane_count_is_0", baseline.live_lane_count == 0,
        "release_all_lanes must leave no lane behind, otherwise the "
        "live_lane_count == N assertion below is measuring leftovers");

  // Phase 3: N concurrent lanes.
  std::atomic<int> decode_failures{0};
  std::atomic<int> hash_mismatches{0};
  std::atomic<int> ownership_violations{0};
  std::atomic<int> successful_decodes{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(thread_count));
  for (int t = 0; t < thread_count; ++t) {
    workers.emplace_back([&, t]() {
      for (int pass = 0; pass < repeat; ++pass) {
        for (size_t i = 0; i < corpus.size(); ++i) {
          // Each thread starts at a different file so the lanes are working on
          // DIFFERENT frames at the same instant — a cross-lane region overlap
          // then shows up as a wrong hash, whereas lockstep threads decoding
          // the same frame would write identical bytes and hide it (the
          // 2026-09-03 Stage-3 false green, docs/logs/2026-09-04).
          const size_t index =
              (i + static_cast<size_t>(t)) % corpus.size();
          const DecodeOutcome outcome =
              decode_and_hash(corpus[index].c_str());
          if (!outcome.ok) {
            std::printf(
                "[ConcurrentRawDecode] thread %d pass %d decode FAILED (%s)\n",
                t, pass, corpus[index].c_str());
            decode_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          successful_decodes.fetch_add(1, std::memory_order_relaxed);
          if (!outcome.wrote_into_caller_buffer) {
            ownership_violations.fetch_add(1, std::memory_order_relaxed);
          }
          if (outcome.hash != reference_hashes[index]) {
            std::printf(
                "[ConcurrentRawDecode] thread %d pass %d HASH MISMATCH (%s) "
                "got=%016llx want=%016llx\n",
                t, pass, corpus[index].c_str(),
                (unsigned long long)outcome.hash,
                (unsigned long long)reference_hashes[index]);
            hash_mismatches.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }
  for (auto& worker : workers) worker.join();

  const ArenaCounters after = read_counters();
  std::printf(
      "[ConcurrentRawDecode] after concurrent phase: allocation_count=%llu "
      "growth_reallocation_count=%llu binding_count=%llu "
      "resident_device_bytes=%llu live_lane_count=%llu\n",
      (unsigned long long)after.allocation_count,
      (unsigned long long)after.growth_reallocation_count,
      (unsigned long long)after.binding_count,
      (unsigned long long)after.resident_device_bytes,
      (unsigned long long)after.live_lane_count);

  const int64_t allocation_delta =
      static_cast<int64_t>(after.allocation_count) -
      static_cast<int64_t>(baseline.allocation_count);
  const int64_t growth_delta =
      static_cast<int64_t>(after.growth_reallocation_count) -
      static_cast<int64_t>(baseline.growth_reallocation_count);
  const int64_t binding_delta =
      static_cast<int64_t>(after.binding_count) -
      static_cast<int64_t>(baseline.binding_count);
  const int expected_decodes =
      thread_count * repeat * static_cast<int>(corpus.size());
  std::printf(
      "[ConcurrentRawDecode] deltas: allocation=%lld growth=%lld binding=%lld "
      "successful_decodes=%d expected_decodes=%d\n",
      (long long)allocation_delta, (long long)growth_delta,
      (long long)binding_delta, successful_decodes.load(), expected_decodes);

  // (a) correctness under concurrency.
  CHECK("all_concurrent_decodes_succeeded",
        decode_failures.load() == 0 &&
            successful_decodes.load() == expected_decodes,
        "every dispatched decode must succeed, otherwise the counter deltas "
        "below describe a shorter run than the one being judged");
  CHECK("every_decode_wrote_into_its_own_caller_buffer",
        ownership_violations.load() == 0,
        "result.rgba_ptr must equal the destination this thread passed in");
  CHECK("concurrent_hashes_match_single_threaded_reference",
        hash_mismatches.load() == 0,
        "bit-exactness under N lanes — a cross-lane arena region overlap "
        "shows up here first");

  // (b) one lane per decoding thread.
  CHECK("live_lane_count_equals_thread_count",
        after.live_lane_count == static_cast<uint64_t>(thread_count),
        "per-lane isolation: fewer lanes than decoding threads means threads "
        "shared an arena or ran unarened; more means lane identity is not "
        "thread identity");

  // (c) each lane allocated its own three regions exactly once.
  CHECK("allocation_delta_equals_three_per_lane",
        allocation_delta == 3ll * thread_count,
        "three regions per lane from a clean baseline; a shared arena would "
        "allocate 3 in total and an absent arena 0");

  // (d) binding coverage / binding_failure proxy.
  CHECK("binding_delta_at_least_three_per_decode",
        binding_delta >= 3ll * expected_decodes,
        // The literal token the artifact greps for is DELIBERATELY not spelled
        // in any string this driver prints: RUN 1 of this gate showed the
        // grep matching this very PASS message, so the check would have
        // reported a hit on a healthy run (the 2026-08-28 self-collision
        // family). The token lives only in the artifact and in comments.
        "three regions bound per decode; a failed region binding lowers this, "
        "and the artifact greps the arena log for the failure event");

  std::printf("[ConcurrentRawDecode] TOTAL failures=%d\n", failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}
