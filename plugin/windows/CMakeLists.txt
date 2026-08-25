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

# The DLL itself is produced on a Windows build machine (W9/W12 in
# docs/logs/2026-08-21/windows-ffi-upgrade-findings.md — this repo cannot
# compile or link Windows binaries) and committed here afterwards. At
# authoring time on macOS this file does not exist yet; that is expected and
# does not break `set()` below, since CMake only resolves the path string,
# it does not require the file to exist until something tries to install/copy
# it. The consuming install(FILES ...) step in Halcyon's windows/CMakeLists.txt
# will fail loudly (missing file) if a Windows build is attempted before W12
# lands — that is the intended, non-silent failure mode once this variable is
# wired correctly.
set(ceyx_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/dng_decoder_native.dll"
  PARENT_SCOPE
)
