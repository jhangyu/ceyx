# tests.cmake - host test executables (part 2)
#
# T6 (2026-10-02 techdebt campaign): part 1 (Android cross tests and the
# first host tests) lives in tests_early.cmake; the production generic-RAW
# wiring that used to sit between the two parts lives in generic_raw.cmake.

# Re-guard (split mechanics): see the end of generic_raw.cmake.
if(NOT DNG_HOST_GENERATORS_ONLY)

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

    # R4 item 2 (2026-09-05): multi-threaded repro for the shared-static race at
    # third_party/libraw/src/metadata/normalize_model.cpp:406
    # (`static const char *orig;` inside LibRaw::GetNormalizedModel()). Deliberately
    # links ONLY libraw_vendored -- no DNG SDK, no Halide AOT, no sample files -- so
    # it is outside the five pre-existing link-failure targets and can be built and
    # run on its own in seconds. Per user ruling r-2 a direct multi-threaded call of
    # the function is sufficient race acceptance evidence; no camera RAW is needed.
    add_executable(test_normalize_model_race tests/test_normalize_model_race.cpp)
    target_include_directories(test_normalize_model_race PRIVATE ${INC_DIR})
    target_link_libraries(test_normalize_model_race PRIVATE libraw_vendored)
    target_link_libraries(test_normalize_model_race PRIVATE Threads::Threads)

    # P17 T7: the single LibRaw -> RawGpuInput adapter and its test.
    # (F-R4-05: the round-4 EXISTS guard is gone — the sources are committed,
    # and a guarded target drops silently with no red signal.)
    # Round 1 Task 1.7: libraw_gpu_input_adapter.cpp now calls
    # raw_build_render_params/raw_render_eval_from_params (DNG SDK, and
    # transitively dng_render_halide.cpp's toIdentityHueSatMap/Curve and the
    # Halide AOT archives) to drive the auto-exposure bisection's render_eval
    # callback. Rather than hand-duplicating that dependency chain's source
    # list here, link the production library directly -- it already compiles
    # every one of these pipeline sources via NATIVE_SOURCES' GLOB_RECURSE and
    # already carries the AOT link deps test_raw_render_params relies on the
    # same way.
    add_executable(test_libraw_adapter
        tests/test_libraw_adapter.cpp)
    target_include_directories(test_libraw_adapter PRIVATE ${INC_DIR})
    target_link_libraries(test_libraw_adapter PRIVATE dng_decoder_native libraw_vendored dng_sdk)
    if(APPLE)
        target_link_libraries(test_libraw_adapter PRIVATE
            ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
    endif()
    if(DNG_LINUX_TEST_LIBS)
        target_link_libraries(test_libraw_adapter PRIVATE ${DNG_LINUX_TEST_LIBS})
    endif()

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
    ${CEYX_PIPELINE_STATIC_SOURCES})
target_include_directories(test_device_handoff PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
ceyx_link_pipeline_static(test_device_handoff)

# ceyx-gpu-orient productionization plan Task 5 / gate G-A: PRODUCTION Metal
# fused Stage4 EXIF-orientation gate (docs/logs/2026-09-07/
# gpu_orient_productionization_plan.md). Dispatches dng_render_stage4 and
# dng_render_stage4_scaled_preavg directly (not through the host bridge, so
# device_interface/device can be inspected before copy_to_host()), and uses
# native/tests/oracle/ceyx_orient_oracle.cpp as the CPU oracle (plan ruling on
# spec D1(d): ceyx_orient.cpp/.h survive Task 9 as a TEST-ONLY oracle, moved
# out of native/src/ in Task 9 Step 9.2 so it no longer ships in the dylib).
# Source set and link list mirror test_device_handoff above; this is
# macOS/Metal only (G-14: not added to CI, not gated on Vulkan/Android).
add_executable(test_stage4_oriented tests/test_stage4_oriented.cpp
    tests/oracle/ceyx_orient_oracle.cpp
    ${CEYX_PIPELINE_STATIC_SOURCES})
target_include_directories(test_stage4_oriented PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
if(DNG_USE_LIBJPEG)
    target_link_libraries(test_stage4_oriented dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT} ${JPEG_LIBRARIES})
else()
    target_link_libraries(test_stage4_oriented dng_sdk Halide::Halide ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_demosaic_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/rectilinear_warp${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_render_stage4${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial${DNG_AOT_LIB_EXT} ${HALIDE_OUTPUT_DIR}/dng_opcode_polynomial3${DNG_AOT_LIB_EXT})
endif()
if(NOT DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_stage4_oriented ${DNG_STAGE4_NONSPLIT_AOT_LIBS})
    add_dependencies(test_stage4_oriented ${DNG_STAGE4_NONSPLIT_AOT_TARGETS})
endif()
add_dependencies(test_stage4_oriented halide_runtime_target)
add_dependencies(test_stage4_oriented dng_demosaic_aot_target)
add_dependencies(test_stage4_oriented dng_demosaic_warp_aot_target)
add_dependencies(test_stage4_oriented dng_warp_aot_target)
add_dependencies(test_stage4_oriented dng_render_aot_target)
add_dependencies(test_stage4_oriented dng_opcode_polynomial_aot_target)
add_dependencies(test_stage4_oriented dng_opcode_polynomial3_aot_target)
# T20-fix F1: see the test_device_handoff block above.
target_link_libraries(test_stage4_oriented
    ${DNG_FUSED_BAYER_AOT_LIBS}
        # mem8 v3 T12: the yuv420 output variant of the SAME Stage-4 family.
        # Required on every target that compiles dng_render_halide.cpp, for
        # exactly the reason the fused archive above is (ffi.cmake links it
        # into the shipping library the same way).
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_yuv420${DNG_AOT_LIB_EXT})
if(TARGET raw_bayer_fused_render_aot_target)
    add_dependencies(test_stage4_oriented ${DNG_FUSED_BAYER_AOT_TARGETS})
    add_dependencies(test_stage4_oriented dng_render_yuv420_aot_target)
endif()
if(APPLE)
    target_link_libraries(test_stage4_oriented ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_stage4_oriented ${DNG_LINUX_TEST_LIBS})
endif()

# Task 2 (mutex rework): concurrent-vs-serial byte-for-byte correctness gate.
# Source set and link list mirror test_device_handoff above — this target also
# compiles the pipeline sources directly, so it needs the identical AOT
# archives and dependency chain (including both DNG_STAGE4_SPLIT_KERNEL
# branches and the platform link libs).
# R2-T2 (2026-09-05, parallel-decode campaign): this target is STATICALLY linked
# and lists its pipeline sources by hand, so — unlike the dylib, where
# pipeline.cmake's GLOB_RECURSE sweeps src/pipeline/*.cpp in automatically — the
# two campaign override TUs must be named here or the binary silently contains
# NEITHER (R2-T1 FINDING 3: every colour-identity run before this line was driven
# by a binary with zero queue-pool symbols, so AC5 had never been checked against
# the changed code path). dng_metal_context.cpp = R1-T2's per-thread
# MTLCommandQueue pool; dng_copy_lock.cpp = R2-T2's striped device-copy locks.
# Both are Apple-only via their own #if guards; elsewhere they are empty objects.
add_executable(test_concurrent_decode tests/test_concurrent_decode.cpp
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_metal_context.cpp
    src/pipeline/dng_copy_lock.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp
    src/pipeline/dng_render_halide.cpp
    src/pipeline/render_parameter_upload_cache.cpp
    src/pipeline/raw_persistent_device_arena.cpp)
target_include_directories(test_concurrent_decode PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/pipeline
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
ceyx_link_pipeline_static(test_concurrent_decode)

# R4 item 1: slot-configuration bookkeeping.
#
# Links the real dng_decoder_native dylib rather than recompiling the pipeline
# sources: recompiling dng_pipeline.cpp would drag the whole Halide AOT chain
# in for a bookkeeping test, and linking the dylib means the C ABI groups
# exercise the symbol AS EXPORTED FROM THE SHIPPING ARTIFACT. That is the
# lesson recorded at the test_concurrent_decode block above (a test binary
# missing the campaign's TUs silently contains none of the changed code).
#
# DecodeSlotPool is header-only (src/pipeline/decode_context.h) and needs only
# HalideBuffer.h plus the C++ stdlib — verified, no DNG SDK dependency — so the
# local-pool groups compile without any of the heavy include dirs.
# (Block authored by impl-sync-opus; applied by lead-r1-opus, tests.cmake being
# a lead-serialised shared file this round.)
add_executable(test_slot_config tests/test_slot_config.cpp)
target_include_directories(test_slot_config PRIVATE
    ${INC_DIR}
    ${SRC_DIR}/pipeline
    ${HALIDE_DIR}/include)
target_link_libraries(test_slot_config dng_decoder_native)

# R4 item 3: cold-start gate for Halide's unsynchronised
# metal_api_supports_set_bytes / metal_api_checked_device memo cache.
# EVIDENCE TRACK B: ThreadSanitizer cannot see this race — halide_runtime.a is
# emitted by the Halide AOT generator and is never compiled by our toolchain, so
# no flag can instrument the only object containing both racing accesses
# (docs/logs/2026-09-05/item3-P0-result.md). The harness instead observes the
# race's precondition directly: how many threads are inside the bracket that
# contains the racy block.
#
# Compiles dng_metal_context.cpp and dng_copy_lock.cpp DIRECTLY, per the lesson
# recorded at the test_concurrent_decode block above: a test binary that omits
# the campaign's override TUs silently contains none of the changed code and
# every green it produces is about the wrong binary.
# No RAW sample data and no decode machinery: the memo branch only needs a Metal
# kernel launch with a non-empty argument block, which dng_demosaic_bilinear's
# two scalar arguments provide.
# (Block authored by impl-copylock-opus; applied by lead-r1-opus, tests.cmake
# being a lead-serialised shared file this round.)
if(APPLE AND NOT DNG_FORCE_VULKAN)
    add_executable(test_metal_api_gate tests/test_metal_api_gate.cpp
        src/pipeline/dng_metal_context.cpp
        src/pipeline/dng_copy_lock.cpp)
    target_include_directories(test_metal_api_gate PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${SRC_DIR}/pipeline
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(test_metal_api_gate
        Halide::Halide
        ${HALIDE_OUTPUT_DIR}/halide_runtime${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/dng_demosaic_bilinear${DNG_AOT_LIB_EXT}
        ${COREFOUNDATION_LIBRARY}
        ${CORESERVICES_LIBRARY}
        ${METAL_LIBRARY}
        ${FOUNDATION_LIBRARY})
    add_dependencies(test_metal_api_gate halide_runtime_target)
    add_dependencies(test_metal_api_gate dng_demosaic_aot_target)
endif()

# R1-T3 (parallel-decode campaign): generic-RAW link closure for the dual-route.
# The harness above compiles the DNG pipeline sources directly and does NOT link
# dng_decoder_native, so reaching raw_pipeline_decode_file requires the generic
# RAW sources here too. Without this, every non-DNG input fails -2
# (kDngErrParseFailed) because dng_pipeline.cpp never calls the RAW router —
# which made ARW, the workload that motivated the campaign, unmeasurable.
# Scoped strictly to this target; DNG behaviour is unchanged (the DNG branch of
# decodeRouted is the identical call it always made).
if(DNG_ENABLE_GENERIC_RAW)
    target_sources(test_concurrent_decode PRIVATE
        src/pipeline/raw_file_router.cpp
        src/pipeline/raw_gpu_pipeline.cpp
        # raw_gpu_pipeline.cpp's decode_file_*_into entries call
        # raw_timing_log_emit (src/pipeline/raw_timing_log.cpp). The shipped
        # dylib picks that TU up from pipeline.cmake's src/ glob; this target
        # compiles the pipeline sources directly, so it must name the TU or the
        # link fails on an undefined raw_timing_log_emit.
        src/pipeline/raw_timing_log.cpp
        src/pipeline/raw_contract_validate.cpp
        src/pipeline/raw_auto_exposure.cpp
        src/pipeline/raw_render_eval.cpp
        src/pipeline/raw_render_params_builder.cpp
        src/pipeline/libraw_frontend.cpp
        src/pipeline/libraw_gpu_input_adapter.cpp
        # raw_gpu_pipeline.cpp's decodeFileImpl delegates DNG-probed inputs back
        # to the DNG FFI. WP1 (2026-09-08): the gpu-orient campaign's T9 rewired
        # this delegation onto ceyx_decode_into_buffer (ceyx_decode_into_ffi.cpp)
        # — this comment previously named the old dng_decode_and_process_sized /
        # dng_free_result symbols, and the actual symbol this target now needs
        # was never added; nothing rebuilt this target since T9 to surface the
        # gap. The harness never reaches that delegation — it routes DNG itself,
        # before calling the RAW path — but the symbol must resolve.
        # ceyx_decode_into_ffi.cpp itself calls raw_record_decode_into_diagnostics
        # (raw_ffi_api.cpp:109 / raw_ffi_api.h:104), so that file joins the link
        # too. NOTE for WP5: raw_ffi_api.cpp also hosts raw_decode_and_process,
        # which WP5 deletes — re-check this block when that lands.
        src/ffi/dng_ffi_api.cpp
        src/ffi/ceyx_decode_into_ffi.cpp
        # T5b: the decode-into body moved to the pipeline layer.
        src/pipeline/decode_into.cpp
        src/ffi/raw_ffi_api.cpp)
    target_compile_definitions(test_concurrent_decode PRIVATE
        DNG_CONCURRENT_TEST_GENERIC_RAW=1)
    target_link_libraries(test_concurrent_decode
        libraw_vendored
        ${HALIDE_OUTPUT_DIR}/raw_bayer_demosaic${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_xtrans_demosaic${DNG_AOT_LIB_EXT}
        ${HALIDE_OUTPUT_DIR}/raw_linear_rgb_normalize${DNG_AOT_LIB_EXT})
    add_dependencies(test_concurrent_decode
        raw_bayer_demosaic_aot_target
        raw_xtrans_demosaic_aot_target
        raw_linear_rgb_normalize_aot_target)
endif()

# Task 1 (mutex rework): concurrency probe over the already-lock-free RAW path.
# Measures Halide's own GPU serialisation with the DNG single-flight mutex out
# of the picture. Not a correctness test; produces timings only.
add_executable(probe_concurrent_raw tests/probe_concurrent_raw.cpp)
target_include_directories(probe_concurrent_raw PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(probe_concurrent_raw dng_decoder_native)
add_dependencies(probe_concurrent_raw dng_decoder_native)

# R2-T3 (b): asserts the R1-T2 Metal queue pool actually pools (count > 1,
# never exceeding cap) during a real concurrent decode. Deliberately linked
# against the dng_decoder_native SHARED library -- same pattern as
# probe_concurrent_raw immediately above -- and NOT compiled from pipeline
# .cpp sources directly, unlike test_concurrent_decode: r2t1-opus FINDING 3
# (ceyx/tmp/verify/r2t1-instrument-validation.txt) established that the
# latter pattern links ZERO queue-pool code, so a colour-identity/behavior
# green from it is not evidence about the actual shipping artifact. This
# target's own binary is what R2-T3's nm marker check (AC4/AC5) must be run
# against, alongside the dylib itself.
add_executable(test_metal_queue_pool tests/test_metal_queue_pool.cpp)
target_include_directories(test_metal_queue_pool PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_metal_queue_pool dng_decoder_native)
add_dependencies(test_metal_queue_pool dng_decoder_native)


# R2 sized decode acceptance gate (AC5 extent / AC5-D crop-vs-scale / AC6 memory).
# Drives the PRODUCTION sized entry (dng_pipeline_decode_to_rgb_sized) and
# compares against a same-ordering CPU reference rendered through the production
# Stage4 AOT. Like other tests that reach production render-param construction,
# it #includes dng_render_halide.cpp directly to reach buildRenderParams, so
# that file must NOT be listed as a separate source here or every symbol in it
# would be defined twice.
add_executable(test_sized_decode tests/test_sized_decode.cpp
    src/pipeline/dng_pipeline.cpp
    src/pipeline/dng_halide_device.cpp
    src/pipeline/dng_opcodelist2_halide.cpp
    src/pipeline/dng_mosaic_halide.cpp
    src/pipeline/dng_warp_halide.cpp
    src/pipeline/render_parameter_upload_cache.cpp
    src/pipeline/raw_persistent_device_arena.cpp
    src/pipeline/dng_metal_context.cpp)
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
    ${DNG_STAGE4_NONSPLIT_AOT_LIBS}
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
add_dependencies(test_sized_decode ${DNG_STAGE4_NONSPLIT_AOT_TARGETS})
add_dependencies(test_sized_decode dng_opcode_polynomial_aot_target)
add_dependencies(test_sized_decode dng_opcode_polynomial3_aot_target)
# T20-fix F1: see the test_device_handoff block above.
target_link_libraries(test_sized_decode
    ${DNG_FUSED_BAYER_AOT_LIBS}
        # mem8 v3 T12: the yuv420 output variant of the SAME Stage-4 family.
        # Required on every target that compiles dng_render_halide.cpp, for
        # exactly the reason the fused archive above is (ffi.cmake links it
        # into the shipping library the same way).
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_yuv420${DNG_AOT_LIB_EXT})
if(TARGET raw_bayer_fused_render_aot_target)
    add_dependencies(test_sized_decode ${DNG_FUSED_BAYER_AOT_TARGETS})
    add_dependencies(test_sized_decode dng_render_yuv420_aot_target)
endif()
# F-T4-1: tests/test_sized_decode.cpp #includes dng_render_halide.cpp (see the
# add_executable note above), so the split archive is required here too.
if(DNG_STAGE4_SPLIT_KERNEL)
    target_link_libraries(test_sized_decode ${DNG_STAGE4_SPLIT_AOT_LIBS})
    add_dependencies(test_sized_decode ${DNG_STAGE4_SPLIT_AOT_TARGETS})
endif()
if(APPLE)
    target_link_libraries(test_sized_decode ${COREFOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY} ${METAL_LIBRARY} ${FOUNDATION_LIBRARY})
endif()
if(DNG_LINUX_TEST_LIBS)
    target_link_libraries(test_sized_decode ${DNG_LINUX_TEST_LIBS})
endif()

# DNG SDK Decode Pipeline Test Tool (with Halide Stage3 demosaic)
add_executable(test_decode tests/test_decode.cpp
    ${CEYX_PIPELINE_STATIC_SOURCES})
target_include_directories(test_decode PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${SRC_DIR}/pipeline
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
ceyx_link_pipeline_static(test_decode)

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

# --- Codec expansion (2026-08-30) ------------------------------------------
# test_abi_layout has no external codec dependency: it only pins struct layouts
# and error-code values, so it builds even on a platform with no dist at all.
add_executable(test_abi_layout tests/test_abi_layout.cpp)
target_include_directories(test_abi_layout PRIVATE ${INC_DIR})
target_link_libraries(test_abi_layout PRIVATE dng_decoder_native)

# Codec round-trip targets (2026-08-30 codec expansion, Tasks 7/8/9).
add_executable(test_codec_roundtrip tests/test_codec_roundtrip.cpp)
target_include_directories(test_codec_roundtrip PRIVATE ${INC_DIR} ${SRC_DIR})
target_link_libraries(test_codec_roundtrip PRIVATE dng_decoder_native)

add_executable(test_codec_heif tests/test_codec_heif.cpp)
target_include_directories(test_codec_heif PRIVATE ${INC_DIR} ${SRC_DIR})
target_link_libraries(test_codec_heif PRIVATE dng_decoder_native)

add_executable(test_codec_jxl tests/test_codec_jxl.cpp)
target_include_directories(test_codec_jxl PRIVATE ${INC_DIR} ${SRC_DIR})
target_link_libraries(test_codec_jxl PRIVATE dng_decoder_native)

# ---------------------------------------------------------------------------
# WP10 (AMENDMENT 3): ONE format-agnostic caller-owned-buffer gate.
#
# This replaces the two per-format targets AMENDMENT 2b needed
# (test_decode_into_buffer + test_raw_decode_into_buffer). A3 routes both
# formats through a single entry pair, so there is one target, owned by one
# member — the shared-file contention 2b required is designed out.
#
# NO if(DNG_ENABLE_GENERIC_RAW) guard, deliberately: ceyx_decode_into_ffi.cpp is
# always compiled and both ceyx_* symbols exist in every configuration. In an
# OFF build a RAW input returns kCeyxErrFormatUnsupportedInBuild, which is the
# behaviour AC15.8 asserts — so this target must BUILD there in order to test it.
# The test prints [GAP] for any sample class it was not given.
#
# Placed INSIDE the NOT DNG_CROSS_BUILD guard: a target after that endif() would
# be emitted for cross builds, which have no host runner.
#
# Linked against the SHARED library on purpose — rationale at tests.cmake
# :1741-1749: compiling the pipeline sources into a test links none of the
# shipping code, so a green from that shape says nothing about the artifact,
# and WP10's whole risk is "the symbol is not in the shipped binary".
# ---------------------------------------------------------------------------
# --- mem8 v3 T12 milestone 4: the yuv420 output arm's correctness suite -----
# Y1..Y9. Same shared-dylib linkage rationale as test_ceyx_decode_into below
# and tests.cmake:1741-1749: compiling the pipeline sources into a test links
# none of the shipping code, so a green from that shape says nothing about the
# artifact -- and "the yuv420 symbol is not in the shipped binary" is exactly
# the silently-absent-feature failure this campaign has already paid for.
#
# NO if(DNG_ENABLE_GENERIC_RAW) guard, for the same reason the target below
# carries none: the ceyx_* entries are always compiled, and the cases report
# their own failures rather than failing to build.
#
# It reaches libjpeg through the dylib's own encode/still surface
# (ceyx_encode_rgba8 / ceyx_still_decode_rgba), which is Y4a's INDEPENDENT
# authority -- asserting the converter against the oracle it was transcribed
# from cannot fail, so the comparison has to run real libjpeg.
add_executable(test_stage4_yuv420_output tests/test_stage4_yuv420_output.cpp)
target_include_directories(test_stage4_yuv420_output PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include
    ${JPEG_INCLUDE_DIRS})
target_link_libraries(test_stage4_yuv420_output dng_decoder_native
                      ${JPEG_LIBRARIES})
add_dependencies(test_stage4_yuv420_output dng_decoder_native)
# --- end T12 milestone 4 ----------------------------------------------------

# --- mem8 v3 T13: the yuv420 -> RGBA8 CONVERTER's own suite (C1..C5) -------
# Distinct target from test_stage4_yuv420_output on purpose: that one owns the
# OUTPUT ARM (do the kernels write the right planes), this one owns the
# CONVERTER alone (given planes, is the inverse libjpeg's, at every extent,
# opaque, allocating nothing). Same shared-dylib linkage rationale as above --
# linking the pipeline sources in would test none of the shipped binary.
#
# No libjpeg include/link: the suite's forward transform is deliberately an
# INDEPENDENT transcription of the vendored sources (so C1 can fail), and its
# real-output arm goes through the dylib's decode entries.
add_executable(test_yuv420_to_rgba tests/test_yuv420_to_rgba.cpp)
target_include_directories(test_yuv420_to_rgba PRIVATE ${INC_DIR} ${SRC_DIR})
target_link_libraries(test_yuv420_to_rgba dng_decoder_native)
add_dependencies(test_yuv420_to_rgba dng_decoder_native)
# --- end T13 ----------------------------------------------------------------

add_executable(test_ceyx_decode_into tests/test_ceyx_decode_into.cpp)
target_include_directories(test_ceyx_decode_into PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_ceyx_decode_into dng_decoder_native)
add_dependencies(test_ceyx_decode_into dng_decoder_native)

# --- r5 remediation (Task #3, 2026-09-06): -213/-301 error-map regression ---
# See test_errmap_dst_too_small.cpp header comment for exactly what this does
# and does not cover (the ceyx_decode_into_ffi.cpp:154 RAW-arm branch is
# structurally unreachable via the public API with the current sample corpus;
# this target covers the internal code's reality and the boundary's
# never-leaks-213 contract instead). Same linkage rationale as
# test_ceyx_decode_into above: shared dylib, not pipeline sources compiled in.
add_executable(test_errmap_dst_too_small tests/test_errmap_dst_too_small.cpp)
target_include_directories(test_errmap_dst_too_small PRIVATE
    ${INC_DIR}
    ${SRC_DIR}
    ${DNG_SDK_DIR}
    ${HALIDE_OUTPUT_DIR}
    ${HALIDE_DIR}/include)
target_link_libraries(test_errmap_dst_too_small dng_decoder_native)
add_dependencies(test_errmap_dst_too_small dng_decoder_native)
# --- end r5 remediation Task #3 ---

# --- R6 fix: decode-into path must populate legacy RAW diagnostics ---------
# Guarded by DNG_ENABLE_GENERIC_RAW: it calls raw_last_diagnostics(), which is
# declared alongside raw_ffi_api.cpp's other symbols and (per pipeline.cmake's
# NOT DNG_ENABLE_GENERIC_RAW filter above) does not exist in an OFF build's
# dylib at all -- unlike test_ceyx_decode_into/test_errmap_dst_too_small,
# whose ceyx_* entries are always compiled. Same shared-dylib linkage
# rationale as those two targets.
if(DNG_ENABLE_GENERIC_RAW)
    add_executable(test_raw_diagnostics_freshness
        tests/test_raw_diagnostics_freshness.cpp)
    target_include_directories(test_raw_diagnostics_freshness PRIVATE
        ${INC_DIR}
        ${SRC_DIR}
        ${DNG_SDK_DIR}
        ${HALIDE_OUTPUT_DIR}
        ${HALIDE_DIR}/include)
    target_link_libraries(test_raw_diagnostics_freshness dng_decoder_native)
    add_dependencies(test_raw_diagnostics_freshness dng_decoder_native)
endif()
# --- end R6 fix ---

# --- Task 11: CI capability probe (orientation fusion) ---------------------
# Deliberately NOT linked against dng_decoder_native: it dlopen's (LoadLibrary
# on Windows) a library PATH given on argv at runtime instead, so the same
# binary can probe either the just-built dylib or, locally, a fixture from an
# older commit (negative control) -- linking it directly would defeat that.
# CMAKE_DL_LIBS is libdl on Linux and empty on macOS/Windows; that variable
# is already collected into DNG_LINUX_TEST_LIBS above for the `if(UNIX AND
# NOT APPLE AND NOT ANDROID)` case, but this target sits outside that block
# so it links CMAKE_DL_LIBS explicitly here instead.
add_executable(orient_capability_probe tests/orient_capability_probe.cpp)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(orient_capability_probe PRIVATE ${CMAKE_DL_LIBS})
endif()
# CI FIX (Round 3, was CI-red): the probe's second Stage4 kernel check must
# match whichever second kernel THIS platform actually built and linked into
# dng_decoder_native (ffi.cmake links dng_render_stage4_scaled_preavg when
# DNG_STAGE4_SPLIT_KERNEL is OFF, dng_render_stage4_split when it is ON --
# never both). Deriving ORIENT_PROBE_SPLIT_KERNEL from that SAME variable
# (set earlier in native/cmake/halide_aot.cmake) keeps the probe's kernel
# list in lockstep with the link wiring by construction, instead of a
# second hand-maintained platform check that could silently drift.
if(DNG_STAGE4_SPLIT_KERNEL)
    target_compile_definitions(orient_capability_probe PRIVATE ORIENT_PROBE_SPLIT_KERNEL=1)
endif()
# --- end Task 11 ---

# --- win-parity plan P1: pressure-relief capability probe (AC1) ------------
# Modelled on orient_capability_probe above: dlopen's a library path given on
# argv at runtime instead of linking dng_decoder_native directly, so the same
# binary can probe a fresh build or an older fixture (negative control).
add_executable(pressure_relief_capability_probe tests/pressure_relief_capability_probe.cpp)
if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(pressure_relief_capability_probe PRIVATE ${CMAKE_DL_LIBS})
endif()
# --- end win-parity plan P1 ---

endif() # NOT DNG_CROSS_BUILD (test targets)

endif() # NOT DNG_HOST_GENERATORS_ONLY (entire runtime section)
