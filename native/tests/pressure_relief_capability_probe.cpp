// pressure_relief_capability_probe.cpp -- win-parity plan P1 (D1), AC1.
//
// Loads a library PATH given on argv[1] at runtime (dlopen on POSIX,
// LoadLibraryA on Windows) and calls ceyx_pool_pressure_relief() through it.
// Deliberately NOT linked against dng_decoder_native, exactly like
// orient_capability_probe.cpp: the same binary must be able to probe either a
// freshly built library or an older fixture (negative control) by path.
//
// Prints one machine-readable line and exits 0 only when the relief result
// satisfies the contract's ">= 0" half (kCeyxPressureReliefUnsupported == -1
// is a FAILURE here, by design of this probe).
#include <cstdint>
#include <cstdio>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

int main(int argc, char **argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: probe <library-path>\n"); return 2; }
#if defined(_WIN32)
  HMODULE h = LoadLibraryA(argv[1]);
  void *sym = h ? reinterpret_cast<void *>(GetProcAddress(h, "ceyx_pool_pressure_relief")) : nullptr;
#else
  void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  void *sym = h ? dlsym(h, "ceyx_pool_pressure_relief") : nullptr;
#endif
  if (!h)   { std::printf("PRESSURE_RELIEF_PROBE lib=%s LOAD=FAIL\n", argv[1]);     return 3; }
  if (!sym) { std::printf("PRESSURE_RELIEF_PROBE lib=%s SYMBOL=ABSENT\n", argv[1]); return 4; }
  const int64_t rv = reinterpret_cast<int64_t (*)()>(sym)();
  std::printf("PRESSURE_RELIEF_PROBE lib=%s RESULT=%lld\n", argv[1], static_cast<long long>(rv));
  return rv >= 0 ? 0 : 1;   // -1 (kCeyxPressureReliefUnsupported) is a FAILURE here
}
