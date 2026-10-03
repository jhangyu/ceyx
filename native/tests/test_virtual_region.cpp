// test_virtual_region.cpp -- memory-reclamation campaign M5 (PARITY.md clause 1).
// One reserve / commit-on-grow / decommit contract, checked identically on
// every leg with the per-OS instrument in test_memory_instrument.h.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "test_memory_instrument.h"
#include "test_report.h"
#include "virtual_region.h"

namespace {
constexpr const char kReportPrefix[] = "VirtualRegion";
constexpr size_t kMiB = size_t{1024} * 1024;
constexpr size_t kReserve = 1024 * kMiB;
constexpr size_t kDrive = 256 * kMiB;

void report(const char *name, bool ok, const char *detail) {
  test_report::report(kReportPrefix, name, ok, detail);
}
}  // namespace

int main() {
  char detail[160];
  const size_t start = process_backing_bytes();
  VirtualRegion region(kReserve);
  const size_t after_reserve = process_backing_bytes();
  std::snprintf(detail, sizeof detail, "start=%zu after_reserve=%zu", start, after_reserve);
  report("V1_reserve_holds_no_backing",
         region.base() != nullptr && region.committed_bytes() == 0 &&
             after_reserve < start + 16 * kMiB,
         detail);

  const bool committed = region.ensure_committed(kDrive);
  if (committed) std::memset(region.base(), 0x5A, kDrive);
  const size_t driven = process_backing_bytes();
  std::snprintf(detail, sizeof detail, "driven=%zu", driven);
  report("V2_commit_then_touch_raises_backing",
         committed && driven >= after_reserve + kDrive - 16 * kMiB, detail);

  region.decommit();
  const size_t after_decommit = process_backing_bytes();
  std::snprintf(detail, sizeof detail, "after_decommit=%zu", after_decommit);
  report("V3_decommit_returns_backing",
         region.committed_bytes() == 0 &&
             driven - after_decommit >= (kDrive / 10) * 9,
         detail);

  const bool recommitted = region.ensure_committed(kDrive);
  bool roundtrip = false;
  if (recommitted) {
    region.base()[kDrive - 1] = 0x7E;
    roundtrip = region.base()[kDrive - 1] == 0x7E;
  }
  report("V4_recommit_after_decommit_is_usable", recommitted && roundtrip, "");

  report("V5_commit_past_capacity_is_refused", !region.ensure_committed(kReserve + 1), "");
  return test_report::finish(kReportPrefix);
}
