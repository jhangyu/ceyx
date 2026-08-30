# x64-linux-heif — dynamic libheif/libde265 (LGPL-3 §4(d)(1) replaceability),
# static kvazaar/aom (permissive licences). The community triplet
# x64-linux-dynamic exists but makes EVERY port dynamic, which would break the
# licence-driven asymmetry described in handoff §B (K1/N1).
#
# SCOPE NOTE: nothing on Linux links the HEIF stack today —
# .github/workflows/linux_build.yml:113 passes -DDNG_ENABLE_HEIF=OFF. This leg
# was added by user ruling (2026-08-31) to prove the overlay ports build on
# Linux, not because a Linux consumer exists. See DEVIATIONS.md D11.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)

if(PORT STREQUAL "kvazaar" OR PORT STREQUAL "aom")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()

# K15 / handoff §B: kvazaar and aom are static archives linked into a SHARED
# libheif. Without position-independent code the failure appears at link time
# naming libheif, not the archive actually at fault.
#
# The carrier already covers this: vcpkg's scripts/toolchains/linux.cmake:84-85
# appends -fPIC to CMAKE_C_FLAGS_INIT and CMAKE_CXX_FLAGS_INIT unconditionally,
# for every port on this triplet. So this is one more pitfall the overlay-port
# route absorbs structurally (same family as N7 and N16 on Windows).
# ports/kvazaar/portfile.cmake still passes -DCMAKE_POSITION_INDEPENDENT_CODE=ON
# explicitly, deliberately — belt-and-braces, same reasoning as N8: a setting
# that is correct today by virtue of someone else's default is not a setting we
# want to depend on silently.
