cmake_minimum_required(VERSION 3.14)
# W3 (2026-08-21, Windows port): opt in to CMAKE_MSVC_RUNTIME_LIBRARY so the
# `windows-vulkan` preset can actually select the static CRT (/MT) — with the
# 3.14 minimum this policy would default to OLD and the setting would be a
# silent no-op. The policy only governs MSVC runtime flag selection, so it has
# no effect on the macOS or Android builds. Must precede project().
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()
project(dng_decoder_native VERSION 1.0.0 LANGUAGES CXX C)

# Perf fix (2026-07-04): default to Release when the caller (script or IDE)
# does not set CMAKE_BUILD_TYPE. Without this, both the macOS host configure
# and the Android two-stage cross-compile in build_native_watchdog.py silently
# produced unoptimized, assert-enabled binaries (no -O flag, no -DNDEBUG).
# Explicit -DCMAKE_BUILD_TYPE=... from a caller (e.g. --cmake-arg) still wins.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (default: Release)" FORCE)
endif()

# W7-2 (TD-4): Halide vendor version pin.
# See third_party/halide/VERSION for full provenance and ABI notes.
# Changing Halide version requires full AOT artifact regen (schedule ABI is not stable).
message(STATUS "Halide vendored: 21.0.0 (see third_party/halide/VERSION)")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(INC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

if(APPLE)
    add_compile_definitions(qMacOS=1 qWinOS=0)
elseif(WIN32)
    add_compile_definitions(qMacOS=0 qWinOS=1)
    # W3 (2026-08-21, Windows port, risk R4): four dng_sdk translation units
    # include <windows.h> once qWinOS=1 (dng_pthread.cpp, dng_date_time.cpp,
    # dng_string.cpp, dng_utils.cpp). Its unqualified min/max macros collide
    # with <algorithm> and with the SDK's own Min_*/Max_* helpers, and the
    # full header set drags in GDI's ERROR macro. Both are predefined
    # project-wide rather than per-target so every TU (dng_sdk,
    # dng_decoder_native, tests) sees the same windows.h shape.
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
elseif(ANDROID)
    add_compile_definitions(qAndroid=1 qMacOS=0 qWinOS=0)
elseif(UNIX)
    add_compile_definitions(qLinux=1 qMacOS=0 qWinOS=0)
endif()

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
else()
    find_package(JPEG REQUIRED)
    set(DNG_USE_LIBJPEG ON)
endif()

# =============================================================================
# Options
# =============================================================================
option(DNG_DEMOSAIC_WARP_FAST_CODEGEN
       "Use fast 8x8 GPU schedule for fused demosaic+warp AOT; OFF uses 16x16 + channel unroll schedule"
       ON)
option(DNG_DEMOSAIC_WARP_STRICT_FLOAT
       "Append -strict_float to dng_demosaic_warp AOT target (Vulkan FP contraction probe); default OFF = identical to previous behaviour"
       OFF)


option(DNG_DIAGNOSTIC_BUILD
       "Build deprecated research generators (RectilinearWarpStrictFloatGenerator, RectilinearWarpDebugGenerator) for diagnostics"
       OFF)

# =============================================================================
# P17 T1: Generic RAW frontend (LibRaw + bundled RawSpeed3)
# =============================================================================
# Option declared here (with the other early options); the actual
# add_subdirectory/target wiring is deferred to just above the libraw_smoke
# target definition, AFTER the Halide find_package() below. RawSpeed3's own
# CMakeLists.txt mutates several global CACHE variables (WITH_OPENMP,
# CMAKE_POLICY_VERSION_MINIMUM, etc.) as a side effect of its own configure;
# doing that before find_package(Halide REQUIRED COMPONENTS Halide) was
# observed to break the AOT generator executables' link against the system
# zlib bundled inside Halide's compression support (undefined _uncompress).
# Deferring the vendored subdirectories until after Halide is fully
# configured avoids that ordering hazard without patching RawSpeed3 itself.
option(DNG_ENABLE_GENERIC_RAW
       "Build the generic (non-DNG) RAW frontend backed by LibRaw + RawSpeed3" ON)

# --- Phase 14: Cross-compile and AOT target options ---
option(DNG_HOST_GENERATORS_ONLY
       "Only build host-side Halide generators and run AOT (Stage 1 of cross-compile)"
       OFF)

option(DNG_CROSS_BUILD
       "Cross-compile mode: skip generator compilation, use prebuilt AOT artifacts"
       OFF)

set(DNG_PREBUILT_AOT_DIR "" CACHE PATH
    "Path to prebuilt AOT artifacts (used when DNG_CROSS_BUILD=ON)")

set(DNG_AOT_TARGET_OVERRIDE "" CACHE STRING
    "Override AOT target string (e.g. arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query)")

# R3-3: Halide Vulkan runtime fork with VkPipelineCache cross-launch
# persistence (Android only). OFF = stock runtime, feature cleanly absent.
# See native/halide_runtime_fork/README.md.
option(DNG_VK_PIPELINE_CACHE
       "R3-3: fork the Halide Vulkan runtime to persist VkPipelineCache across launches (Android only; OFF = stock runtime)"
       ON)

# Phase 14: Force diagnostic options OFF on Android (no Metal backend)
if(ANDROID)
    set(DNG_DIAGNOSTIC_BUILD OFF CACHE BOOL "" FORCE)
endif()

# =============================================================================
# Phase 14: dng_sdk, dng_decoder_native, and test targets are skipped when
# building only host-side generators (Stage 1 of cross-compile).
# =============================================================================
if(NOT DNG_HOST_GENERATORS_ONLY)

# Adobe DNG SDK
set(DNG_SDK_DIR ${THIRD_PARTY_DIR}/dng_sdk/source)
file(GLOB_RECURSE DNG_SOURCES "${DNG_SDK_DIR}/*.cpp")
list(REMOVE_ITEM DNG_SOURCES "${DNG_SDK_DIR}/dng_xmp_sdk.cpp" "${DNG_SDK_DIR}/dng_xmp.cpp")

# Halide-accelerated bridge implementations (Stage3 demosaic, warp, Stage4 render).
    # W6 H-3: bridge .cpp live in src/ (auto-discovered by glob for dng_decoder_native;
    # explicitly listed for standalone test targets).
    # Production/test_decode call the AOT entry directly and fail fast if GPU is unsupported.

# Inject our XMP stubs into the base DNG SDK library so its internal references resolve
list(APPEND DNG_SOURCES "${SRC_DIR}/dng_xmp_stub.cpp")

add_library(dng_sdk STATIC ${DNG_SOURCES})
# Route A adoption (P16, 2026-06-12): pin the SDK render reference to IEEE stepwise.
# The Stage4 99.62 dB ±1 LSB residual is solely the default clang -ffp-contract=on
# FMA grouping in the SDK render math (P2-1 forensics). The render bottlenecks
# (RefBaseline* in dng_reference.cpp + the inline dng_1d_table::Interpolate lerp +
# matrix/spline/huesat helpers) are *inline-shared across many TUs*; per-source
# COMPILE_OPTIONS leaves a residual 98 px ±1 LSB because COMDAT folding can still
# bind a call to a contract=on copy emitted by an un-pinned TU. The robust fix that
# reproduces the P2-1 private-build (-ffp-contract=off whole libdng_sdk → byte-exact
# vs Halide strict, 0 diff) is a library-wide option. Stage1/2/3 outputs quantize to
# uint16 and are empirically unaffected (PSNR 999 + Stage3 SHA unchanged — see
# Task_P16_routeA_adoption.md). The Halide Metal strict_float Stage4 kernel is the
# matching GPU side; together Stage4-isolated is byte-exact.
# W3 (2026-08-21, Windows port, risk R3): the flag above is load-bearing, not a
# style choice, so the Windows toolchain is *chosen* by it rather than the other
# way round. clang-cl accepts the clang driver spelling when it is routed
# through /clang:; MSVC's cl.exe has no equivalent (/fp:strict is a different,
# stronger-but-not-identical guarantee and has never been validated against the
# byte-exact Stage4 reference), so cl.exe is rejected at configure time instead
# of silently producing a different colour pipeline. Override deliberately with
# -DDNG_ALLOW_MSVC_FP_CONTRACT=ON if you are re-baselining the colour gate.
option(DNG_ALLOW_MSVC_FP_CONTRACT
       "Allow building with MSVC cl.exe despite the missing -ffp-contract=off equivalent (colour byte-exactness is then UNVERIFIED)"
       OFF)
if(MSVC AND CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(dng_sdk PRIVATE /clang:-ffp-contract=off)
    elseif(DNG_ALLOW_MSVC_FP_CONTRACT)
        message(WARNING "Building dng_sdk with cl.exe: -ffp-contract=off has no "
                        "equivalent, Stage4 colour byte-exactness is UNVERIFIED.")
        target_compile_options(dng_sdk PRIVATE /fp:strict)
    else()
        message(FATAL_ERROR
            "dng_sdk requires -ffp-contract=off (see the Route A note above); "
            "cl.exe does not support it. Configure with clang-cl, e.g. the "
            "'windows-vulkan' preset (-DCMAKE_CXX_COMPILER=clang-cl), or set "
            "-DDNG_ALLOW_MSVC_FP_CONTRACT=ON to accept an unverified colour path.")
    endif()
else()
    target_compile_options(dng_sdk PRIVATE -ffp-contract=off)
endif()
target_include_directories(dng_sdk PUBLIC ${DNG_SDK_DIR} ${INC_DIR})
if(DNG_USE_LIBJPEG)
    target_compile_definitions(dng_sdk PUBLIC qDNGThreadSafe=1 qDNGXMPDocOps=0 qDNGXMPFiles=0 qDNGUseLibJPEG=1)
else()
    target_compile_definitions(dng_sdk PUBLIC qDNGThreadSafe=1 qDNGXMPDocOps=0 qDNGXMPFiles=0 qDNGUseLibJPEG=0)
endif()
if(ANDROID)
    target_compile_definitions(dng_sdk PUBLIC qDNG64Bit=1)
endif()
if(DNG_USE_LIBJPEG)
    target_include_directories(dng_sdk PUBLIC ${JPEG_INCLUDE_DIRS})
    target_link_libraries(dng_sdk ZLIB::ZLIB ${JPEG_LIBRARIES})
else()
    target_link_libraries(dng_sdk ZLIB::ZLIB)
endif()

# Native library
file(GLOB_RECURSE NATIVE_SOURCES "${SRC_DIR}/*.cpp")
# W7-4 (TD-17): Exclude entire research/ subdirectory from production dylib.
# Complements the Generator filter below — any new WarpUtils.cpp or helper
# placed under src/research/ is automatically excluded without updating regex.
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/research/.*")
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/Dng[A-Z][A-Za-z0-9]*Generator\\.cpp$")
# P17 T9: the generic-RAW Halide generators are named Raw*Generator.cpp, which
# the Dng-prefixed filter above does not match. Without this they get swept
# into dng_decoder_native by the GLOB_RECURSE and fail to compile (Halide
# Generator API + Halide.h are generator-only).
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/Raw[A-Z][A-Za-z0-9]*Generator\\.cpp$")
# P17 R5 (F1): the CPU demosaic oracle is test-only. The kernel tests compile
# it themselves; keeping it in the dylib exported the reference symbols from
# the production ABI.
list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/raw_demosaic_reference\\.cpp$")
# P17 T6: src/libraw_frontend.cpp includes libraw/libraw.h, which is only on an
# include path when DNG_ENABLE_GENERIC_RAW is ON (libraw_vendored, below).
# The GLOB_RECURSE above would otherwise sweep it into dng_decoder_native in
# OFF builds too and break them.
if(NOT DNG_ENABLE_GENERIC_RAW)
    list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/libraw_(frontend|gpu_input_adapter)\\.cpp$")
    # P17 T10: the generic RAW route and its C ABI call into the frontend TUs
    # filtered out above, so they must drop out of OFF builds together.
    list(FILTER NATIVE_SOURCES EXCLUDE REGEX ".*/raw_(gpu_pipeline|ffi_api)\\.cpp$")
endif()
# W6 H-3: bridge .cpp moved from dng_sdk_custom/source/ into src/;
# now auto-discovered by file(GLOB_RECURSE) above.
# Only add target if there are sources, dummy for now to avoid target creation error
if (NOT NATIVE_SOURCES)
    file(WRITE ${SRC_DIR}/dummy.cpp "void dummy(){}")
    set(NATIVE_SOURCES ${SRC_DIR}/dummy.cpp)
endif()

add_library(dng_decoder_native SHARED ${NATIVE_SOURCES})
target_include_directories(dng_decoder_native PUBLIC
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR})
if(DNG_USE_LIBJPEG)
    target_link_libraries(dng_decoder_native dng_sdk ${JPEG_LIBRARIES})
else()
    target_link_libraries(dng_decoder_native dng_sdk)
endif()

endif() # NOT DNG_HOST_GENERATORS_ONLY

# =============================================================================
# Halide: generators, AOT commands, and linking
# =============================================================================
set(HALIDE_DIR ${THIRD_PARTY_DIR}/halide)

# Phase 14: In cross-build mode, skip Halide find_package and all generator targets.
# We only need Halide headers for AOT-generated .h includes.
if(NOT DNG_CROSS_BUILD)
    list(APPEND CMAKE_PREFIX_PATH ${HALIDE_DIR})
    # R2 fix (F4, round-1 review, second half): find_package(ZLIB), called
    # from inside the RawSpeed3 subdirectory further below (deliberately
    # placed after this find_package(Halide) call within THIS configure),
    # always caches its find results (ZLIB_INCLUDE_DIR, ZLIB_LIBRARY_DEBUG,
    # ZLIB_LIBRARY_RELEASE, ...) -- find_package() caching is unconditional
    # CMake behavior, unrelated to the CACHE/FORCE choices made further
    # below in the generic-RAW block. On the *next* configure, those
    # entries are already present in CMakeCache.txt when find_package(Halide)
    # runs here, i.e. BEFORE RawSpeed3 gets to run again -- reproducing the
    # exact undefined `_uncompress` AOT-generator link failure that placing
    # RawSpeed3's add_subdirectory() after this find_package(Halide) call
    # was meant to avoid (see comment above; mechanically reproduced this
    # round via a reconfigure-in-place proof). Force a clean zlib cache
    # slate immediately before every find_package(Halide) call, fresh
    # configure or not, so Halide always resolves zlib the same way a
    # fresh build dir would.
    foreach(_dng_zlib_cache_var
            ZLIB_INCLUDE_DIR ZLIB_LIBRARY ZLIB_LIBRARY_DEBUG ZLIB_LIBRARY_RELEASE
            ZLIB_FOUND FIND_PACKAGE_MESSAGE_DETAILS_ZLIB)
        unset(${_dng_zlib_cache_var} CACHE)
    endforeach()
    # COMPONENTS Halide (not the bare form): HalideConfig.cmake defaults to
    # requiring the optional PNG/JPEG components when none are named, which
    # hard-fails configure on machines without system libpng/libjpeg even
    # though nothing in this project uses Halide's PNG/JPEG image-IO helpers.
    find_package(Halide REQUIRED COMPONENTS Halide)

    # W6 M-4: DngHalideGenerator.cpp (legacy dng_pipeline all-in-one kernel) archived.
    # The standalone halide_runtime.a below replaces its runtime-anchor role.
    # Source retained in src/ for reference; not compiled.

    if(WIN32)
        # The prebuilt Halide v21 Windows distribution ships /MD (dynamic CRT)
        # libs, but the windows-vulkan preset forces /MT (static CRT, see the
        # CMP0091 comment at the top of this file) for the shipped artifact.
        # lld-link's /failifmismatch rejects linking MT objects against an MD
        # lib, so give the generator executables below (build-time-only tools,
        # never shipped) /MD to match Halide_GenGen.lib/Halide.lib, then
        # restore /MT afterwards for dng_decoder_native and everything else.
        set(DNG_SAVED_MSVC_RUNTIME_LIBRARY ${CMAKE_MSVC_RUNTIME_LIBRARY})
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    endif()

    # R2 fix (F4, round-1 review, third leg — the actual root cause):
    # HalideConfig.cmake (third_party/halide/lib/cmake/Halide/HalideConfig.cmake)
    # never calls find_dependency(ZLIB) for the plain `Halide` component (only
    # for the unused PNG/JPEG components), so `Halide::Generator`'s link
    # closure never carries an explicit zlib dependency; these generator
    # executables `-force_load` the vendored static libHalide.a, which pulls
    # in LLVM's Compression.cpp.o/CRC.cpp.o (referencing zlib's
    # `_uncompress`/`_compress2`/`_compressBound`/`_crc32`) unconditionally.
    # Whether that resolves at link time turned out to depend on ambient
    # linker/SDK state that differs between a fresh configure and a
    # reconfigure-in-place (mechanically reproduced this round: identical
    # link.txt succeeded after a fresh configure+build, then failed with
    # undefined `_uncompress`/`_crc32` after reconfiguring the SAME build
    # dir and relinking — confirmed by manually re-running the captured
    # link.txt command, which fails deterministically without `-lz` and
    # succeeds deterministically with it added). Linking `ZLIB::ZLIB`
    # explicitly (already resolved earlier in this file, ~line 107) removes
    # the dependency on that ambient state entirely, on top of (not instead
    # of) the ZLIB-cache-slate reset above and the ordering-after-Halide
    # placement of the generic-RAW block.
    add_executable(rectilinear_warp_generator ${SRC_DIR}/RectilinearWarpGenerator.cpp)
    target_include_directories(rectilinear_warp_generator PRIVATE ${INC_DIR})
    target_link_libraries(rectilinear_warp_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    add_executable(dng_demosaic_generator ${SRC_DIR}/DngDemosaicGenerator.cpp)
    target_include_directories(dng_demosaic_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_demosaic_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # P17 T9: fused normalize + Bayer demosaic generator for the generic RAW
    # route. New generator on purpose: dng_demosaic_bilinear's AOT output is a
    # pinned regression artifact and the DNG route must not normalize twice.
    add_executable(raw_bayer_demosaic_generator ${SRC_DIR}/RawBayerDemosaicGenerator.cpp)
    target_include_directories(raw_bayer_demosaic_generator PRIVATE ${INC_DIR} ${SRC_DIR})
    target_link_libraries(raw_bayer_demosaic_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # P17 T11: fused normalize + X-Trans 6x6 demosaic generator for the
    # generic RAW route. Separate kernel from the Bayer one: the CFA is a
    # runtime 6x6 buffer, not a 2x2 phase pair.
    add_executable(raw_xtrans_demosaic_generator ${SRC_DIR}/RawXTransDemosaicGenerator.cpp)
    target_include_directories(raw_xtrans_demosaic_generator PRIVATE ${INC_DIR} ${SRC_DIR})
    target_link_libraries(raw_xtrans_demosaic_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # P19 T7: normalize-only pre-pass for the generic-RAW linear-RGB (Foveon
    # X3F) route. No demosaic, no neighbourhood access -- see the class
    # comment in RawLinearRgbNormalizeGenerator.cpp.
    add_executable(raw_linear_rgb_normalize_generator ${SRC_DIR}/RawLinearRgbNormalizeGenerator.cpp)
    target_include_directories(raw_linear_rgb_normalize_generator PRIVATE ${INC_DIR} ${SRC_DIR})
    target_link_libraries(raw_linear_rgb_normalize_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    add_executable(dng_demosaic_warp_generator ${SRC_DIR}/DngDemosaicWarpGenerator.cpp)
    target_include_directories(dng_demosaic_warp_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_demosaic_warp_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # Research & diagnostic generators (only built if DNG_DIAGNOSTIC_BUILD=ON)
    if(DNG_DIAGNOSTIC_BUILD)
        add_executable(rectilinear_warp_strict_float_generator ${SRC_DIR}/research/RectilinearWarpStrictFloatGenerator.cpp)
        target_include_directories(rectilinear_warp_strict_float_generator PRIVATE ${INC_DIR})
        target_link_libraries(rectilinear_warp_strict_float_generator PRIVATE Halide::Generator ZLIB::ZLIB)

        add_executable(rectilinear_warp_debug_generator ${SRC_DIR}/research/RectilinearWarpDebugGenerator.cpp)
        target_include_directories(rectilinear_warp_debug_generator PRIVATE ${INC_DIR})
        target_link_libraries(rectilinear_warp_debug_generator PRIVATE Halide::Generator ZLIB::ZLIB)
    endif()

    add_executable(dng_render_generator ${SRC_DIR}/DngRenderGenerator.cpp)
    target_include_directories(dng_render_generator PRIVATE ${INC_DIR})
    target_link_libraries(dng_render_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # Phase 10 Sprint C1: MapPolynomial AOT generator (Stage 2 OpcodeList2 GPU).
    add_executable(dng_opcode_polynomial_generator
        ${SRC_DIR}/DngOpcodePolynomialGenerator.cpp)
    target_link_libraries(dng_opcode_polynomial_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    add_executable(dng_opcode_polynomial3_generator
        ${SRC_DIR}/DngOpcodePolynomial3Generator.cpp)
    target_link_libraries(dng_opcode_polynomial3_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    if(WIN32)
        set(CMAKE_MSVC_RUNTIME_LIBRARY ${DNG_SAVED_MSVC_RUNTIME_LIBRARY})

        # The Halide v21 Windows distribution ships shared-only CMake targets
        # (no Halide-static-targets.cmake), so Halide::Generator resolves to an
        # import lib and the generators need Halide.dll at *run* time. The AOT
        # custom commands below run them from the build tree, where the DLL is
        # neither on PATH nor beside the .exe, so they die with
        # STATUS_DLL_NOT_FOUND (0xC0000135). Stage the DLL beside them once via
        # a single target every generator depends on; a per-generator POST_BUILD
        # copy would have parallel ninja jobs writing the same file at once.
        set(DNG_STAGED_HALIDE_DLL ${CMAKE_CURRENT_BINARY_DIR}/Halide.dll)
        add_custom_command(
            OUTPUT ${DNG_STAGED_HALIDE_DLL}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${HALIDE_DIR}/bin/Release/Halide.dll ${DNG_STAGED_HALIDE_DLL}
            DEPENDS ${HALIDE_DIR}/bin/Release/Halide.dll
            COMMENT "Staging Halide.dll next to the Halide generators..."
        )
        add_custom_target(dng_stage_halide_dll DEPENDS ${DNG_STAGED_HALIDE_DLL})

        set(DNG_HALIDE_GENERATORS
            rectilinear_warp_generator
            dng_demosaic_generator
            dng_demosaic_warp_generator
            dng_render_generator
            dng_opcode_polynomial_generator
            dng_opcode_polynomial3_generator
            raw_bayer_demosaic_generator
            raw_xtrans_demosaic_generator
            raw_linear_rgb_normalize_generator)
        if(DNG_DIAGNOSTIC_BUILD)
            list(APPEND DNG_HALIDE_GENERATORS
                rectilinear_warp_strict_float_generator
                rectilinear_warp_debug_generator)
        endif()
        foreach(_dng_gen IN LISTS DNG_HALIDE_GENERATORS)
            add_dependencies(${_dng_gen} dng_stage_halide_dll)
        endforeach()
    endif()

endif() # NOT DNG_CROSS_BUILD

# =============================================================================
# Phase 14: AOT output directory and target string
# =============================================================================
if(DNG_CROSS_BUILD)
    if(NOT DNG_PREBUILT_AOT_DIR)
        message(FATAL_ERROR "DNG_CROSS_BUILD=ON requires DNG_PREBUILT_AOT_DIR to be set")
    endif()
    set(HALIDE_OUTPUT_DIR "${DNG_PREBUILT_AOT_DIR}")
else()
    set(HALIDE_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/halide_generated)
    file(MAKE_DIRECTORY ${HALIDE_OUTPUT_DIR})
endif()

# --- Phase 14: Platform-specific Halide AOT target strings ---
if(DNG_AOT_TARGET_OVERRIDE)
    set(AOT_TARGET "${DNG_AOT_TARGET_OVERRIDE}")
else()
    if(APPLE)
        set(AOT_TARGET "host-metal-no_asserts-no_bounds_query")
    elseif(ANDROID)
        # Strict Android Vulkan device contract: arm64-v8a Vulkan with Halide vk_int8/vk_int16/vk_int64.
        set(AOT_TARGET "arm-64-android-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query")
    elseif(WIN32)
        # W6a (2026-08-21, Windows port): same strict Vulkan device contract as
        # Android, x86-64 host. The pipeline has no CPU fallback route
        # (dng_pipeline_v2.cpp requireGpuBackend), so a GPU feature is mandatory
        # rather than optional here.
        set(AOT_TARGET "x86-64-windows-vulkan-vk_int8-vk_int16-vk_int64-no_asserts-no_bounds_query")
    else()
        set(AOT_TARGET "host-no_asserts-no_bounds_query")
    endif()
endif()
message(STATUS "Halide AOT target: ${AOT_TARGET}")

# W6b (2026-08-21, Windows port, risk R8): Halide names AOT static-library
# outputs after the *target* platform, not the host — `.a` on POSIX and `.lib`
# for windows targets (see
# third_party/halide/lib/cmake/HalideHelpers/HalideGeneratorHelpers.cmake).
# Every AOT artifact path below therefore goes through this variable; a literal
# ".a" left anywhere in the AOT/link blocks is a missing-file link error on
# Windows whose message points at the build directory, not at this file.
if(AOT_TARGET MATCHES "windows")
    set(DNG_AOT_LIB_EXT ".lib")
else()
    set(DNG_AOT_LIB_EXT ".a")
endif()

# W7 (2026-08-21, Windows port, risk R2): the three-channel-split Stage4 kernel
# (dng_render_stage4_android) exists to dodge the Halide v21 SPIR-V Tuple R==G
# bug — a *Vulkan* codegen defect, not an Android platform trait. Select it by
# the AOT target's backend instead of by platform, so Windows-Vulkan inherits
# the same workaround. Android is unaffected: both its presets resolve
# AOT_TARGET to a string containing "vulkan", exactly the cases the previous
# `ANDROID OR override MATCHES "android"` condition covered.
if(AOT_TARGET MATCHES "vulkan")
    set(DNG_STAGE4_SPLIT_KERNEL ON)
else()
    set(DNG_STAGE4_SPLIT_KERNEL OFF)
endif()
message(STATUS "Halide AOT lib extension: ${DNG_AOT_LIB_EXT}; Stage4 split kernel: ${DNG_STAGE4_SPLIT_KERNEL}")

# Stage4 render uses same base target with -no_runtime (fixes hardcoded Metal bug)
# Route A adoption (P16, 2026-06-12): the macOS Metal production Stage4 render is
# pinned to IEEE stepwise via strict_float so its output is byte-exact against the
# -ffp-contract=off SDK render reference (dng_reference.cpp, see below). The Android
# Vulkan target is intentionally left unchanged — Halide v21 Vulkan
# unstrictify_float() drops strict semantics, making strict_float a no-op there
# (documented semantic gap; Android re-verified in W7 integration). Generic host
# CPU build also unchanged.
if(APPLE AND NOT ANDROID AND NOT (DNG_AOT_TARGET_OVERRIDE AND DNG_AOT_TARGET_OVERRIDE MATCHES "android"))
    set(DNG_RENDER_STAGE4_AOT_TARGET "${AOT_TARGET}-strict_float-no_runtime")
else()
    set(DNG_RENDER_STAGE4_AOT_TARGET "${AOT_TARGET}-no_runtime")
endif()
set(DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE "-1" CACHE STRING
    "Android Stage4 diagnostic stage (-1 full render)")

# =============================================================================
# AOT custom commands (only when NOT cross-building — cross-build uses prebuilt)
# =============================================================================
if(NOT DNG_CROSS_BUILD)

# W6 M-4: Standalone Halide runtime replaces legacy dng_pipeline.a runtime anchor.
# Any generator binary can emit the runtime via `-r`; we use dng_demosaic_generator.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    COMMAND dng_demosaic_generator -r halide_runtime -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}
    DEPENDS dng_demosaic_generator
    COMMENT "Generating standalone Halide runtime..."
)
add_custom_target(halide_runtime_target DEPENDS ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp.h
    COMMAND rectilinear_warp_generator -g rectilinear_warp -f rectilinear_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS rectilinear_warp_generator
    COMMENT "Generating Halide AOT Rectilinear Warp..."
)
add_custom_target(dng_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear.h
    COMMAND dng_demosaic_generator -g dng_demosaic_bilinear -f dng_demosaic_bilinear -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS dng_demosaic_generator
    COMMENT "Generating Halide AOT Stage3 Demosaic..."
)
add_custom_target(dng_demosaic_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT})

# P17 T9: generic-RAW fused normalize + Bayer demosaic AOT kernel.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic.h
    COMMAND raw_bayer_demosaic_generator -g raw_bayer_demosaic -f raw_bayer_demosaic
            -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS raw_bayer_demosaic_generator
    COMMENT "Generating Halide AOT generic Bayer normalize+demosaic..."
)
add_custom_target(raw_bayer_demosaic_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT})

# P17 T11: generic-RAW fused normalize + X-Trans demosaic AOT kernel.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic.h
    COMMAND raw_xtrans_demosaic_generator -g raw_xtrans_demosaic -f raw_xtrans_demosaic
            -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS raw_xtrans_demosaic_generator
    COMMENT "Generating Halide AOT generic X-Trans normalize+demosaic..."
)
add_custom_target(raw_xtrans_demosaic_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT})

# P19 T7: generic-RAW linear-RGB normalize-only AOT kernel (no demosaic).
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize.h
    COMMAND raw_linear_rgb_normalize_generator -g raw_linear_rgb_normalize -f raw_linear_rgb_normalize
            -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime
    DEPENDS raw_linear_rgb_normalize_generator
    COMMENT "Generating Halide AOT generic linear-RGB normalize..."
)
add_custom_target(raw_linear_rgb_normalize_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp.h
    COMMAND dng_demosaic_warp_generator -g dng_demosaic_warp -f dng_demosaic_warp -o ${HALIDE_OUTPUT_DIR} target=${AOT_TARGET}-no_runtime$<$<BOOL:${DNG_DEMOSAIC_WARP_STRICT_FLOAT}>:-strict_float> fast_codegen=$<IF:$<BOOL:${DNG_DEMOSAIC_WARP_FAST_CODEGEN}>,true,false>
    DEPENDS dng_demosaic_warp_generator
    COMMENT "Generating Halide AOT Stage3 Demosaic+Warp..."
)
add_custom_target(dng_demosaic_warp_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4.h
    COMMAND dng_render_generator -g dng_render_stage4 -f dng_render_stage4 -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render..."
)
add_custom_target(dng_render_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT})

# Sized decode (targetWidth): box-filter-downscaling Stage4 variant. Emitted
# from the SAME generator binary via -g/-f, exactly like the Android variant
# below, so that dng_render_stage4 itself stays byte-identical (its output SHAs
# are pinned gate artifacts — Gotcha #99).
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled.h
    COMMAND dng_render_generator -g dng_render_stage4_scaled -f dng_render_stage4_scaled -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render (box-filter scaled)..."
)
add_custom_target(dng_render_scaled_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT})

# Variant A of the sized kernel: box-averages the Stage3 source BEFORE the
# colour math (the variant above averages after it). The two co-exist on
# purpose so the averaging-order trade-off can be measured side by side on a
# real photograph; see the class comments in DngRenderGenerator.cpp.
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg.h
    COMMAND dng_render_generator -g dng_render_stage4_scaled_preavg -f dng_render_stage4_scaled_preavg -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
    DEPENDS dng_render_generator
    COMMENT "Generating Halide AOT Stage4 Render (box-filter scaled, pre-average)..."
)
add_custom_target(dng_render_scaled_preavg_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT})

# Phase 14: Vulkan three-channel-split Stage4 generator (was Android-only; see
# the DNG_STAGE4_SPLIT_KERNEL note above).
# Eliminates Tuple + dim(2) codegen that triggers Halide v21 SPIR-V R==G bug.
if(DNG_STAGE4_SPLIT_KERNEL)
    add_custom_command(
        OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android.h
        COMMAND dng_render_generator -g dng_render_stage4_android -f dng_render_stage4_android -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET} diag_stage=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE}
        DEPENDS dng_render_generator
        COMMENT "Generating Halide AOT Stage4 Render (Android 3-channel split)..."
    )
    add_custom_target(dng_render_android_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android${DNG_AOT_LIB_EXT})

    # P14-W4-4 GO/NO-GO probe: isolated interleaved flat-1D src-read AOT.
    # Separate kernel/signature; does not touch the production Stage4 kernel.
    add_custom_command(
        OUTPUT ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe.h
        COMMAND dng_render_generator -g dng_render_stage4_android_probe -f dng_render_stage4_android_probe -o ${HALIDE_OUTPUT_DIR} target=${DNG_RENDER_STAGE4_AOT_TARGET}
        DEPENDS dng_render_generator
        COMMENT "Generating Halide AOT Stage4 Render PROBE (Android interleaved-src go/no-go)..."
    )
    add_custom_target(dng_render_android_probe_aot_target DEPENDS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe${DNG_AOT_LIB_EXT})
endif()

# Phase 10 Sprint C1: MapPolynomial AOT (Stage 2 OpcodeList2 GPU).
add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial.h
    COMMAND dng_opcode_polynomial_generator -g dng_opcode_polynomial
        -f dng_opcode_polynomial -o ${HALIDE_OUTPUT_DIR}
        target=${AOT_TARGET}-no_runtime
    DEPENDS dng_opcode_polynomial_generator
    COMMENT "Generating Halide AOT Stage2 OpcodeList2 MapPolynomial..."
)
add_custom_target(dng_opcode_polynomial_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT})

add_custom_command(
    OUTPUT ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3.h
    COMMAND dng_opcode_polynomial3_generator -g dng_opcode_polynomial3
        -f dng_opcode_polynomial3 -o ${HALIDE_OUTPUT_DIR}
        target=${AOT_TARGET}-no_runtime
    DEPENDS dng_opcode_polynomial3_generator
    COMMENT "Generating Halide AOT Stage2 OpcodeList2 batched MapPolynomial..."
)
add_custom_target(dng_opcode_polynomial3_aot_target
    DEPENDS ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})

# When building host generators only (Stage 1), pull AOT targets into ALL
# so `cmake --build` actually runs the generators.
if(DNG_HOST_GENERATORS_ONLY)
    set(_DNG_ALL_AOT_DEPS
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT}
    )
    if(DNG_STAGE4_SPLIT_KERNEL)
        list(APPEND _DNG_ALL_AOT_DEPS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android${DNG_AOT_LIB_EXT})
        list(APPEND _DNG_ALL_AOT_DEPS ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe${DNG_AOT_LIB_EXT})
    endif()
    add_custom_target(dng_all_aot ALL DEPENDS ${_DNG_ALL_AOT_DEPS})
endif()

endif() # NOT DNG_CROSS_BUILD (AOT custom commands)

# =============================================================================
# Link AOT artifacts and platform libraries into dng_decoder_native
# =============================================================================
if(NOT DNG_HOST_GENERATORS_ONLY)

target_include_directories(dng_decoder_native PUBLIC ${HALIDE_OUTPUT_DIR})

if(NOT DNG_CROSS_BUILD)
    add_dependencies(dng_decoder_native halide_runtime_target)
    add_dependencies(dng_decoder_native dng_demosaic_aot_target)
    add_dependencies(dng_decoder_native dng_demosaic_warp_aot_target)
    add_dependencies(dng_decoder_native dng_warp_aot_target)
    add_dependencies(dng_decoder_native dng_render_aot_target)
    if(TARGET dng_render_android_aot_target)
        add_dependencies(dng_decoder_native dng_render_android_aot_target)
    endif()
    add_dependencies(dng_decoder_native dng_opcode_polynomial_aot_target)
    add_dependencies(dng_decoder_native dng_opcode_polynomial3_aot_target)
    # P17 T9: src/raw_demosaic_reference.cpp (auto-globbed above) calls the
    # raw_bayer_demosaic AOT entry, so the dylib depends on and links it.
    add_dependencies(dng_decoder_native raw_bayer_demosaic_aot_target)
    # P17 T11: the same reference TU also calls the raw_xtrans_demosaic AOT
    # entry.
    add_dependencies(dng_decoder_native raw_xtrans_demosaic_aot_target)
    # P19 T7: generic-RAW linear-RGB normalize-only AOT kernel (Foveon X3F
    # pre-pass ahead of the shared Stage4 handoff).
    add_dependencies(dng_decoder_native raw_linear_rgb_normalize_aot_target)
endif()

# Halide setup — We only need Halide::Runtime for AOT, but Halide::Halide is fine
# W6 M-4: halide_runtime.a is the standalone Halide runtime (Metal/Vulkan backend
# symbols: _halide_copy_to_device, _halide_malloc, _halide_metal_run, etc.).
# All AOT kernels use -no_runtime and link against halide_runtime.a.
target_include_directories(dng_decoder_native PUBLIC ${HALIDE_DIR}/include)

# W6 M-4 completion: Halide::Halide (host libHalide, 196MB shared lib) is no longer
# linked into dng_decoder_native. The standalone halide_runtime.a provides all AOT
# runtime symbols statically. Halide::Halide remains linked by generator executables
# and test targets that use the Halide host API directly.

# AOT artifacts (always linked — either freshly built or prebuilt)
target_link_libraries(dng_decoder_native
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT})
# R2 sized decode: the pre-average scaled Stage4 kernel is dispatched by
# dng_render_halide.cpp on the non-split (macOS/Metal) branch only, so it is
# linked only there. The split branch refuses sized requests instead.
if(NOT DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(dng_decoder_native
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT})
    if(NOT DNG_CROSS_BUILD)
        add_dependencies(dng_decoder_native dng_render_scaled_preavg_aot_target)
    endif()
endif()
# W7: link the split Stage4 kernel wherever it is generated (Vulkan targets).
if(DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(dng_decoder_native
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android${DNG_AOT_LIB_EXT})
    target_compile_definitions(dng_decoder_native PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
endif()
if(APPLE)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation)
    find_library(CORESERVICES_LIBRARY CoreServices)
    find_library(METAL_LIBRARY Metal)
    find_library(FOUNDATION_LIBRARY Foundation)
    target_link_libraries(dng_decoder_native
        ${COREFOUNDATION_LIBRARY}
        ${CORESERVICES_LIBRARY}
        ${METAL_LIBRARY}        # Required for Halide Metal GPU backend
        ${FOUNDATION_LIBRARY}   # Required for Halide Metal GPU backend
    )

    set_target_properties(dng_decoder_native PROPERTIES
        FRAMEWORK FALSE
        MACOSX_RPATH TRUE
    )
elseif(ANDROID)
    find_library(VULKAN_LIBRARY vulkan)
    if(NOT VULKAN_LIBRARY)
        message(FATAL_ERROR "Vulkan not found in NDK sysroot. Ensure ANDROID_PLATFORM >= android-24.")
    endif()
    find_library(LOG_LIBRARY log)
    target_link_libraries(dng_decoder_native ${VULKAN_LIBRARY} ${LOG_LIBRARY})
elseif(WIN32)
    # W8 (2026-08-21, Windows port): the Halide Vulkan runtime resolves its
    # entry points against the loader import library. The Vulkan SDK installer
    # sets VULKAN_SDK; -DDNG_VULKAN_LIB=<path to vulkan-1.lib> overrides it.
    set(DNG_VULKAN_LIB "" CACHE FILEPATH
        "Path to vulkan-1.lib (defaults to $ENV{VULKAN_SDK}/Lib/vulkan-1.lib)")
    if(NOT DNG_VULKAN_LIB)
        find_library(VULKAN_LIBRARY
            NAMES vulkan-1
            HINTS "$ENV{VULKAN_SDK}/Lib" "$ENV{VULKAN_SDK}/Lib32")
        if(NOT VULKAN_LIBRARY)
            message(FATAL_ERROR
                "vulkan-1.lib not found. Install the LunarG Vulkan SDK (so that "
                "VULKAN_SDK is set) or pass -DDNG_VULKAN_LIB=<path/to/vulkan-1.lib>. "
                "There is no CPU fallback route in this pipeline.")
        endif()
    else()
        set(VULKAN_LIBRARY "${DNG_VULKAN_LIB}")
    endif()
    message(STATUS "Linking Vulkan loader: ${VULKAN_LIBRARY}")
    target_link_libraries(dng_decoder_native ${VULKAN_LIBRARY})
endif()

# Phase 14 W0 acceptance smoke: Android cross-build test binary for ADB.
# Full host test targets stay under the NOT DNG_CROSS_BUILD block below.
if(ANDROID AND DNG_CROSS_BUILD)
    add_executable(test_android_vulkan_capability
        tests/android_vulkan_capability_probe.cpp)
    target_link_libraries(test_android_vulkan_capability PRIVATE
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})

    add_executable(test_decode_android tests/test_decode.cpp
        src/dng_pipeline_v2.cpp
        src/dng_halide_device.cpp
        src/dng_opcodelist2_halide.cpp
        src/dng_mosaic_halide.cpp
        src/dng_warp_halide.cpp
        src/dng_render_halide.cpp)
    target_include_directories(test_decode_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(test_decode_android PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android_probe${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(test_decode_android PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_STAGE4_INTERLEAVED_SRC_PROBE)
        target_compile_definitions(test_decode_android PRIVATE DNG_STAGE4_INTERLEAVED_SRC_PROBE=1)
    endif()
    if(DNG_USE_LIBJPEG)
        target_link_libraries(test_decode_android PRIVATE ${JPEG_LIBRARIES})
    endif()
    add_dependencies(test_decode_android test_android_vulkan_capability)

    # P14-W4-4 measurement: Android cross-build of the production C ABI harness.
    # Drives the FFI entry dng_decode_and_process -> dng_pipeline_v2_decode_to_rgb
    # -> decodeStages -> runHalideStage3And4Fused (the device-handoff path that
    # test_decode_android bypasses). Lets us measure on cc5bf709 whether device
    # handoff actually triggers and what [Stage4-Perf] FromDevice: reports.
    # Measurement-only target; no kernel / device-ownership code is touched.
    add_executable(dng_ffi_harness_android tests/dng_ffi_harness.cpp
        src/dng_ffi_api.cpp
        src/dng_pipeline_v2.cpp
        src/dng_halide_device.cpp
        src/dng_opcodelist2_halide.cpp
        src/dng_mosaic_halide.cpp
        src/dng_warp_halide.cpp
        src/dng_render_halide.cpp)
    target_include_directories(dng_ffi_harness_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(dng_ffi_harness_android PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(dng_ffi_harness_android PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_USE_LIBJPEG)
        target_link_libraries(dng_ffi_harness_android PRIVATE ${JPEG_LIBRARIES})
    endif()

    # matrix-eng ask (2026-07-04, Task #3): Android cross-build of the device-handoff
    # PSNR gate (Stage3->Stage4 device-dirty handoff vs host-copy fallback), mirroring
    # dng_ffi_harness_android above but for tests/test_device_handoff.cpp. Uses the
    # Android/Vulkan Stage4 AOT variant (dng_render_stage4_android.a), not the Metal
    # one linked by the macOS-only test_device_handoff target below.
    add_executable(test_device_handoff_android tests/test_device_handoff.cpp
        src/dng_pipeline_v2.cpp
        src/dng_halide_device.cpp
        src/dng_opcodelist2_halide.cpp
        src/dng_mosaic_halide.cpp
        src/dng_warp_halide.cpp
        src/dng_render_halide.cpp)
    target_include_directories(test_device_handoff_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(test_device_handoff_android PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_android${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(test_device_handoff_android PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_USE_LIBJPEG)
        target_link_libraries(test_device_handoff_android PRIVATE ${JPEG_LIBRARIES})
    endif()

    # ------------------------------------------------------------------------
    # R3-3: Halide Vulkan runtime fork (VkPipelineCache persistence).
    # See native/halide_runtime_fork/README.md for the weak-override mechanism.
    # The fork object is post-processed with llvm-objcopy --weaken: the fork TU
    # and halide_runtime.a both define ~271 internal helper methods STRONG
    # (LinkedList/BlockStorage/... — non-inline methods from src/runtime
    # internal headers); weakening the fork object lets the archive win those
    # (ABI-identical v21 sources) while the weak-vs-weak vulkan entry points
    # resolve to the fork (objects precede archives on the link line —
    # validated by scripts/tmp/r3_3_link_probe.sh disassembly evidence).
    # ------------------------------------------------------------------------
    if(DNG_VK_PIPELINE_CACHE)
        set(DNG_VK_FORK_DIR ${CMAKE_CURRENT_SOURCE_DIR}/halide_runtime_fork)
        add_library(halide_vulkan_fork OBJECT ${DNG_VK_FORK_DIR}/vulkan.cpp)
        target_include_directories(halide_vulkan_fork PRIVATE
            ${DNG_VK_FORK_DIR}
            ${DNG_VK_FORK_DIR}/upstream)
        # Match the upstream Halide runtime build environment:
        # runtime_internal.h rejects hosted compiles; COMPILING_HALIDE_RUNTIME
        # makes HalideRuntime.h use runtime_internal.h typedefs.
        target_compile_definitions(halide_vulkan_fork PRIVATE COMPILING_HALIDE_RUNTIME)
        target_compile_options(halide_vulkan_fork PRIVATE
            -ffreestanding -fno-exceptions -fno-rtti)

        set(DNG_VK_FORK_WEAK_OBJ ${CMAKE_CURRENT_BINARY_DIR}/halide_vulkan_fork_weak.o)
        add_custom_command(
            OUTPUT ${DNG_VK_FORK_WEAK_OBJ}
            COMMAND ${CMAKE_OBJCOPY} --weaken $<TARGET_OBJECTS:halide_vulkan_fork> ${DNG_VK_FORK_WEAK_OBJ}
            DEPENDS halide_vulkan_fork $<TARGET_OBJECTS:halide_vulkan_fork>
            COMMENT "R3-3: weakening Halide Vulkan fork object (avoid dup-strong collisions with halide_runtime.a)"
            VERBATIM)
        add_custom_target(halide_vulkan_fork_weak DEPENDS ${DNG_VK_FORK_WEAK_OBJ})

        # dng_decoder_native (production .so) + the three Android test
        # binaries link halide_runtime.a directly, so all four need the fork
        # object — otherwise the FFI harness would benchmark the stock runtime.
        foreach(_dng_vkpc_tgt
                dng_decoder_native
                dng_ffi_harness_android
                test_decode_android
                test_device_handoff_android)
            if(TARGET ${_dng_vkpc_tgt})
                target_sources(${_dng_vkpc_tgt} PRIVATE ${DNG_VK_FORK_WEAK_OBJ})
                add_dependencies(${_dng_vkpc_tgt} halide_vulkan_fork_weak)
            endif()
        endforeach()
    endif()
endif()

# =============================================================================
# Test targets — only built for native host builds (not cross-compile, not
# generator-only). Phase 14: guarded to prevent Android/cross-compile breakage.
# =============================================================================
if(NOT DNG_CROSS_BUILD)

# P12-W0B-03: production C ABI verification harness.
add_executable(dng_ffi_harness tests/dng_ffi_harness.cpp)
target_include_directories(dng_ffi_harness PRIVATE ${INC_DIR})
target_link_libraries(dng_ffi_harness PRIVATE dng_decoder_native)

# 2026-08-16 CFA phase: end-to-end pixel color-correctness gate. Decodes a
# real DNG through the production C ABI and asserts a known channel relation
# (blue sky must have B >> R), which is the only observable a wrong Bayer
# phase corrupts. Driven by run_decode_matrix.py against an external sample.
add_executable(test_cfa_color tests/test_cfa_color.cpp)
target_include_directories(test_cfa_color PRIVATE ${INC_DIR})
target_link_libraries(test_cfa_color PRIVATE dng_decoder_native)

# P17 T2: plain-C raw pipeline contract ABI test (raw_pipeline_contract.h).
# Pure header test, no LibRaw/decoder dependency; not gated by
# DNG_ENABLE_GENERIC_RAW.
add_executable(test_raw_contract_abi tests/test_raw_contract_abi.cpp)
target_include_directories(test_raw_contract_abi PRIVATE ${INC_DIR})

# P17 R2/T5: magic-byte RawFileRouter test. src/raw_file_router.cpp is
# already swept into dng_decoder_native by the file(GLOB_RECURSE
# NATIVE_SOURCES ...) above (~line 348); no LibRaw/DNG SDK/Halide
# dependency, not gated by DNG_ENABLE_GENERIC_RAW.
add_executable(test_raw_file_router
    tests/test_raw_file_router.cpp
    src/raw_file_router.cpp)
target_include_directories(test_raw_file_router PRIVATE ${INC_DIR})

# P17 R2/T3: layout classification + RawGpuInput validator test.
# src/raw_contract_validate.cpp is already swept into dng_decoder_native by
# the GLOB_RECURSE above.
add_executable(test_raw_layout_contract
    tests/test_raw_layout_contract.cpp
    src/raw_contract_validate.cpp)
target_include_directories(test_raw_layout_contract PRIVATE ${INC_DIR})

# P17 R2/T8: shared Stage4 core + LibRaw RenderParams builder test.
# src/raw_render_params_builder.cpp is already swept into
# dng_decoder_native by the GLOB_RECURSE above.
add_executable(test_raw_render_params tests/test_raw_render_params.cpp)
target_include_directories(test_raw_render_params PRIVATE ${INC_DIR})
target_link_libraries(test_raw_render_params PRIVATE dng_decoder_native dng_sdk)
if(APPLE)
    target_link_libraries(test_raw_render_params PRIVATE
        ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# P17 T1: Generic RAW frontend (LibRaw + bundled RawSpeed3) wiring, deferred
# until after Halide is fully configured (see option() comment above).
if(DNG_ENABLE_GENERIC_RAW)
    set(LIBRAW_DIR ${THIRD_PARTY_DIR}/libraw)
    set(LIBRAW_CMAKE_OVERLAY_DIR ${THIRD_PARTY_DIR}/libraw-cmake)
    if(NOT EXISTS ${LIBRAW_DIR}/libraw/libraw.h)
        message(FATAL_ERROR
            "DNG_ENABLE_GENERIC_RAW=ON but ${LIBRAW_DIR} is missing. Run "
            "dng_processor/native/scripts/fetch_libraw_dist.sh first, or "
            "configure with -DDNG_ENABLE_GENERIC_RAW=OFF.")
    endif()
    if(NOT EXISTS ${LIBRAW_CMAKE_OVERLAY_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "DNG_ENABLE_GENERIC_RAW=ON but ${LIBRAW_CMAKE_OVERLAY_DIR} is missing "
            "(LibRaw ships no CMakeLists.txt of its own; this project vendors the "
            "community LibRaw-cmake overlay, see PROVENANCE.md). Run "
            "dng_processor/native/scripts/fetch_libraw_dist.sh first.")
    endif()

    # RawSpeed3 build policy (spec section 6.6): no OpenMP, no tools/tests/
    # benchmarks/fuzzers, and no standalone RawSpeed target exposed to the app.
    #
    # R2 fix (F4, round-1 review): these used to be `CACHE ... FORCE` entries.
    # A CMake cache is read at the *start* of every configure, including the
    # second and later ones over the same build dir — so cached values here
    # would already be set BEFORE find_package(Halide) on any reconfigure,
    # reproducing the exact zlib/`_uncompress` link corruption this whole
    # block is deferred (until after Halide) to avoid in the first place.
    # Plain (non-cache) `set()` is directory-scoped and inherited by
    # add_subdirectory() children, and this project's cmake_minimum_required
    # is 3.14 (>= 3.13), so CMP0077 already defaults to NEW: the RawSpeed3 /
    # LibRaw-cmake subprojects' own option()/set(... CACHE) calls see these
    # normal variables and do not override them. No cache residue, so a
    # reconfigure starts from the same pre-find_package(Halide) state as a
    # fresh configure. See PROVENANCE.md "Local modifications" for the
    # policy-floor and pugixml-download notes below (F6).
    if(POLICY CMP0077)
        cmake_policy(SET CMP0077 NEW)
    endif()
    set(WITH_OPENMP OFF)
    set(RAWSPEED_ENABLE_WERROR OFF)
    set(BUILD_TOOLS OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_BENCHMARKING OFF)
    set(BUILD_FUZZERS OFF)
    set(USE_XMLLINT OFF)
    set(USE_BUNDLED_PUGIXML ON)
    # RawSpeed3's own Pugixml.cmake fetches a hash-pinned tarball
    # (pugixml-1.9.tar.gz, SHA512-verified) when not found locally; see
    # third_party/libraw/RawSpeed3/rawspeed/cmake/Modules/Pugixml.cmake.in.
    set(ALLOW_DOWNLOADING_PUGIXML ON)
    # pugixml-1.9's own CMakeLists.txt predates CMake 3.5's minimum-version
    # policy floor; this only relaxes the sub-build's own cmake_minimum_required
    # check, not this project's. CMAKE_POLICY_VERSION_MINIMUM is a plain CMake
    # variable (not an option()-backed cache entry), so a non-cache set() here
    # is honored identically to a cached one, without the reconfigure hazard.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

    # Builds the `rawspeed` static target from RawSpeed3's own (real) CMake
    # build. This library is never linked into any Halide/AOT target and is
    # only ever consumed via the glue below, never exposed as a standalone
    # dependency of dng_decoder_native (spec section 6.6.1/6.6.3).
    add_subdirectory(${LIBRAW_DIR}/RawSpeed3/rawspeed rawspeed3-build EXCLUDE_FROM_ALL)

    # LibRaw at the pinned revision ships no CMakeLists.txt of its own
    # (see third_party/libraw/README.cmake: unmaintained by the LibRaw team
    # since 2014). This project vendors the community LibRaw/LibRaw-cmake
    # overlay as a third pinned dependency (see PROVENANCE.md) and points it
    # at our vendored LibRaw source tree via LIBRAW_PATH. That overlay only
    # knows the legacy RawSpeed v1 codec path (ENABLE_RAWSPEED), so
    # RawSpeed3 support is NOT part of the overlay and is glued on below —
    # this is a documented local modification, see PROVENANCE.md.
    # LIBRAW_PATH is a genuine exception to the non-cache conversion above:
    # unlike every other variable in this block, LibRaw-cmake's own
    # CMakeLists.txt (third_party/libraw-cmake/CMakeLists.txt:36) sets it
    # via a raw `set(LIBRAW_PATH ... CACHE STRING doc)` (no FORCE) rather
    # than via option() — that call is governed by CMP0126 ("set(CACHE)
    # does not remove a normal variable of the same name"), not CMP0077.
    # LibRaw-cmake's own cmake_minimum_required() tops out below CMake 3.21
    # (where CMP0126 was introduced), so CMP0126 defaults OLD in that
    # subdirectory scope: its own set(CACHE) call unconditionally deletes
    # any same-named *normal* variable in scope and reinitializes the cache
    # entry to its own default (its own CMAKE_CURRENT_SOURCE_DIR) — verified
    # by reproducing a fresh-configure failure at
    # third_party/libraw-cmake/CMakeLists.txt:47 (`file(READ
    # ${LIBRAW_PATH}/libraw/libraw_version.h ...)` pointed at
    # third_party/libraw-cmake/libraw/... instead of third_party/libraw/...)
    # when this was made a plain set() like the others. Keeping this one
    # variable CACHE FORCE pre-empts that: our forced cache entry already
    # exists by the time LibRaw-cmake's non-FORCE `set(... CACHE ...)` runs,
    # so it is a no-op (existing cache entries are left alone without
    # FORCE) and our value is used. This is unrelated to the F4 hazard
    # (LIBRAW_PATH is a path string never consulted by zlib/Halide
    # discovery), so caching it does not reintroduce the reconfigure bug.
    set(LIBRAW_PATH ${LIBRAW_DIR} CACHE STRING "" FORCE)
    set(ENABLE_RAWSPEED OFF)
    set(ENABLE_OPENMP OFF)
    set(ENABLE_EXAMPLES OFF)
    set(LIBRAW_INSTALL OFF)
    # P19 W2: LibRaw ships the Kalpanika x3f-tools Foveon decoder in src/x3f/,
    # dead unless USE_X3FTOOLS is defined. The overlay declares
    # option(ENABLE_X3FTOOLS ... OFF) and turns it into -DUSE_X3FTOOLS
    # (third_party/libraw-cmake/CMakeLists.txt:90,349-351); its GLOB_RECURSE
    # already sweeps src/x3f/*.cpp into the `raw` target either way, so this is
    # purely the define.
    #
    # Plain non-cache set() on purpose (PROVENANCE.md "R2 F4 fix note"):
    # a CACHE FORCE here would be replayed on every later configure BEFORE
    # find_package(Halide), which is the exact ordering that corrupted the
    # Halide/zlib link. CMP0077 (NEW since cmake_minimum_required 3.14) makes
    # the subproject's option() honour this directory-scoped value.
    set(ENABLE_X3FTOOLS ON)
    add_subdirectory(${LIBRAW_CMAKE_OVERLAY_DIR} libraw-cmake-build EXCLUDE_FROM_ALL)

    # --- RawSpeed3 C-API glue (local modification, see PROVENANCE.md) ---
    # Generates rawspeed3_c_api/cameras.cpp from RawSpeed3/rawspeed/data/cameras.xml
    # at configure time (LibRaw's own documented build step, RawSpeed3/README.md).
    set(RAWSPEED3_CAPI_DIR ${LIBRAW_DIR}/RawSpeed3/rawspeed3_c_api)
    set(RAWSPEED3_CAMERAS_XML ${LIBRAW_DIR}/RawSpeed3/rawspeed/data/cameras.xml)
    set(RAWSPEED3_CAMERAS_CPP ${CMAKE_CURRENT_BINARY_DIR}/rawspeed3-cameras/cameras.cpp)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/rawspeed3-cameras)
    execute_process(
        COMMAND sh ${RAWSPEED3_CAPI_DIR}/rsxml2c.sh ${RAWSPEED3_CAMERAS_XML}
        OUTPUT_FILE ${RAWSPEED3_CAMERAS_CPP}
        RESULT_VARIABLE _rsxml2c_rc)
    if(NOT _rsxml2c_rc EQUAL 0)
        message(FATAL_ERROR "rsxml2c.sh failed to generate RawSpeed3 cameras.cpp (rc=${_rsxml2c_rc})")
    endif()

    # rawspeed3_capi.cpp expects pugixml at a fixed relative path
    # (RawSpeed3/pugixml/pugixml.hpp, sibling of rawspeed3_c_api/) that only
    # exists when pugixml is vendored alongside RawSpeed3 by hand. We instead
    # let RawSpeed3's own CMake fetch a hash-pinned pugixml (see
    # ALLOW_DOWNLOADING_PUGIXML above), so its source dir is elsewhere.
    # Rather than patch the vendored .cpp, forward the expected include path
    # to the real pugixml source dir via a generated shim header (CMake-only
    # glue, documented in PROVENANCE.md "Local modifications").
    get_target_property(_rawspeed3_pugixml_src pugixml SOURCE_DIR)
    if(NOT _rawspeed3_pugixml_src)
        message(FATAL_ERROR "Could not resolve pugixml SOURCE_DIR from RawSpeed3's pugixml target")
    endif()
    set(_rawspeed3_pugixml_shim ${CMAKE_CURRENT_BINARY_DIR}/rawspeed3-pugixml-shim)
    file(MAKE_DIRECTORY ${_rawspeed3_pugixml_shim}/include_anchor)
    file(MAKE_DIRECTORY ${_rawspeed3_pugixml_shim}/pugixml)
    file(WRITE ${_rawspeed3_pugixml_shim}/pugixml/pugixml.hpp
        "#include \"${_rawspeed3_pugixml_src}/src/pugixml.hpp\"\n")

    # `raw` is exported by the overlay above; extend it in place with the
    # RawSpeed3 C-API wrapper sources rather than forking the overlay.
    target_sources(raw PRIVATE
        ${RAWSPEED3_CAPI_DIR}/rawspeed3_capi.cpp
        ${RAWSPEED3_CAMERAS_CPP})
    target_include_directories(raw PRIVATE
        ${RAWSPEED3_CAPI_DIR}
        ${_rawspeed3_pugixml_shim}/include_anchor)
    target_compile_definitions(raw PRIVATE USE_RAWSPEED3 USE_RAWSPEED_BITS)
    target_link_libraries(raw PRIVATE rawspeed rawspeed_get_number_of_processor_cores)

    # P17 R5 (F1): the vendored LibRaw static lib was re-exported wholesale
    # (431 symbols) by dng_decoder_native. Hidden visibility keeps LibRaw
    # internal to the dylib; our own extern "C" ABI is unaffected (it lives in
    # project TUs with default visibility). Property set here, not in the
    # vendored overlay (third_party/ is read-only).
    set_target_properties(raw PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # RawSpeed3's core-count helper declares its symbol with an explicit
    # __attribute__((visibility("default"))) (GetNumberOfProcessorCores.cpp),
    # which -fvisibility=hidden cannot override — unexport it at link time
    # on the dylib instead (vendored tree is read-only).
    if(TARGET dng_decoder_native AND APPLE)
        target_link_options(dng_decoder_native PRIVATE
            "LINKER:-unexported_symbol,_rawspeed_get_number_of_processor_cores")
    endif()

    add_library(libraw_vendored INTERFACE)
    target_include_directories(libraw_vendored INTERFACE ${LIBRAW_DIR})
    target_link_libraries(libraw_vendored INTERFACE raw)
    target_compile_definitions(libraw_vendored INTERFACE DNG_ENABLE_GENERIC_RAW=1)

    # P17 T6: the single generic decoder owner. src/libraw_frontend.cpp is
    # already in NATIVE_SOURCES via GLOB_RECURSE (~line 348, filtered out when
    # this option is OFF), so dng_decoder_native only needs LibRaw's usage
    # requirements here. Keyword-less signature deliberately, to match the
    # existing target_link_libraries(dng_decoder_native dng_sdk) at ~line 368
    # (CMake forbids mixing plain and PRIVATE/PUBLIC forms on one target).
    if(TARGET dng_decoder_native)
        target_link_libraries(dng_decoder_native libraw_vendored)
    endif()

    add_executable(test_libraw_frontend
        tests/test_libraw_frontend.cpp
        src/libraw_frontend.cpp
        src/raw_contract_validate.cpp)
    target_include_directories(test_libraw_frontend PRIVATE ${INC_DIR})
    target_link_libraries(test_libraw_frontend PRIVATE libraw_vendored)

    # P17 T7: the single LibRaw -> RawGpuInput adapter and its test.
    # (F-R4-05: the round-4 EXISTS guard is gone — the sources are committed,
    # and a guarded target drops silently with no red signal.)
    add_executable(test_libraw_adapter
        tests/test_libraw_adapter.cpp
        src/libraw_frontend.cpp
        src/libraw_gpu_input_adapter.cpp
        src/raw_contract_validate.cpp)
    target_include_directories(test_libraw_adapter PRIVATE ${INC_DIR})
    target_link_libraries(test_libraw_adapter PRIVATE libraw_vendored)

    # P17 T10: the generic RAW route end to end. Links the production dylib on
    # purpose, so the exported C ABI and the SHARED RGBA pool are the ones under
    # test rather than a second copy (spec 13.1).
    add_executable(test_raw_end_to_end tests/test_raw_end_to_end.cpp)
    target_include_directories(test_raw_end_to_end PRIVATE ${INC_DIR} ${HALIDE_OUTPUT_DIR} ${HALIDE_DIR}/include)
    target_link_libraries(test_raw_end_to_end dng_decoder_native libraw_vendored)
    if(APPLE)
        target_link_libraries(test_raw_end_to_end
            ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
    endif()
endif()

# P17 T13: malformed input, resource limits, cancellation, GPU-mandatory.
# Links the production dylib so the shared RGBA pool under test is the real one.
if(DNG_ENABLE_GENERIC_RAW)
    add_executable(test_raw_hardening tests/test_raw_hardening.cpp)
    target_include_directories(test_raw_hardening
        PRIVATE ${INC_DIR} ${HALIDE_OUTPUT_DIR} ${HALIDE_DIR}/include)
    target_link_libraries(test_raw_hardening dng_decoder_native libraw_vendored)
    if(APPLE)
        target_link_libraries(test_raw_hardening
            ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY}
            ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
    endif()
endif()

# P17 T1: CPU-only LibRaw smoke test. Links neither Halide nor
# dng_decoder_native (spec: prove the dependency before any GPU work).
if(DNG_ENABLE_GENERIC_RAW)
    add_executable(libraw_smoke tests/libraw_smoke.cpp)
    target_link_libraries(libraw_smoke PRIVATE libraw_vendored)
endif()


# W5-4 / TD-2: Device handoff path independent test.
# Validates that Stage3→Stage4 (lossless) and Stage2→Stage4 (lossy) device
# handoff paths produce bit-identical (PSNR ≥99dB) output vs host-copy fallback.
add_executable(test_device_handoff tests/test_device_handoff.cpp
    src/dng_pipeline_v2.cpp
    src/dng_halide_device.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_warp_halide.cpp
    src/dng_render_halide.cpp)
target_include_directories(test_device_handoff PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_device_handoff dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT} ${JPEG_LIBRARIES})
else()
    target_link_libraries(test_device_handoff dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})
endif()
# R2 sized decode: this target compiles dng_render_halide.cpp directly, so it
# needs the scaled kernel the sized dispatch calls (macOS/Metal branch only).
if(NOT DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_device_handoff
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT})
    add_dependencies(test_device_handoff dng_render_scaled_preavg_aot_target)
endif()
add_dependencies(test_device_handoff halide_runtime_target)
add_dependencies(test_device_handoff dng_demosaic_aot_target)
add_dependencies(test_device_handoff dng_demosaic_warp_aot_target)
add_dependencies(test_device_handoff dng_warp_aot_target)
add_dependencies(test_device_handoff dng_render_aot_target)
add_dependencies(test_device_handoff dng_opcode_polynomial_aot_target)
add_dependencies(test_device_handoff dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_device_handoff ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()


# Sized decode (targetWidth) R1: standalone gate for the box-filter-scaled
# Stage4 AOT. Self-contained — synthesises a Stage3-shaped source and its
# render parameters, so it needs no DNG sample and no dng_sdk. It compares
# dng_render_stage4_scaled(full src) against dng_render_stage4(CPU box
# downscale of the same src) and gates the PSNR.
add_executable(test_stage4_scaled tests/test_stage4_scaled.cpp)
target_include_directories(test_stage4_scaled PRIVATE
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_stage4_scaled
    Halide::Halide
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT})
add_dependencies(test_stage4_scaled halide_runtime_target)
add_dependencies(test_stage4_scaled dng_render_aot_target)
add_dependencies(test_stage4_scaled dng_render_scaled_aot_target)
add_dependencies(test_stage4_scaled dng_render_scaled_preavg_aot_target)
if(APPLE)
    target_link_libraries(test_stage4_scaled ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()


# Real-photograph AC7 measurement for both sized-kernel variants, plus viewable
# image output. NOTE: tests/test_stage4_scaled_photo.cpp #includes
# src/dng_render_halide.cpp directly (to reach the production buildRenderParams
# without editing a production source), so that file must NOT be listed here as
# a separate source or every symbol in it would be defined twice.
add_executable(test_stage4_scaled_photo tests/test_stage4_scaled_photo.cpp
    src/dng_pipeline_v2.cpp
    src/dng_halide_device.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_warp_halide.cpp)
target_include_directories(test_stage4_scaled_photo PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_stage4_scaled_photo
    dng_sdk
    Halide::Halide
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_stage4_scaled_photo ${JPEG_LIBRARIES})
endif()
add_dependencies(test_stage4_scaled_photo halide_runtime_target)
add_dependencies(test_stage4_scaled_photo dng_demosaic_aot_target)
add_dependencies(test_stage4_scaled_photo dng_demosaic_warp_aot_target)
add_dependencies(test_stage4_scaled_photo dng_warp_aot_target)
add_dependencies(test_stage4_scaled_photo dng_render_aot_target)
add_dependencies(test_stage4_scaled_photo dng_render_scaled_aot_target)
add_dependencies(test_stage4_scaled_photo dng_render_scaled_preavg_aot_target)
add_dependencies(test_stage4_scaled_photo dng_opcode_polynomial_aot_target)
add_dependencies(test_stage4_scaled_photo dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_stage4_scaled_photo ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()


# R2 sized decode acceptance gate (AC5 extent / AC5-D crop-vs-scale / AC6 memory).
# Drives the PRODUCTION sized entry (dng_pipeline_v2_decode_to_rgb_sized) and
# compares against a same-ordering CPU reference rendered through the production
# Stage4 AOT. Like test_stage4_scaled_photo it #includes dng_render_halide.cpp to
# reach buildRenderParams, so that file must NOT be listed as a separate source
# here or every symbol in it would be defined twice.
add_executable(test_sized_decode tests/test_sized_decode.cpp
    src/dng_pipeline_v2.cpp
    src/dng_halide_device.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_warp_halide.cpp)
target_include_directories(test_sized_decode PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_sized_decode
    dng_sdk
    Halide::Halide
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_sized_decode ${JPEG_LIBRARIES})
endif()
add_dependencies(test_sized_decode halide_runtime_target)
add_dependencies(test_sized_decode dng_demosaic_aot_target)
add_dependencies(test_sized_decode dng_demosaic_warp_aot_target)
add_dependencies(test_sized_decode dng_warp_aot_target)
add_dependencies(test_sized_decode dng_render_aot_target)
add_dependencies(test_sized_decode dng_render_scaled_preavg_aot_target)
add_dependencies(test_sized_decode dng_opcode_polynomial_aot_target)
add_dependencies(test_sized_decode dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_sized_decode ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()


# Color accuracy / visual regression test
add_executable(test_color_accuracy tests/test_color_accuracy.cpp)
target_include_directories(test_color_accuracy PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_color_accuracy dng_sdk)
if(APPLE)
    target_link_libraries(test_color_accuracy ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

add_executable(test_dng_layout tests/test_dng_layout.cpp)
target_include_directories(test_dng_layout PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_layout dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_layout ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# Tile testing tool
add_executable(test_dng_tiles tests/test_dng_tiles.cpp)
target_include_directories(test_dng_tiles PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_tiles dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_tiles ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()


add_executable(test_dng_preview tests/test_dng_preview.cpp)
target_include_directories(test_dng_preview PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_preview dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_preview ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

# DNG SDK Decode Pipeline Test Tool (with Halide Stage3 demosaic)
add_executable(test_decode tests/test_decode.cpp
    src/dng_pipeline_v2.cpp
    src/dng_halide_device.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_warp_halide.cpp
    src/dng_render_halide.cpp)
target_include_directories(test_decode PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT} ${JPEG_LIBRARIES})
else()
    target_link_libraries(test_decode dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})
endif()
# R2 sized decode: same as test_device_handoff — compiles the render TU directly.
if(NOT DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_decode
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_scaled_preavg${DNG_AOT_LIB_EXT})
    add_dependencies(test_decode dng_render_scaled_preavg_aot_target)
endif()
add_dependencies(test_decode halide_runtime_target)
add_dependencies(test_decode dng_demosaic_aot_target)
add_dependencies(test_decode dng_demosaic_warp_aot_target)
add_dependencies(test_decode dng_warp_aot_target)
add_dependencies(test_decode dng_render_aot_target)
add_dependencies(test_decode dng_opcode_polynomial_aot_target)
add_dependencies(test_decode dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_decode ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# Phase 5.3: Halide Demosaic PSNR Test
# 2026-08-16: dng_opcodelist2_halide.cpp / dng_halide_device.cpp added to the
# source list. libdng_sdk's dng_opcode_list::Apply references the Stage2
# OpcodeList2 Halide bridge symbols unconditionally, so this target had been
# failing to link since that bridge landed (no test_demosaic_halide binary was
# ever produced). Linking the bridge in restores the target; the polynomial
# AOT archives below satisfy the bridge's own kernel references.
add_executable(test_demosaic_halide tests/test_demosaic_halide.cpp
    src/dng_mosaic_halide.cpp
    src/dng_opcodelist2_halide.cpp
    src/dng_halide_device.cpp)
target_include_directories(test_demosaic_halide PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# W6 M-4: halide_runtime.a provides Metal/Vulkan runtime symbols for all -no_runtime AOT kernels.
target_link_libraries(test_demosaic_halide dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})
add_dependencies(test_demosaic_halide halide_runtime_target)
add_dependencies(test_demosaic_halide dng_warp_aot_target)
add_dependencies(test_demosaic_halide dng_demosaic_aot_target)
add_dependencies(test_demosaic_halide dng_opcode_polynomial_aot_target)
add_dependencies(test_demosaic_halide dng_opcode_polynomial3_aot_target)
if(APPLE)
    target_link_libraries(test_demosaic_halide ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# 2026-08-16 CFA phase: all-four-Bayer-phases unit check on a synthetic
# mosaic. Covers both the Halide AOT kernel and the CPU reference demosaic
# plus get_cfa_pattern's phase expansion. No DNG fixture required.
add_executable(test_cfa_phase tests/test_cfa_phase.cpp
    src/dng_mosaic_halide.cpp)
target_include_directories(test_cfa_phase PRIVATE
    ${INC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# dng_sdk: the resolver case feeds dng_resolve_cfa_phase a synthetic dng_mosaic_info.
target_link_libraries(test_cfa_phase dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT})
add_dependencies(test_cfa_phase halide_runtime_target)
add_dependencies(test_cfa_phase dng_demosaic_aot_target)
if(APPLE)
    target_link_libraries(test_cfa_phase ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# P17 T9: fused normalize + Bayer demosaic AOT kernel vs same-algorithm CPU
# reference (>=99 dB / max_abs<=1) plus the constant-field phase oracle.
add_executable(test_raw_bayer_kernel
    tests/test_raw_bayer_kernel.cpp
    src/raw_demosaic_reference.cpp)
target_include_directories(test_raw_bayer_kernel PRIVATE ${INC_DIR} ${HALIDE_OUTPUT_DIR} ${HALIDE_DIR}/include)
add_dependencies(test_raw_bayer_kernel raw_bayer_demosaic_aot_target
                 raw_xtrans_demosaic_aot_target halide_runtime_target)
target_link_libraries(test_raw_bayer_kernel
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT})
if(APPLE)
    target_link_libraries(test_raw_bayer_kernel
        ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# P17 T11: fused normalize + X-Trans 6x6 demosaic AOT kernel vs same-formula
# CPU reference (>=99 dB / max_abs<=1), plus the 5x5 coverage property and the
# constant-field oracle.
add_executable(test_raw_xtrans_kernel
    tests/test_raw_xtrans_kernel.cpp
    src/raw_demosaic_reference.cpp)
target_include_directories(test_raw_xtrans_kernel PRIVATE ${INC_DIR} ${HALIDE_OUTPUT_DIR} ${HALIDE_DIR}/include)
add_dependencies(test_raw_xtrans_kernel raw_xtrans_demosaic_aot_target
                 raw_bayer_demosaic_aot_target halide_runtime_target)
target_link_libraries(test_raw_xtrans_kernel
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT})
if(APPLE)
    target_link_libraries(test_raw_xtrans_kernel
        ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# P19 T7: linear-RGB normalize AOT kernel vs same-formula CPU reference
# (>=99 dB / max_abs<=1), plus the constant-field oracle and a strided source.
add_executable(test_raw_linear_rgb_kernel
    tests/test_raw_linear_rgb_kernel.cpp
    src/raw_demosaic_reference.cpp)
target_include_directories(test_raw_linear_rgb_kernel PRIVATE ${INC_DIR} ${HALIDE_OUTPUT_DIR} ${HALIDE_DIR}/include)
add_dependencies(test_raw_linear_rgb_kernel raw_linear_rgb_normalize_aot_target
                 raw_bayer_demosaic_aot_target raw_xtrans_demosaic_aot_target
                 halide_runtime_target)
target_link_libraries(test_raw_linear_rgb_kernel
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT})
if(APPLE)
    target_link_libraries(test_raw_linear_rgb_kernel
        ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# Debug demosaic test
add_executable(test_demosaic_debug tests/test_demosaic_debug.cpp
    src/dng_mosaic_halide.cpp)
target_include_directories(test_demosaic_debug PRIVATE
    ${INC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
# W6 M-4: same standalone runtime as test_demosaic_halide above.
target_link_libraries(test_demosaic_debug dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT})
add_dependencies(test_demosaic_debug halide_runtime_target)
add_dependencies(test_demosaic_debug dng_warp_aot_target)
add_dependencies(test_demosaic_debug dng_demosaic_aot_target)
if(APPLE)
    target_link_libraries(test_demosaic_debug ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()

# Phase 5.1: Halide Render PSNR Test (placeholder - needs DNG SDK data)
add_executable(test_render_halide tests/test_render_halide.cpp)
target_include_directories(test_render_halide PRIVATE
    ${INC_DIR}
    ${DNG_SDK_DIR})
target_link_libraries(test_render_halide dng_sdk)
if(APPLE)
    target_link_libraries(test_render_halide ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

endif() # NOT DNG_CROSS_BUILD (test targets)

endif() # NOT DNG_HOST_GENERATORS_ONLY (entire runtime section)
