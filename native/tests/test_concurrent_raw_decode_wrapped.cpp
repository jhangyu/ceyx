// test_concurrent_raw_decode_wrapped.cpp — under-load gate for the
// UNIFIED-WRAPPED destination path specifically (R3-T4 close-out item,
// GPU copy-elimination campaign, plan
// docs/logs/2026-09-11/plan-gpu-copy-elimination.md §4.3, §8.2 item 3,
// §9.4 "under-load repeat").
//
// WHY THIS EXISTS, SEPARATELY FROM test_concurrent_raw_decode.cpp: that gate
// (deliberately left untouched, per this task's original instructions) uses
// plain `std::vector<uint8_t>` destinations, which are essentially never
// page-aligned, so every one of its decodes takes the unified-degraded or
// fallback destination path -- it has never once exercised the WRAPPED path
// under concurrency. The campaign's highest-severity hazard is exactly there:
// `copy_to_host()` was the only GPU completion wait on this route (plan §4.3
// "Removing copy_to_host() therefore removes the only synchronisation
// point"); the wrapped path replaces it with an explicit
// halide_device_sync, and per plan §8.2 item 3 a MISSING fence produces
// correct output on an idle single-threaded machine and torn frames only
// under concurrent load. Single-thread proof that the wrapped path fires and
// is bit-exact already exists (native/tests/tmp/r3-close-ac2-aligned-
// probe.txt, 7/7 unified=1); this driver is what a missing/ineffective fence
// would actually be caught by.
//
// PROCEDURE
//   Phase 1 (serial reference): decode every corpus file once on the main
//     thread into a plain heap buffer, hash the output. These are the
//     single-lane reference hashes -- the WRAPPED path must reproduce them
//     bit-for-bit, exactly as every other path in this campaign must.
//   Phase 2 (counter baseline): read the zero-copy counters once, before any
//     concurrent decode, so every assertion below is a DELTA.
//   Phase 3 (concurrent, WRAPPED path forced by construction): N threads
//     (>= 4, same floor as test_concurrent_raw_decode.cpp), each decoding
//     every corpus file `repeat` times into its OWN posix_memalign'd
//     destination (pointer aligned to kRawDeviceArenaAlignmentBytes,
//     capacity rounded up to a page multiple) with
//     develop.caller_destination_is_page_aligned=true, hashing each output
//     and comparing to the phase-1 reference. A missing/ineffective
//     halide_device_sync shows up here as a hash mismatch (a torn frame),
//     not as a crash -- so the hash comparison IS the fence test.
//
// ASSERTIONS (all mandatory)
//   (a) every concurrent decode succeeded and its hash equals the
//       single-threaded reference for that file -- THE fence test;
//   (b) destination_wrap_count delta == total successful concurrent
//       decodes -- proves every one of those decodes actually took the
//       wrapped path (not silently the degraded/fallback path, which would
//       make (a) pass for the wrong reason -- this driver would then be
//       testing nothing new over test_concurrent_raw_decode.cpp);
//   (c) destination_alignment_degradation_count delta == 0 for the same
//       reason, from the other side.
//
//   Phase 4 (R4-T3, round-3 review finding S1 — THE ERROR-PATH fence test):
//     phase 3 only ever exercises SUCCESSFUL wrapped decodes, so it cannot see
//     the one exit the RAII design did not cover: the kernel-failure return.
//     That return happens after halide_metal_run has already COMMITTED a
//     command buffer writing the caller's pages; it used to detach the binding
//     and hand an error back without waiting, after which the pipeline
//     releases the caller's MTLBuffer and the caller is free to release or
//     reuse the destination block — the GPU then writes memory that has been
//     reclaimed. raw_ffi_api.h's lifetime contract promises the call "does not
//     return until all GPU work against the buffer has completed", with no
//     error-path exception.
//
//     A genuine nonzero AOT result nearly always surfaces BEFORE submission,
//     so this window is not reachable by ordinary means — hence the
//     TEST-ONLY hook dngRenderStage4SetKernelFailureInjectionArmed(), which
//     makes an otherwise successful dispatch report failure at exactly that
//     post-submission position. N concurrent lanes each run one injected
//     decode into their own aligned destination and, the instant the call
//     returns, (i) snapshot the destination and (ii) after a settling delay
//     hash it again.
//
//     ASSERTIONS (d) every injected decode returned an ERROR -- proves the
//     injection is actually armed, so (e)/(f) are not passing vacuously;
//     (e) the snapshot taken immediately after the failing return hashes equal
//     to the phase-1 reference -- i.e. the frame was already fully written at
//     return, which is only true if the GPU was retired before returning (an
//     unretired command buffer yields a torn/partial frame, exactly the
//     signature phase 3's fence mutation produced);
//     (f) the post-settling hash equals the immediate snapshot -- no bytes
//     were written into the caller's pages AFTER the call returned, the
//     direct use-after-release signal.
//
// Usage:
//   test_concurrent_raw_decode_wrapped [--threads N] [--repeat R] [<raw_file>...]
// Defaults: N=4, R=2, the 3-file in-repo corpus used by the sibling gate.
// Exit 0 iff every assertion passed.

#include <atomic>
#include <chrono>
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
  std::printf("[ConcurrentRawDecodeWrapped] %s -> %s (%s)\n", name,
              ok ? "PASS" : "FAIL", detail);
  if (!ok) ++failures;
}

#define CHECK(name, cond, detail) report(name, (cond), detail)

// FNV-1a 64, identical construction to test_concurrent_raw_decode.cpp and
// test_zero_copy_capability_paths.cpp, so hashes are directly comparable
// across all three drivers.
uint64_t hash_bytes(const uint8_t* data, size_t byte_count) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < byte_count; ++i) {
    h ^= static_cast<uint64_t>(data[i]);
    h *= 1099511628211ull;
  }
  return h;
}

size_t round_up_to_page(size_t value) {
  const size_t page = ceyx::kRawDeviceArenaAlignmentBytes;
  return ((value + page - 1) / page) * page;
}

RawDevelopParams develop_params(bool destination_is_page_aligned) {
  RawDevelopParams develop{};
  develop.exposure_ev = 0.0f;
  develop.tone_curve_strength = 1.0f;
  develop.output_space = kRawOutputColorSpaceSrgb;
  develop.max_output_long_edge = 0u;
  develop.auto_exposure_mode = kRawAutoExposureOff;
  develop.caller_destination_is_page_aligned = destination_is_page_aligned;
  return develop;
}

struct DecodeOutcome {
  bool ok = false;
  uint64_t hash = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Plain heap reference decode (phase 1 only) -- deliberately NOT page-aligned
// by construction, so it is a control for what the wrapped path must match,
// never itself a wrapped decode.
DecodeOutcome decode_and_hash_reference(const char* path) {
  DecodeOutcome outcome;
  const RawDevelopParams develop = develop_params(/*destination_is_page_aligned=*/false);
  uint32_t probe_width = 0, probe_height = 0;
  if (raw_pipeline_probe_output_size(path, develop.max_output_long_edge,
                                     &probe_width, &probe_height) !=
          kRawSuccess ||
      probe_width == 0 || probe_height == 0) {
    return outcome;
  }
  const size_t capacity = static_cast<size_t>(probe_width) * probe_height * 4u;
  std::vector<uint8_t> destination(capacity);
  RawPipelineResult result{};
  const RawErrorCode rc = raw_pipeline_decode_file_into(
      path, develop, destination.data(), destination.size(), result);
  if (rc != kRawSuccess || result.rgba_ptr == nullptr) return outcome;
  outcome.width = result.width;
  outcome.height = result.height;
  outcome.hash = hash_bytes(destination.data(),
                            static_cast<size_t>(result.width) * result.height *
                                4u);
  outcome.ok = true;
  return outcome;
}

// Wrapped-path decode: posix_memalign'd destination, page-multiple capacity,
// develop.caller_destination_is_page_aligned=true -- the same recipe
// test_zero_copy_capability_paths.cpp's single-threaded phase 4 uses,
// run here from N concurrent threads instead of the main thread alone.
DecodeOutcome decode_and_hash_wrapped(const char* path) {
  DecodeOutcome outcome;
  const RawDevelopParams develop = develop_params(/*destination_is_page_aligned=*/true);
  uint32_t probe_width = 0, probe_height = 0;
  if (raw_pipeline_probe_output_size(path, develop.max_output_long_edge,
                                     &probe_width, &probe_height) !=
          kRawSuccess ||
      probe_width == 0 || probe_height == 0) {
    return outcome;
  }
  const size_t need = static_cast<size_t>(probe_width) * probe_height * 4u;
  const size_t aligned_capacity = round_up_to_page(need);
  void* aligned_ptr = nullptr;
  if (posix_memalign(&aligned_ptr, ceyx::kRawDeviceArenaAlignmentBytes,
                     aligned_capacity) != 0 ||
      aligned_ptr == nullptr) {
    return outcome;
  }
  uint8_t* aligned_dst = static_cast<uint8_t*>(aligned_ptr);
  RawPipelineResult result{};
  const RawErrorCode rc = raw_pipeline_decode_file_into(
      path, develop, aligned_dst, aligned_capacity, result);
  if (rc == kRawSuccess && result.rgba_ptr != nullptr) {
    outcome.width = result.width;
    outcome.height = result.height;
    outcome.hash = hash_bytes(
        aligned_dst,
        static_cast<size_t>(result.width) * result.height * 4u);
    outcome.ok = true;
  }
  std::free(aligned_ptr);
  return outcome;
}

// R4-T3 (S1): declared locally rather than by including dng_render_params.h,
// which pulls in the Adobe DNG SDK headers this driver has no include path
// for. The authoritative declaration lives at native/include/
// dng_render_params.h; it is extern "C" with an `int` parameter precisely so
// this mirror cannot silently diverge in mangling or ABI.
extern "C" void dngRenderStage4SetKernelFailureInjectionArmed(int armed);
extern "C" int dngRenderStage4KernelFailureInjectionIsArmed();

struct InjectedFailureOutcome {
  bool setup_ok = false;
  bool decode_returned_error = false;
  bool snapshot_matches_reference = false;
  bool no_bytes_written_after_return = false;
  uint64_t snapshot_hash = 0;
  uint64_t settled_hash = 0;
};

// One wrapped-path decode with the post-submission kernel failure ARMED.
// Everything up to raw_pipeline_decode_file_into is identical to
// decode_and_hash_wrapped above; what differs is what happens after it
// returns.
InjectedFailureOutcome decode_with_injected_kernel_failure(
    const char* path, uint64_t reference_hash) {
  InjectedFailureOutcome outcome;
  const RawDevelopParams develop = develop_params(/*destination_is_page_aligned=*/true);
  uint32_t probe_width = 0, probe_height = 0;
  if (raw_pipeline_probe_output_size(path, develop.max_output_long_edge,
                                     &probe_width, &probe_height) !=
          kRawSuccess ||
      probe_width == 0 || probe_height == 0) {
    return outcome;
  }
  const size_t pixel_bytes =
      static_cast<size_t>(probe_width) * probe_height * 4u;
  const size_t aligned_capacity = round_up_to_page(pixel_bytes);
  void* aligned_ptr = nullptr;
  if (posix_memalign(&aligned_ptr, ceyx::kRawDeviceArenaAlignmentBytes,
                     aligned_capacity) != 0 ||
      aligned_ptr == nullptr) {
    return outcome;
  }
  uint8_t* aligned_dst = static_cast<uint8_t*>(aligned_ptr);
  // Allocated BEFORE the decode so the snapshot after it is a bare memcpy --
  // an allocation there would widen the post-return window this phase is
  // trying to measure.
  std::vector<uint8_t> snapshot(pixel_bytes, 0u);
  outcome.setup_ok = true;

  RawPipelineResult result{};
  const RawErrorCode rc = raw_pipeline_decode_file_into(
      path, develop, aligned_dst, aligned_capacity, result);
  // The instant the call returns: per the FFI lifetime contract there is now
  // no GPU work outstanding against these pages, error or not.
  std::memcpy(snapshot.data(), aligned_dst, pixel_bytes);

  outcome.decode_returned_error = (rc != kRawSuccess);
  outcome.snapshot_hash = hash_bytes(snapshot.data(), pixel_bytes);
  outcome.snapshot_matches_reference = (outcome.snapshot_hash == reference_hash);

  // Settling window: generous relative to a single Stage4 dispatch, so a
  // command buffer that was still in flight at return has certainly landed by
  // now. If the destination changed during it, the GPU wrote pages the caller
  // already owns again -- the use-after-release itself, not a proxy for it.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  outcome.settled_hash = hash_bytes(aligned_dst, pixel_bytes);
  outcome.no_bytes_written_after_return =
      (outcome.settled_hash == outcome.snapshot_hash);

  std::free(aligned_ptr);
  return outcome;
}

struct ZeroCopyCounters {
  uint64_t destination_wrap_count = 0;
  uint64_t destination_alignment_degradation_count = 0;
};

ZeroCopyCounters read_zero_copy_counters() {
  ZeroCopyCounters c;
  int32_t path_is_enabled = 0, override_state = 0;
  uint64_t source_mosaic_wrap_count = 0;
  const int32_t rc = ceyx_debug_zero_copy_capability_counters(
      &path_is_enabled, &override_state, &c.destination_wrap_count,
      &c.destination_alignment_degradation_count, &source_mosaic_wrap_count);
  if (rc != 0) {
    std::printf(
        "[ConcurrentRawDecodeWrapped] WARNING: zero-copy counter probe "
        "returned %d\n",
        (int)rc);
  }
  return c;
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
  // Same floor as test_concurrent_raw_decode.cpp, same reason: fewer than 4
  // lanes does not meaningfully exercise concurrent-fence behaviour.
  if (thread_count < 4) {
    std::printf(
        "[ConcurrentRawDecodeWrapped] threads=%d refused: this gate "
        "requires at least 4 concurrent lanes\n",
        thread_count);
    return 2;
  }
  if (repeat < 1) return 2;
  std::printf("[ConcurrentRawDecodeWrapped] threads=%d repeat=%d files=%zu\n",
              thread_count, repeat, corpus.size());

  // Phase 1: single-lane reference.
  std::vector<uint64_t> reference_hashes(corpus.size(), 0);
  bool reference_ok = true;
  for (size_t i = 0; i < corpus.size(); ++i) {
    const DecodeOutcome outcome = decode_and_hash_reference(corpus[i].c_str());
    if (!outcome.ok) {
      std::printf("[ConcurrentRawDecodeWrapped] reference decode FAILED (%s)\n",
                  corpus[i].c_str());
      reference_ok = false;
      continue;
    }
    reference_hashes[i] = outcome.hash;
    std::printf(
        "[ConcurrentRawDecodeWrapped] reference corpus[%zu]=%s %ux%u "
        "hash=%016llx\n",
        i, corpus[i].c_str(), outcome.width, outcome.height,
        (unsigned long long)outcome.hash);
  }
  CHECK("reference_decodes_ok", reference_ok,
        "every corpus file must decode single-threaded before the "
        "concurrent wrapped-path hashes mean anything");
  if (!reference_ok) {
    std::printf("[ConcurrentRawDecodeWrapped] TOTAL failures=%d\n", failures);
    return 1;
  }

  // Phase 2: counter baseline.
  const ZeroCopyCounters baseline = read_zero_copy_counters();
  std::printf(
      "[ConcurrentRawDecodeWrapped] baseline: destination_wrap_count=%llu "
      "destination_alignment_degradation_count=%llu\n",
      (unsigned long long)baseline.destination_wrap_count,
      (unsigned long long)baseline.destination_alignment_degradation_count);

  // Phase 3: N concurrent lanes, every decode on the WRAPPED path.
  std::atomic<int> decode_failures{0};
  std::atomic<int> hash_mismatches{0};
  std::atomic<int> successful_decodes{0};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(thread_count));
  for (int t = 0; t < thread_count; ++t) {
    workers.emplace_back([&, t]() {
      for (int pass = 0; pass < repeat; ++pass) {
        for (size_t i = 0; i < corpus.size(); ++i) {
          // Staggered start, same reasoning as test_concurrent_raw_decode.cpp:
          // lanes working on DIFFERENT frames at the same instant makes a
          // cross-lane fence/ordering defect show up as a wrong hash rather
          // than being hidden by lockstep identical writes.
          const size_t index = (i + static_cast<size_t>(t)) % corpus.size();
          const DecodeOutcome outcome =
              decode_and_hash_wrapped(corpus[index].c_str());
          if (!outcome.ok) {
            std::printf(
                "[ConcurrentRawDecodeWrapped] thread %d pass %d decode "
                "FAILED (%s)\n",
                t, pass, corpus[index].c_str());
            decode_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
          }
          successful_decodes.fetch_add(1, std::memory_order_relaxed);
          if (outcome.hash != reference_hashes[index]) {
            std::printf(
                "[ConcurrentRawDecodeWrapped] thread %d pass %d HASH "
                "MISMATCH (%s) got=%016llx want=%016llx -- a torn frame is "
                "exactly what a missing/ineffective halide_device_sync "
                "produces under load\n",
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

  const ZeroCopyCounters after = read_zero_copy_counters();
  const int64_t wrap_delta =
      static_cast<int64_t>(after.destination_wrap_count) -
      static_cast<int64_t>(baseline.destination_wrap_count);
  const int64_t degradation_delta =
      static_cast<int64_t>(after.destination_alignment_degradation_count) -
      static_cast<int64_t>(baseline.destination_alignment_degradation_count);
  const int expected_decodes =
      thread_count * repeat * static_cast<int>(corpus.size());
  std::printf(
      "[ConcurrentRawDecodeWrapped] deltas: wrap=%lld degradation=%lld "
      "successful_decodes=%d expected_decodes=%d\n",
      (long long)wrap_delta, (long long)degradation_delta,
      successful_decodes.load(), expected_decodes);

  CHECK("all_concurrent_wrapped_decodes_succeeded",
        decode_failures.load() == 0 &&
            successful_decodes.load() == expected_decodes,
        "every dispatched wrapped-path decode must succeed");
  char fence_detail[192];
  std::snprintf(fence_detail, sizeof(fence_detail),
               "THE fence test: a missing/ineffective halide_device_sync "
               "under concurrent load produces torn frames here, not a "
               "crash; observed: hash_mismatches=%d successful_decodes=%d",
               hash_mismatches.load(), successful_decodes.load());
  CHECK("concurrent_wrapped_hashes_match_single_threaded_reference",
        hash_mismatches.load() == 0, fence_detail);
  char wrap_detail[192];
  std::snprintf(wrap_detail, sizeof(wrap_detail),
               "every concurrent decode in this driver supplies an aligned "
               "destination, so ALL of them must take the wrapped path -- "
               "observed: wrap_delta=%lld successful_decodes=%d",
               (long long)wrap_delta, successful_decodes.load());
  CHECK("wrap_delta_equals_successful_decodes",
        wrap_delta == static_cast<int64_t>(successful_decodes.load()),
        wrap_detail);
  CHECK("no_alignment_degradation_under_load", degradation_delta == 0,
        "the degraded-path counter must stay at 0 -- every decode here is "
        "genuinely page-aligned, so degradation firing would mean the wrap "
        "silently failed under concurrent load specifically");

  // Phase 4 (R4-T3 / S1): the ERROR-path fence test. See the header comment.
  std::printf(
      "[ConcurrentRawDecodeWrapped] phase 4: injecting post-submission Stage4 "
      "kernel failures on the wrapped path (S1)\n");
  dngRenderStage4SetKernelFailureInjectionArmed(1);
  CHECK("kernel_failure_injection_armed",
        dngRenderStage4KernelFailureInjectionIsArmed() == 1,
        "the test-only hook must report armed, otherwise every phase-4 "
        "assertion below would pass on ordinary successful decodes");

  std::atomic<int> injected_setup_failures{0};
  std::atomic<int> injected_decodes_that_did_not_error{0};
  std::atomic<int> injected_torn_frames{0};
  std::atomic<int> injected_late_writes{0};
  std::atomic<int> injected_decodes{0};
  std::vector<std::thread> injection_workers;
  injection_workers.reserve(static_cast<size_t>(thread_count));
  for (int t = 0; t < thread_count; ++t) {
    injection_workers.emplace_back([&, t]() {
      for (size_t i = 0; i < corpus.size(); ++i) {
        const size_t index = (i + static_cast<size_t>(t)) % corpus.size();
        const InjectedFailureOutcome outcome =
            decode_with_injected_kernel_failure(corpus[index].c_str(),
                                                reference_hashes[index]);
        if (!outcome.setup_ok) {
          injected_setup_failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        injected_decodes.fetch_add(1, std::memory_order_relaxed);
        if (!outcome.decode_returned_error) {
          injected_decodes_that_did_not_error.fetch_add(
              1, std::memory_order_relaxed);
        }
        if (!outcome.snapshot_matches_reference) {
          std::printf(
              "[ConcurrentRawDecodeWrapped] thread %d S1 TORN FRAME AT ERROR "
              "RETURN (%s) got=%016llx want=%016llx -- the destination was "
              "not fully written when the failing call returned, i.e. GPU "
              "work was still outstanding against the caller's pages\n",
              t, corpus[index].c_str(),
              (unsigned long long)outcome.snapshot_hash,
              (unsigned long long)reference_hashes[index]);
          injected_torn_frames.fetch_add(1, std::memory_order_relaxed);
        }
        if (!outcome.no_bytes_written_after_return) {
          std::printf(
              "[ConcurrentRawDecodeWrapped] thread %d S1 USE-AFTER-RETURN "
              "WRITE (%s) snapshot=%016llx settled=%016llx -- the GPU wrote "
              "the caller's pages AFTER the failing call returned\n",
              t, corpus[index].c_str(),
              (unsigned long long)outcome.snapshot_hash,
              (unsigned long long)outcome.settled_hash);
          injected_late_writes.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& worker : injection_workers) worker.join();
  dngRenderStage4SetKernelFailureInjectionArmed(0);

  std::printf(
      "[ConcurrentRawDecodeWrapped] phase 4 totals: injected_decodes=%d "
      "not_errored=%d torn_frames=%d late_writes=%d setup_failures=%d\n",
      injected_decodes.load(), injected_decodes_that_did_not_error.load(),
      injected_torn_frames.load(), injected_late_writes.load(),
      injected_setup_failures.load());

  CHECK("injected_failure_decodes_ran", injected_setup_failures.load() == 0 &&
            injected_decodes.load() ==
                thread_count * static_cast<int>(corpus.size()),
        "every phase-4 lane must have reached the decode call");
  CHECK("injected_kernel_failure_surfaced_as_error",
        injected_decodes.load() > 0 &&
            injected_decodes_that_did_not_error.load() == 0,
        "the injected post-submission failure must be reported as an error by "
        "the FFI -- if any decode reported success the injection did not take "
        "effect and the two assertions below prove nothing");
  CHECK("error_return_frame_is_complete_S1",
        injected_torn_frames.load() == 0,
        "S1: a wrapped-destination decode that fails AFTER submission must "
        "still retire the GPU before returning; a torn frame here means the "
        "error path detached and returned with work in flight");
  CHECK("no_gpu_writes_after_error_return_S1",
        injected_late_writes.load() == 0,
        "S1: nothing may be written into the caller's pages after the failing "
        "call returns -- the caller is free to release them at that instant");

  std::printf("[ConcurrentRawDecodeWrapped] TOTAL failures=%d\n", failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}
