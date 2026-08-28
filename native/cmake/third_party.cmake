# third_party.cmake - zlib and libjpeg-turbo acquisition
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 52-211)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.
# P14-W0B-07: Android NDK sysroot has libz.so but no CMake config file.
# Probe via find_library/find_path first; synthesize the imported target so
# the rest of the build can use ZLIB::ZLIB uniformly on all platforms.
if(ANDROID)
    find_library(ZLIB_LIBRARY_ANDROID z)
    find_path(ZLIB_INCLUDE_DIR_ANDROID zlib.h)
    if(ZLIB_LIBRARY_ANDROID AND ZLIB_INCLUDE_DIR_ANDROID)
        if(NOT TARGET ZLIB::ZLIB)
            add_library(ZLIB::ZLIB SHARED IMPORTED)
            set_target_properties(ZLIB::ZLIB PROPERTIES
                IMPORTED_LOCATION "${ZLIB_LIBRARY_ANDROID}"
                INTERFACE_INCLUDE_DIRECTORIES "${ZLIB_INCLUDE_DIR_ANDROID}")
        endif()
        set(ZLIB_FOUND TRUE)
        message(STATUS "Found zlib via NDK sysroot: ${ZLIB_LIBRARY_ANDROID}")
    endif()
endif()

# W2a (2026-08-21, Windows port): Windows has no system zlib, so the plain
# find_package(ZLIB REQUIRED) below is a hard configure failure there.
# Acquisition order on Windows:
#   1. -DDNG_ZLIB_ROOT=<prefix> or a vcpkg/CMAKE_PREFIX_PATH-provided config
#      (find_package first — a user-supplied build always wins);
#   2. otherwise fetch and build zlib from source (static, no examples), which
#      keeps the produced DLL free of an external zlib1.dll dependency in the
#      same spirit as the static libjpeg-turbo policy above.
if(WIN32 AND NOT ZLIB_FOUND)
    set(DNG_ZLIB_ROOT "" CACHE PATH
        "zlib install prefix (contains include/zlib.h and lib/); empty = fetch from source")
    if(DNG_ZLIB_ROOT)
        list(APPEND CMAKE_PREFIX_PATH "${DNG_ZLIB_ROOT}")
    endif()
    find_package(ZLIB QUIET)
    if(NOT ZLIB_FOUND)
        include(FetchContent)
        set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(dng_zlib
            URL      https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
            URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
        FetchContent_MakeAvailable(dng_zlib)
        # zlib's own CMake exports `zlibstatic` (and `zlib` for the shared
        # build) but no namespaced target, and its build-tree include dirs are
        # not attached to the target. Supply both so the rest of this file can
        # keep using ZLIB::ZLIB uniformly.
        target_include_directories(zlibstatic PUBLIC
            "${dng_zlib_SOURCE_DIR}" "${dng_zlib_BINARY_DIR}")
        if(NOT TARGET ZLIB::ZLIB)
            add_library(ZLIB::ZLIB ALIAS zlibstatic)
        endif()
        set(ZLIB_FOUND TRUE)
        message(STATUS "Building vendored zlib 1.3.1 from source (Windows)")
    endif()
endif()

if(NOT ZLIB_FOUND)
    find_package(ZLIB REQUIRED)
endif()

# JPEG library for DNG SDK lossy JPEG support (qDNGUseLibJPEG=1).
# Uses standard jpeglib.h API — any libjpeg-compatible library works.
# Android: build vendored libjpeg-turbo from source (NEON SIMD on arm64).
# macOS/host: STATICALLY link libjpeg-turbo (libjpeg.a).
#   Rationale (2026-08-17, P0 sandbox blocker): linking Homebrew's
#   libjpeg.dylib stamps an absolute LC_LOAD_DYLIB entry
#   (/opt/homebrew/opt/jpeg-turbo/lib/libjpeg.8.dylib) into
#   libdng_decoder_native.dylib. Any host app with App Sandbox enabled cannot
#   read /opt/homebrew, so dyld fails the dependency with
#   "file system sandbox blocked open()" and dlopen of our dylib fails outright.
#   Static linking removes the external dependency entirely (option B of
#   docs/logs/2026-08-17/handover-to-dng-decoder-team-libjpeg-blocker.md).
# W2b (2026-08-21, Windows port): Windows has no system libjpeg either, so it
# reuses the Android vendored-source branch. SIMD differs: Android gets NEON
# with no extra tooling, while libjpeg-turbo's x86 SIMD path needs NASM — if
# NASM is absent we fall back to WITH_SIMD=OFF rather than failing configure
# (risk R6; JPEG decode is not on the Halide hot path).
if(ANDROID OR WIN32)
    find_package(JPEG QUIET)
    if(NOT JPEG_FOUND)
        # Build libjpeg-turbo from vendored source.
        # Minimal build: static library only, no turbojpeg API, no tools.
        set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
        set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
        set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
        if(ANDROID)
            set(WITH_SIMD ON CACHE BOOL "" FORCE)
            set(DNG_JPEG_SIMD_NOTE "NEON SIMD")
        else()
            find_program(DNG_NASM_EXECUTABLE NAMES nasm nasmw yasm)
            if(DNG_NASM_EXECUTABLE)
                set(WITH_SIMD ON CACHE BOOL "" FORCE)
                set(DNG_JPEG_SIMD_NOTE "x86 SIMD via ${DNG_NASM_EXECUTABLE}")
            else()
                set(WITH_SIMD OFF CACHE BOOL "" FORCE)
                set(DNG_JPEG_SIMD_NOTE "SIMD OFF - NASM not found")
            endif()
        endif()
        set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "libjpeg-turbo")
        add_subdirectory(${THIRD_PARTY_DIR}/libjpeg-turbo libjpeg-turbo-build EXCLUDE_FROM_ALL)
        # Expose headers (src/ has jpeglib.h etc, binary dir has generated jconfig.h)
        set(JPEG_INCLUDE_DIRS
            ${THIRD_PARTY_DIR}/libjpeg-turbo/src
            ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-build)
        set(JPEG_LIBRARIES jpeg-static)
        message(STATUS "Building vendored libjpeg-turbo 3.1.0 (${DNG_JPEG_SIMD_NOTE})")
    endif()
    set(DNG_USE_LIBJPEG ON)
elseif(APPLE)
    # Locate the static archive. Override with -DDNG_JPEG_TURBO_ROOT=<prefix>
    # if libjpeg-turbo lives outside the probed locations.
    set(DNG_JPEG_TURBO_ROOT "" CACHE PATH
        "libjpeg-turbo install prefix containing lib/libjpeg.a and include/jpeglib.h")
    if(NOT DNG_JPEG_TURBO_ROOT)
        find_program(DNG_BREW_EXECUTABLE brew)
        if(DNG_BREW_EXECUTABLE)
            execute_process(
                COMMAND ${DNG_BREW_EXECUTABLE} --prefix jpeg-turbo
                OUTPUT_VARIABLE DNG_BREW_JPEG_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
                RESULT_VARIABLE DNG_BREW_JPEG_RESULT)
            if(DNG_BREW_JPEG_RESULT EQUAL 0 AND DNG_BREW_JPEG_PREFIX)
                set(DNG_JPEG_TURBO_ROOT "${DNG_BREW_JPEG_PREFIX}" CACHE PATH "" FORCE)
            endif()
        endif()
    endif()

    find_library(DNG_JPEG_STATIC_LIBRARY
        NAMES libjpeg.a
        HINTS "${DNG_JPEG_TURBO_ROOT}/lib"
              /opt/homebrew/opt/jpeg-turbo/lib
              /usr/local/opt/jpeg-turbo/lib
        NO_DEFAULT_PATH)
    find_path(DNG_JPEG_STATIC_INCLUDE_DIR
        NAMES jpeglib.h
        HINTS "${DNG_JPEG_TURBO_ROOT}/include"
              /opt/homebrew/opt/jpeg-turbo/include
              /usr/local/opt/jpeg-turbo/include
        NO_DEFAULT_PATH)

    # --- Cross-arch guard (2026-08-28, macOS CI Intel leg) -------------------
    # The probes above resolve a Homebrew prefix, and Homebrew installs exactly
    # ONE architecture: the host's. On an Apple-silicon runner building
    # -DCMAKE_OSX_ARCHITECTURES=x86_64 (which is how the Intel matrix leg is
    # produced now that GitHub retired the macos-13 Intel image), that yields an
    # arm64-only libjpeg.a and the link fails on an architecture mismatch.
    #
    # Verify the archive actually contains every requested architecture, and if
    # it does not, build the vendored libjpeg-turbo from source for the target
    # arch instead. Static either way, so the App-Sandbox invariant recorded
    # above (no absolute LC_LOAD_DYLIB into /opt/homebrew) still holds.
    set(DNG_JPEG_WANT_ARCHS "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT DNG_JPEG_WANT_ARCHS)
        set(DNG_JPEG_WANT_ARCHS "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(DNG_JPEG_ARCH_OK TRUE)
    if(DNG_JPEG_STATIC_LIBRARY)
        execute_process(COMMAND lipo -archs "${DNG_JPEG_STATIC_LIBRARY}"
                        OUTPUT_VARIABLE DNG_JPEG_HAVE_ARCHS
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE DNG_JPEG_LIPO_RC)
        if(NOT DNG_JPEG_LIPO_RC EQUAL 0)
            # Cannot prove compatibility; keep the probed archive (previous
            # behaviour) rather than discarding a working build over a failed
            # `lipo` invocation.
            set(DNG_JPEG_HAVE_ARCHS "<lipo failed>")
        else()
            foreach(_want IN LISTS DNG_JPEG_WANT_ARCHS)
                if(NOT "${DNG_JPEG_HAVE_ARCHS}" MATCHES "(^| )${_want}( |$)")
                    set(DNG_JPEG_ARCH_OK FALSE)
                endif()
            endforeach()
        endif()
    endif()

    if(DNG_JPEG_STATIC_LIBRARY AND NOT DNG_JPEG_ARCH_OK)
        message(STATUS
            "Probed libjpeg.a has archs '${DNG_JPEG_HAVE_ARCHS}' but this build "
            "targets '${DNG_JPEG_WANT_ARCHS}'; building the vendored "
            "libjpeg-turbo from source instead.")
        set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
        set(ENABLE_STATIC ON CACHE BOOL "" FORCE)
        set(WITH_TURBOJPEG OFF CACHE BOOL "" FORCE)
        # SIMD off: libjpeg-turbo selects its SIMD sources from
        # CMAKE_SYSTEM_PROCESSOR (still the arm64 host here) and needs NASM for
        # the x86 path, so a cross-arch build with SIMD on assembles the wrong
        # instruction set. JPEG decode is not on the Halide hot path — the same
        # trade-off the Windows branch above already documents.
        set(WITH_SIMD OFF CACHE BOOL "" FORCE)
        set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME "libjpeg-turbo")
        add_subdirectory(${THIRD_PARTY_DIR}/libjpeg-turbo libjpeg-turbo-build EXCLUDE_FROM_ALL)
        set(JPEG_INCLUDE_DIRS
            ${THIRD_PARTY_DIR}/libjpeg-turbo/src
            ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-build)
        set(JPEG_LIBRARIES jpeg-static)
        set(DNG_USE_LIBJPEG ON)
        message(STATUS
            "Building vendored libjpeg-turbo 3.1.0 for ${DNG_JPEG_WANT_ARCHS} (SIMD OFF, cross-arch)")
    else()
        # `return()` is deliberately NOT used to skip this block: this file is
        # include()d at the top level of native/CMakeLists.txt, where return()'s
        # meaning depends on policy CMP0140 / the CMake version, and getting it
        # wrong silently abandons the rest of the project configure.
        if(NOT DNG_JPEG_STATIC_LIBRARY OR NOT DNG_JPEG_STATIC_INCLUDE_DIR)
            message(FATAL_ERROR
                "Static libjpeg-turbo not found (need lib/libjpeg.a + include/jpeglib.h).\n"
                "  probed DNG_JPEG_TURBO_ROOT='${DNG_JPEG_TURBO_ROOT}'\n"
                "  libjpeg.a  = '${DNG_JPEG_STATIC_LIBRARY}'\n"
                "  jpeglib.h  = '${DNG_JPEG_STATIC_INCLUDE_DIR}'\n"
                "Install it (`brew install jpeg-turbo`) or pass "
                "-DDNG_JPEG_TURBO_ROOT=<prefix>.\n"
                "Do NOT fall back to the shared libjpeg.dylib: the resulting absolute "
                "dependency makes libdng_decoder_native.dylib unloadable inside "
                "App-Sandboxed host apps.")
        endif()

        set(JPEG_LIBRARIES ${DNG_JPEG_STATIC_LIBRARY})
        set(JPEG_INCLUDE_DIRS ${DNG_JPEG_STATIC_INCLUDE_DIR})
        set(DNG_USE_LIBJPEG ON)
        message(STATUS "Using STATIC libjpeg-turbo: ${DNG_JPEG_STATIC_LIBRARY}")
    endif()
else()
    find_package(JPEG REQUIRED)
    set(DNG_USE_LIBJPEG ON)
endif()
