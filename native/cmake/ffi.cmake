# ffi.cmake - AOT/platform link wiring of the shipped C-ABI dylib
#
# Extracted verbatim from native/CMakeLists.txt (pre-split lines 823-936)
# by the 2026-08-25 Ceyx restructure, Round 1 Stream 1B. Included via
# include() (not add_subdirectory()) so variable scope and target resolution
# stay identical to the monolith.
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
        ${HALIDE_OUTPUT_DIR}/dng_render_stage4_split${DNG_AOT_LIB_EXT})
    target_compile_definitions(dng_decoder_native PRIVATE
        DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE=${DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE})
endif()
if(APPLE)
    # R1-T2 (2026-09-05, parallel-decode campaign): the Metal context override
    # (src/pipeline/dng_metal_context.cpp) is listed explicitly here, Apple-only,
    # so the queue pool's platform scope is visible in the build system and not
    # only in that TU's #if guard. pipeline.cmake's GLOB_RECURSE already picks the
    # file up on every platform and CMake de-duplicates identical absolute source
    # paths within a target, so this line documents scope and guarantees the TU is
    # present on Apple; it never causes a second compile. On non-Apple (and on
    # Apple with DNG_FORCE_VULKAN) the TU's guard makes it an empty object file,
    # so Vulkan platforms are bit-for-bit untouched.
    target_sources(dng_decoder_native PRIVATE
        ${SRC_DIR}/pipeline/dng_metal_context.cpp)

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
elseif(UNIX AND NOT APPLE)
    # Linux port (2026-08-28, D4): the Halide Vulkan runtime dlopen's
    # libvulkan.so.1 at runtime (halide_runtime_fork/upstream/vulkan_interface.h),
    # so this branch deliberately does NOT probe for or hard-require a Vulkan SDK
    # at build time — a link-time Vulkan dependency would be both unnecessary and a
    # build-environment regression. The runtime prerequisite is a Vulkan ICD on host
    # (libvulkan1 + a driver such as mesa-vulkan-drivers). We link only libdl (for
    # the runtime's dlopen) and pthreads. Branch is UNIX AND NOT APPLE, never bare
    # UNIX, because in CMake APPLE implies UNIX (R5).
    find_package(Threads REQUIRED)
    target_link_libraries(dng_decoder_native ${CMAKE_DL_LIBS} Threads::Threads)

    # $ORIGIN rpath (task #18, 2026-09-01): dng_decoder_native.so dynamically
    # links libheif.so.1/libde265.so.0 (see cmake/heif.cmake's POST_BUILD
    # staging in this same UNIX-AND-NOT-APPLE family). Without this, CMake's
    # default build-tree RPATH bakes in the ABSOLUTE build-machine path to
    # native/third_party/heif-dist-linux/lib -- which resolves fine for the
    # in-tree `ldd`/CI verify step (surfacing no failure there) but is
    # meaningless once the .so is copied elsewhere (plugin/linux/Libraries,
    # a Halcyon app bundle, a release tarball): the loader looks for a path
    # that only ever existed on the CI runner. BUILD_RPATH_USE_ORIGIN +
    # INSTALL_RPATH "$ORIGIN" makes the recorded RPATH relative to wherever
    # the .so itself ends up, so a sibling libheif.so.1/libde265.so.0 staged
    # next to it (same directory) is always found, mirroring the intent of
    # the APPLE branch's @rpath/@loader_path pair and the WIN32 branch's
    # directory-relative DLL search order.
    set_target_properties(dng_decoder_native PROPERTIES
        BUILD_RPATH_USE_ORIGIN TRUE
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH FALSE
    )

    # Size fix (2026-08-29): the Linux .so came out at 22.7MB vs ~6MB elsewhere.
    # Measured on the CI artifact (run 33186830473): .debug_* = 16.78MB = 74% of
    # the file, dominated by .debug_gnu_pubnames/pubtypes; .symtab is already
    # absent and .dynsym+.dynstr is only 1.2%, so neither stripping symbols nor
    # hidden visibility is the lever. No `-g` exists anywhere in this tree — the
    # DWARF rides in from prebuilt static inputs (the vendored Halide dist still
    # carries its build bot's paths). This is a linker-behaviour difference, not
    # a flag difference: ELF ld copies each input's .debug_* into the .so, while
    # macOS ld leaves DWARF in the .o files (dsymutil collects it) and MSVC puts
    # it in a PDB. --strip-debug drops only the debug sections, so the dynamic
    # symbol table (and thus the FFI export surface) is untouched. Release only,
    # so a local RelWithDebInfo/Debug build stays debuggable.
    target_link_options(dng_decoder_native PRIVATE
        $<$<CONFIG:Release>:-Wl,--strip-debug>)
endif()

# Re-guard (split mechanics): the enclosing `if(NOT DNG_HOST_GENERATORS_ONLY)`
# opened above is closed here and re-opened at the top of tests.cmake, because
# CMake requires flow-control blocks to balance within one file. The condition
# is unchanged in between, so execution is identical to the monolith.
endif() # NOT DNG_HOST_GENERATORS_ONLY (continued in tests.cmake)
