# tests_early.cmake - Android cross-build tests and host test targets (part 1)
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 938-1753)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.

# Re-guard (split mechanics): continuation of the `if(NOT DNG_HOST_GENERATORS_ONLY)`
# block that ffi.cmake had to close at its end; see the note there.
if(NOT DNG_HOST_GENERATORS_ONLY)

# Threads is needed by host test targets on every platform (e.g.
# test_normalize_model_race below), not just Linux. The Linux-only
# find_package inside the `if(UNIX AND NOT APPLE AND NOT ANDROID)` block meant
# the imported target Threads::Threads did not exist on Windows/macOS, so the
# Windows CMake configure failed. Resolve it once here, at file scope.
find_package(Threads REQUIRED)

# =============================================================================
# T1 (2026-10-02 techdebt campaign): ONE declaration of what a statically
# linked pipeline test compiles and links.
#
# Several test executables compile the DNG pipeline sources directly instead
# of linking dng_decoder_native (the R2-T2 note at test_concurrent_decode says
# why that shape exists). Each used to carry a hand-copied source list and a
# hand-copied link block; drift between those copies already invalidated one
# run of evidence. The lists and the link sequence now live here once.
#
# The link helpers are FUNCTIONS, not an INTERFACE library, on purpose: they
# issue the same target_link_libraries()/add_dependencies() calls in the same
# order the copies did, so every link line is byte-identical by construction.
# Static archive order decides which duplicate symbol wins (see the Vulkan
# fork note in the Android block), so never reorder these lists.
#
# Deliberately NOT routed through the helpers:
#   test_concurrent_decode sources - literal list kept: its object order puts
#       dng_metal_context.cpp/dng_copy_lock.cpp third/fourth, and object order
#       can change static-initializer order. (Its link block uses the helper.)
#   test_stage4_oriented link block - literal: it has no split-kernel archive
#       block, unlike the helper.
#   test_sized_decode - #includes dng_render_halide.cpp instead of compiling
#       it, and links the pre-average pair at a different position.
# =============================================================================
set(CEYX_PIPELINE_STATIC_SOURCES
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp
    src/pipeline/dng_render_halide.cpp
    # C3 param cache (plan 2026-09-11): dng_render_halide.cpp references the
    # render-parameter upload cache, whose Metal body needs the shared device
    # handle from dng_metal_context.cpp (production context).
    src/pipeline/render_parameter_upload_cache.cpp
    src/pipeline/raw_persistent_device_arena.cpp
    src/pipeline/dng_metal_context.cpp)
# The Android cross copies compile the first six only.
set(CEYX_PIPELINE_STATIC_SOURCES_ANDROID
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp
    src/pipeline/dng_render_halide.cpp)

# Host helper. Keyword-less target_link_libraries() signature, because
# test_concurrent_decode receives further keyword-less calls later (generic-RAW
# block) and CMake forbids mixing the plain and PRIVATE forms on one target.
function(ceyx_link_pipeline_static target)
    if(DNG_USE_LIBJPEG)
        target_link_libraries(${target} dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT} ${JPEG_LIBRARIES})
    else()
        target_link_libraries(${target} dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})
    endif()
    # R2 sized decode: a target compiling dng_render_halide.cpp needs the
    # scaled kernel the sized dispatch calls (non-split branch only).
    if(NOT DNG_STAGE4_SPLIT_KERNEL)
        target_link_libraries(${target} ${DNG_STAGE4_NONSPLIT_AOT_LIBS})
        add_dependencies(${target} ${DNG_STAGE4_NONSPLIT_AOT_TARGETS})
    endif()
    add_dependencies(${target} halide_runtime_target)
    add_dependencies(${target} dng_demosaic_aot_target)
    add_dependencies(${target} dng_demosaic_warp_aot_target)
    add_dependencies(${target} dng_warp_aot_target)
    add_dependencies(${target} dng_render_aot_target)
    add_dependencies(${target} dng_opcode_polynomial_aot_target)
    add_dependencies(${target} dng_opcode_polynomial3_aot_target)
    # T20-fix F1 / mem8 v3 T12: dng_render_halide.cpp dispatches the fused
    # Bayer pair and the yuv420 Stage-4 variant, so every target compiling it
    # links them, exactly as ffi.cmake does for the shipping library.
    target_link_libraries(${target}
        ${DNG_FUSED_BAYER_AOT_LIBS}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_yuv420${DNG_AOT_LIB_EXT})
    if(TARGET raw_bayer_fused_render_aot_target)
        add_dependencies(${target} ${DNG_FUSED_BAYER_AOT_TARGETS})
        add_dependencies(${target} dng_render_yuv420_aot_target)
    endif()
    # F-T4-1: the split branch of dng_render_halide.cpp calls
    # dng_render_stage4_split(), so split-kernel builds link that pair here
    # too, mirroring ffi.cmake.
    if(DNG_STAGE4_SPLIT_KERNEL)
        target_link_libraries(${target} ${DNG_STAGE4_SPLIT_AOT_LIBS})
        add_dependencies(${target} ${DNG_STAGE4_SPLIT_AOT_TARGETS})
    endif()
    if(APPLE)
        target_link_libraries(${target} ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
    endif()
    if(DNG_LINUX_TEST_LIBS)
        target_link_libraries(${target} ${DNG_LINUX_TEST_LIBS})
    endif()
endfunction()

# Android cross helper. PRIVATE signature, as those blocks always used.
# Extra archives passed after the target name are linked immediately after
# the split pair (test_decode_android passes the split probe archive).
# JPEG_LIBRARIES is read at call time: the Android blocks run BEFORE the
# generic-RAW FindJPEG shim, so they keep the pre-shim value, as before.
function(ceyx_link_pipeline_static_android target)
    target_link_libraries(${target} PRIVATE
        dng_sdk
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT}
        ${DNG_STAGE4_SPLIT_AOT_LIBS}
        ${ARGN}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT}
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})
    target_compile_definitions(${target} PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
    if(DNG_USE_LIBJPEG)
        target_link_libraries(${target} PRIVATE ${JPEG_LIBRARIES})
    endif()
    # T20-fix F1 / mem8 v3 T12: see ceyx_link_pipeline_static above.
    target_link_libraries(${target} PRIVATE
        ${DNG_FUSED_BAYER_AOT_LIBS}
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_yuv420${DNG_AOT_LIB_EXT})
endfunction()

# Phase 14 W0 acceptance smoke: Android cross-build test binary for ADB.
# Full host test targets stay under the NOT DNG_CROSS_BUILD block below.
if(ANDROID AND DNG_CROSS_BUILD)
    add_executable(test_android_vulkan_capability
        tests/android_vulkan_capability_probe.cpp)
    target_link_libraries(test_android_vulkan_capability PRIVATE
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})

    add_executable(test_decode_android tests/test_decode.cpp
        ${CEYX_PIPELINE_STATIC_SOURCES_ANDROID})
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
    ceyx_link_pipeline_static_android(test_decode_android
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split_probe${DNG_AOT_LIB_EXT})
    add_dependencies(test_decode_android test_android_vulkan_capability)

    # P14-W4-4 measurement: Android cross-build of the production C ABI harness.
    # Drives the FFI entry dng_decode_and_process -> dng_pipeline_decode_to_rgb
    # -> decodeStages -> runHalideStage3And4Fused (the device-handoff path that
    # test_decode_android bypasses). Lets us measure on cc5bf709 whether device
    # handoff actually triggers and what [Stage4-Perf] FromDevice: reports.
    # Measurement-only target; no kernel / device-ownership code is touched.
    add_executable(dng_ffi_harness_android tests/dng_ffi_harness.cpp
        src/ffi/dng_ffi_api.cpp
        src/ffi/heap_page_return.cpp
        ${CEYX_PIPELINE_STATIC_SOURCES_ANDROID})
    target_include_directories(dng_ffi_harness_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    ceyx_link_pipeline_static_android(dng_ffi_harness_android)

    # matrix-eng ask (2026-07-04, Task #3): Android cross-build of the device-handoff
    # PSNR gate (Stage3->Stage4 device-dirty handoff vs host-copy fallback), mirroring
    # dng_ffi_harness_android above but for tests/test_device_handoff.cpp. Uses the
    # Android/Vulkan Stage4 AOT variant (dng_render_stage4_split.a), not the Metal
    # one linked by the macOS-only test_device_handoff target below.
    add_executable(test_device_handoff_android tests/test_device_handoff.cpp
        ${CEYX_PIPELINE_STATIC_SOURCES_ANDROID})
    target_include_directories(test_device_handoff_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    ceyx_link_pipeline_static_android(test_device_handoff_android)

    # T-V0 (2026-09-19, spec-cpu-levers.md section 3.3b): Android cross-build of
    # the generic-RAW Bayer kernel oracle. The L3 staged producer
    # (`normalized.compute_at(dst, xo)`, RawBayerDemosaicGenerator.cpp:74) ships
    # on three Vulkan platforms and has never been executed on a Vulkan device;
    # the v21 materialized-producer channel-collapse landmine
    # (DngRenderGenerator.cpp:526-530) is what this target exists to rule in or
    # out. Same source as the host test_raw_bayer_kernel target below, so the
    # oracle cannot drift between platforms.
    add_executable(test_raw_bayer_kernel_android
        tests/test_raw_bayer_kernel.cpp
        src/pipeline/raw_demosaic_reference.cpp)
    target_include_directories(test_raw_bayer_kernel_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    # A Vulkan target must produce a device allocation; assert it rather than
    # letting a silent CPU fallback pass as a Vulkan green.
    target_compile_definitions(test_raw_bayer_kernel_android PRIVATE
        DNG_EXPECT_GPU_DEVICE=1)
    target_link_libraries(test_raw_bayer_kernel_android PRIVATE
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT}
        ${VULKAN_LIBRARY}
        ${LOG_LIBRARY})

    # Task 10 (2026-09-19, cpu-levers second-client campaign): Android
    # cross-build of the same probe_concurrent_raw.cpp driver already used for
    # the Metal 1-vs-N-client wall-clock measurement (host target at line
    # ~1793 below, host-only guard). No source-file changes -- identical
    # sources, linked against the shipped dng_decoder_native SHARED lib
    # exactly like the host target does, so the on-device binary drives the
    # same production FFI entry (raw_pipeline_decode_file_into) as the .so
    # already pushed to jniLibs. Coarse-grained (whole-decode wall_ms) only:
    # this device has no per-kernel Vulkan timing (raw_gpu_timing_probe.cpp's
    # CEYX_GPU_TIMING is Metal-only, stubbed to "disabled" off-Apple) -- see
    # docs/logs/2026-09-19/cpu-levers-final-breakdown.md Task 10 gap report.
    add_executable(probe_concurrent_raw_android tests/probe_concurrent_raw.cpp)
    target_include_directories(probe_concurrent_raw_android PRIVATE ${INC_DIR})
    target_link_libraries(probe_concurrent_raw_android PRIVATE dng_decoder_native)
    add_dependencies(probe_concurrent_raw_android dng_decoder_native)

    # mem8 v3 T12 milestone 4, on-device leg (2026-09-20): Android cross-build
    # of the SAME tests/test_stage4_yuv420_output.cpp the host target below
    # builds. No source-file change -- identical sources, linked against the
    # shipped dng_decoder_native SHARED lib exactly as the host target does, so
    # a Metal/Vulkan comparison cannot drift because the two binaries were
    # built from different code. The suite self-labels its arm via host_arm(),
    # which reports the split (Vulkan) arm automatically under
    # DNG_STAGE4_SPLIT_KERNEL, so the on-device artifact is self-describing.
    # libjpeg: Y4a compresses with libjpeg-turbo directly, and the Android
    # branch of third_party.cmake builds the vendored jpeg-static, so
    # ${JPEG_LIBRARIES} resolves here exactly as it does for the host target.
    add_executable(test_stage4_yuv420_output_android
        tests/test_stage4_yuv420_output.cpp)
    target_include_directories(test_stage4_yuv420_output_android PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include
        ${JPEG_INCLUDE_DIRS})
    target_link_libraries(test_stage4_yuv420_output_android PRIVATE
        dng_decoder_native
        ${JPEG_LIBRARIES}
        ${LOG_LIBRARY})
    # host_arm() and Y1's golden-literal SCOPING both key off
    # DNG_STAGE4_SPLIT_KERNEL (test_stage4_yuv420_output.cpp). The macro is
    # defined for every target by cmake/halide_aot.cmake from the same variable
    # that selects the library's Stage4 archives, so this binary's view of the
    # build configuration is identical to dng_decoder_native's.
    add_dependencies(test_stage4_yuv420_output_android dng_decoder_native)

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
        # T6: this attaches the fork object to the SHIPPED dng_decoder_native.
        # It stays in this test file because the fork object is defined only
        # in this Android-cross block; moving it would change execution order.
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

# native-rotation spec Task 1: pure 8-case EXIF orientation pass. No
# Halide/DNG-SDK/LibRaw dependency (deliberately standalone, spec §1.1/Task 1
# constraint), so this links directly against the one source file rather than
# the whole decoder library, same pattern as test_raw_layout_contract above.
add_executable(test_ceyx_orient
    tests/test_ceyx_orient.cpp
    tests/oracle/ceyx_orient_oracle.cpp)
target_include_directories(test_ceyx_orient PRIVATE ${INC_DIR})

# Round 1 Task 1.2: histogram-based auto-exposure estimator, plain math over a
# caller-supplied buffer view -- no LibRaw/Halide/DNG SDK dependency, so this
# links directly against the two sources rather than the whole decoder
# library, same pattern as test_raw_layout_contract above.
add_executable(test_raw_auto_exposure
    tests/test_raw_auto_exposure.cpp
    src/pipeline/raw_auto_exposure.cpp)
target_include_directories(test_raw_auto_exposure PRIVATE ${INC_DIR})

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

# GPU copy elimination campaign (plan docs/logs/2026-09-11): AC5 param-cache
# counter gate and the AC4 reference-hash baseline driver. Both link the
# production dylib and are driven locally, never in CI.
add_executable(test_render_parameter_cache tests/test_render_parameter_cache.cpp)
target_include_directories(test_render_parameter_cache PRIVATE ${INC_DIR})
target_link_libraries(test_render_parameter_cache PRIVATE dng_decoder_native)

add_executable(raw_corpus_hash_baseline tests/raw_corpus_hash_baseline.cpp)
target_include_directories(raw_corpus_hash_baseline PRIVATE ${INC_DIR})
target_link_libraries(raw_corpus_hash_baseline PRIVATE dng_decoder_native)

add_executable(raw_corpus_ev_gate tests/raw_corpus_ev_gate.cpp)
target_include_directories(raw_corpus_ev_gate PRIVATE ${INC_DIR})
target_link_libraries(raw_corpus_ev_gate PRIVATE dng_decoder_native)

# AC1 gate (plan §9.1): per-lane persistent device arena allocation counters.
add_executable(test_persistent_device_arena tests/test_persistent_device_arena.cpp)
target_include_directories(test_persistent_device_arena PRIVATE ${INC_DIR})
target_link_libraries(test_persistent_device_arena PRIVATE dng_decoder_native)

# T1 gate (mem8 SR-1): arena idle release down to a lane floor. Needs
# ${HALIDE_DIR}/include on top of ${INC_DIR}, unlike its AC1 sibling above,
# because S3 constructs a halide_buffer_t of its own to hold a live region
# binding open across a shrink call.
add_executable(test_persistent_device_arena_shrink
    tests/test_persistent_device_arena_shrink.cpp)
target_include_directories(test_persistent_device_arena_shrink PRIVATE
    ${INC_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_persistent_device_arena_shrink PRIVATE dng_decoder_native)

# T3 gate (mem8 SR-6): DNG-route DecodeContext idle decommit, cases D1-D6 plus
# acceptance item 2. Needs ${SRC_DIR}/pipeline for decode_context.h and
# ${HALIDE_DIR}/include for the HalideBuffer.h that header pulls in.
# It constructs its OWN small DecodeSlotPool and never calls decodeSlotPool():
# touching the process-wide accessor would mmap 8 x 1.5 GiB just to run a test.
add_executable(test_dng_slot_decommit tests/test_dng_slot_decommit.cpp)
target_include_directories(test_dng_slot_decommit PRIVATE
    ${INC_DIR}
    ${SRC_DIR}/pipeline
    ${HALIDE_DIR}/include)
target_link_libraries(test_dng_slot_decommit PRIVATE dng_decoder_native)

# Memory-reclamation campaign M1: the idle funnel's shared telemetry on every
# leg. F1 needs no GPU/file; F2-F4 take one decodable RAW as argv[1].
add_executable(test_idle_funnel tests/test_idle_funnel.cpp)
target_include_directories(test_idle_funnel PRIVATE
    ${INC_DIR}
    ${SRC_DIR}/pipeline
    ${HALIDE_DIR}/include)
target_link_libraries(test_idle_funnel PRIVATE dng_decoder_native)
add_dependencies(test_idle_funnel dng_decoder_native)

# Memory-reclamation campaign M5: VirtualRegion contract V1-V5, every leg.
add_executable(test_virtual_region tests/test_virtual_region.cpp)
target_include_directories(test_virtual_region PRIVATE ${SRC_DIR}/pipeline ${CMAKE_CURRENT_SOURCE_DIR}/tests)

# T3-real gate (mem8 SR-6): the same funnel, driven by a REAL DNG decode
# through the shipping FFI entry instead of a synthetic arena. Closes the one
# gap the sibling gate above structurally cannot: that the production decode
# path reaches the state the guard is meant to detect.
# Takes the DNG path as argv[1]; exits 2 (not 0) if the file cannot be decoded,
# so a skipped real decode can never read as a pass.
add_executable(test_dng_slot_decommit_real tests/test_dng_slot_decommit_real.cpp)
target_include_directories(test_dng_slot_decommit_real PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/pipeline
    ${HALIDE_DIR}/include)
target_link_libraries(test_dng_slot_decommit_real PRIVATE dng_decoder_native)
add_dependencies(test_dng_slot_decommit_real dng_decoder_native)

# Multi-lane RAW concurrency gate (R2.5): per-lane arena isolation under
# concurrent generic-RAW decodes; prerequisite evidence for C2's under-load AC4.
add_executable(test_concurrent_raw_decode tests/test_concurrent_raw_decode.cpp)
target_include_directories(test_concurrent_raw_decode PRIVATE ${INC_DIR})
target_link_libraries(test_concurrent_raw_decode PRIVATE dng_decoder_native)

# R3-T4: C2 zero-copy capability-gate path gate -- forced-fallback (AC6) and
# alignment-degraded (AC4 run 3) destination paths, both bit-exact-checked
# against a natural-gate reference and both asserted via the
# ceyx_debug_zero_copy_capability_counters probe. Same link shape as
# test_concurrent_raw_decode above: calls only through raw_pipeline_decode_
# file_into, never compiles dng_render_halide.cpp directly, so the round-2
# handoff's "list render_parameter_upload_cache.cpp + raw_persistent_device_
# arena.cpp + dng_metal_context.cpp" rule does not apply here -- one link
# against the whole library covers it, same as its sibling gate.
add_executable(test_zero_copy_capability_paths
    tests/test_zero_copy_capability_paths.cpp)
target_include_directories(test_zero_copy_capability_paths PRIVATE
    ${INC_DIR} ${SRC_DIR}/pipeline)
target_link_libraries(test_zero_copy_capability_paths PRIVATE dng_decoder_native)

# R3-T4 close-out: under-load gate for the WRAPPED destination path
# specifically (team-lead's final-round item) -- a sibling of
# test_concurrent_raw_decode, not a rewrite of it (that target stays
# untouched per this task's original instructions). Every lane here uses a
# posix_memalign'd, page-aligned destination, so it is the driver that can
# actually catch a missing/ineffective halide_device_sync fence under
# concurrency (plan §8.2 item 3) -- test_concurrent_raw_decode's plain heap
# buffers never reach the wrapped path at all. Same link shape as its
# siblings above.
add_executable(test_concurrent_raw_decode_wrapped
    tests/test_concurrent_raw_decode_wrapped.cpp)
target_include_directories(test_concurrent_raw_decode_wrapped PRIVATE
    ${INC_DIR})
target_link_libraries(test_concurrent_raw_decode_wrapped PRIVATE dng_decoder_native)

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

# T6 (2026-10-02 techdebt campaign) re-guard: the `if(NOT DNG_HOST_GENERATORS_ONLY)`
# block opened at the top of this file is closed here and re-opened at the
# top of generic_raw.cmake, because CMake requires flow-control blocks to
# balance within one file. The condition is unchanged in between.
endif() # NOT DNG_HOST_GENERATORS_ONLY (continued in generic_raw.cmake)
