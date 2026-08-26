# Vendored LLVM OpenMP runtime (libomp)

Committed directly (756 KB total) so a blank checkout builds with full desktop
OpenMP and no Homebrew prerequisite. Too small to warrant the fetch-script
pattern used for Halide (>100 MB).

## Source

| | |
|---|---|
| Upstream project | LLVM OpenMP runtime — https://openmp.llvm.org/ |
| Version | **22.1.8** |
| Obtained from | Homebrew bottle `libomp` 22.1.8 (keg-only), formula https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/lib/libomp.rb |
| Copied from | `/opt/homebrew/Cellar/libomp/22.1.8/` on macOS 24.6.0 / Apple clang 17.0.0 |
| Date vendored | 2026-08-27 |

## Contents and digests (sha256)

| File | sha256 |
|---|---|
| `lib/libomp.dylib` | `d42b5c1021ece1057fd19b747635f0ded51241b5bbdd59a064698c9db7b83ed6` |
| `include/omp.h` | `5974470842520cea4bc50136e2329bbf4e36ba928d317e86f7def2ba1752d3d4` |
| `LICENSE.TXT` | `8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee` |

Files are **pristine upstream copies**, deliberately unmodified — the digests
above can be checked against a fresh `brew install libomp` of the same version.
In particular `lib/libomp.dylib` still carries its upstream install name
`/opt/homebrew/opt/libomp/lib/libomp.dylib`; the build rewrites a *copy* in the
build directory to `@rpath/libomp.dylib` (see below). Do not rewrite the file
here — that would break digest verification against upstream.

## Why only `omp.h`

`omp.h` is the only OpenMP header anything in this project includes (verified by
grep across `native/src`, `native/tests`, LibRaw and RawSpeed3 sources — the
consumers are `RawSpeed3/.../adt/Mutex.h`,
`common/GetNumberOfProcessorCores.cpp`, and `utilities/rsbench/main.cpp`).
Upstream also ships `omp-tools.h` / `ompt.h` / `ompx.h`, which exist for OMPT
tooling/profiler attachment and are not referenced here, so they are omitted.
`omp.h` includes no other headers, so nothing dangles.

## LIMITATION: arm64 only

`lib/libomp.dylib` is **arm64-only** (`lipo -archs` → `arm64`). It is not a fat
binary and will not link an x86_64 (Intel Mac) build.

`native/cmake/tests.cmake` therefore checks the vendored library's
architectures against the build's target architecture and **falls back to the
Homebrew prefix search when they do not match**, rather than preferring the
vendored copy unconditionally. So an Intel Mac keeps working exactly as before
(via `brew install libomp`), and only loses the "no prerequisite" property.
Adding an x86_64 slice (`lipo -create`) would remove that caveat.

## How the build consumes this

1. `tests.cmake` prefers this directory, subject to the architecture check.
2. It copies the chosen `libomp.dylib` into the build directory, sets its
   install name to `@rpath/libomp.dylib`, and ad-hoc re-signs it.
3. `OpenMP_omp_LIBRARY` points at that build-directory copy, so the shared
   library and every test executable link **one and the same image**.

Step 3 is not cosmetic. When the dylib and the executables reference libomp
under two different install names, dyld maps two separate images with two
independent sets of OpenMP thread-team state, which segfaulted every worker
thread in `test_raw_end_to_end` (`tmp/verify/fuji_SIGSEGV_ROOTCAUSE.md`).

## Licence

Apache License v2.0 **with LLVM Exceptions**, per the bundled `LICENSE.TXT`
(its own first line: "The LLVM Project is under the Apache License v2.0 with
LLVM Exceptions"). Note that `brew info libomp` reports "License: MIT", which
does not match the shipped licence text; the file is authoritative. Recorded in
`THIRD_PARTY_LICENSES.md`.
