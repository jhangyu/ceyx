// test_memory_instrument.h -- per-OS reading of the anonymous memory this
// process holds: the quantity VirtualRegion::decommit() must give back.
// Test instrumentation only (outside the parity guard's scope):
//   Windows  PrivateUsage   (commit charge -- the B5 quantity)
//   Apple    phys_footprint (measured: resident_size does not move for madvise)
//   Linux    RssAnon        (/proc/self/status)
#pragma once
#include <cstddef>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <cstdio>
#include <cstring>
#endif

inline size_t process_backing_bytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&pmc),
                               sizeof(pmc))) {
    return 0;
  }
  return static_cast<size_t>(pmc.PrivateUsage);
#elif defined(__APPLE__)
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO,
                reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<size_t>(info.phys_footprint);
#else
  FILE *f = std::fopen("/proc/self/status", "r");
  if (f == nullptr) return 0;
  char line[256];
  size_t kb = 0;
  while (std::fgets(line, sizeof line, f) != nullptr) {
    if (std::strncmp(line, "RssAnon:", 8) == 0) {
      std::sscanf(line + 8, "%zu", &kb);
      break;
    }
  }
  std::fclose(f);
  return kb * 1024;
#endif
}

// Resident pages: what funnel step 4a must drop. Windows reads WorkingSetSize
// (VirtualUnlock removes pages from the working set while the range stays
// committed, so PrivateUsage would not move); elsewhere the backing reading
// already counts resident anonymous pages.
inline size_t process_resident_bytes() {
#if defined(_WIN32)
  PROCESS_MEMORY_COUNTERS pmc{};
  if (!K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
  return static_cast<size_t>(pmc.WorkingSetSize);
#else
  return process_backing_bytes();
#endif
}
