// Build-capability observability export (WI-5, platform-parity campaign,
// docs/logs/2026-09-12/platform-parity-plan.md step 5.1).
//
// Answers "what is compiled into THIS artifact" as a queryable, asserted,
// per-platform fact instead of something only discoverable by reading CMake.
// Values are derived from preprocessor state (the same defines the codec/RAW
// routes themselves compile from), not from probing the filesystem at
// runtime -- a green link with a capability ON therefore entails the library
// was actually linked, the same argument codec_capability_probe.py already
// makes for the codec surface (native/scripts/codec_capability_probe.py:23-31).
//
// This surface has no Dart lookupFunction site (WI-5 plan, "Open items for
// lead" (b)): it is asserted by native/scripts/codec_capability_probe.py's
// --expect-cap flag via ceyx_build_capabilities, not by the WI-13 symbol
// manifest. Keep the two mechanisms separate.

#include <cstring>

#if defined(_WIN32)
#define CEYX_FFI_EXPORT __declspec(dllexport)
#else
#define CEYX_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

// CEYX_HAVE_LCMS2 is propagated from LibRaw's own ENABLE_LCMS option by
// native/cmake/tests.cmake (immediately after the libraw-cmake
// add_subdirectory, the only point where its effective value is knowable --
// detail-lcms2-sourcing.md §D.1). Under OQ-N4 option Z it is forced OFF on
// every platform, so this define lands 0 everywhere by one mechanism rather
// than by a platform conditional -- kept anyway as the instrument that would
// catch a future accidental re-enable (an unfalsifiable ICC=0 without it).
#ifndef CEYX_HAVE_LCMS2
#define CEYX_HAVE_LCMS2 0
#endif

#ifndef DNG_ENABLE_HEIF
#define DNG_ENABLE_HEIF 0
#endif

#ifndef CEYX_ENABLE_WEBP
#define CEYX_ENABLE_WEBP 0
#endif

#ifndef CEYX_ENABLE_JXL
#define CEYX_ENABLE_JXL 0
#endif

#ifndef DNG_ENABLE_GENERIC_RAW
#define DNG_ENABLE_GENERIC_RAW 0
#endif

#if defined(_OPENMP)
#define CEYX_HAVE_OPENMP 1
#else
#define CEYX_HAVE_OPENMP 0
#endif

namespace {

// Stable, comma-separated, append-only list -- new capabilities are added to
// the end, never renamed or removed (callers may hold onto ordinal
// assumptions across releases).
constexpr const char *kCapabilityNames =
    "ICC,OPENMP,HEIF,WEBP,JXL,RAW";

}  // namespace

extern "C" {

// Returns 1/0 for a known capability name, -1 for an unknown one. `name`
// must be one of the comma-separated tokens ceyx_build_capability_names()
// returns. "LCMS" is accepted as a synonym for "ICC" (both name the same
// underlying define) since the plan text uses both interchangeably.
CEYX_FFI_EXPORT int ceyx_build_capabilities(const char *name) {
    if (name == nullptr) {
        return -1;
    }
    if (std::strcmp(name, "ICC") == 0 || std::strcmp(name, "LCMS") == 0) {
        return CEYX_HAVE_LCMS2 ? 1 : 0;
    }
    if (std::strcmp(name, "OPENMP") == 0) {
        return CEYX_HAVE_OPENMP ? 1 : 0;
    }
    if (std::strcmp(name, "HEIF") == 0) {
        return DNG_ENABLE_HEIF ? 1 : 0;
    }
    if (std::strcmp(name, "WEBP") == 0) {
        return CEYX_ENABLE_WEBP ? 1 : 0;
    }
    if (std::strcmp(name, "JXL") == 0) {
        return CEYX_ENABLE_JXL ? 1 : 0;
    }
    if (std::strcmp(name, "RAW") == 0) {
        return DNG_ENABLE_GENERIC_RAW ? 1 : 0;
    }
    return -1;
}

CEYX_FFI_EXPORT const char *ceyx_build_capability_names(void) {
    return kCapabilityNames;
}

}  // extern "C"
