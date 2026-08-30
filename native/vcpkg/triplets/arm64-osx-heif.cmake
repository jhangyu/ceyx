# arm64-osx-heif — dynamic libheif/libde265 (LGPL-3 §4(d)(1) replaceability),
# static kvazaar/aom (permissive licences). The community triplet
# arm64-osx-dynamic exists but makes EVERY port dynamic, which would break the
# licence-driven asymmetry described in handoff §B (K1/N1).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 11.0)
set(VCPKG_BUILD_TYPE release)

if(PORT STREQUAL "kvazaar" OR PORT STREQUAL "aom")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()
