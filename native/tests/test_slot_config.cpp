// test_slot_config.cpp — R4 item 1 (three-layer parallelism sync).
//
// Proves the native slot cap is CONFIGURABLE and that the configured value is
// the bound acquirers actually observe. Under ruling r-6 it additionally
// proves the value is NOT clamped against the machine's recommended width:
// the user's setting is the single authority end to end.
//
// No decode, no sample files, no GPU — this is the cheap mechanical half of
// the evidence. test_concurrent_decode --configure-slots is the expensive half
// that exercises the real decode path.
//
// LINKAGE. This binary links the real dng_decoder_native dylib rather than
// recompiling the pipeline sources. Two reasons: recompiling dng_pipeline.cpp
// would drag in the whole Halide AOT chain for what is a bookkeeping test, and
// — the important one — groups (e)/(f) then exercise the C ABI as EXPORTED
// FROM THE SHIPPING ARTIFACT, not from a private rebuild. That is the same
// lesson cmake/tests.cmake:1568-1575 records: a test binary that omits the
// campaign's TUs silently contains none of the changed code, and every run
// against it is evidence about nothing.
//
// TWO POOLS, ON PURPOSE. Groups (b)-(d) construct a LOCAL DecodeSlotPool (the
// class is header-only) to test resize bookkeeping in isolation. Groups (e)/(f)
// go through the C ABI to the dylib's own process-global pool. These are
// different objects and nothing here asserts consistency between them.
//
// Output contract: one `[slotcfg] PASS|FAIL <name>` line per check, then
// either `[slotcfg] ALL PASS` (exit 0) or `[slotcfg] FAILURES=<n>` (exit 1).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "dng_pipeline_config.h"
#include "decode_context.h"
#include "dng_ffi_api.h"

namespace {

int g_failures = 0;

void check(bool ok, const char *what, long long got, long long want) {
  if (ok) {
    std::printf("[slotcfg] PASS %s (got=%lld)\n", what, got);
  } else {
    std::printf("[slotcfg] FAIL %s got=%lld want=%lld\n", what, got, want);
    ++g_failures;
  }
}

// Runs `threads` acquirers against `pool`, each briefly holding a slot, and
// returns the maximum number observed holding one SIMULTANEOUSLY.
//
// The counter is maintained by the ACQUIRERS, not by the pool. A counter the
// pool keeps against its own free list cannot falsify the pool — it is bounded
// by construction — so it could never catch the pool handing one context to
// two callers. This one can.
size_t observedPeak(DecodeSlotPool &pool, size_t threads, int iterations) {
  std::atomic<size_t> live{0};
  std::atomic<size_t> peak{0};
  std::vector<std::thread> workers;
  workers.reserve(threads);
  for (size_t t = 0; t < threads; ++t) {
    workers.emplace_back([&pool, &live, &peak, iterations]() {
      for (int i = 0; i < iterations; ++i) {
        DecodeSlotPool::Slot slot = pool.acquire();
        const size_t now = live.fetch_add(1) + 1;
        size_t observed = peak.load();
        while (now > observed && !peak.compare_exchange_weak(observed, now)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        live.fetch_sub(1);
      }
    });
  }
  for (auto &w : workers) w.join();
  return peak.load();
}

// A small arena keeps this test's own footprint trivial. Arena size is
// irrelevant to slot bookkeeping, which is all that is under test here.
constexpr size_t kTestArenaBytes = static_cast<size_t>(8) * 1024 * 1024;

}  // namespace

int main() {
  // ---- (a) the recommendation arithmetic is self-consistent --------------
  // Recomputed here from the same public inputs, so a typo in the derivation
  // shows up as a mismatch rather than being echoed back at us.
  const size_t rec61 = PipelineConfig::decodeRecommendedSlots();
  const size_t budget = PipelineConfig::decodeRecommendationBudgetBytes();
  const size_t per61 = PipelineConfig::residentBytesPerSlotForPixels(
      PipelineConfig::kDecodePixels61MP);
  size_t expected61 = budget / per61;
  if (expected61 > PipelineConfig::kAbsoluteMaxDecodeSlots) {
    expected61 = PipelineConfig::kAbsoluteMaxDecodeSlots;
  }
  if (expected61 < 1) expected61 = 1;
  check(rec61 == expected61, "61MP recommendation matches recomputation",
        static_cast<long long>(rec61), static_cast<long long>(expected61));

  std::printf("[slotcfg] INFO physical=%zu budget=%zu per24=%zu per61=%zu "
              "per108=%zu rec24=%zu rec61=%zu rec108=%zu\n",
              PipelineConfig::physicalMemoryBytes(), budget,
              PipelineConfig::residentBytesPerSlotForPixels(
                  PipelineConfig::kDecodePixels24MP),
              per61,
              PipelineConfig::residentBytesPerSlotForPixels(
                  PipelineConfig::kDecodePixels108MP),
              PipelineConfig::decodeRecommendedSlotsForPixels(
                  PipelineConfig::kDecodePixels24MP),
              rec61,
              PipelineConfig::decodeRecommendedSlotsForPixels(
                  PipelineConfig::kDecodePixels108MP));

  // The published per-slot figures must match the derivation doc exactly.
  check(PipelineConfig::residentBytesPerSlotForPixels(
            PipelineConfig::kDecodePixels24MP) == 469762048u,
        "24MP per-slot resident == 448 MiB",
        static_cast<long long>(PipelineConfig::residentBytesPerSlotForPixels(
            PipelineConfig::kDecodePixels24MP)),
        469762048LL);
  check(per61 == 872415232u, "61MP per-slot resident == 832 MiB",
        static_cast<long long>(per61), 872415232LL);
  check(PipelineConfig::residentBytesPerSlotForPixels(
            PipelineConfig::kDecodePixels108MP) == 1409286144u,
        "108MP per-slot resident == 1344 MiB",
        static_cast<long long>(PipelineConfig::residentBytesPerSlotForPixels(
            PipelineConfig::kDecodePixels108MP)),
        1409286144LL);

  // Bigger frames can never recommend MORE slots than smaller ones.
  const size_t rec24 = PipelineConfig::decodeRecommendedSlotsForPixels(
      PipelineConfig::kDecodePixels24MP);
  const size_t rec108 = PipelineConfig::decodeRecommendedSlotsForPixels(
      PipelineConfig::kDecodePixels108MP);
  check(rec24 >= rec61 && rec61 >= rec108,
        "recommendations are monotonic in frame size",
        static_cast<long long>(rec61), static_cast<long long>(rec61));

  // ---- (b) resize bookkeeping: grow, then narrow, everything idle --------
  DecodeSlotPool pool(2, kTestArenaBytes);
  check(pool.configured_slots() == 2, "initial configured",
        static_cast<long long>(pool.configured_slots()), 2);
  pool.resize(5);
  check(pool.configured_slots() == 5, "configured after grow",
        static_cast<long long>(pool.configured_slots()), 5);
  check(pool.physical_slots() == 5, "physical after grow",
        static_cast<long long>(pool.physical_slots()), 5);
  pool.resize(2);
  check(pool.configured_slots() == 2, "configured after narrow",
        static_cast<long long>(pool.configured_slots()), 2);
  check(pool.physical_slots() == 2, "physical after idle narrow",
        static_cast<long long>(pool.physical_slots()), 2);

  // ---- (c) the configured value is the bound acquirers observe -----------
  pool.resize(3);
  const size_t peak3 = observedPeak(pool, 5, 10);
  check(peak3 == 3, "peak holders at N=3 with 5 threads",
        static_cast<long long>(peak3), 3);

  // Negative control. The SAME harness at N=1 must show exactly 1; without
  // this, a peak of 3 above could be thread-timing luck rather than a
  // measurement of the bound.
  pool.resize(1);
  const size_t peak1 = observedPeak(pool, 5, 10);
  check(peak1 == 1, "negative control: peak holders at N=1 with 5 threads",
        static_cast<long long>(peak1), 1);

  // ---- (d) AC-1c boundary at the host slider maximum --------------------
  // Ruling r-6: this asserts EXACTLY 8, not min(8, recommendation). If any
  // memory-derived clamp is ever reintroduced, this is the check that fails.
  const size_t kHostSliderMax = 8;
  pool.resize(kHostSliderMax);
  check(pool.configured_slots() == kHostSliderMax,
        "configured at host slider maximum, unclamped",
        static_cast<long long>(pool.configured_slots()),
        static_cast<long long>(kHostSliderMax));
  const size_t peakMax = observedPeak(pool, kHostSliderMax + 2, 8);
  // The bound must be REACHED, not merely respected: with more threads than
  // slots the high-water must EQUAL the configured value. A bound that is
  // never approached proves nothing.
  check(peakMax == kHostSliderMax,
        "peak holders at slider maximum with N+2 threads (bound REACHED)",
        static_cast<long long>(peakMax),
        static_cast<long long>(kHostSliderMax));

  // ---- (e) the C ABI honours the request, unclamped ----------------------
  // Touches the PROCESS-GLOBAL singleton, so it must come last: its contexts
  // carry the production arena reserve. On POSIX that is address space only
  // and is never touched here.
  const int32_t eff8 = dng_decode_configure_slots(8);
  check(eff8 == 8, "C ABI: configure(8) returns 8 even below recommendation",
        eff8, 8);
  check(dng_decode_configured_slots() == 8, "C ABI: configured reads back 8",
        dng_decode_configured_slots(), 8);

  // Only the allocation-sanity bound applies, and it sits above the slider
  // range so it can never bite a real user setting.
  const int32_t effHuge = dng_decode_configure_slots(9999);
  check(static_cast<size_t>(effHuge) == PipelineConfig::kAbsoluteMaxDecodeSlots,
        "C ABI: absurd request bounded by allocation sanity only", effHuge,
        static_cast<long long>(PipelineConfig::kAbsoluteMaxDecodeSlots));
  const int32_t effZero = dng_decode_configure_slots(0);
  check(effZero == 1, "C ABI: zero floors to 1", effZero, 1);

  // Restore something sane for anything that runs after us in-process.
  dng_decode_configure_slots(2);

  // ---- (f) the recommendation is reportable but never enforcing ---------
  const int32_t recDefault = dng_decode_recommended_slots_for_pixels(0);
  check(static_cast<size_t>(recDefault) == rec61,
        "C ABI: pixels=0 yields the 61MP default recommendation", recDefault,
        static_cast<long long>(rec61));
  check(dng_decode_recommendation_class_pixels(1) ==
            static_cast<int64_t>(PipelineConfig::kDecodePixels61MP),
        "C ABI: class 1 is the 61MP frame",
        static_cast<long long>(dng_decode_recommendation_class_pixels(1)),
        static_cast<long long>(PipelineConfig::kDecodePixels61MP));

  // ---- (h) RULING r-8: the default is 4, and it is NOT a ceiling --------
  //
  // The header comment asserts "this is a DEFAULT, not a CAP". A comment
  // cannot fail, so both halves of that claim are checked here.
  //
  // The second assertion is the load-bearing one: the constant r-8 replaces
  // (kMaxDecodeSlots) had the SAME VALUE, 4, and was a genuine cap. A default
  // of 4 and a cap of 4 are indistinguishable from the unconfigured starting
  // value alone — they diverge only when something asks for more than 4. So
  // asking for 8 and requiring 8 back is what separates the two, and it is
  // what would fail if anyone ever reinstated the old behaviour.
  check(PipelineConfig::decodeSlotCount() ==
            PipelineConfig::kDefaultDecodeSlots,
        "r-8: unconfigured default is kDefaultDecodeSlots",
        static_cast<long long>(PipelineConfig::decodeSlotCount()),
        static_cast<long long>(PipelineConfig::kDefaultDecodeSlots));
  check(PipelineConfig::kDefaultDecodeSlots == 4,
        "r-8: kDefaultDecodeSlots is 4",
        static_cast<long long>(PipelineConfig::kDefaultDecodeSlots), 4);
  {
    const int32_t aboveDefault = dng_decode_configure_slots(8);
    check(aboveDefault == 8,
          "r-8: the default is NOT a ceiling — configure(8) > default returns 8",
          aboveDefault, 8);
    dng_decode_configure_slots(2);
  }

  // ---- (g) THE AC-1c CLAIM, MADE FALSIFIABLE ----------------------------
  //
  // WHY THIS GROUP EXISTS. AC-1c requires proof that a user setting reaches
  // the native layer UNCLAMPED *when it exceeds the derived recommendation*.
  // On a large-memory machine that condition is unreachable through the
  // normal classes: here every class recommends kAbsoluteMaxDecodeSlots (16),
  // the host slider maxes at 8, and 8 < 16. Worse, the absurd-request check
  // in group (e) returns 16 under BOTH hypotheses — "bounded by allocation
  // sanity" and "bounded by the recommendation" predict the identical value
  // whenever the recommendation has itself saturated at the sanity bound. So
  // group (e) alone cannot falsify a recommendation-based clamp.
  //
  // This group manufactures the condition instead of waiting for hardware
  // that happens to produce it: it asks for the recommendation at a frame so
  // large that the recommendation is genuinely small on ANY machine, then
  // configures ABOVE that number and requires the request to survive intact.
  // If any recommendation-derived clamp is ever reintroduced, this fails on
  // every machine, not just on small ones.
  const int64_t kAbsurdFramePixels = 10000000000LL;  // 10 gigapixels
  const int32_t recAbsurd =
      dng_decode_recommended_slots_for_pixels(kAbsurdFramePixels);
  check(recAbsurd >= 1 && recAbsurd < 8,
        "recommendation for a 10-gigapixel frame is small (< 8)", recAbsurd,
        7);
  if (recAbsurd < 8) {
    const int32_t effAbove = dng_decode_configure_slots(8);
    check(effAbove == 8,
          "C ABI: request of 8 ABOVE a recommendation of that frame is "
          "honoured UNCLAMPED",
          effAbove, 8);
    check(dng_decode_configured_slots() == 8,
          "C ABI: the above-recommendation value reads back unchanged",
          dng_decode_configured_slots(), 8);
  } else {
    // Declare the skip rather than let it vanish into a comfortable pass.
    std::printf("[slotcfg] SKIP above-recommendation checks: the "
                "10-gigapixel recommendation came back %d, not < 8\n",
                recAbsurd);
    ++g_failures;
  }
  dng_decode_configure_slots(2);

  if (g_failures == 0) {
    std::printf("[slotcfg] ALL PASS\n");
    return 0;
  }
  std::printf("[slotcfg] FAILURES=%d\n", g_failures);
  return 1;
}
