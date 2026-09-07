// orient_capability_probe.cpp -- CI build-integrity probe (Task 11 of
// docs/logs/2026-09-07/gpu_orient_productionization_plan.md, user-ordered
// Phase 3 addition, G-E in that plan's gate ledger).
//
// Loads a BUILT libdng_decoder_native (path given as argv[1]) at runtime and
// asserts it exports the fused-orientation Halide AOT kernels: the Stage4
// entries must each carry the six host-computed affine-coefficient scalar
// arguments (orient_a_x, orient_b_x, orient_c_x, orient_a_y, orient_b_y,
// orient_c_y -- T7b's "kernel is pure multiply-add" formulation, bac3cbe) in
// the introspection struct Halide's codegen emits for every AOT filter
// (`<kernel>_metadata()`, declared in HalideRuntime.h as
// `halide_filter_metadata_t`). A pre-fusion build exports the SAME function
// NAMES (dng_render_stage4, and either dng_render_stage4_scaled_preavg on
// Metal or dng_render_stage4_split on Vulkan -- the generator names never
// changed), so symbol presence alone cannot distinguish a fused library from
// a stale one; the metadata argument list is the actual capability signal
// this probe checks. This is exactly the failure mode R-14 describes: the
// FFI symbol lookup that consumes these kernels is guarded, so one
// silently-stale/mismatched kernel nulls the whole feature group with no
// crash and no red functional test.
//
// LOCKSTEP NOTE: this argument-name list is coupled to the fused kernels'
// signature by construction (that coupling IS the capability signal — see
// above). Any future task that renames/reshapes these scalar arguments MUST
// update kRequiredArgs below (and android_build.yml's `strings` literal,
// currently orient_a_x) in the SAME change. This file has already been
// updated once for exactly this reason: T7b (bac3cbe) replaced the original
// single "orientation" + unoriented_width/unoriented_height scalars with the
// six affine coefficients checked below.
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
//
// PLATFORM-AWARE SECOND ENTRY (fix, Round 3): dng_render_stage4 itself is
// linked into dng_decoder_native on every platform (native/cmake/ffi.cmake),
// but the SECOND Stage4 AOT kernel differs by platform, gated by CMake's
// DNG_STAGE4_SPLIT_KERNEL (native/cmake/halide_aot.cmake, native/cmake/ffi.cmake):
//   - Metal (macOS, DNG_STAGE4_SPLIT_KERNEL=OFF): dng_render_stage4_scaled_preavg
//     is linked; dng_render_stage4_split is never built at all on this branch.
//   - Vulkan (Linux/Windows/Android, DNG_STAGE4_SPLIT_KERNEL=ON):
//     dng_render_stage4_split is linked instead; scaled_preavg is never built
//     on this branch (sized/"scaled" decode requests fall back to CPU resample
//     by design on the split branch -- productionization plan R1d). Both
//     kernels declare the same six orient_* affine-coefficient Input<>
//     scalars (see DngRenderStage4ScaledPreAvg / DngRenderStage4Android in
//     native/generators/DngRenderGenerator.cpp), so the required-args check
//     below is identical either way -- only WHICH kernel's metadata symbol
//     gets checked changes. ORIENT_PROBE_SPLIT_KERNEL is defined by
//     native/cmake/tests.cmake from the SAME DNG_STAGE4_SPLIT_KERNEL variable
//     CMake used to decide which kernel got linked, so this cannot drift out
//     of lockstep with the actual build the way a hand-maintained duplicate
//     flag could.
#if defined(ORIENT_PROBE_SPLIT_KERNEL)
const KernelCheck kChecks[] = {
    {"dng_render_stage4_metadata", "dng_render_stage4"},
    {"dng_render_stage4_split_metadata", "dng_render_stage4_split"},
};
#else
const KernelCheck kChecks[] = {
    {"dng_render_stage4_metadata", "dng_render_stage4"},
    {"dng_render_stage4_scaled_preavg_metadata",
     "dng_render_stage4_scaled_preavg"},
};
#endif

// T7b (bac3cbe): the fused kernels take six host-computed affine
// coefficients instead of a single orientation code + unoriented extent.
// All six must be present; this is a stronger check than the single-name
// check it replaces (a build missing just one coefficient would silently
// mis-warp, not merely mis-report an extent).
const char *const kRequiredArgs[] = {
    "orient_a_x", "orient_b_x", "orient_c_x",
    "orient_a_y", "orient_b_y", "orient_c_y",
};

// Returns the first required argument name NOT found in md's argument list,
// or nullptr if all are present.
const char *FirstMissingRequiredArg(const HalideFilterMetadataMini *md) {
  if (md == nullptr || md->arguments == nullptr) {
    return kRequiredArgs[0];
  }
  for (const char *required : kRequiredArgs) {
    bool found = false;
    for (int32_t i = 0; i < md->num_arguments; ++i) {
      const char *name = md->arguments[i].name;
      if (name != nullptr && std::strcmp(name, required) == 0) {
        found = true;
        break;
      }
    }
    if (!found) {
      return required;
    }
  }
  return nullptr;
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
    const char *missing_arg = FirstMissingRequiredArg(md);
    if (missing_arg != nullptr) {
      std::fprintf(stderr,
                   "%s is exported but declares no '%s' argument -- "
                   "pre-T7b or otherwise stale kernel shape\n",
                   check.kernel_label, missing_arg);
      std::printf("PROBE_RESULT=missing:%s.%s\n", check.kernel_label, missing_arg);
      rc = 1;
      break;
    }
    std::fprintf(stderr, "ok: %s exports all six affine-coefficient args\n",
                 check.kernel_label);
  }

  CloseLib(handle);
  if (rc == 0) {
    std::printf("PROBE_RESULT=ok\n");
  }
  return rc;
}
