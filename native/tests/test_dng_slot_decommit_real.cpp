// test_dng_slot_decommit_real.cpp — mem8 T3-real (SR-6), cases R0..R6.
//
// WHAT THIS INSTRUMENT IS, AND HOW IT DIFFERS FROM test_dng_slot_decommit:
// the sibling gate drives the arena SYNTHETICALLY (it builds its own small
// DecodeSlotPool and memsets into it) and says so in its own banner. It
// therefore proves the mechanism and NOT that the mechanism is reachable from
// a real decode. This binary closes exactly that gap: it decodes a REAL DNG
// file through the shipping FFI entry (ceyx_decode_into_buffer, the same entry
// test_metal_queue_pool uses).
//
// THE HEADLINE, because it inverts what this file was written to show: on this
// host's Metal decode path the funnel reclaims ADDRESS SPACE, NOT MEMORY. The
// arenas hold ZERO host-resident pages when it runs, so there is nothing for
// madvise to return. committed_bytes() is raised inside allocate() from the
// bump OFFSET (decode_context.h:94) — it counts bytes HANDED OUT, never pages
// FAULTED IN — and the Stage-3 working set lives device-side since T4 (SR-7).
// Do not quote the funnel's byte figure as a footprint saving.
//
// WHAT IS FATAL HERE, and why it is arranged this way:
//   R0/R1  the pool is cold, then a real decode makes
//          dng_decode_slot_pool_exists() read TRUE. This is the gap D0/D8
//          structurally cannot close: D8 proves the substituted predicate
//          discriminates on a NON-PUBLISHING CONSTRUCTION path, but only a real
//          decode proves the PRODUCTION path reaches that state at all. The
//          frozen spec's `g_configured_slots != 0` would have skipped the DNG
//          idle half forever, and R1 observes that predicate reading 0 on the
//          very run where the substituted one fires.
//   R2/R3_committed/R4  the funnel's bookkeeping and D-P1-4's high-water pin.
//   R5     the mechanical encoding of the headline: arena host-resident bytes
//          are ZERO across the shrink, measured over the arenas' EXACT ranges
//          via dng_debug_arena_ranges(). Fatal ON PURPOSE. If anyone
//          reintroduces host-touched arena pages, R5 goes red and SR-6's claim
//          must be re-examined; if the probe breaks or interrogates a zero-byte
//          span, R5 also goes red rather than agreeing with the expected zero.
//   R6     the debug accessor R5 depends on does not construct the pool when
//          queried cold — it is production code sharing the idle funnel's
//          guard, so instrumentation must stay free on a pure-RAW session.
// The two R3 residency/footprint readings are NON-FATAL by ruling (team-lead,
// 2026-09-20): they are NOT-MET on a CORRECTLY FUNCTIONING system, and a gate
// that ships red against correct behaviour teaches everyone to ignore red. The
// synthetic sibling's D7 remains the fatal memory-subject check, and it runs
// where the arena genuinely holds pages.
//
// Full evidence, including the in-run positive control that makes the zero
// trustworthy and the two alternative explanations closed by measurement:
// native/tests/tmp/t3real-90-signoff-facts.txt
//
// Usage: test_dng_slot_decommit_real <dng_file>
// Exit 0 iff every FATAL case passes. A missing/undecodable file is exit 2,
// never a silent pass — a skipped real decode is precisely the hole this
// binary exists to close.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ceyx_decode_into.h"
#include "dng_ffi_api.h"
#include "dng_pipeline_config.h"
#include "dng_pipeline.h"  // the guard pair + committed-bytes disclosure
#include "raw_ffi_api.h"   // the one native idle funnel

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

// Physical footprint of THIS process, or 0 when unavailable.
size_t processFootprintBytes() {
#if defined(__APPLE__)
  task_vm_info_data_t info;
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<size_t>(info.phys_footprint);
#else
  return 0;
#endif
}

// T3REAL DIAGNOSTIC — TEMPORARY. Resident bytes over an explicit address
// range. phys_footprint is a WHOLE-PROCESS figure and the decode's own
// asynchronous teardown moves far more than the funnel does, so it cannot
// attribute a change to the arenas. This can — but only if it is itself
// validated, which residentControlCheck() below does with a positive control.
size_t residentBytes(void *b, size_t n) {
#if defined(__APPLE__)
  size_t total = 0;
  mach_vm_address_t a = (mach_vm_address_t)b;
  const mach_vm_address_t end = a + n;
  while (a < end) {
    mach_vm_size_t sz = 0;
    vm_region_extended_info_data_t vi;
    mach_msg_type_number_t c = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t o = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), (mach_vm_address_t *)&a, &sz,
                       VM_REGION_EXTENDED_INFO, (vm_region_info_t)&vi, &c,
                       &o) != KERN_SUCCESS)
      break;
    if (a >= end) break;
    total += (size_t)vi.pages_resident * (size_t)getpagesize();
    a += sz;
  }
  return total;
#else
  (void)b;
  (void)n;
  return 0;
#endif
}

// Host-resident bytes over EXACTLY the decode arenas' mapped ranges, obtained
// from dng_debug_arena_ranges() rather than guessed from the VM map.
//
// THE REGION-SCANNING HEURISTICS THAT PRECEDED THIS WERE ALL UNSOUND, and each
// failed silently with a plausible number. Recorded so no one reintroduces one:
//   (1) region size == kDecodeArenaReserveBytes -> found ZERO regions.
//   (2) any region size >= 1 GiB -> UNFALSIFIABLE. The kernel SPLITS a
//       reservation at the touched/untouched boundary, so the moment an arena
//       holds pages its touched prefix drops below the filter and the surviving
//       >= 1 GiB region is the UNTOUCHED tail. It read 0 both when the arenas
//       were empty and when they were full. Mutation M5b is what exposed it.
//   (3) contiguous runs >= 1 GiB -> falsifiable but OVER-ATTRIBUTING: the
//       reserves are allocated adjacently and coalesce into one ~6 GiB run that
//       also swallows unrelated neighbours (~212 MB on a clean tree).
//   (4) contiguous runs of an exact multiple of the reserve -> matched nothing.
// Exact ranges are the only sound attribution. No fallback scanner is kept: an
// instrument known to over-attribute must not remain as an alternative path.
size_t arenaResidentBytes(int *out_arenas, size_t *out_span) {
#if defined(__APPLE__)
  DngDebugArenaRange ranges[64];
  const size_t n = dng_debug_arena_ranges(ranges, 64);
  const size_t used = n < 64 ? n : 64;
  size_t total = 0, span = 0;
  int counted = 0;
  for (size_t i = 0; i < used; ++i) {
    if (ranges[i].base == nullptr || ranges[i].bytes == 0) continue;
    total += residentBytes(const_cast<void *>(ranges[i].base), ranges[i].bytes);
    span += ranges[i].bytes;
    ++counted;
  }
  if (out_arenas) *out_arenas = counted;
  if (out_span) *out_span = span;
  return total;
#else
  if (out_arenas) *out_arenas = 0;
  if (out_span) *out_span = 0;
  return 0;
#endif
}

// POSITIVE CONTROL for residentBytes(). A zero reading from an unvalidated
// residency probe is indistinguishable from a broken probe — that is the exact
// failure shape this campaign keeps paying for. This maps 64 MiB the same way
// DecodeArena does, touches every page, and requires the probe to SEE them.
bool residentControlCheck() {
#if defined(__APPLE__)
  const size_t kN = 64u * 1024u * 1024u;
  void *p = mmap(nullptr, kN, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
                 -1, 0);
  if (p == MAP_FAILED) {
    std::printf("[DngSlotDecommitReal] CONTROL mmap failed\n");
    return false;
  }
  const size_t r_untouched = residentBytes(p, kN);
  std::memset(p, 0xC0, kN);
  const size_t r_touched = residentBytes(p, kN);
  const int rc = madvise(p, kN, MADV_FREE_REUSABLE);
  const size_t r_after = residentBytes(p, kN);
  std::printf(
      "[DngSlotDecommitReal] CONTROL residentBytes over a 64MiB anon mapping: "
      "untouched=%zu touched=%zu madvise_rc=%d after=%zu -- probe is %s\n",
      r_untouched, r_touched, rc, r_after,
      (r_touched >= kN / 2) ? "VALIDATED (it can see resident pages)"
                            : "BROKEN (it reads 0 on known-resident pages)");
  munmap(p, kN);
  return r_untouched == 0 && r_touched >= kN / 2;
#else
  return false;
#endif
}

int failures = 0;

void report(const char *name, bool ok, const char *detail) {
  std::printf("[DngSlotDecommitReal] %s -> %s (%s)\n", name,
              ok ? "PASS" : "FAIL", detail);
  if (!ok) ++failures;
}
#define CHECK(name, cond, detail) report(name, (cond), detail)

// MEASURED AND REPORTED, NOT FATAL. Used for exactly one thing: the two R3
// readings, which are red on a CORRECTLY FUNCTIONING system (see the banner at
// the end of main and native/tests/tmp/t3real-90-signoff-facts.txt). A gate
// that ships red against correct behaviour teaches every reader to ignore red,
// which is how a real regression gets waved through. The truth those two
// readings express is instead pinned MECHANICALLY and FATALLY by
// R5_arena_pages_are_not_host_resident_on_this_path below, which fails the
// moment the world changes in either direction.
void observe(const char *name, bool ok, const char *detail) {
  std::printf("[DngSlotDecommitReal] %s -> %s [NON-FATAL, MEASURED] (%s)\n",
              name, ok ? "as-expected" : "NOT-MET", detail);
}
#define OBSERVE(name, cond, detail) observe(name, (cond), detail)

// Several concurrent decodes, so more than one slot in the pool is actually
// driven and the subsequent floor-0 decommit has more than one context to
// empty. A single decode would exercise exactly one context and make the
// funnel's byte figure indistinguishable from one slot's noise.
constexpr int kDecodeThreads = 4;

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dng_file>\n", argv[0]);
    return 2;
  }
  const std::string dng_file = argv[1];

  const bool residency_probe_ok = residentControlCheck();

  std::printf(
      "[DngSlotDecommitReal] REAL DNG DECODE through ceyx_decode_into_buffer: "
      "%s\n",
      dng_file.c_str());

  // -------------------------------------------------------------------
  // R0 — cold premise. Nothing has decoded yet, so the pool must not exist.
  // This is what makes R1 a TRANSITION rather than a standing fact: without
  // it, a pool that had somehow been constructed by static init would make
  // R1 green while proving nothing about the decode path.
  // -------------------------------------------------------------------
  const bool existed_before = dng_decode_slot_pool_exists();
  const size_t fp_cold = processFootprintBytes();
  // R6 setup: ask the new debug accessor BEFORE anything has decoded. It must
  // answer without building the pool — the same non-constructing discipline
  // dng_decode_slot_pool_exists() exists to enforce (T3.5 hazard row 3).
  DngDebugArenaRange cold_ranges[8];
  const size_t cold_range_count = dng_debug_arena_ranges(cold_ranges, 8);
  const bool exists_after_cold_query = dng_decode_slot_pool_exists();
  const size_t fp_after_cold_query = processFootprintBytes();
  CHECK("R0_pool_is_cold_before_any_decode", !existed_before,
        "the guard must read false on a process that has not decoded; if it "
        "is already true, R1 below observes no transition");
  std::printf(
      "[DngSlotDecommitReal] R6 cold dng_debug_arena_ranges -> %zu ranges | "
      "pool_exists after the query=%d | phys_footprint %zu -> %zu\n",
      cold_range_count, (int)exists_after_cold_query, fp_cold,
      fp_after_cold_query);
  CHECK("R6_debug_accessor_does_not_construct_the_pool",
        cold_range_count == 0 && !exists_after_cold_query &&
            (fp_after_cold_query < fp_cold + 64u * 1024u * 1024u),
        "the accessor added for R5 is production code on the same guard as the "
        "idle funnel: querying it on a pure-RAW session must return 0 and build "
        "nothing. A non-zero count, a flipped guard, or a footprint jump means "
        "asking an instrumentation question now costs 8 x 1.5 GiB of mmap");

  // -------------------------------------------------------------------
  // The real decode.
  // -------------------------------------------------------------------
  int32_t pw = 0, ph = 0;
  const int32_t prc =
      ceyx_probe_output_size(dng_file.c_str(), /*max_dim=*/0, &pw, &ph);
  if (prc != 0 || pw <= 0 || ph <= 0) {
    std::fprintf(stdout,
                 "[DngSlotDecommitReal] FATAL probe failed rc=%d w=%d h=%d — "
                 "cannot run a real decode, refusing to report a pass\n",
                 prc, pw, ph);
    return 2;
  }
  const size_t dst_bytes =
      static_cast<size_t>(pw) * static_cast<size_t>(ph) * 4u;

  int decode_failures = 0;
  {
    std::vector<int> per_thread_fail(kDecodeThreads, 0);
    std::vector<std::thread> threads;
    for (int t = 0; t < kDecodeThreads; ++t) {
      threads.emplace_back([&, t]() {
        std::vector<uint8_t> dst(dst_bytes, 0);
        DngResult *r = ceyx_decode_into_buffer(dng_file.c_str(), /*max_dim=*/0,
                                               dst.data(), dst.size());
        if (!r || r->error_code != 0 || r->rgba_data != dst.data()) {
          per_thread_fail[t] = 1;
        }
        if (r) {
          // Invariant I1: the caller owns dst; never let dng_free_result see
          // it.
          r->rgba_data = nullptr;
          dng_free_result(r);
        }
      });
    }
    for (auto &th : threads) th.join();
    for (int f : per_thread_fail) decode_failures += f;
  }

  // -------------------------------------------------------------------
  // R1 — ACCEPTANCE 1. The guard reads TRUE after a real decode.
  // -------------------------------------------------------------------
  const bool exists_after = dng_decode_slot_pool_exists();
  const size_t published_raw = dng_decode_published_slot_count_raw();
  const size_t committed_after_decode = dng_decode_committed_context_bytes();
  const size_t high_water_after_decode = dng_decode_arena_high_water_bytes();

  std::printf(
      "[DngSlotDecommitReal] R1 decode_failures=%d (%dx%d, %d threads) | "
      "pool_exists %d -> %d | published_raw=%zu | committed=%zu high_water=%zu\n",
      decode_failures, pw, ph, kDecodeThreads, (int)existed_before,
      (int)exists_after, published_raw, committed_after_decode,
      high_water_after_decode);

  CHECK("R1_real_decode_succeeded", decode_failures == 0,
        "every thread must have decoded into its own buffer; a failed decode "
        "makes everything below a statement about an error path");
  CHECK("R1_guard_reads_true_after_a_real_decode", exists_after,
        "ACCEPTANCE 1: the production decode path constructs the pool, and the "
        "substituted predicate sees it. A false here means the DNG idle half "
        "is dead in production while every synthetic gate stays green");
  CHECK("R1_committed_bytes_are_nonzero_after_a_real_decode",
        committed_after_decode > 0,
        "the red baseline for R2/R3: if a real decode leaves nothing committed "
        "there is nothing for the funnel to reclaim and R2 would be vacuous");
  // Not an acceptance item; recorded because it is the whole reason the guard
  // was substituted, and now observed on the PRODUCTION path rather than on
  // D8's stand-in.
  std::printf(
      "[DngSlotDecommitReal] NOTE spec's rejected predicate reads %zu after a "
      "real decode (0 == would have skipped the DNG half forever)\n",
      published_raw);

  // -------------------------------------------------------------------
  // R2/R3 — ACCEPTANCE 2. The funnel releases non-zero bytes, and they are
  // visible in phys_footprint. The real-decode analogue of D1/D2 + D7.
  //
  // PRE-REGISTERED PASS RULE, fixed before any number was seen:
  //   R2  released_bytes > 0
  //   R3  phys_footprint drop >= released_bytes / 2
  // The halving is deliberate slack: the subject is "did the pages come back",
  // not a precise accounting, and other subsystems are free to fault pages in
  // between the two readings. A decommit that returns nothing physical drops 0
  // and fails regardless of the slack.
  // -------------------------------------------------------------------
  int rg_before = 0, rg_after = 0;
  size_t span_before = 0, span_after = 0;
  const size_t arena_res_before = arenaResidentBytes(&rg_before, &span_before);
  const size_t fp_driven = processFootprintBytes();
  const int64_t released = ceyx_native_idle_shrink(0);
  const size_t fp_after = processFootprintBytes();
  const size_t arena_res_after = arenaResidentBytes(&rg_after, &span_after);

  const size_t grew = fp_driven > fp_cold ? fp_driven - fp_cold : 0;
  const size_t dropped = fp_driven > fp_after ? fp_driven - fp_after : 0;
  const size_t committed_after_shrink = dng_decode_committed_context_bytes();
  const size_t high_water_after_shrink = dng_decode_arena_high_water_bytes();

  std::printf(
      "[DngSlotDecommitReal] R2/R3 phys_footprint cold=%zu driven=%zu "
      "after=%zu | grew=%zu dropped=%zu | idle_shrink returned %lld | "
      "committed %zu -> %zu | high_water %zu -> %zu\n",
      fp_cold, fp_driven, fp_after, grew, dropped, (long long)released,
      committed_after_decode, committed_after_shrink, high_water_after_decode,
      high_water_after_shrink);

  CHECK("R2_funnel_released_nonzero_bytes_after_a_real_decode", released > 0,
        "ACCEPTANCE 2 (accounting): ceyx_native_idle_shrink must report bytes "
        "handed back from contexts a REAL decode drove. A zero means the DNG "
        "half is wired but inert on the production path");
  std::printf(
      "[DngSlotDecommitReal] R3 ARENA-EXACT resident %zu -> %zu over %d/%d "
      "arenas spanning %zu/%zu bytes (drop %zu) | residency probe validated=%d\n",
      arena_res_before, arena_res_after, rg_before, rg_after, span_before,
      span_after,
      arena_res_before > arena_res_after ? arena_res_before - arena_res_after
                                         : 0,
      residency_probe_ok ? 1 : 0);
  // NON-FATAL by ruling (team-lead, 2026-09-20). Both of these are NOT-MET on
  // a correctly functioning system today, for the reason R5 below pins
  // mechanically. Shipping them fatal would mean shipping a gate that is red
  // against correct behaviour, which trains readers to ignore red.
  OBSERVE("R3_arenas_held_physical_pages_before_the_shrink",
          residency_probe_ok && rg_before > 0 &&
              arena_res_before >= static_cast<size_t>(released) / 2,
          "TWO-SIDED, and the growth arm comes FIRST: if the arenas hold no "
          "resident pages when the funnel runs, then the funnel's byte figure "
          "is ADDRESS SPACE, not memory, and the drop arm below cannot mean "
          "anything. committed_bytes() is raised by allocate() from the bump "
          "OFFSET (decode_context.h:94), so it counts bytes handed out, never "
          "pages faulted in");
  OBSERVE("R3_release_is_visible_in_phys_footprint",
          fp_cold > 0 && dropped >= static_cast<size_t>(released) / 2,
          "would be ACCEPTANCE 2 (memory) if there were pages to return. The "
          "synthetic sibling gate's D7 remains the FATAL memory-subject check, "
          "and it runs where the arena genuinely holds pages");

  // -------------------------------------------------------------------
  // R5 — FATAL, and the mechanical encoding of today's truth.
  //
  // The two readings above are NOT-MET because the DNG arena holds no
  // host-resident pages on this path at all. That is a FACT ABOUT THE SYSTEM,
  // and a fact is assertable even when it is disappointing. Pinning it here
  // means the gate goes red the moment the world changes IN EITHER DIRECTION:
  //   - someone reintroduces host-touched arena pages (T4/T20 rework, a CPU
  //     fallback path, a non-Metal backend) -> R5 fails, and the two readings
  //     above become meaningful again, so SR-6's claim must be re-examined;
  //   - the probe itself breaks -> residency_probe_ok is false -> R5 fails.
  // Either way a human is forced back to the claim that actually changed,
  // instead of the finding quietly rotting in a document.
  // -------------------------------------------------------------------
  CHECK("R5_arena_pages_are_not_host_resident_on_this_path",
        residency_probe_ok && rg_before > 0 && span_before > 0 &&
            arena_res_before == 0 && arena_res_after == 0,
        "TODAY'S TRUTH, asserted so a change to it cannot pass silently: the "
        "DNG decode arenas hold ZERO host-resident pages across the idle "
        "shrink, so T3 reclaims address space and not memory on this path. "
        "Measured over the arenas' EXACT ranges via dng_debug_arena_ranges(), "
        "not guessed from the VM map — every region-scanning heuristic tried "
        "before it was unsound and one was unfalsifiable. Requires a non-zero "
        "measured SPAN and a validated positive control, so a zero can never "
        "mean 'nothing was interrogated' and a broken or blind probe fails "
        "here rather than agreeing with the expected zero");

  CHECK("R3_committed_is_zeroed_by_the_funnel", committed_after_shrink == 0,
        "floor 0 releases every free context; a non-zero remainder means "
        "contexts were skipped");

  // -------------------------------------------------------------------
  // R4 — D-P1-4 on the real path: high-water must NOT move across a decommit.
  // -------------------------------------------------------------------
  CHECK("R4_high_water_unchanged_across_a_real_decommit",
        high_water_after_decode > 0 &&
            high_water_after_shrink == high_water_after_decode,
        "high-water is the existing monotonic disclosure figure behind "
        "dng_decode_arena_high_water_bytes(); if a decommit moved it, that "
        "disclosure would silently change meaning for every consumer");

  std::printf(
      "\n"
      "[DngSlotDecommitReal] ============ READ THIS BEFORE QUOTING ANY NUMBER "
      "ABOVE ============\n"
      "[DngSlotDecommitReal] On this host's Metal DNG decode path the idle "
      "funnel reclaims ADDRESS SPACE, not memory. It reported %lld bytes while "
      "the arenas held %zu host-resident bytes — they are never CPU-touched, "
      "because the Stage-3 working set lives device-side (T4/SR-7) and\n"
      "[DngSlotDecommitReal] committed_bytes() is raised from the bump OFFSET "
      "in allocate() (decode_context.h:94), i.e. it counts bytes handed out, "
      "never pages faulted in. The two R3 lines above are therefore NOT-MET on "
      "a CORRECTLY FUNCTIONING system and are non-fatal by ruling;\n"
      "[DngSlotDecommitReal] R5 asserts that fact fatally instead, so a change "
      "in either direction forces re-examination. Do NOT quote the funnel's "
      "byte figure as a footprint saving. Full evidence, including the "
      "positive control that makes the zero trustworthy and the two\n"
      "[DngSlotDecommitReal] alternative explanations closed by measurement: "
      "native/tests/tmp/t3real-90-signoff-facts.txt\n"
      "[DngSlotDecommitReal] ================================================="
      "==================\n\n",
      (long long)released, arena_res_before);

  std::printf("[DngSlotDecommitReal] TOTAL failures=%d\n", failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}
