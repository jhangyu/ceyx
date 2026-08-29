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

# =============================================================================
# Modular build description (2026-08-25 Ceyx restructure, Round 1 Stream 1B).
# The fragments below are include()d, never add_subdirectory()'d: they share
# this scope's variables (SRC_DIR, INC_DIR, THIRD_PARTY_DIR, HALIDE_DIR,
# HALIDE_OUTPUT_DIR, DNG_AOT_LIB_EXT, ...) and reference each other's targets
# by name. The include() order below reproduces the monolith's original
# top-to-bottom execution order exactly; reordering it breaks documented
# ordering hazards (RawSpeed3 vs find_package(Halide), zlib cache slate).
# =============================================================================
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/third_party.cmake)

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

# F-R3-1 MoltenVK arbitration switch (2026-08-28, default OFF). When ON, the
# host bridge selects Halide's Vulkan device interface + split Stage4 kernel on
# Apple too (instead of the hardcoded Metal path), so a Vulkan AOT build
# (DNG_AOT_TARGET_OVERRIDE=host-vulkan-...) can run over MoltenVK on macOS to
# tell a lavapipe environment fault apart from a real kernel bug. OFF reproduces
# the committed Apple/Metal behaviour byte-for-byte (AOT artifacts are emitted by
# the Halide generators, which this switch does not touch). Pair it with the
# matching Vulkan AOT target override in a SEPARATE build dir; never the default
# macOS build.
option(DNG_FORCE_VULKAN
       "Force the Vulkan Halide device interface + split Stage4 kernel on Apple (MoltenVK arbitration; OFF = committed Metal path)"
       OFF)
if(DNG_FORCE_VULKAN)
    add_compile_definitions(DNG_FORCE_VULKAN=1)
endif()

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

# HEIC/HEIF decode route (spec section 7, phase 2). ON by default on platforms
# where the vendored dist exists; OFF drops both HEIF translation units and
# every libheif/libde265 dependency, which is the documented degradation for a
# platform whose dist has not been built (the app must still start and simply
# report HEIC files as unreadable).
option(DNG_ENABLE_HEIF
       "Build the HEIC/HEIF decode route (libheif + libde265, dynamically linked)" ON)

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

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/pipeline.cmake)

# =============================================================================
# Halide: generators, AOT commands, and linking
# =============================================================================
set(HALIDE_DIR ${THIRD_PARTY_DIR}/halide)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/generators.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/halide_aot.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ffi.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/heif.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/encode.cmake)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests.cmake)
