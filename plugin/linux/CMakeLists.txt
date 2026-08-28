# Packaging-only CMake project: it carries the prebuilt
# libdng_decoder_native.so into the host app's build output directory. There is
# no plugin class and no source is compiled here — `ffiPlugin: true` in
# pubspec.yaml means Flutter's tooling only needs this file to discover the
# bundled-library path, it does not generate any Dart<->C++ registration for it.
#
# Mirrors the packaging-only siblings in this package: windows/CMakeLists.txt
# (`ceyx_bundled_libraries`), macOS (ceyx.podspec, `vendored_libraries`) and
# Android (build.gradle, `jniLibs`).
#
# The Flutter tooling requires CMake 3.14+ (same floor as every other
# `linux/CMakeLists.txt` template Flutter generates for plugins) — do not raise
# this.
cmake_minimum_required(VERSION 3.14)

project(ceyx_library LANGUAGES CXX)

# ---------------------------------------------------------------------------
# CRITICAL: variable name contract
#
# Halcyon's generated linux/flutter/generated_plugins.cmake does:
#
#   foreach(ffi_plugin ${FLUTTER_FFI_PLUGIN_LIST})
#     add_subdirectory(flutter/ephemeral/.plugin_symlinks/${ffi_plugin}/linux plugins/${ffi_plugin})
#     list(APPEND PLUGIN_BUNDLED_LIBRARIES ${${ffi_plugin}_bundled_libraries})
#   endforeach(ffi_plugin)
#
# `${ffi_plugin}` resolves to the pubspec package name `ceyx`, so the
# dereferenced variable name at that call site is literally
# `ceyx_bundled_libraries`. If this file sets any other name (a typo, wrong
# case, missing package prefix), `${ceyx_bundled_libraries}` is simply empty in
# the parent scope — CMake does not error, `flutter build linux` succeeds, and
# the .so is silently never copied next to the executable. The failure only
# shows up later as a runtime `DynamicLibrary.open` error with no compile-time
# signal pointing back here. Do not rename this variable; do not drop
# PARENT_SCOPE.
# ---------------------------------------------------------------------------

# The .so is produced by the Linux Build (Vulkan native) workflow, which writes
# native/build-linux/libdng_decoder_native.so, and is committed here afterwards.
# Until it is committed, Libraries/ holds only .gitkeep. That is deliberate and
# it fails LOUDLY, not silently: Halcyon's linux/CMakeLists.txt runs
# `install(FILES "${bundled_library}" ...)` over PLUGIN_BUNDLED_LIBRARIES, which
# errors on a missing file at install time.
#
# ORDERING CONTRACT: `linux: ffiPlugin: true` in pubspec.yaml and the committed
# .so must reach the default branch TOGETHER. Landing the platform declaration
# first turns every consuming `flutter build linux` red until the binary
# follows.
set(ceyx_bundled_libraries
  "${CMAKE_CURRENT_SOURCE_DIR}/Libraries/libdng_decoder_native.so"
  PARENT_SCOPE
)
