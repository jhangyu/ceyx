# generic_raw.cmake - production LibRaw/RawSpeed3 wiring of dng_decoder_native
#
# T6 (2026-10-02 techdebt campaign): moved verbatim out of tests.cmake so the
# shipped library's own generic-RAW dependencies are a top-level include and
# cannot disappear with a "make tests optional" change. Included from
# CMakeLists.txt AFTER tests_early.cmake and BEFORE tests.cmake, the exact
# position this block always executed at.
#
# ORDERING IS LOAD-BEARING: the FindJPEG shim below redefines JPEG_LIBRARIES /
# JPEG_INCLUDE_DIRS. The Android cross test targets in tests_early.cmake
# (test_decode_android, dng_ffi_harness_android, test_device_handoff_android,
# test_stage4_yuv420_output_android) read the PRE-shim values; every target
# configured after this file sees the shim. Do not include this file earlier.

# Re-guard (split mechanics): see the end of tests_early.cmake.
if(NOT DNG_HOST_GENERATORS_ONLY)

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
    # MOVED OUT 2026-09-12 (CI run 34697591379). The base policy computation
    # that used to live here -- `if(ANDROID OR IOS) OFF else() ON` -- now lives
    # in cmake/openmp_policy.cmake, which native/CMakeLists.txt includes BEFORE
    # every consumer. It was wrong for it to live here for two independent
    # reasons, both of which silently disarmed heif.cmake's Windows
    # OpenMP-runtime staging (and its FATAL guard) rather than failing:
    #   1. ORDERING: tests.cmake is included AFTER heif.cmake, so the variable
    #      did not exist when heif.cmake read it.
    #   2. SCOPE: this point is nested inside `if(NOT DNG_HOST_GENERATORS_ONLY)`
    #      -> `if(DNG_ENABLE_GENERIC_RAW)`, so the project-wide policy was also
    #      hostage to two unrelated feature flags.
    # What REMAINS here is only the Apple-specific NARROWING below (probe for a
    # real libomp binary, honest-OFF if absent) -- a refinement of the policy,
    # not a competing definition of it.
    #
    # Assert rather than assume: an undefined guard variable FAILS OPEN in
    # CMake (empty name evaluates false), which is exactly the silent-skip
    # failure mode this whole change exists to remove.
    if(NOT DEFINED CEYX_ENABLE_DESKTOP_OPENMP)
        message(FATAL_ERROR
            "CEYX_ENABLE_DESKTOP_OPENMP is not defined at cmake/tests.cmake. "
            "It is set by cmake/openmp_policy.cmake, which native/CMakeLists.txt "
            "must include BEFORE this file. Someone reordered or removed that "
            "include. Failing loudly instead of defaulting: a false-y undefined "
            "value silently turns OpenMP off AND skips the Windows OpenMP "
            "runtime staging + its FATAL check, shipping an unloadable DLL "
            "(CI run 34697591379).")
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

    # F-T8b-2-STYLE FIX (2026-09-08, task #14 Part B): RawSpeed3 declares
    # `option(WITH_OPENMP "Enable OpenMP support." ON)`
    # (native/third_party/libraw/RawSpeed3/rawspeed/CMakeLists.txt:58) — the
    # IDENTICAL mechanism that made libraw-cmake's ENABLE_OPENMP a no-op
    # (fixed at ~line 1067 above, commit 68dd3a5): a plain set() of the same
    # name is a normal variable, and CMP0077=NEW is not guaranteed to be in
    # effect in a FetchContent/add_subdirectory child scope, so option()
    # here would clear it and RawSpeed3's own default (ON) would win on
    # mobile in violation of the P17 policy (OpenMP ON for desktop, OFF for
    # iOS/Android) recorded at the top of this file. Using the FORCEd-CACHE
    # pattern proven at the ENABLE_LCMS fix (~line 1313) and the
    # ENABLE_OPENMP fix (~line 1067) so both branches are forced and
    # intentional rather than relying on find_package(OpenMP) failing by
    # accident. RawSpeed3 (ENABLE_RAWSPEED) is not built as of this fix, so
    # this path is unverified beyond a clean reconfigure of the existing
    # build tree — the RawSpeed3-enabled path remains untested until that
    # engine is ever enabled.
    if(CEYX_ENABLE_DESKTOP_OPENMP)
        set(WITH_OPENMP ON CACHE BOOL
            "RawSpeed3 OpenMP: ON for desktop per the P17 policy" FORCE)
    else()
        set(WITH_OPENMP OFF CACHE BOOL
            "RawSpeed3 OpenMP: OFF for mobile (Android/iOS) per the P17 policy" FORCE)
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
    # SYMMETRIC-PINNING (2026-09-01, user ruling): widened from
    # `DNG_CROSS_BUILD OR WIN32` to also include native (non-cross) macOS.
    # Established by observation, not by reading the code and assuming it
    # already worked: before this widening, a native-macOS configure with a
    # real vendored native/third_party/jpegturbo-arm64/ directory in place
    # still resolved JPEG to `/opt/homebrew/lib/libjpeg.dylib` (live off this
    # machine's package manager) -- the whole shim block below, including the
    # JPEG-WIRING-X86_64 arch-suffixed vendored-dir lookup, was unreachable on
    # native macOS because it lived entirely inside this cross/Windows-only
    # gate. Widening it here is what lets a pinned native/third_party/
    # jpegturbo-arm64/ (or lcms2-arm64/, already covered -- its own gate,
    # `if(APPLE AND NOT ANDROID AND NOT IOS)`, already ran unconditionally on
    # every Apple leg and needed NO change, confirmed the same way: a real
    # vendored native/third_party/lcms2-arm64/ resolved correctly with zero
    # code changes) replace the live-off-this-machine resolution on the
    # native leg too, matching the ruling's symmetric-pinning intent.
    # The BINARY_PACKAGE_BUILD sub-block just below stays genuinely
    # cross-only via its own nested `if(DNG_CROSS_BUILD)` -- this widening
    # only affects the JPEG shim past it.
    if(DNG_CROSS_BUILD OR WIN32 OR APPLE)
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
        # JPEG-WIRING-X86_64 (2026-09-01): on native (non-cross) macOS, this
        # whole `if(DNG_CROSS_BUILD OR WIN32)` block is skipped, so
        # RawSpeed3's bare find_package(JPEG) runs UNSHIMMED and resolves via
        # CMake's own builtin FindJPEG.cmake — which, on this host, finds
        # Homebrew's SHARED libjpeg.dylib (not the DNG SDK's carefully
        # arch-checked STATIC ${JPEG_LIBRARIES} from third_party.cmake's
        # elseif(APPLE) branch just above, a completely separate consumer).
        # That is confirmed, not assumed: inspecting the real committed
        # decoder's load commands (native/scripts/tmp/real-artifact-check/
        # libdng_decoder_native.dylib.deps.txt) shows @rpath/libjpeg.8.dylib
        # as a direct dependency, while libheif.1.dylib's own deps
        # (native/scripts/tmp/ci-t11-parity/libheif.1.dylib.build.otool.txt)
        # do NOT reference jpeg at all — ruling out "it's transitive via
        # HEIF" and pointing at RawSpeed3's own unshimmed dynamic resolution,
        # which propagates through LibRaw's static link into the shared
        # decoder as an INTERFACE dependency. On the macOS x86_64 CROSS leg,
        # this same shim block IS active (DNG_CROSS_BUILD=ON) and currently
        # points RawSpeed3 at the same STATIC vendored archive the DNG SDK
        # uses — correct and working, but it means x86_64 produces NO dynamic
        # jpeg dependency at all, so there is nothing for
        # bundle_macos_dylib_deps.py to vendor and the shipped Intel decoder
        # ends up missing the jpeg companion arm64 has. This is a genuine
        # missing-companion capability gap, not a wrong-architecture bug like
        # the OpenMP/LCMS2 fixes above — verified via this exact configure:
        # before this override, "Looking for JPEG - found" resolves to the
        # STATIC archive with no cache entry or resulting library anywhere
        # named libjpeg*.dylib.
        #
        # Fix: on ANY macOS leg (native or cross — SYMMETRIC-PINNING widened
        # this from cross-only, see the outer gate's own comment above), if a
        # vendored DYNAMIC jpeg-turbo exists at the arch-suffixed vendored dir
        # (mirroring the libomp-<arch>/, lcms2-<arch>/ convention above),
        # point ONLY this shim (RawSpeed3's consumer) at it instead of the
        # static archive — reproducing the exact edge the arm64 leg produces
        # by accident today (and, once a pinned native/third_party/
        # jpegturbo-arm64/ lands, replacing that accident with a deterministic
        # pinned resolution instead of "whatever this machine's Homebrew has
        # installed"). The DNG SDK's own direct static jpeg link
        # (third_party.cmake, used for its own App-Sandbox-safety reasons)
        # is completely untouched by this — only RawSpeed3's separate,
        # already-independent JPEG resolution changes.
        set(_dng_shim_jpeg_include_dirs "${JPEG_INCLUDE_DIRS}")
        set(_dng_shim_jpeg_libraries "${JPEG_LIBRARIES}")
        if(APPLE)
            set(_dng_jpeg_want_archs "${CMAKE_OSX_ARCHITECTURES}")
            if(NOT _dng_jpeg_want_archs)
                set(_dng_jpeg_want_archs "${CMAKE_SYSTEM_PROCESSOR}")
            endif()
            foreach(_dng_jpeg_want_arch IN LISTS _dng_jpeg_want_archs)
                set(_dng_jpeg_dyn_dir "${THIRD_PARTY_DIR}/jpegturbo-${_dng_jpeg_want_arch}")
                if(EXISTS "${_dng_jpeg_dyn_dir}/include/jpeglib.h")
                    file(GLOB _dng_jpeg_dyn_lib "${_dng_jpeg_dyn_dir}/lib/libjpeg.*.dylib")
                    list(LENGTH _dng_jpeg_dyn_lib _dng_jpeg_dyn_lib_count)
                    if(_dng_jpeg_dyn_lib_count GREATER 0)
                        list(GET _dng_jpeg_dyn_lib 0 _dng_jpeg_dyn_lib_first)
                        execute_process(COMMAND lipo -archs "${_dng_jpeg_dyn_lib_first}"
                                        OUTPUT_VARIABLE _dng_jpeg_dyn_have
                                        OUTPUT_STRIP_TRAILING_WHITESPACE
                                        ERROR_QUIET RESULT_VARIABLE _dng_jpeg_dyn_rc)
                        if(_dng_jpeg_dyn_rc EQUAL 0
                           AND "${_dng_jpeg_dyn_have}" MATCHES "(^| )${_dng_jpeg_want_arch}( |$)")
                            # ARTIFACT-ID FIX (2026-09-01): identical defect
                            # class as LCMS2-X86_64's — this vendored dir is a
                            # raw extracted Homebrew bottle whose own
                            # `otool -D` is the literal unresolved token
                            # "@@HOMEBREW_PREFIX@@/opt/jpeg-turbo/lib/
                            # libjpeg.8.dylib", not a real path. Linking
                            # directly against it would bake that broken
                            # token into the decoder's LC_LOAD_DYLIB
                            # verbatim (ld/dyld record a dependency's load
                            # command from ITS OWN LC_ID_DYLIB, not from the
                            # path used to find it). Reuse the same
                            # copy-then-rewrite shape as the OpenMP
                            # consumption path above and the LCMS2 fix
                            # immediately above it, rather than inventing a
                            # third one.
                            set(_dng_jpeg_dyn_vendored "${CMAKE_BINARY_DIR}/libjpeg.8.dylib")
                            if(NOT EXISTS "${_dng_jpeg_dyn_vendored}"
                               OR "${_dng_jpeg_dyn_lib_first}" IS_NEWER_THAN "${_dng_jpeg_dyn_vendored}")
                                file(COPY "${_dng_jpeg_dyn_lib_first}"
                                     DESTINATION "${CMAKE_BINARY_DIR}"
                                     FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                                                      GROUP_READ GROUP_EXECUTE
                                                      WORLD_READ WORLD_EXECUTE)
                                get_filename_component(_dng_jpeg_dyn_lib_first_name
                                                        "${_dng_jpeg_dyn_lib_first}" NAME)
                                if(NOT _dng_jpeg_dyn_lib_first_name STREQUAL "libjpeg.8.dylib")
                                    file(RENAME "${CMAKE_BINARY_DIR}/${_dng_jpeg_dyn_lib_first_name}"
                                                 "${_dng_jpeg_dyn_vendored}")
                                endif()
                                execute_process(COMMAND install_name_tool -id "@rpath/libjpeg.8.dylib"
                                                        "${_dng_jpeg_dyn_vendored}"
                                                RESULT_VARIABLE _dng_jpeg_dyn_id_rc)
                                execute_process(COMMAND codesign --force --sign -
                                                        "${_dng_jpeg_dyn_vendored}"
                                                RESULT_VARIABLE _dng_jpeg_dyn_sign_rc)
                                if(NOT _dng_jpeg_dyn_id_rc EQUAL 0 OR NOT _dng_jpeg_dyn_sign_rc EQUAL 0)
                                    message(FATAL_ERROR
                                        "[ceyx] failed to vendor libjpeg.8.dylib "
                                        "(install_name_tool rc=${_dng_jpeg_dyn_id_rc}, "
                                        "codesign rc=${_dng_jpeg_dyn_sign_rc}). Refusing "
                                        "to continue: a partially-vendored copy would "
                                        "ship the same broken-placeholder-ID defect this "
                                        "fix exists to close.")
                                endif()
                                message(STATUS "[ceyx] vendored libjpeg.8.dylib -> ${_dng_jpeg_dyn_vendored} (@rpath/libjpeg.8.dylib)")
                            endif()
                            set(_dng_shim_jpeg_include_dirs "${_dng_jpeg_dyn_dir}/include")
                            set(_dng_shim_jpeg_libraries "${_dng_jpeg_dyn_vendored}")
                            message(STATUS
                                "[ceyx] RawSpeed3 JPEG: using arch-verified "
                                "DYNAMIC ${_dng_jpeg_dyn_vendored} vendored from "
                                "${_dng_jpeg_dyn_dir} (companion parity with "
                                "the arm64 leg's own incidental dynamic jpeg "
                                "dependency); DNG SDK's own static jpeg link "
                                "is unaffected.")
                        else()
                            message(STATUS
                                "[ceyx] ${_dng_jpeg_dyn_dir} has jpeglib.h but "
                                "no matching-arch dylib (lipo -archs => "
                                "'${_dng_jpeg_dyn_have}', wanted "
                                "'${_dng_jpeg_want_arch}'); RawSpeed3 stays on "
                                "the static vendored jpeg-turbo (no capability "
                                "regression, just no jpeg companion in the "
                                "release asset for this leg).")
                        endif()
                    endif()
                endif()
            endforeach()
        endif()
        file(WRITE ${_dng_jpeg_shim_dir}/FindJPEG.cmake
"# Generated by cmake/tests.cmake for hosts with no system libjpeg (NDK
# cross builds and Windows); resolves JPEG to the
# vendored libjpeg-turbo built by cmake/third_party.cmake in this build tree
# (or, on the macOS x86_64 cross leg when one is vendored, to a DYNAMIC
# arch-matched jpeg-turbo -- see JPEG-WIRING-X86_64 above).
set(JPEG_FOUND TRUE)
set(JPEG_INCLUDE_DIRS \"${_dng_shim_jpeg_include_dirs}\")
list(GET JPEG_INCLUDE_DIRS 0 JPEG_INCLUDE_DIR)
set(JPEG_LIBRARIES \"${_dng_shim_jpeg_libraries}\")
set(JPEG_LIBRARY \"${_dng_shim_jpeg_libraries}\")
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

    # PORTABLE-BASELINE (2026-09-08): RawSpeed3's CpuMarch.cmake compiles with
    # `-march=native` unless BINARY_PACKAGE_BUILD is set
    # (RawSpeed3/rawspeed/cmake/Modules/CpuMarch.cmake:3-9), and the
    # BINARY_PACKAGE_BUILD block above is deliberately cross-only. That left
    # every NATIVE build -- notably the Linux publish leg -- compiling RawSpeed3
    # for the *builder's* CPU. The AVX-512-capable GitHub runner that produced
    # the v0.1.19 Linux release baked EVEX-prefixed instructions into
    # libdng_decoder_native.so (254 disassembly lines, all in rawspeed/pugixml
    # symbols; see docs/logs/2026-09-08/), some of them reached during ELF
    # static init -- so dlopen() SIGILLs on any machine without AVX-512.
    #
    # This is the "guard written on the motivating platform, not on the real
    # precondition" family (lessons 2026-08-28): the real precondition is not
    # "cross-compiling", it is "this artifact will run on machines other than
    # the builder" -- which is true of EVERY artifact this project produces.
    # So the portable baseline is set unconditionally here, not per-platform.
    #
    # Mechanism: CpuMarch.cmake is a no-op when RAWSPEED_MARCH is already
    # DEFINED, so pre-defining it drives upstream through its own documented
    # knob and needs no patch to the vendored tree. `-mtune=generic` is exactly
    # what upstream's own binary-distribution branch selects (CpuMarch.cmake:27-35):
    # tuning only, no ISA raise. No compiler is excluded: this used to sit
    # under `if(NOT MSVC)` on the belief that MSVC has no -march=native, but
    # clang-cl sets MSVC=TRUE AND accepts -march=native, so every Windows
    # artifact inherited the runner's ISA (v0.1.28's pugixml faulted on a
    # non-AVX-512 CPU with an EVEX vpcmpneqq; v0.1.13/18/24 were contaminated
    # the same way). ci.py assert-no-avx512 --platform windows is the gate.
    #
    # Written as CACHE INTERNAL ... FORCE, mirroring how CpuMarch.cmake itself
    # stores the value (CpuMarch.cmake:38). A plain set() would NOT be enough:
    # any build dir already configured before this fix carries a cached INTERNAL
    # RAWSPEED_MARCH=-march=native, which is replayed as DEFINED at the top of
    # every later configure -- so a `if(NOT DEFINED)` guard would silently keep
    # the poisoned value. An explicit -DRAWSPEED_MARCH=<something> on the command
    # line is still honoured: it lands in the cache as a non-"native" value and
    # is preserved by the first branch below.
    if(DEFINED RAWSPEED_MARCH AND NOT RAWSPEED_MARCH MATCHES "native")
        message(STATUS "RawSpeed3: honouring explicit RAWSPEED_MARCH=${RAWSPEED_MARCH}")
    else()
        set(RAWSPEED_MARCH "-mtune=generic" CACHE INTERNAL "" FORCE)
        message(STATUS "RawSpeed3 portable baseline: RAWSPEED_MARCH=${RAWSPEED_MARCH} "
                       "(no -march=native; published artifacts must run off the build machine)")
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
    # F-T8b-2 FIX (2026-09-07): these MUST be FORCEd CACHE entries, not plain
    # set()s. libraw-cmake declares `option(ENABLE_OPENMP ... ON)`
    # (third_party/libraw-cmake/CMakeLists.txt:84) and CMP0077=NEW is NOT in
    # effect in that child directory scope, so option() CLEARS a plain normal
    # variable of the same name and the default (ON) wins. Both branches below
    # were therefore no-ops: desktop got ON by accident, and mobile got ON in
    # violation of the P17 user ruling at the top of this file (line ~407,
    # "OpenMP ON for desktop, OFF for mobile").
    #
    # This was predicted verbatim at lines ~1302-1310 of this file and called
    # "latent, currently-harmless": harmless only because find_package(OpenMP)
    # failed by itself on desktops with no libomp, which masked the broken
    # override. The Android NDK removes that accidental safety net -- its clang
    # accepts -fopenmp=libomp and the NDK ships libomp, so the lookup SUCCEEDS,
    # ENABLE_OPENMP stays ON, and the Android build linked libomp.so.
    #
    # Measured consequence before this fix: the shipped
    # plugin/android/.../libdng_decoder_native.so carried a libomp.so DT_NEEDED
    # entry while using ZERO OpenMP symbols (pure link-line residue), and
    # Android has no /system/lib64/libomp.so -- so dlopen(RTLD_NOW) failed with
    # 'library "libomp.so" not found' and the library could not load at all.
    # Evidence: tmp/verify/orient_prod_t8b_so_refresh.txt, findings F-T8b-1/2.
    #
    # The FORCEd-CACHE pattern is the one mechanism proven to work here -- see
    # the identical ENABLE_LCMS fix at line ~1313, adopted after a plain set()
    # was measured NOT to work. option() leaves an existing cache entry alone.
    if(CEYX_ENABLE_DESKTOP_OPENMP)
        set(ENABLE_OPENMP ON CACHE BOOL
            "LibRaw OpenMP: ON for desktop per the P17 policy (F-T8b-2)" FORCE)
    else()
        set(ENABLE_OPENMP OFF CACHE BOOL
            "LibRaw OpenMP: OFF for mobile (Android/iOS) per the P17 policy (F-T8b-2)" FORCE)
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
    # R2-T3 X3FTOOLS FIX: the plain non-cache set() this used to be (see git
    # blame) is silently discarded on a VIRGIN configure. libraw-cmake's own
    # cmake_minimum_required() predates CMake 3.13, so CMP0077 defaults OLD
    # in that subdirectory scope; OLD behavior for option() REMOVES any
    # existing normal variable of the same name and (re)creates the cache
    # entry from the option()'s own default (OFF). On a virgin configure no
    # cache entry exists yet, so this always fires and our ON is lost. On a
    # reconfigure the cache entry already exists and option() leaves
    # existing cache entries alone, which is why the plain set() looked like
    # it worked once a workaround (-DENABLE_X3FTOOLS=ON on the command line,
    # which seeds the cache directly) had already been used once.
    #
    # Fix: CACHE FORCE it here instead, same precedent as the LIBRAW_PATH
    # exception above (:1009) -- our forced cache entry exists before
    # libraw-cmake's option() call runs, so that call's "only create if
    # absent" rule leaves it alone regardless of CMP0077 policy. This is
    # unrelated to the F4 Halide/zlib ordering hazard: that hazard is about
    # variables Halide/zlib's own find_package() reads, and ENABLE_X3FTOOLS
    # is LibRaw-only. -DENABLE_X3FTOOLS=ON should still be passed on every
    # configure command as belt-and-braces until this fix is independently
    # re-verified across a full campaign.
    set(ENABLE_X3FTOOLS ON CACHE BOOL "" FORCE)

    # --- LCMS2 forced OFF (OQ-N4 option Z, 2026-09-12) ----------------------
    # WI-5 (docs/logs/2026-09-12/platform-parity-plan.md step 5.2b): lcms2 is
    # dead code on every platform -- it enters the build only through
    # LibRaw's ENABLE_LCMS, whose sole consumer is LibRaw::apply_profile(),
    # reachable only from dcraw_process(), which ceyx architecturally never
    # calls, and only when imgdata.params.camera_profile is non-NULL, which
    # ceyx never sets. Colour management is done with fixed matrices plus
    # DNG-SDK colour spaces. The former arch-guard block (~230 lines) that
    # sourced a real lcms2 for the macos-x86_64 cross leg is deleted along
    # with the vendored native/third_party/lcms2-{arm64,x86_64}/ trees; there
    # is nothing left to source.
    #
    # The FORCE CACHE form (not a plain set()) is required for the same two
    # measured reasons the deleted block's own comment recorded and which
    # must survive this deletion verbatim:
    #   (1) seeding a "...-NOTFOUND" CACHE value does not work -- find_path()/
    #       find_library() treat any "-NOTFOUND"-suffixed value as "not yet
    #       searched" and re-run their own unchecked search regardless.
    #   (2) a plain (non-cache) `set(ENABLE_LCMS OFF)` is NOT seen by
    #       libraw-cmake's own child-scope option() call -- CMP0077 does not
    #       apply there (confirmed via CMake's own dev warning on this exact
    #       configure), so the option() call clears it right back to ON.
    # A FORCEd CACHE BOOL is the only mechanism proven to work: option()
    # unconditionally leaves alone a cache entry that already exists.
    set(ENABLE_LCMS OFF CACHE BOOL
        "ceyx never calls dcraw_process; LibRaw's only lcms2 consumer is unreachable -- OQ-N4 option Z, 2026-09-12"
        FORCE)
    # The macos-x86_64 cross leg's capability assertion reads this line from
    # the configure log (it cannot dlopen a foreign-arch artifact) -- keep it
    # even though the deleted block's own "[ceyx] LCMS2: ..." status line is
    # gone with the rest of that apparatus (WI-5 plan step 5.4).
    message(STATUS "[ceyx] LCMS2: disabled (OQ-N4 option Z)")

    add_subdirectory(${LIBRAW_CMAKE_OVERLAY_DIR} libraw-cmake-build EXCLUDE_FROM_ALL)

    # WI-5 step 5.2: propagate ENABLE_LCMS's effective value to ceyx's own
    # target so ceyx_build_capabilities("ICC") reports a MEASURED answer,
    # not an asserted constant. Reading the variable here, immediately after
    # the add_subdirectory() above, is the only point where its effective
    # value is knowable (detail-lcms2-sourcing.md §D.1). Under OQ-N4 option Z
    # ENABLE_LCMS is unconditionally OFF (see the forced-CACHE set() above),
    # so this define lands 0 on every platform by one mechanism rather than
    # a platform conditional -- kept anyway as the instrument that would
    # catch a future accidental re-enable; deleting it would make ICC=0
    # unfalsifiable.
    target_compile_definitions(dng_decoder_native PRIVATE
        CEYX_HAVE_LCMS2=$<BOOL:${ENABLE_LCMS}>)

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

endif() # NOT DNG_HOST_GENERATORS_ONLY (continued in tests.cmake)
