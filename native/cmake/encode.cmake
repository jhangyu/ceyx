# encode.cmake - RGBA8 -> JPEG / WebP encode route (2026-08-30).
#
# JPEG reuses the libjpeg-turbo that third_party.cmake already resolves for the
# DNG SDK, so this file only has to put its headers on dng_decoder_native
# (pipeline.cmake attaches them to dng_sdk PUBLIC, which propagates, but stating
# it here keeps the encode TU independent of that detail).
#
# WebP comes from vcpkg (D5, 2026-08-31): native/vcpkg/vcpkg.json pins libwebp
# to exactly 1.6.0 and the overlay triplets keep it STATIC (only libheif and
# libde265 are dynamic, for the LGPL-3 §4(d)(1) relink duty). It replaces the
# dist that native/scripts/fetch_libwebp_dist.sh used to build, on exactly the
# platforms that script covered — macOS and Linux. Resolution is
# find_package(WebP CONFIG), i.e. the WebPConfig.cmake the vcpkg port installs
# (portfile.cmake: vcpkg_cmake_config_fixup(PACKAGE_NAME WebP ...)), reached
# through the vcpkg toolchain file or CMAKE_PREFIX_PATH.
#
# Static, not dynamic like the heif dist: libwebp is BSD-3 (no LGPL relink
# duty) and third_party.cmake's App-Sandbox rule forbids an absolute
# LC_LOAD_DYLIB into a Homebrew prefix. CONFIG mode is what enforces that now —
# it matches only a package that ships WebPConfig.cmake, so a bare system
# /usr/lib/libwebp.so cannot be picked up silently the way an unqualified
# find_library() would allow.
#
# A MISSING libwebp IS A HARD CONFIGURE ERROR on macOS/Linux/Windows.
# It used to be a soft degradation (warning + CEYX_ENABLE_WEBP=0, so
# ceyx_encode_webp_rgba8 returned kCeyxEncodeErrUnsupported). That behaviour is
# deliberately removed: with acquisition moved to a package manager, "not
# found" no longer means "the optional dist was not fetched", it means the
# dependency wiring is broken — and the old soft path would have turned that
# into a GREEN build silently shipping a binary with no WebP encoder. Opting
# out is still possible, but only EXPLICITLY, via -DCEYX_ENABLE_WEBP=OFF.
# Android keeps the soft path: it never consumed the dist and vcpkg does not
# supply that platform (see the ANDROID branch below).
#
# Included (not add_subdirectory'd) from native/CMakeLists.txt after
# cmake/heif.cmake, so dng_decoder_native already exists as a target.
if(NOT DNG_HOST_GENERATORS_ONLY)

option(CEYX_ENABLE_WEBP
       "Build the WebP encode route (vcpkg-supplied static libwebp 1.6.0)" ON)

if(DNG_USE_LIBJPEG)
    target_include_directories(dng_decoder_native PRIVATE ${JPEG_INCLUDE_DIRS})
endif()

set(CEYX_WEBP_ENABLED 0)
if(CEYX_ENABLE_WEBP)
    # The four archives this route needs, as the vcpkg port's WebPConfig.cmake
    # names them. sharpyuv: libwebp 1.6 factors its YUV conversion out, and
    # omitting it surfaces as undefined _SharpYuvConvert at dylib link time.
    # libwebpmux + webpdemux: EXIF/XMP/ICC embedding needs the mux writer and
    # the demux reader (2026-08-30, plan Task 8). libwebpmux is in the port's
    # default feature set, so no `features` entry in vcpkg.json is required to
    # get it — but it IS required to keep it, which is why the target is
    # asserted below rather than assumed.
    set(CEYX_WEBP_REQUIRED_TARGETS
        WebP::libwebpmux WebP::webpdemux WebP::webp WebP::sharpyuv)

    find_package(WebP CONFIG QUIET)

    set(CEYX_WEBP_MISSING_TARGETS "")
    if(WebP_FOUND)
        foreach(_webp_target IN LISTS CEYX_WEBP_REQUIRED_TARGETS)
            if(NOT TARGET ${_webp_target})
                list(APPEND CEYX_WEBP_MISSING_TARGETS ${_webp_target})
            endif()
        endforeach()
    endif()

    if(WebP_FOUND AND NOT CEYX_WEBP_MISSING_TARGETS)
        # Imported targets carry their own include dirs and their inter-archive
        # dependencies, so neither target_include_directories() nor the manual
        # mux-before-webp link ordering the raw-archive probe needed is
        # required here — CMake derives the static link order from the
        # exported target graph.
        target_link_libraries(dng_decoder_native ${CEYX_WEBP_REQUIRED_TARGETS})
        set(CEYX_WEBP_ENABLED 1)
        message(STATUS "WebP encode+mux: vcpkg WebP ${WebP_VERSION} (${CEYX_WEBP_REQUIRED_TARGETS})")
    elseif(WIN32)
        # Windows keeps the COMMITTED dist (native/third_party/libwebp-dist-windows,
        # Task 6) as its source, and it is NOT a degraded fallback — it is the
        # only acquisition path this platform has ever had.
        # fetch_libwebp_dist.sh (deleted by D5/A5.4) had no Windows branch at
        # all: manifest.toml [component.libwebp].source.default states in so
        # many words that the Windows dist was built once, by hand, by a
        # since-deleted script on a GitHub runner, and that no `source.windows`
        # block exists because there is nothing live to transcribe. So D5's
        # "libwebp moves to vcpkg" applies to exactly the platforms the script
        # covered — macOS and Linux. Moving Windows onto vcpkg means building
        # the whole overlay stack on that runner and is a separate change.
        #
        # Still a HARD error if the committed dist is unusable: absent or
        # partial, the outcome would again be a green build with no WebP
        # encoder.
        set(WEBP_WIN_DIST "${THIRD_PARTY_DIR}/libwebp-dist-windows")
        find_path(CEYX_WEBP_INCLUDE_DIR NAMES webp/encode.h
                  HINTS "${WEBP_WIN_DIST}/include" NO_DEFAULT_PATH)
        # NAMES lists both spellings: Platform/Windows-Clang.cmake:33 sets
        # CMAKE_FIND_LIBRARY_PREFIXES to "lib" and "", so the bare name would
        # already match the lib-prefixed archives the clang-cl dist installs;
        # listing both is belt-and-braces.
        find_library(CEYX_WEBP_LIBRARY      NAMES webp libwebp
                     HINTS "${WEBP_WIN_DIST}/lib" NO_DEFAULT_PATH)
        find_library(CEYX_SHARPYUV_LIBRARY  NAMES sharpyuv libsharpyuv
                     HINTS "${WEBP_WIN_DIST}/lib" NO_DEFAULT_PATH)
        find_library(CEYX_WEBPMUX_LIBRARY   NAMES webpmux libwebpmux
                     HINTS "${WEBP_WIN_DIST}/lib" NO_DEFAULT_PATH)
        find_library(CEYX_WEBPDEMUX_LIBRARY NAMES webpdemux libwebpdemux
                     HINTS "${WEBP_WIN_DIST}/lib" NO_DEFAULT_PATH)
        if(CEYX_WEBP_INCLUDE_DIR AND CEYX_WEBP_LIBRARY AND CEYX_SHARPYUV_LIBRARY
           AND CEYX_WEBPMUX_LIBRARY AND CEYX_WEBPDEMUX_LIBRARY)
            target_include_directories(dng_decoder_native PRIVATE ${CEYX_WEBP_INCLUDE_DIR})
            # Link order matters for raw static archives: mux/demux reference
            # libwebp's symbols, so they precede it.
            target_link_libraries(dng_decoder_native
                ${CEYX_WEBPMUX_LIBRARY} ${CEYX_WEBPDEMUX_LIBRARY}
                ${CEYX_WEBP_LIBRARY} ${CEYX_SHARPYUV_LIBRARY})
            set(CEYX_WEBP_ENABLED 1)
            message(STATUS "WebP encode+mux: committed Windows dist ${CEYX_WEBP_LIBRARY}")
        else()
            message(FATAL_ERROR
                "WebP encode: the committed Windows dist is missing or not mux-capable.\n"
                "  searched under: ${WEBP_WIN_DIST}\n"
                "  webp/encode.h = '${CEYX_WEBP_INCLUDE_DIR}'\n"
                "  libwebp       = '${CEYX_WEBP_LIBRARY}'\n"
                "  libsharpyuv   = '${CEYX_SHARPYUV_LIBRARY}'\n"
                "  libwebpmux    = '${CEYX_WEBPMUX_LIBRARY}'\n"
                "  libwebpdemux  = '${CEYX_WEBPDEMUX_LIBRARY}'\n"
                "Pass -DCEYX_ENABLE_WEBP=OFF to build without the WebP encode route.")
        endif()
    elseif(ANDROID)
        # Android never consumed the libwebp dist and vcpkg does not supply this
        # platform, so this arm preserves the pre-D5 soft degradation for it
        # ALONE: ceyx_encode_webp_rgba8 compiles to kCeyxEncodeErrUnsupported,
        # the symbol stays exported, and a Dart-side lookup still gets a
        # defined error rather than a crash.
        message(WARNING
            "WebP encode disabled on Android (no vcpkg-supplied libwebp). "
            "JPEG encode is unaffected; ceyx_encode_webp_rgba8 will return "
            "kCeyxEncodeErrUnsupported.")
    else()
        # Desktop: HARD failure. See this file's header — after D5 moved
        # acquisition to vcpkg, "not found" means broken wiring, and degrading
        # to a green build that silently ships no WebP encoder is precisely the
        # failure mode this error exists to prevent.
        message(FATAL_ERROR
            "libwebp not found via find_package(WebP CONFIG).\n"
            "  WebP_FOUND        = '${WebP_FOUND}'\n"
            "  WebP_DIR          = '${WebP_DIR}'\n"
            "  missing targets   = '${CEYX_WEBP_MISSING_TARGETS}'\n"
            "  CMAKE_PREFIX_PATH = '${CMAKE_PREFIX_PATH}'\n"
            "libwebp is supplied by vcpkg (native/vcpkg/vcpkg.json pins 1.6.0). "
            "Configure with the vcpkg toolchain file, or add the vcpkg installed "
            "prefix for this triplet to CMAKE_PREFIX_PATH.\n"
            "To build deliberately WITHOUT the WebP encode route, pass "
            "-DCEYX_ENABLE_WEBP=OFF — that is the only supported way to opt out, "
            "and unlike this error it is visible in the configure line.")
    endif()
else()
    message(STATUS "WebP encode: disabled (CEYX_ENABLE_WEBP=OFF)")
endif()

target_compile_definitions(dng_decoder_native PRIVATE
    CEYX_ENABLE_WEBP=${CEYX_WEBP_ENABLED})

# src/ffi/still_ffi_api.cpp routes HEIC/AVIF into src/heif_encode.cpp (plan
# Task 7). Until that TU exists the arms must answer kCeyxStillErrUnsupported
# instead of leaving MapHeifToStillError / ceyx_heif_still_decode_impl
# undefined at dylib link time. Keyed on the file's existence — the same
# advance-registration pattern cmake/tests.cmake:1433 uses for the codec tests —
# so it flips automatically once Task 7 lands and the build is reconfigured.
if(EXISTS "${SRC_DIR}/heif_encode.cpp")
    set(CEYX_HEIF_ENCODE_TU 1)
else()
    set(CEYX_HEIF_ENCODE_TU 0)
    message(STATUS
        "still-decode: HEIC/AVIF arms stubbed (src/heif_encode.cpp absent)")
endif()
target_compile_definitions(dng_decoder_native PRIVATE
    CEYX_HAS_HEIF_STILL_DECODE=${CEYX_HEIF_ENCODE_TU})

# NOTE (deviation from plan Step 6): the plan adds src/webp_codec.cpp and
# src/ffi/still_ffi_api.cpp via target_sources() here. cmake/pipeline.cmake:90
# already collects them with file(GLOB_RECURSE "${SRC_DIR}/*.cpp"), so an
# explicit target_sources() would compile both TUs twice and fail the dylib
# link with duplicate symbols. Nothing to add.

endif() # NOT DNG_HOST_GENERATORS_ONLY
