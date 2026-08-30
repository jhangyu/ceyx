# Packaging-only CMake project: it carries the prebuilt dng_decoder_native.dll
# into the host app's build output directory. There is no plugin class and no
# source is compiled here — `ffiPlugin: true` in pubspec.yaml means Flutter's
# tooling only needs this file to discover the bundled-library path, it does
# not generate any Dart<->C++ registration for it.
#
# Mirrors the macOS (ceyx.podspec, `vendored_libraries`) and
# Android (build.gradle, `jniLibs`) packaging-only siblings in this package.
#
# The Flutter tooling requires CMake 3.14+ (same floor as every other
# `windows/CMakeLists.txt` template Flutter generates for plugins) — do not
# raise this, it would break older Visual Studio toolchains.
cmake_minimum_required(VERSION 3.14)

project(ceyx_library LANGUAGES CXX)

# ---------------------------------------------------------------------------
# CRITICAL: variable name contract
#
# Halcyon's generated windows/flutter/generated_plugins.cmake does:
#
#   foreach(ffi_plugin ${FLUTTER_FFI_PLUGIN_LIST})
#     add_subdirectory(flutter/ephemeral/.plugin_symlinks/${ffi_plugin}/windows plugins/${ffi_plugin})
#     list(APPEND PLUGIN_BUNDLED_LIBRARIES ${${ffi_plugin}_bundled_libraries})
#   endforeach(ffi_plugin)
#
# `${ffi_plugin}` resolves to the pubspec package name `ceyx`, so
# the dereferenced variable name at that call site is literally
# `ceyx_bundled_libraries`. If this file sets any other name (a
# typo, wrong case, missing package prefix), `${ceyx_bundled_libraries}`
# is simply empty in the parent scope — CMake does not error, `flutter build
# windows` succeeds, and the DLL is silently never copied next to the exe.
# The failure only shows up later as a runtime `DynamicLibrary.open` error
# with no compile-time signal pointing back here. Do not rename this
# variable; do not drop PARENT_SCOPE.
# ---------------------------------------------------------------------------

# The DLLs are produced on a Windows build machine (this repo cannot compile or
# link Windows binaries) and obtained either from a ceyx GitHub Release by
# Halcyon's scripts/build_apps.py, or by copying a CI artifact here by hand.
#
# DERIVED, NOT HAND-WRITTEN. Since the HEIF route was enabled,
# dng_decoder_native.dll dynamically imports heif.dll and libde265.dll (LGPL-3
# requires they stay separate shared libraries -- see
# native/third_party/heif-dist-windows/PROVENANCE.md), and a bundle missing
# either one fails at LoadLibrary with an error naming only the decoder. A
# second hand-maintained list of "which DLLs to ship" would be a duplicate that
# drifts from cmake/heif.cmake's staging; globbing what is actually here makes
# the placement step the single decision point.
#
# CONFIGURE_DEPENDS re-runs the glob when the directory changes, so a DLL
# fetched after the first configure is still picked up.
file(GLOB ceyx_bundled_libraries_local CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/*.dll")

# Loud failure, deliberately. The contract comment above explains why an EMPTY
# ceyx_bundled_libraries is the worst outcome available: CMake does not error,
# `flutter build windows` succeeds, and nothing is ever copied next to the exe.
# The failure then surfaces as a runtime DynamicLibrary.open error with no
# compile-time signal pointing back here.
if(NOT ceyx_bundled_libraries_local)
    message(FATAL_ERROR
        "ceyx/plugin/windows: no bundled libraries found in "
        "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/.\n"
        "Expected at least dng_decoder_native.dll plus its heif.dll and "
        "libde265.dll dependencies.\n"
        "Obtain them from the Halcyon checkout with:\n"
        "  python3 scripts/build_apps.py --fetch-native\n"
        "which downloads the pinned ceyx release assets and places all three "
        "here.")
endif()

# Named assertion for the decoder itself: a Libraries/ holding only the two
# HEIF dependencies would pass the emptiness check above and still ship an
# application with no decoder at all.
set(ceyx_have_decoder FALSE)
foreach(_lib IN LISTS ceyx_bundled_libraries_local)
    get_filename_component(_libname "${_lib}" NAME)
    if(_libname STREQUAL "dng_decoder_native.dll")
        set(ceyx_have_decoder TRUE)
    endif()
endforeach()
if(NOT ceyx_have_decoder)
    message(FATAL_ERROR
        "ceyx/plugin/windows: Libraries/ has DLLs but not "
        "dng_decoder_native.dll:\n  ${ceyx_bundled_libraries_local}")
endif()

# Printed on every configure so the shipped set is visible in the build log,
# rather than being an invisible consequence of what happened to be on disk.
# This is also what makes a stale hand-placed library AUDIBLE instead of
# silently winning over the pinned release.
message(STATUS "ceyx: bundling ${ceyx_bundled_libraries_local}")

set(ceyx_bundled_libraries
  ${ceyx_bundled_libraries_local}
  PARENT_SCOPE
)
