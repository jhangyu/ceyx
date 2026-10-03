#pragma once

namespace ceyx {

// Post-shrink page return (memory-reclamation campaign M4.1): asks the
// platform allocator to hand free heap pages back to the OS. ONE contract on
// every leg; the platform call is an adapter detail. `unavailable` is an OS
// capability limit (Android below API 28), never "this leg has no mechanism".
enum class HeapPageReturn { ran, unavailable };

HeapPageReturn return_free_heap_pages();

}  // namespace ceyx
