// test_dng_slot_decommit.cpp — mem8 T3 (SR-6) gate, cases D1..D6.
//
// WHAT THIS INSTRUMENT IS, STATED UP FRONT (spec T3.3, OQ-3): no DNG corpus
// exists on this host, so the arena is driven SYNTHETICALLY — DecodeSlotPool is
// constructed directly and its contexts' arenas are allocate()d by hand. That
// is legitimate for the STRUCTURAL claims below (a decommit returns pages; the
// floor is respected; a checked-out context is never touched; high-water does
// not move), and it is NOT evidence of any DNG decode saving. No number this
// binary prints may be quoted as a measured DNG saving.
//
// It also deliberately does NOT touch decodeSlotPool(), the process-wide
// accessor: constructing that would mmap 8 x 1.5 GiB. Every pool here is local
// and small.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "decode_context.h"
#include "dng_pipeline.h"  // D8: the guard pair and the non-publishing accessor
#include "raw_ffi_api.h"   // D0/D8: the idle funnel and the T3 residency probe

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace {

// Physical footprint of THIS process, or 0 when unavailable on the platform.
// Test-only: the mach header is included here and never by the production
// header, so decode_context.h stays portable.
//
// phys_footprint, NOT resident_size, and that was MEASURED rather than assumed.
// The first draft of D7 used resident_size and reported a flat zero drop
// against a CORRECT implementation. An isolated probe
// (native/tests/tmp/t3-12-isolated-variants.txt, one variant per process so no
// reading starts from a baseline an earlier call collapsed) showed
// resident_size moving for NO madvise variant on this host, while
// phys_footprint drops by exactly the driven byte count for
// MADV_FREE_REUSABLE. resident_size is simply not the observable for this
// question on Darwin.
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

int failures = 0;

void report(const char *name, bool ok, const char *detail) {
  std::printf("[DngSlotDecommit] %s -> %s (%s)\n", name, ok ? "PASS" : "FAIL",
              detail);
  if (!ok) ++failures;
}
#define CHECK(name, cond, detail) report(name, (cond), detail)

// Small enough that four of them cost nothing, large enough to span several
// pages so the decommit length rounding is actually exercised.
constexpr size_t kReserveBytes = 4u * 1024u * 1024u;

// PRE-REGISTERED (written before the first run, never tuned afterwards):
// each context is driven with exactly this many bytes, so acceptance item 2's
// bound is 2 * round_up_to_page(kDriveBytes) for a floor of 2.
constexpr size_t kDriveBytes = 1u * 1024u * 1024u;

// Drives one context's arena and leaves a known byte pattern behind, then
// resets it the way DecodeSlotPool::release() would.
void driveArena(DecodeContext *ctx, uint8_t tag) {
  void *p = ctx->arena.allocate(kDriveBytes);
  if (p != nullptr) std::memset(p, tag, kDriveBytes);
  ctx->arena.reset();
}

}  // namespace

int main() {
  std::printf(
      "[DngSlotDecommit] SYNTHETIC ARENA DRIVE — no DNG corpus on this host "
      "(OQ-3). Structural coverage only; this binary measures no DNG saving.\n");

  // -------------------------------------------------------------------
  // D0 — THE GUARD IS LIVE, and it must run FIRST, before anything in this
  // binary has constructed the process-wide slot pool.
  //
  // This is the hazard in T3.5 row 3: touching decodeSlotPool() to answer a
  // bookkeeping question would mmap 8 x 1.5 GiB on a pure-RAW session. Every
  // other case here builds its OWN local pool, so this process is a faithful
  // stand-in for a session that has never decoded a DNG.
  //
  // It is also the only case that exercises the guard I substituted for the
  // frozen spec's predicate. The spec proposed "configured slots != 0", which
  // reads 0 on a process that HAS decoded DNGs (the decode path constructs the
  // pool without setting it) and would therefore disable the whole DNG half
  // silently. D0 pins the half of that I can observe here: no construction on
  // a cold process.
  // -------------------------------------------------------------------
  {
    const size_t fp_before = processFootprintBytes();
    uint64_t committed = 1, calls = 1, ctxs_done = 1, physical = 1;
    const int32_t probe_rc = ceyx_debug_dng_slot_residency_counters(
        &committed, &calls, &ctxs_done, &physical);
    const int64_t shrink_bytes = ceyx_native_idle_shrink(2);
    const size_t fp_after = processFootprintBytes();
    const size_t fp_grew = fp_after > fp_before ? fp_after - fp_before : 0;

    std::printf(
        "[DngSlotDecommit] D0 cold probe rc=%d committed=%llu calls=%llu "
        "contexts=%llu physical_slots=%llu | idle_shrink returned %lld | "
        "phys_footprint grew %zu\n",
        probe_rc, (unsigned long long)committed, (unsigned long long)calls,
        (unsigned long long)ctxs_done, (unsigned long long)physical,
        (long long)shrink_bytes, fp_grew);

    CHECK("D0_probe_answers_without_constructing_the_pool",
          probe_rc == 0 && physical == 0 && committed == 0,
          "on a process that has never decoded a DNG the probe must report "
          "zeros; a non-zero physical_slots means asking the question built "
          "the pool");
    // One slot pool is 8 x 1.5 GiB of RESERVE; the contexts only fault pages
    // in when touched, so the sharpest portable signal is that the footprint
    // did not jump by a slot's worth. 64 MiB is far below one context and far
    // above this probe's real cost.
    CHECK("D0_guard_costs_no_footprint",
          fp_grew < 64u * 1024u * 1024u,
          "the idle funnel and its probe must be free on a pure-RAW session; "
          "a jump here means the guard let the pool be constructed");
  }

  // -------------------------------------------------------------------
  // D1 — red baseline: driven-then-released contexts report committed > 0.
  // -------------------------------------------------------------------
  {
    DecodeSlotPool pool(4, kReserveBytes);
    std::vector<DecodeContext *> ctxs;
    {
      std::vector<DecodeSlotPool::Slot> slots;
      for (int i = 0; i < 4; ++i) slots.push_back(pool.acquire());
      for (auto &s : slots) {
        ctxs.push_back(&s.context());
        driveArena(&s.context(), static_cast<uint8_t>(0xA0 + ctxs.size()));
      }
    }  // every Slot destroyed -> every context back in free_

    bool all_committed = true;
    for (auto *c : ctxs) {
      if (c->arena.committed_bytes() == 0) all_committed = false;
    }
    std::printf("[DngSlotDecommit] D1 committed per context:");
    for (auto *c : ctxs)
      std::printf(" %zu", c->arena.committed_bytes());
    std::printf("\n");
    CHECK("D1_driven_contexts_report_committed_bytes", all_committed,
          "this is the red baseline the decommit must change; a zero here "
          "would make every assertion below vacuous");

    // -----------------------------------------------------------------
    // D2 — floor semantics, and the scratch capacity, not just the arena.
    // -----------------------------------------------------------------
    // Give EVERY context a poly3_scratch capacity so the swap idiom is under
    // test too. Deliberately all four rather than one chosen context: free_ is
    // not in construction order (acquire() pops from the back and the Slots
    // destruct in reverse), so "which context the floor keeps" is not
    // something this test should predict. Attaching the scratch to a single
    // guessed context is how the first draft of this case failed against a
    // CORRECT implementation. Asserting over all four is order-independent and
    // strictly stronger: it pins the emptied ones to 0 AND the kept ones to
    // non-zero, so a decommit that cleared everybody would also be caught.
    for (auto *c : ctxs) c->handoff.poly3_scratch.assign(1024, 7);
    size_t scratch_nonzero_before = 0;
    for (auto *c : ctxs) {
      if (c->handoff.poly3_scratch.capacity() > 0) ++scratch_nonzero_before;
    }

    const size_t released = pool.decommit_free_to_floor(2);
    std::printf(
        "[DngSlotDecommit] D2 released=%zu committed after:", released);
    for (auto *c : ctxs) std::printf(" %zu", c->arena.committed_bytes());
    std::printf("\n");

    // free_ is filled in construction order and acquire() pops from the BACK,
    // so after releasing all four the free list order is not the construction
    // order. The invariant that matters is the COUNT, not which identities:
    // exactly `floor` contexts stay warm and the rest are emptied.
    size_t zeroed = 0, warm = 0;
    for (auto *c : ctxs) {
      if (c->arena.committed_bytes() == 0) ++zeroed; else ++warm;
    }
    CHECK("D2_floor_keeps_exactly_two_warm", warm == 2 && zeroed == 2,
          "decommit_free_to_floor(2) on a 4-context pool must empty exactly "
          "the two in excess of the floor");
    CHECK("D2_released_bytes_are_reported", released > 0,
          "the funnel's byte figure must reflect what was actually handed "
          "back, otherwise the idle path logs a silent zero");
    // The scratch must follow the arena exactly: emptied where the arena was
    // emptied, retained where it was kept.
    size_t scratch_zeroed = 0, scratch_kept = 0, scratch_mismatch = 0;
    for (auto *c : ctxs) {
      const bool arena_emptied = c->arena.committed_bytes() == 0;
      const bool scratch_emptied = c->handoff.poly3_scratch.capacity() == 0;
      if (arena_emptied != scratch_emptied) ++scratch_mismatch;
      if (scratch_emptied) ++scratch_zeroed; else ++scratch_kept;
    }
    std::printf(
        "[DngSlotDecommit] D2 scratch nonzero_before=%zu zeroed=%zu kept=%zu "
        "mismatch_vs_arena=%zu\n",
        scratch_nonzero_before, scratch_zeroed, scratch_kept, scratch_mismatch);
    CHECK("D2_scratch_capacity_is_returned",
          scratch_nonzero_before == 4 && scratch_zeroed == 2 &&
              scratch_kept == 2 && scratch_mismatch == 0,
          "swap idiom, not clear(): clear() keeps capacity, so the scratch "
          "would stay resident while every arena assertion still went green. "
          "mismatch_vs_arena != 0 means arena and scratch disagree about "
          "which contexts were released");

    // -----------------------------------------------------------------
    // D3 — the mapping survives. This is the one way T3 can corrupt a decode.
    // -----------------------------------------------------------------
    DecodeContext *victim = nullptr;
    for (auto *c : ctxs) {
      if (c->arena.committed_bytes() == 0) { victim = c; break; }
    }
    bool readback_ok = false;
    if (victim != nullptr) {
      auto *p = static_cast<uint8_t *>(victim->arena.allocate(kDriveBytes));
      if (p != nullptr) {
        std::memset(p, 0x5A, kDriveBytes);
        readback_ok = (p[0] == 0x5A) && (p[kDriveBytes / 2] == 0x5A) &&
                      (p[kDriveBytes - 1] == 0x5A);
      }
      victim->arena.reset();
    }
    CHECK("D3_decommitted_arena_is_still_usable", readback_ok,
          "MADV_FREE / DiscardVirtualMemory must leave a usable mapping; a "
          "failure here means MEM_DECOMMIT-class semantics crept in and the "
          "next decode would fault");
  }

  // -------------------------------------------------------------------
  // D4 — degenerate: floor >= free count is a no-op AND a success.
  // -------------------------------------------------------------------
  {
    DecodeSlotPool pool(2, kReserveBytes);
    std::vector<DecodeContext *> ctxs;
    {
      std::vector<DecodeSlotPool::Slot> slots;
      for (int i = 0; i < 2; ++i) slots.push_back(pool.acquire());
      for (auto &s : slots) {
        ctxs.push_back(&s.context());
        driveArena(&s.context(), 0xC0);
      }
    }
    const size_t before_a = ctxs[0]->arena.committed_bytes();
    const size_t before_b = ctxs[1]->arena.committed_bytes();
    const size_t released = pool.decommit_free_to_floor(2);
    CHECK("D4_degenerate_floor_is_a_successful_no_op",
          released == 0 &&
              ctxs[0]->arena.committed_bytes() == before_a &&
              ctxs[1]->arena.committed_bytes() == before_b,
          "pool of 2 with floor 2 touches nothing and returns 0; a zero "
          "outcome here is SUCCESS, not an error");
  }

  // -------------------------------------------------------------------
  // D5 — a checked-out context is never decommitted. Use-after-free hazard:
  // release_idle_state() issues a Metal device free.
  // -------------------------------------------------------------------
  {
    DecodeSlotPool pool(2, kReserveBytes);
    DecodeContext *held = nullptr;
    size_t held_committed_before = 0;
    {
      DecodeSlotPool::Slot keep = pool.acquire();
      held = &keep.context();
      void *p = held->arena.allocate(kDriveBytes);
      if (p != nullptr) std::memset(p, 0xD5, kDriveBytes);
      held_committed_before = held->arena.committed_bytes();

      // Floor 0 asks for everything free to be released, while this context
      // is checked out and therefore NOT in free_.
      pool.decommit_free_to_floor(0);

      CHECK("D5_checked_out_context_is_untouched",
            held_committed_before > 0 &&
                held->arena.committed_bytes() == held_committed_before,
            "a context held by a live Slot must be skipped entirely; "
            "decommitting it would discard pages the holder still owns");
    }
  }

  // -------------------------------------------------------------------
  // D6 — high-water is PINNED across a decommit (D-P1-4).
  // -------------------------------------------------------------------
  {
    DecodeSlotPool pool(2, kReserveBytes);
    {
      std::vector<DecodeSlotPool::Slot> slots;
      for (int i = 0; i < 2; ++i) slots.push_back(pool.acquire());
      for (auto &s : slots) driveArena(&s.context(), 0xE6);
    }
    const size_t hw_before = pool.high_water_bytes();
    const size_t committed_before = pool.committed_context_bytes();
    pool.decommit_free_to_floor(0);
    const size_t hw_after = pool.high_water_bytes();
    const size_t committed_after = pool.committed_context_bytes();
    std::printf(
        "[DngSlotDecommit] D6 high_water %zu -> %zu | committed %zu -> %zu\n",
        hw_before, hw_after, committed_before, committed_after);
    CHECK("D6_high_water_unchanged_across_decommit",
          hw_before > 0 && hw_after == hw_before,
          "D-P1-4: high-water is the existing monotonic disclosure figure. If "
          "a decommit moved it, dng_decode_arena_high_water_bytes() would "
          "silently change meaning for every existing consumer");
    CHECK("D6_committed_is_the_resettable_one",
          committed_before > 0 && committed_after == 0,
          "committed_bytes is the quantity a decommit zeroes; if it tracked "
          "high-water it could not answer 'are those pages still held'");

    // Acceptance item 2, with the bound pre-registered in the source above:
    // after a floor-2 decommit on a driven pool, the committed total must not
    // exceed two slots' worth.
    DecodeSlotPool pool8(8, kReserveBytes);
    {
      std::vector<DecodeSlotPool::Slot> slots;
      for (int i = 0; i < 8; ++i) slots.push_back(pool8.acquire());
      for (auto &s : slots) driveArena(&s.context(), 0xF8);
    }
    pool8.decommit_free_to_floor(2);
    const size_t total = pool8.committed_context_bytes();
    // kDriveBytes is already page-aligned on every target here; the bound is
    // stated against the drive size, which is what was pre-registered.
    const size_t bound = 2 * kDriveBytes;
    std::printf(
        "[DngSlotDecommit] acceptance-2 committed_total=%zu bound=%zu "
        "(pre-registered as 2 x kDriveBytes)\n",
        total, bound);
    CHECK("A2_committed_total_within_two_slots", total <= bound,
          "width-8 synthetic drive then floor-2 decommit must leave at most "
          "two slots' worth committed");
  }

  // -------------------------------------------------------------------
  // D7 — THE PAGES ACTUALLY COME BACK.
  //
  // WHY THIS CASE EXISTS, recorded because it is the whole point: D1-D6 and A2
  // above ALL PASS GREEN against a decommit() whose madvise has been deleted
  // and which only zeroes committed_ (measured, artifact t3-07-mutation-p6.txt:
  // TOTAL failures=0). Every one of them asserts BOOKKEEPING. That is the same
  // shape as the T1/T17 baton's warning about the plan-literal mutation, and
  // the same shape as T4's counter measuring the fallback branch rather than
  // the allocation. So the suite needs one case whose subject is the physical
  // memory, not the accounting: drive enough pages to clear the noise floor,
  // then watch resident_size across the decommit.
  //
  // This is also why the Apple branch uses MADV_FREE_REUSABLE — plain
  // MADV_FREE is lazy on Darwin and would leave this case unable to observe a
  // correct implementation.
  // -------------------------------------------------------------------
  {
    constexpr size_t kBigReserve = 96u * 1024u * 1024u;
    constexpr size_t kBigDrive = 64u * 1024u * 1024u;
    constexpr int kCtxCount = 4;

    const size_t fp_start = processFootprintBytes();
    if (fp_start == 0) {
      std::printf(
          "[DngSlotDecommit] D7 SKIPPED: no resident-size instrument on this "
          "platform. THIS IS A COVERAGE HOLE, not a pass — on such a platform "
          "nothing in this binary would catch a decommit that returns no "
          "pages.\n");
    } else {
      DecodeSlotPool pool(kCtxCount, kBigReserve);
      {
        std::vector<DecodeSlotPool::Slot> slots;
        for (int i = 0; i < kCtxCount; ++i) slots.push_back(pool.acquire());
        for (auto &s : slots) {
          auto *p = static_cast<uint8_t *>(s.context().arena.allocate(kBigDrive));
          // Touch every page: allocate() only moves the bump offset, so
          // without this the pages were never faulted in and there would be
          // nothing to give back.
          if (p != nullptr) std::memset(p, 0xD7, kBigDrive);
          s.context().arena.reset();
        }
      }
      const size_t fp_driven = processFootprintBytes();
      pool.decommit_free_to_floor(0);
      const size_t fp_after = processFootprintBytes();

      const size_t grew = fp_driven > fp_start ? fp_driven - fp_start : 0;
      const size_t dropped = fp_driven > fp_after ? fp_driven - fp_after : 0;
      std::printf(
          "[DngSlotDecommit] D7 phys_footprint: start=%zu driven=%zu after=%zu | "
          "grew=%zu dropped=%zu (drive total=%zu)\n",
          fp_start, fp_driven, fp_after, grew, dropped,
          kBigDrive * kCtxCount);

      // Two-sided on purpose. The growth arm proves the instrument can see
      // this memory at all, so a zero drop cannot be explained away as "the
      // pages were never resident"; the drop arm is the actual claim. The
      // threshold is half the driven bytes — deliberately loose, because the
      // subject is "did the pages come back", not a precise accounting.
      CHECK("D7_instrument_observed_the_pages_arriving",
            grew >= (kBigDrive * kCtxCount) / 2,
            "phys_footprint must rise when the arenas are touched, otherwise a "
            "flat drop below would be measuring nothing");
      CHECK("D7_decommit_actually_returns_physical_pages",
            dropped >= (kBigDrive * kCtxCount) / 2,
            "THE case that fails when decommit() stops calling madvise. "
            "D1-D6 all stay green against that mutation because they assert "
            "bookkeeping; this one asserts the memory");
    }
  }

  // -------------------------------------------------------------------
  // D8 — GUARD DISCRIMINATION (lead-mandated, 2026-09-20). The case the
  // frozen spec lacks, and the one that proves the substituted predicate does
  // what the spec's could not.
  //
  // MUST RUN LAST: it constructs the process-wide slot pool, irreversibly, so
  // D0's cold-process premise is only true before this point.
  //
  // The pool is constructed here through dng_decode_in_flight_count(), which
  // calls decodeSlotPool() WITHOUT publishing to g_configured_slots — the same
  // shape as the real decode path at dng_pipeline.cpp:1647/:1803. That is the
  // exact production state the spec's `g_configured_slots != 0` guard cannot
  // see. The pair below is the whole point:
  //
  //   spec predicate  dng_decode_published_slot_count_raw() == 0  -> would SKIP
  //   this predicate  dng_decode_slot_pool_exists()        == true -> FIRES
  //
  // If someone later "simplifies" the guard back to the published count, the
  // second assertion below goes red rather than the deliverable going silently
  // dead.
  // -------------------------------------------------------------------
  {
    // Sanity: nothing so far in this binary may have built the global pool.
    const bool existed_before = dng_decode_slot_pool_exists();

    // Construct via a NON-PUBLISHING path.
    const size_t in_flight = dng_decode_in_flight_count();

    const size_t published_after = dng_decode_published_slot_count_raw();
    const bool exists_after = dng_decode_slot_pool_exists();

    uint64_t calls_before = 0, ctxs_before = 0, physical_after = 0;
    ceyx_debug_dng_slot_residency_counters(nullptr, &calls_before, &ctxs_before,
                                           &physical_after);
    // Fire the one native idle funnel. floor 0 = release every free context.
    ceyx_native_idle_shrink(0);
    uint64_t calls_after = 0, ctxs_after = 0;
    ceyx_debug_dng_slot_residency_counters(nullptr, &calls_after, &ctxs_after,
                                           nullptr);

    std::printf(
        "[DngSlotDecommit] D8 existed_before=%d in_flight=%zu | published_raw="
        "%zu (spec predicate) exists=%d (this predicate) physical_slots=%llu | "
        "decommit_calls %llu -> %llu contexts %llu -> %llu\n",
        (int)existed_before, in_flight, published_after, (int)exists_after,
        (unsigned long long)physical_after, (unsigned long long)calls_before,
        (unsigned long long)calls_after, (unsigned long long)ctxs_before,
        (unsigned long long)ctxs_after);

    CHECK("D8_pool_was_cold_until_this_case", !existed_before,
          "D0's premise must still have held; if the pool already existed, D0 "
          "was not measuring a cold process and D8 is not measuring a "
          "transition");
    CHECK("D8_spec_predicate_would_have_skipped_the_dng_half",
          published_after == 0,
          "the pool is now CONSTRUCTED via a non-publishing path, exactly as "
          "the decode path constructs it. The frozen spec's guard reads this "
          "value and would skip the DNG half forever");
    CHECK("D8_substituted_predicate_fires", exists_after && physical_after > 0,
          "construction-tracking sees the pool the published count cannot; "
          "physical_slots > 0 proves real contexts are now visible to the "
          "idle half");
    CHECK("D8_dng_half_actually_ran",
          calls_after > calls_before && ctxs_after > ctxs_before,
          "the funnel must have reached decommit_free_to_floor and touched "
          "contexts; equal counters here mean the DNG half is wired but inert");
  }

  std::printf("[DngSlotDecommit] TOTAL failures=%d\n", failures);
  std::fflush(stdout);
  return failures == 0 ? 0 : 1;
}
