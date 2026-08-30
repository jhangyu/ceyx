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
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)
set(VCPKG_BUILD_TYPE release)

# D5 (2026-08-31): the rule is stated POSITIVELY — dynamic is the exception,
# granted only to the two LGPL-3 ports that require separate replaceability.
# It used to read the other way round (default dynamic, static listed for
# kvazaar/aom). That form was an allowlist with a blind spot: libwebp, added to
# this manifest by D5, was in neither list and silently installed as a DYLIB,
# contradicting manifest.toml's `linkage = "static"` and third_party.cmake's
# App-Sandbox rule against absolute dylib load paths. Caught by inspecting the
# installed artefact (docs/logs/2026-08-31/verify/d5-libwebp-install.txt), not
# by any assertion — hence the assertion added alongside this fix.
# In this form a newly added port defaults to the permissive-licence answer and
# must be named explicitly to become dynamic.
if(PORT STREQUAL "libheif" OR PORT STREQUAL "libde265")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
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
