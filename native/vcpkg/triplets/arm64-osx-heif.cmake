# arm64-osx-heif — dynamic libheif/libde265 (LGPL-3 §4(d)(1) replaceability),
# static kvazaar/aom (permissive licences). The community triplet
# arm64-osx-dynamic exists but makes EVERY port dynamic, which would break the
# licence-driven asymmetry described in handoff §B (K1/N1).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 11.0)
set(VCPKG_BUILD_TYPE release)

# D5 (2026-08-31): stated POSITIVELY — dynamic is the exception, granted only
# to the two LGPL-3 ports. The previous form (default dynamic, static listed for
# kvazaar/aom) was an allowlist whose blind spot made D5's newly added libwebp
# install as a dylib, contradicting manifest.toml's `linkage = "static"` and
# third_party.cmake's App-Sandbox rule against absolute dylib load paths.
# A new port now defaults to static and must be named here to become dynamic.
if(PORT STREQUAL "libheif" OR PORT STREQUAL "libde265")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
