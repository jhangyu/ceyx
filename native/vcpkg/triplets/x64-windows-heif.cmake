# x64-windows-heif — the triplet Spec §3.2 blocker (3) says does not exist.
#
# Our Windows artefacts are DYNAMIC libraries built against the STATIC CRT
# (build_heif_dist_windows.sh(round6):148-157). Built-in triplets cannot express
# that: x64-windows sets both linkages dynamic, x64-windows-static sets both
# static. This file sets them independently.
#
# It also carries the per-port linkage asymmetry that the LICENCE requires
# (handoff §B, K1/N1): libheif and libde265 are LGPL-3 and MUST ship as
# separately replaceable DLLs; kvazaar (BSD-3) and aom (BSD-2 + AOM patent
# grant) are permissive and are statically merged in. This is deliberate
# licence-driven asymmetry, NOT an inconsistency to "clean up".
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME)

# Release-only. The dist ships no debug artefacts and a debug pass roughly
# doubles wall-clock, which matters under the one-round spike cap.
set(VCPKG_BUILD_TYPE release)

if(PORT STREQUAL "kvazaar" OR PORT STREQUAL "aom")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()

# clang-cl is reproduced from manifest.toml's cmake.windows blocks
# (CMAKE_C_COMPILER/CMAKE_CXX_COMPILER = clang-cl) rather than letting vcpkg
# default to cl.exe. Carrier neutrality: the shipped objects must come from the
# same compiler as today. Passed as configure options rather than via
# VCPKG_CHAINLOAD_TOOLCHAIN_FILE deliberately — chainloading would DISCARD
# scripts/toolchains/windows.cmake, and with it the /MT that that file writes
# literally into CMAKE_C_FLAGS_RELEASE (its line 88-90). That literal /MT is
# what makes pitfall N7 (CMP0091 silently ignored below cmake_minimum 3.15)
# structurally impossible here; do not trade it away.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS
    -DCMAKE_C_COMPILER=clang-cl
    -DCMAKE_CXX_COMPILER=clang-cl
)

# Target-feature flags (K12, layer 2). kvazaar's and libde265's CMakeLists read
# if(MSVC) as "this is cl.exe, which accepts every intrinsic unconditionally";
# clang-cl sets MSVC=1 but is clang underneath and hard-errors without these.
#
# VCPKG_C_FLAGS/VCPKG_CXX_FLAGS are APPENDED by windows.cmake:79-80 to its own
# " /nologo /DWIN32 /D_WINDOWS /utf-8" (and /GR /EHsc for C++). They do NOT
# replace them. Pitfall N16 — the shell script's need to hand-restore
# -DWIN32/-D_WINDOWS/-EHsc/-GR because command-line CMAKE_CXX_FLAGS REPLACE
# CMake's defaults — therefore does not arise on this carrier. That is why the
# lists below are shorter than manifest.toml's cmake.windows CMAKE_C_FLAGS:
# every omitted token is one windows.cmake already supplies. See DEVIATIONS.md.
if(PORT STREQUAL "kvazaar")
    set(VCPKG_C_FLAGS "/clang:-msse4.1 /clang:-mavx2")
    set(VCPKG_CXX_FLAGS "/clang:-msse4.1 /clang:-mavx2")
elseif(PORT STREQUAL "libde265")
    set(VCPKG_C_FLAGS "/clang:-msse4.1")
    set(VCPKG_CXX_FLAGS "/clang:-msse4.1")
elseif(PORT STREQUAL "libheif")
    # N9 / layer 3. -DKVZ_STATIC_LIB belongs on LIBHEIF's compile line, not
    # kvazaar's: kvazaar.h declares its API __declspec(dllimport) unless the
    # CONSUMER defines it, and kvazaar installs no CMake package config to
    # propagate the usage requirement. Without it the link fails with
    # "undefined symbol: __declspec(dllimport) kvz_api_get", and libheif's
    # Findkvazaar.cmake check_symbol_exists probe fails first — which is worse,
    # because that failure is NOT fatal and yields a libheif configured without
    # the kvazaar version string.
    # -W3 is transcribed from manifest.toml's libheif cmake.windows flag lists;
    # windows.cmake supplies /nologo /DWIN32 /D_WINDOWS /utf-8 (+ /GR /EHsc for
    # C++) but not a warning level, so it is restored here.
    set(VCPKG_C_FLAGS "-DKVZ_STATIC_LIB -W3")
    set(VCPKG_CXX_FLAGS "-DKVZ_STATIC_LIB -W3")
endif()
