// orient_capability_probe.cpp -- CI build-integrity probe (Task 11 of
// docs/logs/2026-09-07/gpu_orient_productionization_plan.md, user-ordered
// Phase 3 addition, G-E in that plan's gate ledger).
//
// Loads a BUILT libdng_decoder_native (path given as argv[1]) at runtime and
// asserts it exports the fused-orientation Halide AOT kernels: the Stage4
// entries must each carry an "orientation" scalar argument in the
// introspection struct Halide's codegen emits for every AOT filter
// (`<kernel>_metadata()`, declared in HalideRuntime.h as
// `halide_filter_metadata_t`). A pre-fusion build exports the SAME function
// NAMES (dng_render_stage4, dng_render_stage4_scaled_preavg -- the generator
// names never changed), so symbol presence alone cannot distinguish a fused
// library from a stale one; the metadata argument list is the actual
// capability signal this probe checks. This is exactly the failure mode
// R-14 describes: the FFI symbol lookup that consumes these kernels is
// guarded, so one silently-stale/mismatched kernel nulls the whole feature
// group with no crash and no red functional test.
//
// This is a BUILD-INTEGRITY check, not a functional test (plan G-14): it
// never allocates an image buffer, dispatches a kernel over real pixels, or
// reads a sample file. It calls two zero-argument `*_metadata()`
// introspection functions that return a static, compile-time-constant
// struct describing the compiled kernel's argument list -- nothing here
// touches pixel data.
//
// The halide_filter_metadata_t / halide_filter_argument_t layouts are
// duplicated below (not included from HalideRuntime.h) so this probe has NO
// Halide build dependency at all -- it only needs the struct LAYOUT, which
// is a stable, versioned public C ABI Halide guarantees not to break within
// a metadata version (currently always 1; see HalideRuntime.h).
//
// Usage: orient_capability_probe <path-to-libdng_decoder_native.{dylib,so,dll}>
// Prints exactly one of, to stdout, and sets the process exit code to match:
//   PROBE_RESULT=ok                                  (exit 0)
//   PROBE_RESULT=missing:<symbol-or-capability-name>  (exit 1)
//
// G-9/R-16 note: this probe does not use `nm | grep` at all -- it loads the
// library itself and calls into it, so the pipefail-inversion trap that
// hazard names (`nm ... | grep -q PATTERN` reporting a PRESENT symbol as
// failure because grep exits before nm finishes writing, killing nm with
// SIGPIPE) does not apply to this file. The trap does apply to the shell
// wrapper that INVOKES this probe from CI -- see the workflow step comments
// for the local proof that the wrapper avoids it.

#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// Mirrors HalideRuntime.h's halide_type_t / halide_filter_argument_t /
// halide_filter_metadata_t exactly (field order and width), but as a private
// local type so this TU never needs -I into the Halide tree.
struct HalideTypeMini {
  uint8_t code;
  uint8_t bits;
  uint16_t lanes;
};

struct HalideFilterArgumentMini {
  const char *name;
  int32_t kind;
  int32_t dimensions;
  HalideTypeMini type;
  const void *scalar_def;
  const void *scalar_min;
  const void *scalar_max;
  const void *scalar_estimate;
  const int64_t *const *buffer_estimates;
};

struct HalideFilterMetadataMini {
  int32_t version;
  int32_t num_arguments;
  const HalideFilterArgumentMini *arguments;
  const char *target;
  const char *name;
};

using MetadataFn = const HalideFilterMetadataMini *(*)();

struct KernelCheck {
  const char *metadata_symbol;
  const char *kernel_label;
};

// Both are Stage4 AOT entries -- orientation fuses at Stage4 only (Stage3 /
// demosaic-warp is orientation-agnostic; see native/include/dng_pipeline.md
// and Task 1 of the productionization plan). Extend this list if a future
// task adds another fused kernel entry point.
const KernelCheck kChecks[] = {
    {"dng_render_stage4_metadata", "dng_render_stage4"},
    {"dng_render_stage4_scaled_preavg_metadata",
     "dng_render_stage4_scaled_preavg"},
};

bool HasOrientationArg(const HalideFilterMetadataMini *md) {
  if (md == nullptr || md->arguments == nullptr) {
    return false;
  }
  for (int32_t i = 0; i < md->num_arguments; ++i) {
    const char *name = md->arguments[i].name;
    if (name != nullptr && std::strcmp(name, "orientation") == 0) {
      return true;
    }
  }
  return false;
}

#if defined(_WIN32)
using LibHandle = HMODULE;
LibHandle OpenLib(const char *path) { return LoadLibraryA(path); }
void *ResolveSym(LibHandle h, const char *name) {
  return reinterpret_cast<void *>(GetProcAddress(h, name));
}
void CloseLib(LibHandle h) { FreeLibrary(h); }
const char *LastError() {
  static char buf[256];
  std::snprintf(buf, sizeof(buf), "GetLastError=%lu", GetLastError());
  return buf;
}
#else
using LibHandle = void *;
LibHandle OpenLib(const char *path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
void *ResolveSym(LibHandle h, const char *name) { return dlsym(h, name); }
void CloseLib(LibHandle h) { dlclose(h); }
const char *LastError() { return dlerror(); }
#endif

}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "Usage: %s <path-to-libdng_decoder_native>\n", argv[0]);
    std::printf("PROBE_RESULT=missing:argv\n");
    return 1;
  }

  LibHandle handle = OpenLib(argv[1]);
  if (handle == nullptr) {
    const char *err = LastError();
    std::fprintf(stderr, "load failed for %s: %s\n", argv[1],
                 err != nullptr ? err : "(no error string)");
    std::printf("PROBE_RESULT=missing:load\n");
    return 1;
  }

  int rc = 0;
  for (const KernelCheck &check : kChecks) {
    void *sym = ResolveSym(handle, check.metadata_symbol);
    if (sym == nullptr) {
      std::fprintf(stderr, "missing symbol: %s\n", check.metadata_symbol);
      std::printf("PROBE_RESULT=missing:%s\n", check.metadata_symbol);
      rc = 1;
      break;
    }
    MetadataFn fn = reinterpret_cast<MetadataFn>(sym);
    const HalideFilterMetadataMini *md = fn();
    if (!HasOrientationArg(md)) {
      std::fprintf(stderr,
                   "%s is exported but declares no 'orientation' argument "
                   "-- pre-fusion kernel shape\n",
                   check.kernel_label);
      std::printf("PROBE_RESULT=missing:%s.orientation\n", check.kernel_label);
      rc = 1;
      break;
    }
    std::fprintf(stderr, "ok: %s exports 'orientation'\n", check.kernel_label);
  }

  CloseLib(handle);
  if (rc == 0) {
    std::printf("PROBE_RESULT=ok\n");
  }
  return rc;
}
