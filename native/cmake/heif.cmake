# heif.cmake - HEIC/HEIF decode route (libheif + libde265, DYNAMICALLY linked).
#
# The dist is produced by `native/scripts/build_deps.py build heif-stack`; see
# native/third_party/heif-dist/PROVENANCE.md for versions, hashes, the exact
# decode-only configure flags and the LGPL-3 relinking rationale.
#
# Included (not add_subdirectory'd) from native/CMakeLists.txt, after
# cmake/ffi.cmake, so dng_decoder_native already exists as a target.
if(NOT DNG_HOST_GENERATORS_ONLY)

if(DNG_ENABLE_HEIF)
    # Dist selection (2026-08-28). build_deps.py's heif-stack build writes a host-architecture
    # dist to third_party/heif-dist and a cross-architecture one to
    # third_party/heif-dist-<arch>, so an x86_64 cross-build cannot clobber the
    # arm64 dist a shared working tree is using. Prefer the arch-suffixed dist
    # when this build targets a specific architecture, and fall back to the
    # historical unsuffixed path — which is what every non-cross build uses, so
    # their behaviour is unchanged.
    set(HEIF_DIST_DIR ${THIRD_PARTY_DIR}/heif-dist)
    set(HEIF_DIST_HINTS "${HEIF_DIST_DIR}")
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        set(HEIF_DIST_HINTS "")
        foreach(_heif_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
            list(APPEND HEIF_DIST_HINTS "${THIRD_PARTY_DIR}/heif-dist-${_heif_arch}")
        endforeach()
        list(APPEND HEIF_DIST_HINTS "${HEIF_DIST_DIR}")
    elseif(WIN32)
        # The Windows dist is COMMITTED (native/third_party/heif-dist-windows,
        # see its PROVENANCE.md) rather than built on demand: no machine in this
        # project can produce Windows binaries locally, so it is a reviewed,
        # byte-pinned input. It is the ONLY hint -- the unsuffixed path is not
        # appended, because a heif-dist/ left behind by a developer running
        # fetch_heif_deps.sh holds Mach-O dylibs, and letting the search fall
        # through to it would surface as a link error naming the decoder rather
        # than naming this mismatch. (Historically produced by the now-deleted
        # native/scripts/fetch_heif_deps.sh; same hazard applies to a dist left
        # behind by build_deps.py's heif-stack build.)
        set(HEIF_DIST_HINTS "${THIRD_PARTY_DIR}/heif-dist-windows")
    elseif(ANDROID)
        # ANDROID-HEIF (A-T8-FIX, 2026-09-01): the committed dist is
        # arch-suffixed (native/third_party/heif-dist-android-${ANDROID_ABI}),
        # never the unsuffixed ${HEIF_DIST_DIR} -- same isolation rationale as
        # the WIN32/Linux branches: an unsuffixed heif-dist/ left behind by a
        # macOS or Linux build holds foreign-arch bytes (Mach-O or x86_64 ELF)
        # that would surface as an opaque "wrong architecture" failure rather
        # than naming the real mismatch. ANDROID must be checked before the
        # UNIX-AND-NOT-APPLE branch below: the NDK toolchain sets ANDROID
        # while CMAKE_SYSTEM_NAME is still effectively UNIX, so an elseif
        # ordering with UNIX-AND-NOT-APPLE first would silently swallow this
        # branch and search the Linux desktop dist path instead.
        set(HEIF_DIST_HINTS "${THIRD_PARTY_DIR}/heif-dist-android-${ANDROID_ABI}")
    elseif(UNIX AND NOT APPLE)
        # LINUX-HEIF (round 3, user ruling 2026-09-01): the Linux dist is
        # built in-CI (native/scripts/build_deps.py build heif-stack
        # --platform linux --dist .../heif-dist-linux, see linux_build.yml),
        # never committed, under a Linux-specific suffixed directory rather
        # than the unsuffixed ${HEIF_DIST_DIR} default. This mirrors the
        # WIN32 branch's isolation intent immediately above: unsuffixed
        # heif-dist/ is the path macOS's build produces (its dylibs), and a
        # Linux configure searching that same unsuffixed path in a working
        # tree that has ever run a macOS build would find Mach-O bytes that
        # `file`/`nm -D`/`readelf` cannot parse as ELF -- surfacing as an
        # opaque "not a 64-bit ELF" assertion failure inside the carrier
        # rather than naming the real mismatch. The unsuffixed path is not
        # appended as a fallback for the same reason the Windows branch does
        # not append it.
        set(HEIF_DIST_HINTS "${THIRD_PARTY_DIR}/heif-dist-linux")
    endif()
    set(HEIF_INCLUDE_HINTS "")
    set(HEIF_LIB_HINTS "")
    foreach(_heif_hint IN LISTS HEIF_DIST_HINTS)
        list(APPEND HEIF_INCLUDE_HINTS "${_heif_hint}/include")
        list(APPEND HEIF_LIB_HINTS "${_heif_hint}/lib")
    endforeach()

    # NO_CMAKE_FIND_ROOT_PATH (A-T8-FIX, 2026-09-01, verified against a real
    # NDK configure -- not a guess): the Android NDK's android.toolchain.cmake
    # sets CMAKE_FIND_ROOT_PATH_MODE_LIBRARY/_INCLUDE to ONLY, which makes
    # find_library()/find_path() search ONLY inside CMAKE_FIND_ROOT_PATH (the
    # NDK sysroot) regardless of an explicit HINTS path outside it -- NOT
    # bypassed by NO_DEFAULT_PATH, which only suppresses the standard system
    # search locations, not the sysroot-only restriction. Without this flag,
    # a real committed dist at HINTS still resolves to *-NOTFOUND on Android
    # (reproduced locally: `heif.h = 'HEIF_INCLUDE_DIR-NOTFOUND'` even though
    # the file exists at the exact hinted path). No other platform's find
    # calls go through a root-path-restricting toolchain file, so this flag
    # is a no-op everywhere else.
    find_path(HEIF_INCLUDE_DIR
        NAMES libheif/heif.h
        HINTS ${HEIF_INCLUDE_HINTS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    find_library(HEIF_LIBRARY
        NAMES heif
        HINTS ${HEIF_LIB_HINTS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)
    find_library(DE265_LIBRARY
        NAMES de265
        HINTS ${HEIF_LIB_HINTS}
        NO_DEFAULT_PATH
        NO_CMAKE_FIND_ROOT_PATH)

    # NO_DEFAULT_PATH on all three is deliberate: silently linking a Homebrew
    # libheif would stamp an absolute /opt/homebrew load command into the
    # shipped dylib, which App-Sandboxed hosts cannot open — the exact failure
    # third_party.cmake's static-libjpeg note records for libjpeg in 2026-08-17.
    if(NOT HEIF_INCLUDE_DIR OR NOT HEIF_LIBRARY OR NOT DE265_LIBRARY)
        if(WIN32)
            message(FATAL_ERROR
                "HEIF decode is enabled but the vendored dist is missing.\n"
                "  searched under: ${HEIF_DIST_HINTS}\n"
                "  heif.h    = '${HEIF_INCLUDE_DIR}'\n"
                "  libheif   = '${HEIF_LIBRARY}'\n"
                "  libde265  = '${DE265_LIBRARY}'\n"
                "Restore native/third_party/heif-dist-windows by dispatching "
                ".github/workflows/heif_dist_windows.yml.")
        else()
            message(FATAL_ERROR
                "HEIF decode is enabled but the vendored dist is missing.\n"
                "  searched under: ${HEIF_DIST_HINTS}\n"
                "  heif.h    = '${HEIF_INCLUDE_DIR}'\n"
                "  libheif   = '${HEIF_LIBRARY}'\n"
                "  libde265  = '${DE265_LIBRARY}'\n"
                "Run `python3 native/scripts/build_deps.py build heif-stack "
                "--dist ${HEIF_DIST_DIR}`, or configure with "
                "-DDNG_ENABLE_HEIF=OFF to build without the HEIC route.")
        endif()
    endif()

    # Re-derive the dist root from what was actually found, so the POST_BUILD
    # staging below copies out of the SAME dist that was linked (they differ
    # whenever the arch-suffixed dist won the search above).
    get_filename_component(HEIF_DIST_DIR "${HEIF_LIBRARY}" DIRECTORY)
    get_filename_component(HEIF_DIST_DIR "${HEIF_DIST_DIR}" DIRECTORY)

    # Fail at configure time, with a message that names the problem, rather than
    # letting a stale wrong-architecture dist surface as an opaque linker error.
    if(APPLE AND CMAKE_OSX_ARCHITECTURES)
        execute_process(COMMAND lipo -archs "${HEIF_LIBRARY}"
                        OUTPUT_VARIABLE HEIF_HAVE_ARCHS
                        OUTPUT_STRIP_TRAILING_WHITESPACE
                        ERROR_QUIET RESULT_VARIABLE HEIF_LIPO_RC)
        if(HEIF_LIPO_RC EQUAL 0)
            foreach(_heif_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
                if(NOT "${HEIF_HAVE_ARCHS}" MATCHES "(^| )${_heif_arch}( |$)")
                    message(FATAL_ERROR
                        "HEIF dist architecture mismatch.\n"
                        "  ${HEIF_LIBRARY}\n"
                        "  has archs '${HEIF_HAVE_ARCHS}', this build targets "
                        "'${CMAKE_OSX_ARCHITECTURES}'.\n"
                        "Rebuild the dist for the target architecture:\n"
                        "  python3 native/scripts/build_deps.py build heif-stack "
                        "--arch ${_heif_arch}\n"
                        "or configure with -DDNG_ENABLE_HEIF=OFF.")
                endif()
            endforeach()
        endif()
    endif()

    target_include_directories(dng_decoder_native PRIVATE ${HEIF_INCLUDE_DIR})
    target_link_libraries(dng_decoder_native ${HEIF_LIBRARY} ${DE265_LIBRARY})
    target_compile_definitions(dng_decoder_native PRIVATE DNG_ENABLE_HEIF=1)

    # HEIC/AVIF encode + the still-decode delegation (2026-08-30 codec
    # expansion, Task 7).
    #
    # REDUNDANT with pipeline.cmake:90's file(GLOB_RECURSE ${SRC_DIR}/*.cpp),
    # which already sweeps this TU into dng_decoder_native; CMake de-duplicates
    # identical source paths within a target, so the explicit add changes
    # nothing today. It is kept as an explicit declaration so that a future
    # exclusion filter cannot silently drop this TU -- the OFF-branch filter at
    # pipeline.cmake:130 is `.*/heif_(decode|ffi_api)\.cpp$`, and a maintainer
    # widening it to cover heif_encode.cpp would otherwise remove the encode
    # entry points with no diagnostic.
    #
    # Listed on BOTH branches of this if/else on purpose: the TU guards all of
    # its libheif usage on DNG_ENABLE_HEIF internally, so a disabled build still
    # DEFINES ceyx_heif_encode_impl, ceyx_heif_still_decode_impl and
    # MapHeifToStillError, returning the "unsupported" codes. A codec excluded
    # from a build must degrade into a defined error, never into a symbol the
    # caller's lookupFunction cannot find.
    target_sources(dng_decoder_native PRIVATE ${SRC_DIR}/heif_encode.cpp)

    message(STATUS "HEIF: libheif ${HEIF_LIBRARY} + libde265 ${DE265_LIBRARY} (dynamic)")

    if(APPLE)
        # Stage the two dylibs NEXT TO the built decoder library. Two reasons,
        # both load-bearing:
        #  1. dng_decoder_native records them as @rpath/libheif.1.dylib and
        #     @rpath/libde265.0.dylib (their install names, set by the
        #     heif-stack build), and the dylib carries an @loader_path rpath, so a bare
        #     dlopen out of the build directory resolves them.
        #  2. scripts/build_apps.py Phase 1 copies every sibling *.dylib from
        #     the build dir into plugin/macos/Libraries/, which is how they
        #     reach the app bundle. Staging here is what makes that free.
        # copy_if_different, so an unchanged dist does not retrigger the
        # downstream Flutter build every time.
        add_custom_command(TARGET dng_decoder_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libheif.1.dylib"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libheif.1.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libde265.0.dylib"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libde265.0.dylib"
            COMMENT "Staging libheif/libde265 next to dng_decoder_native")
    elseif(WIN32)
        # Stage the two DLLs NEXT TO the built decoder DLL. Windows resolves an
        # imported DLL from the loading module's directory before anything else,
        # so this is the Windows equivalent of the @rpath/@loader_path wiring the
        # APPLE branch above relies on -- and it is REQUIRED, not an optimisation:
        # dng_decoder_native.dll imports heif.dll and libde265.dll, so a directory
        # missing either one fails at LoadLibrary / DynamicLibrary.open with an
        # error that names only the decoder.
        #
        # Source is <dist>/bin, not <dist>/lib: CMake's Windows install layout
        # puts runtime DLLs in bin/ and import libraries (what HEIF_LIBRARY
        # above resolved to) in lib/.
        #
        # The de265 runtime file is libde265.dll while its import library is
        # de265.lib -- that asymmetry is upstream's, and heif.dll's import table
        # names libde265.dll, so the file must keep that exact name here
        # (Contract Amendment 1, 2026-08-30).
        #
        # This staging is also what plugin/windows/CMakeLists.txt's derived
        # bundled-library list and windows_build.yml's staging step consume, so
        # all three ship the same set by construction rather than by three
        # hand-maintained lists happening to agree.
        add_custom_command(TARGET dng_decoder_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/bin/heif.dll"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/heif.dll"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/bin/libde265.dll"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libde265.dll"
            COMMENT "Staging heif.dll/libde265.dll next to dng_decoder_native")
    elseif(ANDROID)
        # Stage the two UNVERSIONED .so files NEXT TO the built decoder .so
        # (A-T8-FIX, 2026-09-01). Unlike the Linux desktop branch below,
        # the Android dist ships unversioned names only (round2-heif-baton.md
        # measured fact: "Filenames are unversioned. Stage libheif.so and
        # libde265.so. There is no libheif.so.1 / libde265.so.0 on this
        # platform, and Gradle would not pack them if there were" -- Android
        # packaging drops any jniLibs file not ending exactly in ".so").
        # Android's dynamic linker resolves a loaded library's DT_NEEDED
        # entries against the same native-library directory the loading .so
        # was found in (no explicit $ORIGIN RPATH needed the way Linux
        # desktop requires one), so staging here next to dng_decoder_native
        # -- which is also what the packaging step below copies into the
        # artifact/jniLibs set -- is sufficient for the loader to find them.
        add_custom_command(TARGET dng_decoder_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libheif.so"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libheif.so"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libde265.so"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libde265.so"
            COMMENT "Staging libheif.so/libde265.so next to dng_decoder_native")
    elseif(UNIX AND NOT APPLE)
        # Stage the two versioned .so files NEXT TO the built decoder .so
        # (task #18, 2026-09-01 -- this branch was entirely missing: the
        # APPLE and WIN32 siblings above both stage; Linux silently did not,
        # even though DNG_ENABLE_HEIF links libheif/libde265 dynamically here
        # too). ffi.cmake's UNIX-AND-NOT-APPLE branch sets an $ORIGIN RPATH
        # on dng_decoder_native so a sibling file in the SAME directory is
        # what the loader actually resolves -- staging anywhere else (e.g.
        # only into the dist tree) would not be found once the .so is copied
        # out of the build tree.
        #
        # Source is <dist>/lib/libheif.so.1 and <dist>/lib/libde265.so.0 --
        # the VERSIONED real files (native/scripts/deps/heif.py's
        # build_libde265()/build_libheif() install the versioned name as the
        # real file and the unversioned name as a symlink; HEIF_LIBRARY /
        # DE265_LIBRARY above resolved via find_library(NAMES heif/de265),
        # which follows the unversioned symlink to that real file). The
        # SONAME each library records at build time is the versioned name
        # (heif.py's _assert_de265_recorded_name asserts this for de265), so
        # staging under the versioned name is what the dynamic loader's
        # DT_NEEDED lookup actually needs to find -- staging the unversioned
        # symlink target alone, without the versioned name present, would
        # fail to resolve.
        add_custom_command(TARGET dng_decoder_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libheif.so.1"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libheif.so.1"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${HEIF_DIST_DIR}/lib/libde265.so.0"
                    "$<TARGET_FILE_DIR:dng_decoder_native>/libde265.so.0"
            COMMENT "Staging libheif.so.1/libde265.so.0 next to dng_decoder_native")
    endif()
else()
    target_compile_definitions(dng_decoder_native PRIVATE DNG_ENABLE_HEIF=0)
    # Listed here too: see the note on the enabled branch above. Also redundant
    # with pipeline.cmake:90's glob, and kept for the same reason -- guarding
    # against a future exclusion filter, NOT because the link depends on it.
    #
    # What the link DOES depend on is that this TU reaches the target somehow:
    # still_ffi_api.cpp and encode_ffi_api.cpp reference the impls
    # unconditionally, so if heif_encode.cpp were ever excluded from an
    # OFF build, "HEIF excluded" would stop being a supported configuration.
    # Today the glob guarantees it, and this line is belt-and-braces.
    target_sources(dng_decoder_native PRIVATE ${SRC_DIR}/heif_encode.cpp)
    message(STATUS "HEIF: disabled (DNG_ENABLE_HEIF=OFF) — no heif_ symbols will be exported")
endif()

# libc++_shared.so staging (A-T8-FIX, 2026-09-01) -- UNCONDITIONAL, not gated
# on DNG_ENABLE_HEIF. native/CMakePresets.json's android-vulkan preset sets
# ANDROID_STL=c++_shared, so dng_decoder_native itself links the NDK's shared
# C++ runtime regardless of which codecs are enabled; without staging this
# file next to (and therefore into the same jniLibs/packaging set as) the
# decoder .so, the app fails to load the decoder at all with an error naming
# libc++_shared.so, not any codec. This is safe alongside the HEIF dist's own
# libheif.so/libde265.so, which link libc++ STATICALLY (round2-heif-baton.md
# measured fact) -- no duplicate-symbol or ABI-crossing hazard, because no
# C++ ABI crosses the libheif C API boundary either library uses.
if(ANDROID)
    # ANDROID_NDK (not CMAKE_ANDROID_NDK) is the variable the NDK's OWN
    # android.toolchain.cmake sets -- this project configures via that
    # toolchain file (native/CMakePresets.json's toolchainFile, invoked with
    # -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake by
    # build_native_watchdog.py), not CMake's own built-in
    # CMAKE_SYSTEM_NAME=Android cross-compiling support, which is what sets
    # CMAKE_ANDROID_NDK instead. Using the wrong one here would silently
    # resolve to an empty/undefined variable.
    if(NOT DEFINED ANDROID_NDK OR NOT ANDROID_NDK)
        message(FATAL_ERROR
            "ANDROID build but ANDROID_NDK is not set -- cannot locate "
            "libc++_shared.so. This should be set by the NDK's own "
            "android.toolchain.cmake; check native/CMakePresets.json's "
            "toolchainFile entry.")
    endif()
    # Host toolchain directory name varies (linux-x86_64, darwin-x86_64), so
    # glob it rather than hardcoding -- same approach as
    # native/tests/run_decode_matrix.py's _libcxx_path() for the same file.
    file(GLOB CEYX_ANDROID_LIBCXX_SHARED
         "${ANDROID_NDK}/toolchains/llvm/prebuilt/*/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so")
    list(LENGTH CEYX_ANDROID_LIBCXX_SHARED _ceyx_libcxx_count)
    if(_ceyx_libcxx_count EQUAL 0)
        message(FATAL_ERROR
            "libc++_shared.so not found under ${ANDROID_NDK}/toolchains/llvm/prebuilt/*/sysroot/usr/lib/aarch64-linux-android/ "
            "-- dng_decoder_native is built with ANDROID_STL=c++_shared and "
            "cannot load without this file staged alongside it.")
    endif()
    list(GET CEYX_ANDROID_LIBCXX_SHARED 0 CEYX_ANDROID_LIBCXX_SHARED_PATH)
    add_custom_command(TARGET dng_decoder_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CEYX_ANDROID_LIBCXX_SHARED_PATH}"
                "$<TARGET_FILE_DIR:dng_decoder_native>/libc++_shared.so"
        COMMENT "Staging libc++_shared.so next to dng_decoder_native (ANDROID_STL=c++_shared)")
endif()

endif() # NOT DNG_HOST_GENERATORS_ONLY
