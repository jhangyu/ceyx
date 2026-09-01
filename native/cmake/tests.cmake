# tests.cmake - Android cross-build and host test executables
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 938-1753)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.

# Re-guard (split mechanics): continuation of the `if(NOT DNG_HOST_GENERATORS_ONLY)`
# block that ffi.cmake had to close at its end; see the note there.
if(NOT DNG_HOST_GENERATORS_ONLY)

# Phase 14 W0 acceptance smoke: Android cross-build test binary for ADB.
# Full host test targets stay under the NOT DNG_CROSS_BUILD block below.
if(ANDROID AND DNG_CROSS_BUILD)
    add_executable(test_android_vulkan_capability
        tests/android_vulkan_capability_probe.cpp)
    target_link_libraries(test_android_vulkan_capability PRIVATE
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})

    add_executable(test_decode_android tests/test_decode.cpp
        src/pipeline/dng_pipeline.cpp
        src/pipeline/dng_halide_device.cpp
        src/pipeline/dng_opcodelist2_halide.cpp
        src/pipeline/dng_mosaic_halide.cpp
        src/pipeline/dng_warp_halide.cpp
        src/pipeline/dng_render_halide.cpp)
    target_include_directories(test_decode_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        # test_decode.cpp includes "concurrent_dng_host.h" unqualified, so the
        # pipeline dir must be on the path here exactly as it is for the host
        # test_decode target below.
        ${SRC_DIR}/pipeline
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
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split_probe${DNG_AOT_LIB_EXT}
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
    # Drives the FFI entry dng_decode_and_process -> dng_pipeline_decode_to_rgb
    # -> decodeStages -> runHalideStage3And4Fused (the device-handoff path that
    # test_decode_android bypasses). Lets us measure on cc5bf709 whether device
    # handoff actually triggers and what [Stage4-Perf] FromDevice: reports.
    # Measurement-only target; no kernel / device-ownership code is touched.
    add_executable(dng_ffi_harness_android tests/dng_ffi_harness.cpp
        src/ffi/dng_ffi_api.cpp
        src/pipeline/dng_pipeline.cpp
        src/pipeline/dng_halide_device.cpp
        src/pipeline/dng_opcodelist2_halide.cpp
        src/pipeline/dng_mosaic_halide.cpp
        src/pipeline/dng_warp_halide.cpp
        src/pipeline/dng_render_halide.cpp)
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
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT}
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
    # Android/Vulkan Stage4 AOT variant (dng_render_stage4_split.a), not the Metal
    # one linked by the macOS-only test_device_handoff target below.
    add_executable(test_device_handoff_android tests/test_device_handoff.cpp
        src/pipeline/dng_pipeline.cpp
        src/pipeline/dng_halide_device.cpp
        src/pipeline/dng_opcodelist2_halide.cpp
        src/pipeline/dng_mosaic_halide.cpp
        src/pipeline/dng_warp_halide.cpp
        src/pipeline/dng_render_halide.cpp)
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
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT}
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
# Linux port (2026-08-28, plan T4). Two additive pieces, no existing block
# edited (plan T4 criterion 5 / spec A7 contract: an `if(APPLE)` block is never
# widened in place — that is how macOS regresses).
#
# 1. DNG_LINUX_TEST_LIBS: the Linux counterpart of the `${COREFOUNDATION_LIBRARY}
#    ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY}` frameworks
#    the `if(APPLE)` blocks below append. Those blocks are purely
#    platform-runtime linking, so each gets a sibling `if(DNG_LINUX_TEST_LIBS)`
#    block appending libdl + pthreads: the Halide runtime archive the test
#    targets link directly dlopen's libvulkan.so.1 and spawns worker threads,
#    exactly as ffi.cmake's own `elseif(UNIX AND NOT APPLE)` branch does for
#    dng_decoder_native. Guard is `UNIX AND NOT APPLE AND NOT ANDROID` (in CMake
#    APPLE implies UNIX, and Android also matches UNIX AND NOT APPLE).
#    Empty on every other platform, so the sibling blocks are no-ops there.
#
# 2. test_linux_vulkan_capability: the standalone ICD gate, mirroring
#    test_android_vulkan_capability above (lines 15-18). Deliberately links
#    NOTHING from the pipeline (no dng_decoder_native, no AOT kernels) so a
#    non-zero exit is attributable to the device rather than to the decoder.
# =============================================================================
if(UNIX AND NOT APPLE AND NOT ANDROID)
    find_package(Threads REQUIRED)
    set(DNG_LINUX_TEST_LIBS ${CMAKE_DL_LIBS} Threads::Threads)

    # Vulkan headers + loader are a *probe-only* build dependency
    # (libvulkan-dev / vulkan-headers). The production .so must keep resolving
    # libvulkan.so.1 by dlopen at runtime (spec AC-L4: no libvulkan in its
    # ldd output), so this find_package must stay confined to this target.
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
        add_executable(test_linux_vulkan_capability
            tests/linux_vulkan_capability_probe.cpp)
        target_link_libraries(test_linux_vulkan_capability PRIVATE Vulkan::Vulkan)
    else()
        # Never skip silently: a dropped target and a passing one are
        # indistinguishable in a build log otherwise (project lesson
        # 2026-08-25). WARNING, not STATUS, so it survives a quiet CI log.
        message(WARNING
            "SKIPPED test_linux_vulkan_capability because Vulkan headers/loader were "
            "not found at configure time (install libvulkan-dev). The runtime "
            "Vulkan ICD gate will NOT run.")
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

# H1 colour gate (spec section 7.5): HEIC decode vs an ImageIO reference.
# Guarded on DNG_ENABLE_HEIF because the executable calls heif_decode_rgba,
# which is not linked into dng_decoder_native in an OFF build.
if(DNG_ENABLE_HEIF)
    add_executable(test_heif_color tests/test_heif_color.cpp)
    target_include_directories(test_heif_color PRIVATE ${INC_DIR})
    target_link_libraries(test_heif_color PRIVATE dng_decoder_native)
endif()

# Phase 13 encode route: RGBA8 -> JPEG/WebP C ABI harness. Links the shipped
# dylib (not the TU) so the gate proves the EXPORTED symbols, which is what the
# Dart FFI lookup resolves.
add_executable(ceyx_encode_harness tests/ceyx_encode_harness.cpp)
target_include_directories(ceyx_encode_harness PRIVATE ${INC_DIR})
target_link_libraries(ceyx_encode_harness PRIVATE dng_decoder_native)

# P17 T2: plain-C raw pipeline contract ABI test (raw_pipeline_contract.h).
# Pure header test, no LibRaw/decoder dependency; not gated by
# DNG_ENABLE_GENERIC_RAW.
add_executable(test_raw_contract_abi tests/test_raw_contract_abi.cpp)
target_include_directories(test_raw_contract_abi PRIVATE ${INC_DIR})

# P17 R2/T5: magic-byte RawFileRouter test. src/pipeline/raw_file_router.cpp is
# already swept into dng_decoder_native by the file(GLOB_RECURSE
# NATIVE_SOURCES ...) above (~line 348); no LibRaw/DNG SDK/Halide
# dependency, not gated by DNG_ENABLE_GENERIC_RAW.
add_executable(test_raw_file_router
    tests/test_raw_file_router.cpp
    src/pipeline/raw_file_router.cpp)
target_include_directories(test_raw_file_router PRIVATE ${INC_DIR})

# P17 R2/T3: layout classification + RawGpuInput validator test.
# src/pipeline/raw_contract_validate.cpp is already swept into dng_decoder_native by
# the GLOB_RECURSE above.
add_executable(test_raw_layout_contract
    tests/test_raw_layout_contract.cpp
    src/pipeline/raw_contract_validate.cpp)
target_include_directories(test_raw_layout_contract PRIVATE ${INC_DIR})

# P17 R2/T8: shared Stage4 core + LibRaw RenderParams builder test.
# src/pipeline/raw_render_params_builder.cpp is already swept into
# dng_decoder_native by the GLOB_RECURSE above.
add_executable(test_raw_render_params tests/test_raw_render_params.cpp)
target_include_directories(test_raw_render_params PRIVATE ${INC_DIR})
target_link_libraries(test_raw_render_params PRIVATE dng_decoder_native dng_sdk)
if(APPLE)
    target_link_libraries(test_raw_render_params PRIVATE
        ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_raw_render_params PRIVATE ${DNG_LINUX_TEST_LIBS})
endif()

# -----------------------------------------------------------------------------
# B1 fix (2026-08-26, round-1 review): the LibRaw/RawSpeed3 wiring below is NOT
# a test dependency — it supplies dng_decoder_native's own usage requirements
# (libraw/ include path, the `raw` static lib, DNG_ENABLE_GENERIC_RAW=1) for
# src/libraw_frontend.cpp, which pipeline.cmake keeps in NATIVE_SOURCES
# whenever DNG_ENABLE_GENERIC_RAW is ON (default ON, CMakeLists.txt:91) —
# cross builds included. It was nevertheless sitting inside the host-only
# `if(NOT DNG_CROSS_BUILD)` block, so the Android leg of the bare watchdog run
# compiled libraw_frontend.cpp with no libraw include path
# ("fatal error: 'libraw/libraw.h' file not found").
#
# The block therefore has to run for cross builds too, so the host-only guard
# is closed here and reopened just below, after the wiring. It is deliberately
# NOT moved to third_party.cmake: that fragment is include()d first
# (CMakeLists.txt:61), i.e. BEFORE find_package(Halide) (generators.cmake:36),
# and running RawSpeed3's add_subdirectory() before Halide is configured is the
# documented zlib/`_uncompress` link hazard this block was deferred to avoid
# (see the option() comment at CMakeLists.txt:78-92). Keeping it in place keeps
# the host command order byte-identical; only the cross path gains it.
endif() # NOT DNG_CROSS_BUILD (host test targets, part 1 of 2)

# P17 T1: Generic RAW frontend (LibRaw + bundled RawSpeed3) wiring, deferred
# until after Halide is fully configured (see option() comment above).
if(DNG_ENABLE_GENERIC_RAW)
    set(LIBRAW_DIR ${THIRD_PARTY_DIR}/libraw)
    set(LIBRAW_CMAKE_OVERLAY_DIR ${THIRD_PARTY_DIR}/libraw-cmake)
    if(NOT EXISTS ${LIBRAW_DIR}/libraw/libraw.h)
        message(FATAL_ERROR
            "DNG_ENABLE_GENERIC_RAW=ON but ${LIBRAW_DIR} is missing. Run "
            "python3 native/scripts/build_deps.py fetch libraw first, or "
            "configure with -DDNG_ENABLE_GENERIC_RAW=OFF.")
    endif()
    if(NOT EXISTS ${LIBRAW_CMAKE_OVERLAY_DIR}/CMakeLists.txt)
        message(FATAL_ERROR
            "DNG_ENABLE_GENERIC_RAW=ON but ${LIBRAW_CMAKE_OVERLAY_DIR} is missing "
            "(LibRaw ships no CMakeLists.txt of its own; this project vendors the "
            "community LibRaw-cmake overlay, see PROVENANCE.md). Run "
            "python3 native/scripts/build_deps.py fetch libraw first.")
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

    # --- Desktop OpenMP (RAW decode accel round, 2026-08-27) ---------------
    # User ruling: OpenMP ON for desktop (macOS/Linux/Windows), OFF for mobile
    # (iOS/Android) per the P17 five-platform policy.
    #
    # OMP-CROSS-FIX (2026-09-01): this used to also gate OFF on DNG_CROSS_BUILD,
    # on the premise that "cross-compiling" implies "cannot build/link OpenMP
    # for the target arch". That premise is false for this project's ONLY
    # DNG_CROSS_BUILD=ON desktop leg (macOS x86_64, built on an arm64 runner):
    # DNG_CROSS_BUILD exists solely to skip Halide's two-stage AOT generator
    # scheme (the arm64 host cannot EXECUTE x86_64 generator binaries — see
    # halide_aot.cmake), not because the toolchain cannot compile/link x86_64
    # code. Apple clang on this host accepts -arch x86_64 for ordinary
    # compile+link (no execution of target-arch code required to build a
    # library), so RawSpeed3/LibRaw's OpenMP-guarded loops are exactly as
    # buildable for the x86_64 leg as for the native arm64 leg, PROVIDED an
    # x86_64 libomp is available (see the vendored/brew search below — a
    # missing binary still degrades to OFF via the explicit "no libomp found"
    # branch, never a silent skip). The true mobile precondition is ANDROID/IOS
    # (no OpenMP runtime in those NDK/iOS-SDK toolchains at all), which is
    # exactly what remains here.
    #
    # This unlocks parallelism that already exists in the vendored trees but
    # was compiled out: RawSpeed3's FujiDecompressor `#pragma omp parallel`
    # plus LibRaw's remaining `#pragma omp parallel for` loops. NOTE: LibRaw's
    # Fuji strip decode (src/decoders/fuji_compressed.cpp) no longer depends on
    # OpenMP at all -- round-2 patch 09 (2026-08-28) replaced its OpenMP branch
    # with an unconditional std::thread pool on every platform, so the Fuji
    # path stays parallel even on mobile and any OpenMP-less toolchain.
    if(ANDROID OR IOS)
        set(CEYX_ENABLE_DESKTOP_OPENMP OFF)
    else()
        set(CEYX_ENABLE_DESKTOP_OPENMP ON)
    endif()

    if(CEYX_ENABLE_DESKTOP_OPENMP AND APPLE)
        # Apple clang rejects a bare `-fopenmp` and ships no libomp, so
        # find_package(OpenMP) fails on a stock toolchain unless it is handed
        # the Homebrew runtime explicitly. Discover the prefix rather than
        # hard-coding it, so this keeps working on Intel Homebrew
        # (/usr/local), a non-default HOMEBREW_PREFIX, or a CI image.
        #
        # CAUTION (measured 2026-08-27): `brew --prefix libomp` prints a
        # plausible path even when the formula is NOT installed, so the EXISTS
        # check below is load-bearing, not defensive padding. Probing by
        # printed path alone yields a false positive and then a silent
        # non-OpenMP build.
        set(_ceyx_libomp_prefix "")

        # OMP-CROSS-FIX (2026-09-01): shared arch-verification helper. Before
        # this fix, the Homebrew-prefix branches below only checked
        # `include/omp.h` existence and never verified the *dylib's* arch —
        # harmless while this block only ran for the native (host-arch) build,
        # but removing the DNG_CROSS_BUILD gate above exposed it: this host's
        # Homebrew ships an arm64 libomp only, and the unchecked branch would
        # have confidently selected it for the x86_64 cross leg, producing an
        # arch-mismatched link failure at build time (a link error, not a
        # silent wrong-arch binary — but a late, confusing one instead of an
        # honest "not found" at configure time). Reuses the same lipo -archs
        # check the vendored-copy branch already performs.
        macro(_ceyx_omp_dylib_matches_target_arch _dir _outvar)
            set(${_outvar} FALSE)
            if(EXISTS "${_dir}/lib/libomp.dylib")
                set(_ceyx_helper_want "${CMAKE_OSX_ARCHITECTURES}")
                if(NOT _ceyx_helper_want)
                    set(_ceyx_helper_want "${CMAKE_SYSTEM_PROCESSOR}")
                endif()
                execute_process(COMMAND lipo -archs "${_dir}/lib/libomp.dylib"
                                OUTPUT_VARIABLE _ceyx_helper_have
                                OUTPUT_STRIP_TRAILING_WHITESPACE
                                ERROR_QUIET RESULT_VARIABLE _ceyx_helper_rc)
                if(_ceyx_helper_rc EQUAL 0)
                    set(${_outvar} TRUE)
                    foreach(_ceyx_helper_w IN LISTS _ceyx_helper_want)
                        if(NOT "${_ceyx_helper_have}" MATCHES "(^| )${_ceyx_helper_w}( |$)")
                            set(${_outvar} FALSE)
                        endif()
                    endforeach()
                endif()
            endif()
        endmacro()

        # Vendored copy first: native/third_party/libomp/ is committed (756 KB)
        # so a blank checkout builds with full OpenMP and no Homebrew
        # prerequisite. See its PROVENANCE.md.
        #
        # BUT it is an arm64-only dylib, not a fat binary. Preferring it
        # unconditionally would break x86_64 (Intel Mac) builds that work today
        # via Homebrew — the link would fail on an architecture mismatch. So
        # accept it only when it actually contains the architecture being built
        # for, and otherwise fall through to the prefix search below.
        set(_ceyx_vendored_omp_dir "${THIRD_PARTY_DIR}/libomp")
        if(EXISTS "${_ceyx_vendored_omp_dir}/include/omp.h"
           AND EXISTS "${_ceyx_vendored_omp_dir}/lib/libomp.dylib")
            # Target arch: CMAKE_OSX_ARCHITECTURES when set (possibly a list),
            # otherwise the host processor.
            set(_ceyx_want_archs "${CMAKE_OSX_ARCHITECTURES}")
            if(NOT _ceyx_want_archs)
                set(_ceyx_want_archs "${CMAKE_SYSTEM_PROCESSOR}")
            endif()
            execute_process(COMMAND lipo -archs
                                    "${_ceyx_vendored_omp_dir}/lib/libomp.dylib"
                            OUTPUT_VARIABLE _ceyx_have_archs
                            OUTPUT_STRIP_TRAILING_WHITESPACE
                            ERROR_QUIET RESULT_VARIABLE _ceyx_lipo_rc)
            set(_ceyx_arch_ok TRUE)
            if(NOT _ceyx_lipo_rc EQUAL 0)
                # Cannot prove compatibility -> do not gamble on a link failure.
                set(_ceyx_arch_ok FALSE)
            else()
                foreach(_want IN LISTS _ceyx_want_archs)
                    if(NOT "${_ceyx_have_archs}" MATCHES "(^| )${_want}( |$)")
                        set(_ceyx_arch_ok FALSE)
                    endif()
                endforeach()
            endif()
            if(_ceyx_arch_ok)
                set(_ceyx_libomp_prefix "${_ceyx_vendored_omp_dir}")
                message(STATUS
                    "[ceyx] desktop OpenMP: using VENDORED libomp "
                    "(${_ceyx_have_archs}) at ${_ceyx_vendored_omp_dir}")
            else()
                message(STATUS
                    "[ceyx] vendored libomp has archs '${_ceyx_have_archs}' but "
                    "this build targets '${_ceyx_want_archs}'; falling back to a "
                    "system libomp. Install one (brew install libomp) or add the "
                    "missing slice to native/third_party/libomp/lib/libomp.dylib.")
            endif()
        endif()

        # OMP-CROSS-FIX (2026-09-01): arch-suffixed vendored directory, one per
        # non-default architecture, mirroring the existing heif-dist-<arch> /
        # libjxl-dist-<arch> convention (fetch_heif_deps.sh, jxl.cmake) used
        # for the macOS x86_64 cross leg's other companions. Tried only when
        # the default (host-arch) vendored copy above did not already resolve
        # a match, so a fat native/third_party/libomp/lib/libomp.dylib always
        # wins. This directory is not committed by this fix — it is the
        # landing spot for whatever trustworthy x86_64 libomp binary task
        # OMP-BINARY-SOURCE (#15) provides with recorded provenance; its
        # absence is not an error, just a cache miss that falls through to the
        # Homebrew/brew search below (and finally the explicit "not found"
        # warning, never a silent skip).
        if(NOT _ceyx_libomp_prefix)
            set(_ceyx_want_archs_suffix "${CMAKE_OSX_ARCHITECTURES}")
            if(NOT _ceyx_want_archs_suffix)
                set(_ceyx_want_archs_suffix "${CMAKE_SYSTEM_PROCESSOR}")
            endif()
            foreach(_want_arch IN LISTS _ceyx_want_archs_suffix)
                set(_ceyx_arch_omp_dir "${THIRD_PARTY_DIR}/libomp-${_want_arch}")
                if(NOT _ceyx_libomp_prefix
                   AND EXISTS "${_ceyx_arch_omp_dir}/include/omp.h"
                   AND EXISTS "${_ceyx_arch_omp_dir}/lib/libomp.dylib")
                    execute_process(COMMAND lipo -archs
                                            "${_ceyx_arch_omp_dir}/lib/libomp.dylib"
                                    OUTPUT_VARIABLE _ceyx_arch_omp_have
                                    OUTPUT_STRIP_TRAILING_WHITESPACE
                                    ERROR_QUIET RESULT_VARIABLE _ceyx_arch_omp_rc)
                    if(_ceyx_arch_omp_rc EQUAL 0
                       AND "${_ceyx_arch_omp_have}" MATCHES "(^| )${_want_arch}( |$)")
                        set(_ceyx_libomp_prefix "${_ceyx_arch_omp_dir}")
                        message(STATUS
                            "[ceyx] desktop OpenMP: using arch-vendored libomp "
                            "(${_ceyx_arch_omp_have}) at ${_ceyx_arch_omp_dir}")
                    else()
                        message(STATUS
                            "[ceyx] ${_ceyx_arch_omp_dir}/lib/libomp.dylib exists "
                            "but does not report arch '${_want_arch}' "
                            "(lipo -archs => '${_ceyx_arch_omp_have}', rc="
                            "${_ceyx_arch_omp_rc}); ignoring it.")
                    endif()
                endif()
            endforeach()
        endif()

        foreach(_candidate IN ITEMS "$ENV{HOMEBREW_PREFIX}/opt/libomp"
                                    "/opt/homebrew/opt/libomp"
                                    "/usr/local/opt/libomp")
            if(NOT _ceyx_libomp_prefix AND EXISTS "${_candidate}/include/omp.h")
                _ceyx_omp_dylib_matches_target_arch("${_candidate}" _ceyx_candidate_arch_ok)
                if(_ceyx_candidate_arch_ok)
                    set(_ceyx_libomp_prefix "${_candidate}")
                else()
                    message(STATUS
                        "[ceyx] ${_candidate} has omp.h but its libomp.dylib "
                        "does not match the target arch; skipping (would be "
                        "a wrong-arch link failure otherwise).")
                endif()
            endif()
        endforeach()
        if(NOT _ceyx_libomp_prefix)
            find_program(_ceyx_brew NAMES brew)
            if(_ceyx_brew)
                execute_process(COMMAND "${_ceyx_brew}" --prefix libomp
                                OUTPUT_VARIABLE _brew_libomp
                                OUTPUT_STRIP_TRAILING_WHITESPACE
                                ERROR_QUIET RESULT_VARIABLE _brew_rc)
                if(_brew_rc EQUAL 0 AND EXISTS "${_brew_libomp}/include/omp.h")
                    _ceyx_omp_dylib_matches_target_arch("${_brew_libomp}" _ceyx_brew_arch_ok)
                    if(_ceyx_brew_arch_ok)
                        set(_ceyx_libomp_prefix "${_brew_libomp}")
                    else()
                        message(STATUS
                            "[ceyx] brew --prefix libomp (${_brew_libomp}) has "
                            "omp.h but its libomp.dylib does not match the "
                            "target arch; skipping.")
                    endif()
                endif()
            endif()
        endif()

        if(_ceyx_libomp_prefix)
            # Hint variables consumed by FindOpenMP. Plain set() on purpose:
            # same directory-scope rationale as the R2/F4 note above, and these
            # must be visible to both subprojects' own find_package(OpenMP).
            if(EXISTS "${_ceyx_libomp_prefix}/lib/libomp.dylib")
                # --- ONE OpenMP runtime image, project-wide (2026-08-27) ---
                # Vendor libomp into the build dir HERE, at configure time, and
                # point every consumer at that copy.
                #
                # Why this is not gold-plating: scripts/bundle_macos_dylib_deps.py
                # runs POST_BUILD on dng_decoder_native and rewrites its Homebrew
                # deps to @rpath/<name>, vendoring a copy alongside. Once OpenMP
                # is on, libomp becomes such a dep, so the DYLIB would load
                # build/libomp.dylib (install name @rpath/libomp.dylib) while the
                # TEST EXECUTABLES, linking FindOpenMP's result directly, would
                # load /opt/homebrew/.../libomp.dylib. dyld keys images by install
                # name, so both get mapped: two OpenMP runtimes, two independent
                # sets of thread-team state, in one process.
                #
                # That is not a theoretical hazard -- it segfaulted
                # test_raw_end_to_end in every OpenMP worker thread
                # (tmp/verify/fuji_51..56). Linking the already-@rpath copy makes
                # the executables and the dylib name the SAME image, which is
                # both the fix and the reason the dylib stays self-contained for
                # distribution.
                set(_ceyx_libomp_src "${_ceyx_libomp_prefix}/lib/libomp.dylib")
                set(_ceyx_libomp_vendored "${CMAKE_BINARY_DIR}/libomp.dylib")
                if(NOT EXISTS "${_ceyx_libomp_vendored}"
                   OR "${_ceyx_libomp_src}" IS_NEWER_THAN "${_ceyx_libomp_vendored}")
                    file(COPY "${_ceyx_libomp_src}"
                         DESTINATION "${CMAKE_BINARY_DIR}"
                         FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                          GROUP_READ GROUP_EXECUTE
                                          WORLD_READ WORLD_EXECUTE)
                    execute_process(COMMAND install_name_tool -id "@rpath/libomp.dylib"
                                            "${_ceyx_libomp_vendored}"
                                    RESULT_VARIABLE _ceyx_omp_id_rc)
                    # install_name_tool invalidates the signature; ad-hoc re-sign
                    # or the image will not load under the hardened runtime.
                    execute_process(COMMAND codesign --force --sign -
                                            "${_ceyx_libomp_vendored}"
                                    RESULT_VARIABLE _ceyx_omp_sign_rc)
                    if(NOT _ceyx_omp_id_rc EQUAL 0 OR NOT _ceyx_omp_sign_rc EQUAL 0)
                        message(FATAL_ERROR
                            "[ceyx] failed to vendor libomp (install_name_tool "
                            "rc=${_ceyx_omp_id_rc}, codesign rc=${_ceyx_omp_sign_rc}). "
                            "Refusing to continue: a partially-vendored libomp "
                            "reintroduces the duplicate-OpenMP-runtime crash.")
                    endif()
                    message(STATUS "[ceyx] vendored libomp -> ${_ceyx_libomp_vendored} (@rpath/libomp.dylib)")
                endif()
                set(OpenMP_omp_LIBRARY "${_ceyx_libomp_vendored}")
            else()
                # Static libomp: linked into each image, so there is no shared
                # runtime to duplicate and no install name to reconcile.
                set(OpenMP_omp_LIBRARY "${_ceyx_libomp_prefix}/lib/libomp.a")
            endif()
            set(OpenMP_CXX_FLAGS
                "-Xpreprocessor -fopenmp -I${_ceyx_libomp_prefix}/include")
            set(OpenMP_CXX_LIB_NAMES "omp")
            set(OpenMP_C_FLAGS
                "-Xpreprocessor -fopenmp -I${_ceyx_libomp_prefix}/include")
            set(OpenMP_C_LIB_NAMES "omp")
            message(STATUS
                "[ceyx] desktop OpenMP: using libomp at ${_ceyx_libomp_prefix}")
        else()
            # Do not pretend. LibRaw's ENABLE_OPENMP silently no-ops when
            # find_package(OpenMP) fails (libraw-cmake CMakeLists.txt:212-214
            # has no REQUIRED and no erroring else), which would produce a
            # green build that is serial at runtime. Turn the request off
            # explicitly and say so, so the build log states the truth.
            set(CEYX_ENABLE_DESKTOP_OPENMP OFF)
            message(WARNING
                "[ceyx] desktop OpenMP requested but no libomp found "
                "(brew install libomp). Building WITHOUT OpenMP; the Fuji "
                "decoders fall back to project patch 09's std::thread pool.")
        endif()
    endif()

    if(CEYX_ENABLE_DESKTOP_OPENMP)
        set(WITH_OPENMP ON)
    else()
        set(WITH_OPENMP OFF)
    endif()
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

    # --- B1 (2026-08-26): cross-compile (NDK) prerequisites -----------------
    # Neither vendored subproject is cross-compile-ready out of the box, and
    # neither is patched here (third_party/ is read-only); both are steered
    # through knobs they already expose.
    #
    # W-CI1 (2026-08-28, Windows CI): this block was originally gated on
    # DNG_CROSS_BUILD alone, but its real precondition is "the host has no
    # system libjpeg/zlib for RawSpeed3's bare find_package() calls to find" —
    # which is a property of the PLATFORM, not of cross-compiling. Windows has
    # exactly that property, and third_party.cmake already treats it
    # identically to Android (`if(ANDROID OR WIN32)` at third_party.cmake:82
    # builds the same vendored libjpeg-turbo). The cross-only gate therefore
    # left Windows uncovered, and no existing platform could reveal it:
    #   Linux   - apt installs libjpeg-dev + zlib1g-dev, so the real
    #             find_package() succeeds.
    #   macOS   - Homebrew jpeg-turbo + system zlib, same.
    #   Android - covered by this very shim.
    #   Windows - none of the above; RawSpeed3 hard-errors
    #             "Did not find JPEG!" / "Did not find ZLIB!".
    # Observed on CI run 33171988843.
    if(DNG_CROSS_BUILD OR WIN32)
        # RawSpeed3 probes the CPU it is *configuring on* via three try_run()s
        # (cmake/Modules/cpu-{cache-line,page,large-page}-size.cmake), which
        # CMake refuses in cross mode. Upstream's own binary-distribution
        # switch takes the hardcoded branch of all three (64 / 4096 / 4096) and
        # additionally stops CpuMarch.cmake:4 probing `-march=native`, which is
        # meaningless for a cross build; it selects `-mtune=generic` instead.
        # Preferred over hand-seeding RAWSPEED_*_EXITCODE cache entries: the
        # values are then upstream's, not ours. They are inert for us anyway —
        # RAWSPEED_PAGESIZE/LARGEPAGESIZE have zero uses in src/librawspeed
        # (config.h.in constants only) and RAWSPEED_CACHELINESIZE has exactly
        # one, an alignas() in VC5Decompressor.cpp:859 — so no assumption about
        # the device's real page size is baked in.
        #
        # W-CI1: stays cross-only. It exists to dodge try_run() CPU probes,
        # which CMake refuses only when cross-compiling; a native Windows build
        # runs them fine, and forcing upstream's binary-distribution branch
        # there would change behaviour beyond this dependency fix.
        if(DNG_CROSS_BUILD)
            set(BINARY_PACKAGE_BUILD ON)
        endif()

        # Both subprojects call a bare find_package(JPEG) (RawSpeed3
        # cmake/src-dependencies.cmake:161, LibRaw-cmake CMakeLists.txt:189)
        # and hard-error when it fails. The NDK sysroot ships no libjpeg, but
        # third_party.cmake has already built the vendored libjpeg-turbo for
        # this ABI and left JPEG_INCLUDE_DIRS/JPEG_LIBRARIES pointing at it —
        # so supply a find module that hands those over. RawSpeed3 then builds
        # its own JPEG::JPEG from them (src-dependencies.cmake:167-173).
        #
        # A find module is used deliberately INSTEAD of seeding the standard
        # JPEG_LIBRARY / JPEG_INCLUDE_DIR *cache* entries: cache entries are
        # replayed at the start of every later configure, so the QUIET
        # find_package(JPEG) at third_party.cmake:83 would then report success
        # and skip building vendored libjpeg-turbo altogether — the same
        # reconfigure-poisoning class as the F4 zlib/Halide hazard above.
        # Same generated-shim technique as the pugixml shim further down.
        set(_dng_jpeg_shim_dir ${CMAKE_CURRENT_BINARY_DIR}/vendored-find-shims)
        file(MAKE_DIRECTORY ${_dng_jpeg_shim_dir})
        # JPEG_VERSION is required, not optional: LibRaw-cmake evaluates
        # `if(${JPEG_VERSION} LESS 80)` unquoted (CMakeLists.txt:191), which is
        # a hard CMake syntax error when the variable is empty. 62 is the
        # honest value — the vendored libjpeg-turbo is built in libjpeg 6.2 API
        # mode (WITH_JPEG7=0/WITH_JPEG8=0, jconfig.h JPEG_LIB_VERSION 62), so
        # LibRaw's JPEG8-gated DNG lossy codec (LIBRAW_USE_DNGLOSSYCODEC) is
        # OFF on Android while it is ON in the host build. That gate is off
        # this product's critical path: DNG files never reach LibRaw at all —
        # the router delegates them to the DNG SDK entry
        # (src/pipeline/raw_gpu_pipeline.cpp:561-566).
        file(WRITE ${_dng_jpeg_shim_dir}/FindJPEG.cmake
"# Generated by cmake/tests.cmake for hosts with no system libjpeg (NDK
# cross builds and Windows); resolves JPEG to the
# vendored libjpeg-turbo built by cmake/third_party.cmake in this build tree.
set(JPEG_FOUND TRUE)
set(JPEG_INCLUDE_DIRS \"${JPEG_INCLUDE_DIRS}\")
list(GET JPEG_INCLUDE_DIRS 0 JPEG_INCLUDE_DIR)
set(JPEG_LIBRARIES \"${JPEG_LIBRARIES}\")
set(JPEG_LIBRARY \"${JPEG_LIBRARIES}\")
set(JPEG_VERSION 62)
set(JPEG_VERSION_STRING \"62\")
")
        # W-CI2 (2026-08-28, Windows CI round 3): there is deliberately NO
        # FindZLIB shim here. Round 2 added one and CI run 33176207480 proved it
        # cannot work: RawSpeed3's CheckZLIB (cmake/Modules/CheckZLIB.cmake:57-66)
        # LINK-tests uncompress() and zError(), and the Windows zlib is an
        # in-tree FetchContent target (`zlibstatic`, third_party.cmake:41-56)
        # that produces no .lib until build time, so a configure-time
        # try_compile has nothing to link. The log signature was unambiguous:
        # every header/prototype check passed and only the two link checks
        # failed.
        #
        # A find-module shim can only ever assert; it cannot make a library
        # exist. So Windows CI now builds a real static zlib BEFORE configuring
        # and passes -DDNG_ZLIB_ROOT/-DZLIB_ROOT
        # (.github/workflows/windows_build.yml), which third_party.cmake:34-39
        # already documents as the supported "a user-supplied build always wins"
        # path. RawSpeed3's own find_package(ZLIB) then resolves to that real
        # install and CheckZLIB link-tests a real archive.
        #
        # Note the JPEG shim above has the same limitation -- run 33176207480
        # logged "Looking for jpeg_mem_src - not found" -- but CheckJPEGSymbols
        # does NOT hard-error on it (no SEND_ERROR, unlike CheckZLIB), so
        # RawSpeed3 simply compiles its jpeg_mem_src_int fallback
        # (decompressors/JpegDecompressor.cpp:132-137). That is the same code
        # path Android has always shipped, so the shim is left in place.

        set(CMAKE_MODULE_PATH ${_dng_jpeg_shim_dir} ${CMAKE_MODULE_PATH})
    endif()

    # Builds the `rawspeed` static target from RawSpeed3's own (real) CMake
    # build. This library is never linked into any Halide/AOT target and is
    # only ever consumed via the glue below, never exposed as a standalone
    # dependency of dng_decoder_native (spec section 6.6.1/6.6.3).
    add_subdirectory(${LIBRAW_DIR}/RawSpeed3/rawspeed rawspeed3-build EXCLUDE_FROM_ALL)

    # W-CI3 (2026-08-28, Windows CI round 4): give the decompressors object
    # library zlib's include directory.
    #
    # Upstream RawSpeed3 attaches ZLIB::ZLIB only to the aggregate `rawspeed`
    # target (cmake/src-dependencies.cmake:215), while the translation unit that
    # includes <zconf.h> -- DeflateDecompressor.cpp -- is compiled by the
    # `rawspeed_decompressors` OBJECT library. Object libraries do not inherit
    # usage requirements from the target that later consumes them, so that TU
    # gets no zlib include path. Upstream clearly knows the pattern: two files
    # away it links JPEG::JPEG to this very target
    # (src/librawspeed/decompressors/CMakeLists.txt:81-82). ZLIB just never got
    # the same line.
    #
    # The defect is universal but LATENT wherever zlib headers sit in a default
    # system include path -- Linux (zlib1g-dev), macOS (SDK), Android (NDK
    # sysroot) -- because the compiler finds zconf.h without any -I. On Windows
    # the headers live in a private prefix, so the omission becomes a hard
    # 'zconf.h' file not found (CI run 33177220892).
    #
    # Scoped to WIN32 deliberately: the other four platforms are green right now
    # and this is a convergence round, not a refactor. Making it unconditional
    # would be more correct and is recorded as a parking-lot item rather than
    # done here.
    #
    # PUBLIC, matching upstream's JPEG line, so anything consuming the object
    # library inherits the include directory too.
    if(WIN32 AND TARGET rawspeed_decompressors AND TARGET ZLIB::ZLIB)
        target_link_libraries(rawspeed_decompressors PUBLIC ZLIB::ZLIB)
        message(STATUS "[ceyx] attached ZLIB::ZLIB to rawspeed_decompressors (W-CI3)")
    endif()

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
    # Desktop OpenMP (RAW decode accel round, 2026-08-27): unlocks LibRaw's
    # remaining `#pragma omp parallel for` loops. (LibRaw's Fuji strip decode
    # is no longer among them -- round-2 patch 09 moved it to an unconditional
    # std::thread pool.) Gated by the same
    # CEYX_ENABLE_DESKTOP_OPENMP computed above (desktop ON / mobile OFF, and
    # forced OFF if no libomp was found) so LibRaw and RawSpeed3 can never
    # disagree about whether OpenMP is in play. The OpenMP hint variables set
    # above are still in scope here and are what let libraw-cmake's
    # find_package(OpenMP) (CMakeLists.txt:213) succeed under Apple clang.
    #
    # NOTE: unlike RawSpeed3 (which SEND_ERRORs when OpenMP is missing),
    # libraw-cmake silently builds serial, so the only trustworthy evidence
    # that this took effect is the "compiled with OpenMP support ... YES" line
    # at libraw-cmake/CMakeLists.txt:385 plus a timing measurement.
    if(CEYX_ENABLE_DESKTOP_OPENMP)
        set(ENABLE_OPENMP ON)
    else()
        set(ENABLE_OPENMP OFF)
    endif()
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

    # --- LCMS2 arch guard (LCMS2-X86_64, 2026-09-01) ------------------------
    # libraw-cmake ships ITS OWN cmake/modules/FindLCMS2.cmake (not CMake's
    # builtin one), which its own set(CMAKE_MODULE_PATH <its dir> ${...})
    # (CMakeLists.txt:119) always puts ahead of anything this project could
    # add to CMAKE_MODULE_PATH — so the module-shadowing shim technique used
    # for JPEG above (a generated FindJPEG.cmake placed on CMAKE_MODULE_PATH)
    # CANNOT work here; libraw's own module always wins that search. That
    # module resolves lcms2 via bare pkg-config (`pkg_check_modules(PC_LCMS2
    # lcms2)`), which — exactly like the OpenMP Homebrew-prefix search this
    # same file used to get wrong — performs NO architecture check at all.
    # Confirmed by local reproduction on this host: an x86_64 cross configure
    # (-DDNG_CROSS_BUILD=ON -DCMAKE_OSX_ARCHITECTURES=x86_64) resolves LCMS2
    # to /opt/homebrew/lib/liblcms2.dylib — the HOST's arm64-only Homebrew
    # install — and reports "Libraw will be compiled with LCMS support ...
    # YES", which would link an arm64 dylib into an x86_64 target. This is
    # the same false-positive-arch class the OpenMP fix above corrected, on a
    # dependency where the fix must take a different shape: instead of a
    # module shim, seed FindLCMS2.cmake's own find_path/find_library CACHE
    # variables before add_subdirectory() runs — find_path/find_library are
    # no-ops when their result variable is already cached, so pre-seeding
    # LCMS2_INCLUDE_DIR/LCMS2_LIBRARIES makes the vendored module use OUR
    # values (or a deliberate NOTFOUND) instead of running its own unchecked
    # search. This has none of the F4/reconfigure-poisoning hazard that ruled
    # out cache-seeding for JPEG (that hazard was specific to
    # third_party.cmake's QUIET find_package(JPEG) probe deciding whether to
    # build vendored libjpeg-turbo at all; no such vendored-build decision
    # exists for LCMS2 in this project — there is no vendored lcms2 source
    # tree, see below).
    set(_ceyx_lcms_want_archs "${CMAKE_OSX_ARCHITECTURES}")
    if(NOT _ceyx_lcms_want_archs)
        set(_ceyx_lcms_want_archs "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    if(APPLE AND NOT ANDROID AND NOT IOS)
        set(_ceyx_lcms_resolved_dir "")
        # Arch-suffixed vendored dir first (LCMS2-X86_64 landing spot,
        # mirroring third_party/libomp-<arch>/, heif-dist-<arch>,
        # libjxl-dist-<arch>): a sourced binary here always wins over the
        # host's Homebrew copy, exactly like the OpenMP vendored-dir search.
        foreach(_lcms_want_arch IN LISTS _ceyx_lcms_want_archs)
            set(_ceyx_lcms_arch_dir "${THIRD_PARTY_DIR}/lcms2-${_lcms_want_arch}")
            if(NOT _ceyx_lcms_resolved_dir
               AND EXISTS "${_ceyx_lcms_arch_dir}/include/lcms2.h"
               AND EXISTS "${_ceyx_lcms_arch_dir}/lib/liblcms2.dylib")
                execute_process(COMMAND lipo -archs
                                        "${_ceyx_lcms_arch_dir}/lib/liblcms2.dylib"
                                OUTPUT_VARIABLE _ceyx_lcms_arch_have
                                OUTPUT_STRIP_TRAILING_WHITESPACE
                                ERROR_QUIET RESULT_VARIABLE _ceyx_lcms_arch_rc)
                if(_ceyx_lcms_arch_rc EQUAL 0
                   AND "${_ceyx_lcms_arch_have}" MATCHES "(^| )${_lcms_want_arch}( |$)")
                    set(_ceyx_lcms_resolved_dir "${_ceyx_lcms_arch_dir}")
                endif()
            endif()
        endforeach()

        # Otherwise, probe the same Homebrew prefixes the OpenMP search uses,
        # but ACTUALLY CHECK the dylib's arch this time (the bug being fixed).
        if(NOT _ceyx_lcms_resolved_dir)
            foreach(_lcms_candidate IN ITEMS "$ENV{HOMEBREW_PREFIX}/opt/little-cms2"
                                              "/opt/homebrew/opt/little-cms2"
                                              "/usr/local/opt/little-cms2")
                if(NOT _ceyx_lcms_resolved_dir
                   AND EXISTS "${_lcms_candidate}/include/lcms2.h")
                    file(GLOB _ceyx_lcms_candidate_dylib
                         "${_lcms_candidate}/lib/liblcms2*.dylib")
                    list(LENGTH _ceyx_lcms_candidate_dylib _ceyx_lcms_dylib_count)
                    if(_ceyx_lcms_dylib_count GREATER 0)
                        list(GET _ceyx_lcms_candidate_dylib 0 _ceyx_lcms_first_dylib)
                        execute_process(COMMAND lipo -archs "${_ceyx_lcms_first_dylib}"
                                        OUTPUT_VARIABLE _ceyx_lcms_have
                                        OUTPUT_STRIP_TRAILING_WHITESPACE
                                        ERROR_QUIET RESULT_VARIABLE _ceyx_lcms_rc)
                        set(_ceyx_lcms_ok TRUE)
                        if(NOT _ceyx_lcms_rc EQUAL 0)
                            set(_ceyx_lcms_ok FALSE)
                        else()
                            foreach(_lcms_want_arch IN LISTS _ceyx_lcms_want_archs)
                                if(NOT "${_ceyx_lcms_have}" MATCHES "(^| )${_lcms_want_arch}( |$)")
                                    set(_ceyx_lcms_ok FALSE)
                                endif()
                            endforeach()
                        endif()
                        if(_ceyx_lcms_ok)
                            set(_ceyx_lcms_resolved_dir "${_lcms_candidate}")
                        else()
                            message(STATUS
                                "[ceyx] ${_lcms_candidate} has lcms2.h but "
                                "${_ceyx_lcms_first_dylib} reports archs "
                                "'${_ceyx_lcms_have}', not '${_ceyx_lcms_want_archs}'; "
                                "not seeding it (would be a wrong-arch link "
                                "failure otherwise).")
                        endif()
                    endif()
                endif()
            endforeach()
        endif()

        if(_ceyx_lcms_resolved_dir)
            set(LCMS2_INCLUDE_DIR "${_ceyx_lcms_resolved_dir}/include" CACHE PATH "" FORCE)
            file(GLOB _ceyx_lcms_lib "${_ceyx_lcms_resolved_dir}/lib/liblcms2*.dylib")
            list(GET _ceyx_lcms_lib 0 _ceyx_lcms_lib_first)
            set(LCMS2_LIBRARIES "${_ceyx_lcms_lib_first}" CACHE FILEPATH "" FORCE)
            message(STATUS
                "[ceyx] LCMS2: seeding arch-verified ${_ceyx_lcms_resolved_dir} "
                "(target arch(es): ${_ceyx_lcms_want_archs})")
        elseif(NOT "${_ceyx_lcms_want_archs}" STREQUAL "${CMAKE_HOST_SYSTEM_PROCESSOR}")
            # Only force an explicit NOTFOUND when we are actually
            # cross-arch'ing (target != host processor) — on the native leg,
            # leave FindLCMS2.cmake's own pkg-config search alone; it has
            # worked correctly there for the whole life of this project (see
            # CI-T11/MACOS-DEPS-DETERMINISM evidence) and forcibly seeding
            # NOTFOUND unconditionally would regress it on a machine whose
            # CMAKE_SYSTEM_PROCESSOR happens to already equal
            # CMAKE_OSX_ARCHITECTURES (the common case) for no benefit.
            #
            # No vendored lcms2 source exists in this repo (only libjpeg-turbo
            # is vendored, third_party/libjpeg-turbo/), so unlike JPEG there
            # is no from-source fallback available here — this is a genuine
            # "not producible by this leg alone" gap, tracked as
            # LCMS2-X86_64 pending a sourced binary landing at
            # third_party/lcms2-<arch>/.
            #
            # CAUTION (measured locally, two dead ends before this one):
            # (1) seeding LCMS2_INCLUDE_DIR/LIBRARIES with a "...-NOTFOUND"
            #     CACHE value does NOT work — find_path()/find_library()
            #     specifically treat any value ending in "-NOTFOUND" as "not
            #     yet searched" and re-run their own unchecked search
            #     regardless, silently overwriting it back to the wrong-arch
            #     Homebrew path.
            # (2) a plain (non-cache) `set(ENABLE_LCMS OFF)`, the pattern this
            #     file uses elsewhere (WITH_OPENMP/ENABLE_X3FTOOLS) on the
            #     assumption that the earlier `cmake_policy(SET CMP0077 NEW)`
            #     makes libraw-cmake's own option() honor it, does NOT work
            #     either — verified via this exact configure: CMake's own dev
            #     warning ("Policy CMP0077 is not set ... option is clearing
            #     the normal variable 'ENABLE_LCMS'") shows the child
            #     directory scope is NOT seeing CMP0077=NEW, and the
            #     "-- Check for LCMS2 availability..." / "Found LCMS2 ...
            #     YES" lines confirm ENABLE_LCMS stayed ON regardless. (The
            #     sibling ENABLE_OPENMP=OFF case in this same file happens to
            #     end up correct anyway, but NOT because the option() override
            #     worked — find_package(OpenMP) still runs, it just fails on
            #     its own for lack of the OpenMP_*_FLAGS hint variables when
            #     no libomp was found. That is a latent, currently-harmless
            #     bug in the existing ENABLE_OPENMP/ENABLE_X3FTOOLS override
            #     pattern, out of this task's scope — flagged to the lead
            #     separately, not fixed here.) LCMS2 has no such accidental
            #     safety net, so the only mechanism proven to actually work
            #     here is a FORCEd CACHE BOOL, which option() unconditionally
            #     leaves alone once the cache entry already exists.
            set(ENABLE_LCMS OFF CACHE BOOL "Disabled: no ${_ceyx_lcms_want_archs} liblcms2 available (OMP-CROSS-FIX/LCMS2-X86_64)" FORCE)
            message(WARNING
                "[ceyx] LCMS2: no ${_ceyx_lcms_want_archs} liblcms2.dylib found "
                "(checked third_party/lcms2-${_ceyx_lcms_want_archs}/ and Homebrew "
                "little-cms2, which is host-arch (${CMAKE_HOST_SYSTEM_PROCESSOR}) "
                "only on this machine). Building WITHOUT LCMS2 colour "
                "management for this leg instead of silently linking a "
                "wrong-arch dylib. See docs/logs LCMS2-X86_64.")
        endif()
    endif()

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
    if(WIN32)
        # rawspeed3_capi.h:5-13 picks __declspec(dllexport) vs (dllimport) on
        # RAWSPEED_BUILDLIB. We COMPILE those functions into `raw`, so without
        # the macro clang-cl sees dllimport on a definition and hard-errors
        # ("dllimport cannot be applied to non-inline function definition", 6x,
        # run 33178093994). Non-Windows builds are unaffected: DllDef expands
        # to nothing outside _MSC_VER.
        target_compile_definitions(raw PRIVATE RAWSPEED_BUILDLIB)
    endif()
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

    # P17 T6: the single generic decoder owner. src/pipeline/libraw_frontend.cpp is
    # already in NATIVE_SOURCES via GLOB_RECURSE (~line 348, filtered out when
    # this option is OFF), so dng_decoder_native only needs LibRaw's usage
    # requirements here. Keyword-less signature deliberately, to match the
    # existing target_link_libraries(dng_decoder_native dng_sdk) at ~line 368
    # (CMake forbids mixing plain and PRIVATE/PUBLIC forms on one target).
    if(TARGET dng_decoder_native)
        target_link_libraries(dng_decoder_native libraw_vendored)
    endif()
endif() # DNG_ENABLE_GENERIC_RAW (LibRaw/RawSpeed3 wiring — all builds)

# TEMPORARY B1 PROBE — removed before commit.
if(DNG_B1_PROBE AND TARGET dng_decoder_native)
    get_target_property(_b1_libs dng_decoder_native LINK_LIBRARIES)
    message(STATUS "B1_PROBE_LINK_LIBRARIES=${_b1_libs}")
    if(TARGET libraw_vendored)
        get_target_property(_b1_inc libraw_vendored INTERFACE_INCLUDE_DIRECTORIES)
        get_target_property(_b1_def libraw_vendored INTERFACE_COMPILE_DEFINITIONS)
        message(STATUS "B1_PROBE_LIBRAW_VENDORED_INCLUDES=${_b1_inc}")
        message(STATUS "B1_PROBE_LIBRAW_VENDORED_DEFS=${_b1_def}")
    else()
        message(STATUS "B1_PROBE_LIBRAW_VENDORED=ABSENT")
    endif()
    if(TARGET raw)
        get_target_property(_b1_raw_type raw TYPE)
        message(STATUS "B1_PROBE_RAW_TARGET_TYPE=${_b1_raw_type}")
    else()
        message(STATUS "B1_PROBE_RAW_TARGET=ABSENT")
    endif()
endif()

# B1 fix: host-only guard reopened; everything from here on is test-target
# territory again. The `endif()` at the very bottom of this file closes it, and
# the `endif()` closing the DNG_ENABLE_GENERIC_RAW block below now closes the
# reopened `if(DNG_ENABLE_GENERIC_RAW)` on the next line.
if(NOT DNG_CROSS_BUILD)
if(DNG_ENABLE_GENERIC_RAW)

    add_executable(test_libraw_frontend
        tests/test_libraw_frontend.cpp
        src/pipeline/libraw_frontend.cpp
        src/pipeline/raw_contract_validate.cpp)
    target_include_directories(test_libraw_frontend PRIVATE ${INC_DIR})
    target_link_libraries(test_libraw_frontend PRIVATE libraw_vendored)

    # P17 T7: the single LibRaw -> RawGpuInput adapter and its test.
    # (F-R4-05: the round-4 EXISTS guard is gone — the sources are committed,
    # and a guarded target drops silently with no red signal.)
    add_executable(test_libraw_adapter
        tests/test_libraw_adapter.cpp
        src/pipeline/libraw_frontend.cpp
        src/pipeline/libraw_gpu_input_adapter.cpp
        src/pipeline/raw_contract_validate.cpp)
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
    if(DNG_LINUX_TEST_LIBS)
        target_link_libraries(test_raw_end_to_end ${DNG_LINUX_TEST_LIBS})
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
    if(DNG_LINUX_TEST_LIBS)
        target_link_libraries(test_raw_hardening ${DNG_LINUX_TEST_LIBS})
    endif()
endif()

# P17 T1: CPU-only LibRaw smoke test. Links neither Halide nor
# dng_decoder_native (spec: prove the dependency before any GPU work).
if(DNG_ENABLE_GENERIC_RAW)
    add_executable(libraw_smoke tests/libraw_smoke.cpp)
    target_link_libraries(libraw_smoke PRIVATE libraw_vendored)
endif()

# Scaled decode gate for the LibRaw path (contract AC-2). Links the production
# dylib so the real exported C ABI + scaled Stage4 dispatch are under test.
if(DNG_ENABLE_GENERIC_RAW)
    add_executable(test_raw_sized_decode tests/test_raw_sized_decode.cpp)
    target_include_directories(test_raw_sized_decode PRIVATE ${INC_DIR})
    target_link_libraries(test_raw_sized_decode dng_decoder_native libraw_vendored)
    if(APPLE)
        target_link_libraries(test_raw_sized_decode
            ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY}
            ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
    endif()
    if(DNG_LINUX_TEST_LIBS)
        target_link_libraries(test_raw_sized_decode ${DNG_LINUX_TEST_LIBS})
    endif()
endif()


# W5-4 / TD-2: Device handoff path independent test.
# Validates that Stage3→Stage4 (lossless) and Stage2→Stage4 (lossy) device
# handoff paths produce bit-identical (PSNR ≥99dB) output vs host-copy fallback.
add_executable(test_device_handoff tests/test_device_handoff.cpp
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp
    src/pipeline/dng_render_halide.cpp)
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
# F-T4-1 (found by T3's Linux run): the mirror image of the
# `if(NOT DNG_STAGE4_SPLIT_KERNEL)` scaled_preavg block above. This target
# compiles dng_render_halide.cpp, whose split branch calls
# dng_render_stage4_split() (dng_render_halide.cpp:1045,1348), so wherever the
# split kernel is the generated one the archive must be linked here too —
# ffi.cmake:69-75 already does exactly this for dng_decoder_native, which is why
# the .so linked cleanly on Linux while these executables did not.
# `dng_render_android_aot_target` (halide_aot.cmake:208) is the custom target
# that produces the archive; the TARGET guard mirrors ffi.cmake:20.
if(DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_device_handoff
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})
    if(TARGET dng_render_android_aot_target)
        add_dependencies(test_device_handoff dng_render_android_aot_target)
    endif()
endif()
if(APPLE)
    target_link_libraries(test_device_handoff ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_device_handoff ${DNG_LINUX_TEST_LIBS})
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
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_stage4_scaled ${DNG_LINUX_TEST_LIBS})
endif()


# Real-photograph AC7 measurement for both sized-kernel variants, plus viewable
# image output. NOTE: tests/test_stage4_scaled_photo.cpp #includes
# src/pipeline/dng_render_halide.cpp directly (to reach the production buildRenderParams
# without editing a production source), so that file must NOT be listed here as
# a separate source or every symbol in it would be defined twice.
add_executable(test_stage4_scaled_photo tests/test_stage4_scaled_photo.cpp
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp)
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
# F-T4-1: tests/test_stage4_scaled_photo.cpp #includes dng_render_halide.cpp
# (see the add_executable note above), so it pulls in the same
# dng_render_stage4_split() call and needs the archive when the split kernel is
# the generated one.
if(DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_stage4_scaled_photo
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})
    if(TARGET dng_render_android_aot_target)
        add_dependencies(test_stage4_scaled_photo dng_render_android_aot_target)
    endif()
endif()
if(APPLE)
    target_link_libraries(test_stage4_scaled_photo ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_stage4_scaled_photo ${DNG_LINUX_TEST_LIBS})
endif()


# R2 sized decode acceptance gate (AC5 extent / AC5-D crop-vs-scale / AC6 memory).
# Drives the PRODUCTION sized entry (dng_pipeline_decode_to_rgb_sized) and
# compares against a same-ordering CPU reference rendered through the production
# Stage4 AOT. Like test_stage4_scaled_photo it #includes dng_render_halide.cpp to
# reach buildRenderParams, so that file must NOT be listed as a separate source
# here or every symbol in it would be defined twice.
add_executable(test_sized_decode tests/test_sized_decode.cpp
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp)
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
# F-T4-1: tests/test_sized_decode.cpp #includes dng_render_halide.cpp (see the
# add_executable note above), so the split archive is required here too.
if(DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_sized_decode
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})
    if(TARGET dng_render_android_aot_target)
        add_dependencies(test_sized_decode dng_render_android_aot_target)
    endif()
endif()
if(APPLE)
    target_link_libraries(test_sized_decode ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_sized_decode ${DNG_LINUX_TEST_LIBS})
endif()


# Color accuracy / visual regression test
add_executable(test_color_accuracy tests/test_color_accuracy.cpp)
target_include_directories(test_color_accuracy PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_color_accuracy dng_sdk)
if(APPLE)
    target_link_libraries(test_color_accuracy ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_color_accuracy ${DNG_LINUX_TEST_LIBS})
endif()

add_executable(test_dng_layout tests/test_dng_layout.cpp)
target_include_directories(test_dng_layout PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_layout dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_layout ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_dng_layout ${DNG_LINUX_TEST_LIBS})
endif()

# Tile testing tool
add_executable(test_dng_tiles tests/test_dng_tiles.cpp)
target_include_directories(test_dng_tiles PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_tiles dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_tiles ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_dng_tiles ${DNG_LINUX_TEST_LIBS})
endif()


add_executable(test_dng_preview tests/test_dng_preview.cpp)
target_include_directories(test_dng_preview PRIVATE ${INC_DIR} ${DNG_SDK_DIR})
target_link_libraries(test_dng_preview dng_sdk)
if(APPLE)
    target_link_libraries(test_dng_preview ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_dng_preview ${DNG_LINUX_TEST_LIBS})
endif()

# DNG SDK Decode Pipeline Test Tool (with Halide Stage3 demosaic)
add_executable(test_decode tests/test_decode.cpp
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp
    src/pipeline/dng_render_halide.cpp)
target_include_directories(test_decode PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/pipeline
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
# F-T4-1: test_decode compiles dng_render_halide.cpp as a source, so it needs
# the split archive on every split-kernel platform (see the test_device_handoff
# block above for the full rationale).
if(DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_decode
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})
    if(TARGET dng_render_android_aot_target)
        add_dependencies(test_decode dng_render_android_aot_target)
    endif()
endif()
if(APPLE)
    target_link_libraries(test_decode ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_decode ${DNG_LINUX_TEST_LIBS})
endif()

# Phase 5.3: Halide Demosaic PSNR Test
# 2026-08-16: dng_opcodelist2_halide.cpp / dng_halide_device.cpp added to the
# source list. libdng_sdk's dng_opcode_list::Apply references the Stage2
# OpcodeList2 Halide bridge symbols unconditionally, so this target had been
# failing to link since that bridge landed (no test_demosaic_halide binary was
# ever produced). Linking the bridge in restores the target; the polynomial
# AOT archives below satisfy the bridge's own kernel references.
add_executable(test_demosaic_halide tests/test_demosaic_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_halide_device.cpp)
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
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_demosaic_halide ${DNG_LINUX_TEST_LIBS})
endif()

# 2026-08-16 CFA phase: all-four-Bayer-phases unit check on a synthetic
# mosaic. Covers both the Halide AOT kernel and the CPU reference demosaic
# plus get_cfa_pattern's phase expansion. No DNG fixture required.
add_executable(test_cfa_phase tests/test_cfa_phase.cpp
    src/pipeline/dng_mosaic_halide.cpp)
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
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_cfa_phase ${DNG_LINUX_TEST_LIBS})
endif()

# P17 T9: fused normalize + Bayer demosaic AOT kernel vs same-algorithm CPU
# reference (>=99 dB / max_abs<=1) plus the constant-field phase oracle.
add_executable(test_raw_bayer_kernel
    tests/test_raw_bayer_kernel.cpp
    src/pipeline/raw_demosaic_reference.cpp)
target_include_directories(test_raw_bayer_kernel PRIVATE ${INC_DIR} ${HALIDE_OUTPUT_DIR} ${HALIDE_DIR}/include)
# raw_linear_rgb_normalize: not used by this test's own code, but the shared
# src/pipeline/raw_demosaic_reference.cpp gained a call into that AOT kernel in
# P19. Only test_raw_linear_rgb_kernel (added in P19) was given the library and
# dependency; these two P17 targets compile the same source and so fail to link
# with undefined _raw_linear_rgb_normalize. Mirrors tests.cmake:1001-1008.
add_dependencies(test_raw_bayer_kernel raw_bayer_demosaic_aot_target
                 raw_xtrans_demosaic_aot_target
                 raw_linear_rgb_normalize_aot_target halide_runtime_target)
target_link_libraries(test_raw_bayer_kernel
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT})
if(APPLE)
    target_link_libraries(test_raw_bayer_kernel
        ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_raw_bayer_kernel ${DNG_LINUX_TEST_LIBS})
endif()

# P17 T11: fused normalize + X-Trans 6x6 demosaic AOT kernel vs same-formula
# CPU reference (>=99 dB / max_abs<=1), plus the 5x5 coverage property and the
# constant-field oracle.
add_executable(test_raw_xtrans_kernel
    tests/test_raw_xtrans_kernel.cpp
    src/pipeline/raw_demosaic_reference.cpp)
target_include_directories(test_raw_xtrans_kernel PRIVATE ${INC_DIR} ${HALIDE_OUTPUT_DIR} ${HALIDE_DIR}/include)
# Same P19 shared-source gap as test_raw_bayer_kernel above; mirrors
# tests.cmake:1001-1008.
add_dependencies(test_raw_xtrans_kernel raw_xtrans_demosaic_aot_target
                 raw_bayer_demosaic_aot_target
                 raw_linear_rgb_normalize_aot_target halide_runtime_target)
target_link_libraries(test_raw_xtrans_kernel
    ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
    ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT})
if(APPLE)
    target_link_libraries(test_raw_xtrans_kernel
        ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_raw_xtrans_kernel ${DNG_LINUX_TEST_LIBS})
endif()

# P19 T7: linear-RGB normalize AOT kernel vs same-formula CPU reference
# (>=99 dB / max_abs<=1), plus the constant-field oracle and a strided source.
add_executable(test_raw_linear_rgb_kernel
    tests/test_raw_linear_rgb_kernel.cpp
    src/pipeline/raw_demosaic_reference.cpp)
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
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_raw_linear_rgb_kernel ${DNG_LINUX_TEST_LIBS})
endif()

# Debug demosaic test
add_executable(test_demosaic_debug tests/test_demosaic_debug.cpp
    src/pipeline/dng_mosaic_halide.cpp)
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
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_demosaic_debug ${DNG_LINUX_TEST_LIBS})
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
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_render_halide ${DNG_LINUX_TEST_LIBS})
endif()

# --- Codec expansion (2026-08-30) ------------------------------------------
# test_abi_layout has no external codec dependency: it only pins struct layouts
# and error-code values, so it builds even on a platform with no dist at all.
add_executable(test_abi_layout tests/test_abi_layout.cpp)
target_include_directories(test_abi_layout PRIVATE ${INC_DIR})
target_link_libraries(test_abi_layout PRIVATE dng_decoder_native)

# The three codec round-trip targets are registered HERE, in advance, each
# guarded on its source file existing. That is deliberate: Tasks 7/8/9 are
# meant to run in parallel and must not contend over this file. A guard that
# is false today simply means that task has not landed yet.
foreach(_codec_test test_codec_roundtrip test_codec_heif test_codec_jxl)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/${_codec_test}.cpp")
        add_executable(${_codec_test} tests/${_codec_test}.cpp)
        target_include_directories(${_codec_test} PRIVATE ${INC_DIR} ${SRC_DIR})
        target_link_libraries(${_codec_test} PRIVATE dng_decoder_native)
        message(STATUS "codec test enabled: ${_codec_test}")
    else()
        # Printed, never silent: a skipped target and a passing one must not
        # look the same in a build log.
        message(STATUS "codec test SKIPPED (source absent): ${_codec_test}")
    endif()
endforeach()

endif() # NOT DNG_CROSS_BUILD (test targets)

endif() # NOT DNG_HOST_GENERATORS_ONLY (entire runtime section)
