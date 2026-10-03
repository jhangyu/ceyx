#include "heap_page_return.h"

// Any C++ standard header pulls in <features.h> on glibc; __GLIBC__ is
// undefined until then and the platform test below would miss glibc Linux.
#include <cstdlib>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__ANDROID__)
#include <dlfcn.h>
#include <malloc.h>
#elif defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace ceyx {

HeapPageReturn return_free_heap_pages() {
#if defined(__APPLE__)
  (void)malloc_zone_pressure_relief(nullptr, 0);
  return HeapPageReturn::ran;
#elif defined(__ANDROID__)
  // minSdk 21 (plugin/android/build.gradle:18); mallopt exists from API 26 and
  // M_PURGE from API 28, so resolve at run time instead of linking.
  using MalloptFn = int (*)(int, int);
  auto *fn = reinterpret_cast<MalloptFn>(dlsym(RTLD_DEFAULT, "mallopt"));
  if (fn == nullptr) return HeapPageReturn::unavailable;
  return fn(M_PURGE, 0) == 1 ? HeapPageReturn::ran : HeapPageReturn::unavailable;
#elif defined(__linux__) && defined(__GLIBC__)
  (void)malloc_trim(0);
  return HeapPageReturn::ran;
#elif defined(_WIN32)
  // The UCRT heap IS the process heap (VS2015+), which is where
  // ceyx_pool_aligned_alloc's _aligned_malloc blocks live.
  (void)HeapCompact(GetProcessHeap(), 0);
  return HeapPageReturn::ran;
#else
#error "return_free_heap_pages needs an adapter for this platform"
#endif
}

}  // namespace ceyx
