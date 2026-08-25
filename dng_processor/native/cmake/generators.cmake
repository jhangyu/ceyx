# generators.cmake - Halide find_package and the generator executables
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 404-569)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.
# Phase 14: In cross-build mode, skip Halide find_package and all generator targets.
# We only need Halide headers for AOT-generated .h includes.
if(NOT DNG_CROSS_BUILD)
    # 2026-08-26 Ceyx restructure Round 2 Stream 2B: generator sources moved to
    # native/generators/ (build-time-only tools, never shipped; see structure
    # audit M3). GEN_DIR points the add_executable() calls below there; the
    # generators still #include "dng_halide_utils.h" from the same directory.
    set(GEN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generators)
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
    add_executable(rectilinear_warp_generator ${GEN_DIR}/RectilinearWarpGenerator.cpp)
    target_include_directories(rectilinear_warp_generator PRIVATE ${INC_DIR} ${GEN_DIR})
    target_link_libraries(rectilinear_warp_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    add_executable(dng_demosaic_generator ${GEN_DIR}/DngDemosaicGenerator.cpp)
    target_include_directories(dng_demosaic_generator PRIVATE ${INC_DIR} ${GEN_DIR})
    target_link_libraries(dng_demosaic_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # P17 T9: fused normalize + Bayer demosaic generator for the generic RAW
    # route. New generator on purpose: dng_demosaic_bilinear's AOT output is a
    # pinned regression artifact and the DNG route must not normalize twice.
    add_executable(raw_bayer_demosaic_generator ${GEN_DIR}/RawBayerDemosaicGenerator.cpp)
    target_include_directories(raw_bayer_demosaic_generator PRIVATE ${INC_DIR} ${SRC_DIR} ${GEN_DIR})
    target_link_libraries(raw_bayer_demosaic_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # P17 T11: fused normalize + X-Trans 6x6 demosaic generator for the
    # generic RAW route. Separate kernel from the Bayer one: the CFA is a
    # runtime 6x6 buffer, not a 2x2 phase pair.
    add_executable(raw_xtrans_demosaic_generator ${GEN_DIR}/RawXTransDemosaicGenerator.cpp)
    target_include_directories(raw_xtrans_demosaic_generator PRIVATE ${INC_DIR} ${SRC_DIR} ${GEN_DIR})
    target_link_libraries(raw_xtrans_demosaic_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # P19 T7: normalize-only pre-pass for the generic-RAW linear-RGB (Foveon
    # X3F) route. No demosaic, no neighbourhood access -- see the class
    # comment in RawLinearRgbNormalizeGenerator.cpp.
    add_executable(raw_linear_rgb_normalize_generator ${GEN_DIR}/RawLinearRgbNormalizeGenerator.cpp)
    target_include_directories(raw_linear_rgb_normalize_generator PRIVATE ${INC_DIR} ${SRC_DIR} ${GEN_DIR})
    target_link_libraries(raw_linear_rgb_normalize_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    add_executable(dng_demosaic_warp_generator ${GEN_DIR}/DngDemosaicWarpGenerator.cpp)
    target_include_directories(dng_demosaic_warp_generator PRIVATE ${INC_DIR} ${GEN_DIR})
    target_link_libraries(dng_demosaic_warp_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # Research & diagnostic generators (only built if DNG_DIAGNOSTIC_BUILD=ON)
    if(DNG_DIAGNOSTIC_BUILD)
        add_executable(rectilinear_warp_strict_float_generator ${GEN_DIR}/research/RectilinearWarpStrictFloatGenerator.cpp)
        target_include_directories(rectilinear_warp_strict_float_generator PRIVATE ${INC_DIR} ${GEN_DIR})
        target_link_libraries(rectilinear_warp_strict_float_generator PRIVATE Halide::Generator ZLIB::ZLIB)

        add_executable(rectilinear_warp_debug_generator ${GEN_DIR}/research/RectilinearWarpDebugGenerator.cpp)
        target_include_directories(rectilinear_warp_debug_generator PRIVATE ${INC_DIR} ${GEN_DIR})
        target_link_libraries(rectilinear_warp_debug_generator PRIVATE Halide::Generator ZLIB::ZLIB)
    endif()

    add_executable(dng_render_generator ${GEN_DIR}/DngRenderGenerator.cpp)
    target_include_directories(dng_render_generator PRIVATE ${INC_DIR} ${GEN_DIR})
    target_link_libraries(dng_render_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    # Phase 10 Sprint C1: MapPolynomial AOT generator (Stage 2 OpcodeList2 GPU).
    add_executable(dng_opcode_polynomial_generator
        ${GEN_DIR}/DngOpcodePolynomialGenerator.cpp)
    target_include_directories(dng_opcode_polynomial_generator PRIVATE ${GEN_DIR})
    target_link_libraries(dng_opcode_polynomial_generator PRIVATE Halide::Generator ZLIB::ZLIB)

    add_executable(dng_opcode_polynomial3_generator
        ${GEN_DIR}/DngOpcodePolynomial3Generator.cpp)
    target_include_directories(dng_opcode_polynomial3_generator PRIVATE ${GEN_DIR})
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
