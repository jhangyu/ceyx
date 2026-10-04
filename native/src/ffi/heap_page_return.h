#pragma once

#include <cstddef>

namespace ceyx {

// Funnel step 4 (memreclaim spec §4). ONE contract on every leg; the platform
// call is an adapter detail inside heap_page_return.cpp.
//
// 4b: ask the platform allocator to hand free heap pages back to the OS.
// `unavailable` is an OS capability limit (Android below API 28), never "this
// leg has no mechanism".
enum class HeapPageReturn { ran, unavailable };
HeapPageReturn return_free_heap_pages();

// 4a: [base, base+bytes) is a LIVE allocation whose contents are no longer
// needed (an idle pooled slot). Its resident pages go back to the OS; the
// range stays valid and reads undefined data until rewritten. Rounds inward to
// page bounds. Returns the bytes passed to the OS call, 0 on refusal.
// CALLER GUARANTEES nothing reads or writes the range during the call.
size_t discard_idle_pages(void *base, size_t bytes);

// Reuse side of 4a (spec §9.1, macOS MADV_FREE_REUSE ruling): call on the
// decode worker before the first write into a pooled slot that may have been
// discarded. macOS: MADV_FREE_REUSE, without which re-touched
// MADV_FREE_REUSABLE pages are not re-charged to phys_footprint. Every other
// leg: no-op (DiscardVirtualMemory / MADV_DONTNEED ranges need no reuse call).
// Rounds inward to page bounds like discard_idle_pages; never fails loudly.
void prepare_slot_reuse(void *base, size_t bytes);

// Step 5 (Layer B, OQ-1 ruling): hand cold LIVE pages to the OS, contents
// preserved (touching a page restores it). Reports the OS's own answer, never a
// constant `ran`. `unavailable` = OS capability (macOS: no app-callable API,
// user-approved exception; Linux/Android: EINVAL / kernel < 5.4); `refused` =
// the call was made and failed.
enum class ColdHandoff { ran, unavailable, refused };
ColdHandoff handoff_cold_pages();

}  // namespace ceyx
