#include "heap_page_return.h"

// Any C++ standard header pulls in <features.h> on glibc; __GLIBC__ is
// undefined until then and the platform test below would miss glibc Linux.
#include <cstdint>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__linux__)  // includes Android
#include <cerrno>
#include <cstdio>
#include <cstring>
#endif
#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__ANDROID__)
#include <dlfcn.h>
#elif defined(__linux__) && defined(__GLIBC__)
#include <malloc.h>
#endif

namespace ceyx {

namespace {
uintptr_t page_bytes() {
#if defined(_WIN32)
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return static_cast<uintptr_t>(si.dwPageSize);
#else
  return static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
#endif
}

// Inward page rounding shared by discard and reuse; false when the range
// covers no whole page.
bool whole_pages(void *base, size_t bytes, void **p, size_t *n) {
  if (base == nullptr || bytes == 0) return false;
  const uintptr_t page = page_bytes();
  const uintptr_t start = reinterpret_cast<uintptr_t>(base);
  const uintptr_t lo = (start + page - 1) / page * page;
  const uintptr_t hi = (start + bytes) / page * page;
  if (hi <= lo) return false;
  *p = reinterpret_cast<void *>(lo);
  *n = static_cast<size_t>(hi - lo);
  return true;
}
}  // namespace

void prepare_slot_reuse(void *base, size_t bytes) {
#if defined(__APPLE__)
  void *p = nullptr;
  size_t n = 0;
  if (whole_pages(base, bytes, &p, &n)) (void)madvise(p, n, MADV_FREE_REUSE);
#else
  (void)base;
  (void)bytes;
#endif
}

size_t discard_idle_pages(void *base, size_t bytes) {
  void *p = nullptr;
  size_t n = 0;
  if (!whole_pages(base, bytes, &p, &n)) return 0;
#if defined(_WIN32)
  // DiscardVirtualMemory: contents dropped, range stays committed (MS Learn).
  // VirtualUnlock on an unlocked range "releases the pages from the process's
  // working set" and returns FALSE/ERROR_NOT_LOCKED by design (MS Learn).
  if (DiscardVirtualMemory(p, n) != ERROR_SUCCESS) return 0;
  (void)VirtualUnlock(p, n);
#elif defined(__APPLE__)
  // The only variant measured to drop phys_footprint at once (virtual_region.h).
  if (madvise(p, n, MADV_FREE_REUSABLE) != 0) return 0;
#else
  // Private anonymous pages become zero-fill-on-demand immediately (madvise(2)).
  if (madvise(p, n, MADV_DONTNEED) != 0) return 0;
#endif
  return n;
}

HeapPageReturn return_free_heap_pages() {
#if defined(__APPLE__)
  (void)malloc_zone_pressure_relief(nullptr, 0);
  return HeapPageReturn::ran;
#elif defined(__ANDROID__)
  // minSdk 21 (plugin/android/build.gradle:18); mallopt exists from API 26,
  // M_PURGE from API 28, M_PURGE_ALL from API 34 (bionic malloc.h), so resolve
  // at run time and use the literal values instead of the NDK macros.
  using MalloptFn = int (*)(int, int);
  auto *fn = reinterpret_cast<MalloptFn>(dlsym(RTLD_DEFAULT, "mallopt"));
  if (fn == nullptr) return HeapPageReturn::unavailable;
  constexpr int kPurgeAll = -104;  // M_PURGE_ALL
  constexpr int kPurge = -101;     // M_PURGE
  if (fn(kPurgeAll, 0) == 1) return HeapPageReturn::ran;
  return fn(kPurge, 0) == 1 ? HeapPageReturn::ran : HeapPageReturn::unavailable;
#elif defined(__linux__) && defined(__GLIBC__)
  (void)malloc_trim(0);
  return HeapPageReturn::ran;
#elif defined(_WIN32)
  // The former heap-compaction call is a documented production no-op (MS Learn);
  // HeapOptimizeResources with a NULL heap decommits free
  // memory of every LFH heap in the process (MS Learn, Windows 8.1+).
  HEAP_OPTIMIZE_RESOURCES_INFORMATION info{HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
  return HeapSetInformation(nullptr, HeapOptimizeResources, &info, sizeof info)
             ? HeapPageReturn::ran
             : HeapPageReturn::unavailable;
#else
#error "heap_page_return needs an adapter for this platform"
#endif
}

ColdHandoff handoff_cold_pages() {
#if defined(_WIN32)
  // The exact v1.0.9-v1.0.17 call (577191b^:lib/services/platform/working_set_trim_io.dart):
  // "removes as many pages as possible from the working set" (MS Learn).
  return SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1),
                                  static_cast<SIZE_T>(-1))
             ? ColdHandoff::ran
             : ColdHandoff::refused;
#elif defined(__linux__)
  // MADV_PAGEOUT (kernel >= 5.4) over private, writable, non-exec anonymous
  // ranges; needs swap/zram or the pages stay resident (F10 precondition).
  constexpr int kMadvPageout = 21;
  std::FILE *maps = std::fopen("/proc/self/maps", "r");
  if (maps == nullptr) return ColdHandoff::refused;
  int ok = 0, einval = 0, other = 0;
  char line[512];
  while (std::fgets(line, sizeof line, maps) != nullptr) {
    unsigned long lo = 0, hi = 0, inode = 0;
    char perms[8] = {};
    int path_at = 0;
    if (std::sscanf(line, "%lx-%lx %7s %*s %*s %lu %n", &lo, &hi, perms, &inode, &path_at) < 4) continue;
    if (perms[0] != 'r' || perms[1] != 'w' || perms[2] == 'x' || perms[3] != 'p') continue;
    if (inode != 0) continue;  // file-backed
    const char *path = line + path_at;
    if (path[0] == '[' && std::strncmp(path, "[heap]", 6) != 0 && std::strncmp(path, "[stack", 6) != 0) continue;
    if (madvise(reinterpret_cast<void *>(lo), hi - lo, kMadvPageout) == 0) ++ok;
    else if (errno == EINVAL) ++einval;
    else ++other;  // e.g. ENOMEM: range vanished between read and call
  }
  std::fclose(maps);
  if (ok > 0) return ColdHandoff::ran;
  return (einval > 0 && other == 0) ? ColdHandoff::unavailable : ColdHandoff::refused;
#elif defined(__APPLE__)
  // APPROVED technically-unreachable exception (spec §9 OQ-1): MADV_PAGEOUT is
  // "internal only" (ENOTSUP), VM_BEHAVIOR_PAGEOUT "development only".
  return ColdHandoff::unavailable;
#else
#error "heap_page_return needs an adapter for this platform"
#endif
}

bool cold_handoff_skipped_for_debug() {
#if defined(_WIN32)
  char v[2] = {};
  return GetEnvironmentVariableA("CEYX_DEBUG_SKIP_COLD_HANDOFF", v, sizeof v) == 1 && v[0] == '1';
#else
  const char *v = std::getenv("CEYX_DEBUG_SKIP_COLD_HANDOFF");
  return v != nullptr && v[0] == '1' && v[1] == '\0';
#endif
}

}  // namespace ceyx
