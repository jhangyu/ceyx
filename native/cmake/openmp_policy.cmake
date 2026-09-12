# openmp_policy.cmake -- the project-wide desktop-OpenMP policy variable.
#
# WHY THIS FILE EXISTS (2026-09-12, CI run 34697591379, Windows leg red).
# CEYX_ENABLE_DESKTOP_OPENMP is a PROJECT-WIDE POLICY, but it used to be
# computed deep inside cmake/tests.cmake, nested in two unrelated guards
# (`if(NOT DNG_HOST_GENERATORS_ONLY)` -> `if(DNG_ENABLE_GENERIC_RAW)`), and
# tests.cmake is included LAST by native/CMakeLists.txt. Its first CONSUMER,
# the Windows OpenMP-runtime staging block in cmake/heif.cmake, is included
# EARLIER. So at heif.cmake time the variable did not exist yet, CMake
# evaluated the empty name as false, and the whole staging block -- including
# the find_file() lookup AND the configure-time FATAL_ERROR meant to catch a
# missing runtime -- was silently skipped. The result was a green build whose
# dng_decoder_native.dll imports libomp140.x86_64.dll while that DLL was
# never staged or shipped: an artifact that cannot load on a user's machine.
# The import-closure gate (WI-4) caught it, which is the gate working as
# designed; this file removes the cause.
#
# Two structural rules follow from that incident, and both are enforced
# rather than merely documented:
#   1. This file is included by native/CMakeLists.txt BEFORE any consumer, so
#      the variable is always DEFINED by the time anything reads it.
#   2. Consumers do not write `if(CEYX_ENABLE_DESKTOP_OPENMP)` against a
#      possibly-undefined name. They first assert it is DEFINED and FATAL out
#      if not (see heif.cmake's WIN32 branch and tests.cmake), because an
#      undefined guard FAILS OPEN -- it skips the check that was supposed to
#      protect the artifact, and does so completely silently.
#
# Scope note: this file computes the BASE policy only (the platform question:
# does this toolchain have an OpenMP runtime at all?). tests.cmake may still
# narrow it to OFF later on Apple when no libomp binary is actually found --
# that is the deliberate "honest OFF" pattern, a refinement of this value
# based on a real probe, not a competing definition of the policy.

# User ruling: OpenMP ON for desktop (macOS/Linux/Windows), OFF for mobile
# (iOS/Android) per the P17 five-platform policy.
#
# OMP-CROSS-FIX (2026-09-01, moved here verbatim from cmake/tests.cmake):
# this used to also gate OFF on DNG_CROSS_BUILD, on the premise that
# "cross-compiling" implies "cannot build/link OpenMP for the target arch".
# That premise is false for this project's ONLY DNG_CROSS_BUILD=ON desktop leg
# (macOS x86_64, built on an arm64 runner): DNG_CROSS_BUILD exists solely to
# skip Halide's two-stage AOT generator scheme (the arm64 host cannot EXECUTE
# x86_64 generator binaries -- see halide_aot.cmake), not because the
# toolchain cannot compile/link x86_64 code. Apple clang on this host accepts
# -arch x86_64 for ordinary compile+link (no execution of target-arch code is
# required to build a library), so RawSpeed3/LibRaw's OpenMP-guarded loops are
# exactly as buildable for the x86_64 leg as for the native arm64 leg,
# PROVIDED an x86_64 libomp is available (see the vendored/brew search in
# tests.cmake -- a missing binary still degrades to OFF via the explicit "no
# libomp found" branch, never a silent skip). The true mobile precondition is
# ANDROID/IOS (no OpenMP runtime in those NDK/iOS-SDK toolchains at all),
# which is exactly what remains here.
#
# This unlocks parallelism that already exists in the vendored trees but was
# compiled out: RawSpeed3's FujiDecompressor `#pragma omp parallel` plus
# LibRaw's remaining `#pragma omp parallel for` loops. NOTE: LibRaw's Fuji
# strip decode (src/decoders/fuji_compressed.cpp) no longer depends on OpenMP
# at all -- round-2 patch 09 (2026-08-28) replaced its OpenMP branch with an
# unconditional std::thread pool on every platform, so the Fuji path stays
# parallel even on mobile and any OpenMP-less toolchain.
if(ANDROID OR IOS)
    set(CEYX_ENABLE_DESKTOP_OPENMP OFF)
else()
    set(CEYX_ENABLE_DESKTOP_OPENMP ON)
endif()

message(STATUS
    "[ceyx] desktop OpenMP policy: CEYX_ENABLE_DESKTOP_OPENMP="
    "${CEYX_ENABLE_DESKTOP_OPENMP} (base platform policy; may be narrowed to "
    "OFF later by an Apple libomp probe in cmake/tests.cmake)")
