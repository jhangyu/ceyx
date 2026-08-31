# libjxl distribution (Windows) — provenance

Built by `native/scripts/deps/win_jxl_dist.py` via
`python native/scripts/build_deps.py build jxl-stack --platform windows
--arch x86_64 --dist native/third_party/libjxl-dist-windows` (the Python
carrier — contract item 10 / ENTRY-POINT RULE,
`docs/logs/2026-09-01/contract-windows-codec-round.md`). Everything under
this directory except this file, `include/`, `lib/`, and `share/` is a build
artefact and is not itself tracked.

CI run: https://github.com/jhangyu/ceyx/actions/runs/33414813319
(`libjxl dist (windows x86_64, clang-cl, static, encode+decode)`,
`workflow_dispatch` on `main`), `JXL_DIST_WINDOWS_RC=0`.

## Pin mechanism (commit-SHA based, not a tarball SHA-256)

Same rationale as the macOS `libjxl-dist`
(`native/third_party/libjxl-dist/PROVENANCE.md`): upstream libjxl has never
published a release source tarball including the `third_party/highway` and
`third_party/brotli` submodules the static build requires, so the pin is a
`git clone` at a tagged commit plus a selective `git submodule update
--init` for only the submodules the static core library links (brotli,
highway, skcms).

- Tag: `v0.12.0`
- Commit: `a7a9c787341cf703dede03c2009fa460cae5e5df`
- Arch: `x86_64` (Windows)
- Toolchain: `clang-cl` + MSVC developer environment (x64), Ninja generator,
  `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`, `CMAKE_BUILD_TYPE=Release`.

### Submodule pins (`git submodule status` for brotli, highway, skcms)

```
 028fb5a23661f123017c060daa546b55cf4bde29 third_party/brotli (028fb5a)
 457c891775a7397bdb0376bb1031e6e027af1c48 third_party/highway (457c891)
 96d9171c94b937a1b5f0293de7309ac16311b722 third_party/skcms (96d9171)
```

Identical tag, commit and submodule SHAs to the macOS `libjxl-dist` build —
same upstream pin, different platform toolchain output.

## Build flags (from `win_jxl_dist.py::_CMAKE_ARGS_BASE`)

```
-DCMAKE_BUILD_TYPE=Release
-DCMAKE_C_COMPILER=clang-cl
-DCMAKE_CXX_COMPILER=clang-cl
-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded
-DBUILD_SHARED_LIBS=OFF
-DBUILD_TESTING=OFF
-DJPEGXL_ENABLE_TOOLS=OFF
-DJPEGXL_ENABLE_BENCHMARK=OFF
-DJPEGXL_ENABLE_EXAMPLES=OFF
-DJPEGXL_ENABLE_FUZZERS=OFF
-DJPEGXL_ENABLE_DOXYGEN=OFF
-DJPEGXL_ENABLE_MANPAGES=OFF
-DJPEGXL_ENABLE_SJPEG=OFF
-DJPEGXL_ENABLE_OPENEXR=OFF
-DJPEGXL_ENABLE_SKCMS=ON
-DJPEGXL_ENABLE_JNI=OFF
-DJPEGXL_FORCE_SYSTEM_BROTLI=OFF
-DJPEGXL_FORCE_SYSTEM_HWY=OFF
```

`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` (static `/MT`, not `/MD`) is
mandatory for the same reason as every other Windows dist in this project: a
`/MD` archive linked into the `/MT` decoder DLL fails as duplicate symbols or
heap corruption, never as a clean configure error. `-DJPEGXL_ENABLE_SKCMS=ON`
and the full highway target set are load-bearing per R7 (never disabled to
make the build pass — see `win_jxl_dist.py` module docstring).

## Licence and linkage

libjxl, brotli and skcms are BSD-3-Clause; highway (Google) is Apache-2.0. All
four are linked **statically** into the Windows product build — BSD-3/Apache-2.0
carry no relink duty, unlike the LGPL-3 heif-dist, which is why this dist is
static where heif-dist is dynamic (see `cmake/jxl.cmake`). skcms is pulled in
because this dist builds with `-DJPEGXL_ENABLE_SKCMS=ON`; its object code
ships inside `jxl_cms.lib`, which `jxl.lib`'s `JxlGetDefaultCms` requires at
link time (same load-bearing relationship as the macOS dist). Licence files
for all four vendored dependencies are under `share/licenses/{libjxl,highway,brotli,skcms}/`
and must ship alongside any distributed build that includes JXL support.
`share/licenses/skcms/LICENSE` is byte-identical to the macOS `libjxl-dist`'s
copy (`native/third_party/libjxl-dist/share/licenses/skcms/LICENSE`) — both
dists pin the same `third_party/skcms` submodule commit `96d9171c94b937a1b5f0293de7309ac16311b722`
(see the submodule pins table above), so vendoring the already-verified file
carries the exact BSD-3 text at that pinned commit, not a re-fetch of a
possibly-different revision.

## Symbol proof (build-time, `win_jxl_dist.py::assert_symbols`)

`llvm-nm --defined-only lib/jxl.lib`, captured to a file (never piped into a
matcher — see module docstring's no-pipe rationale) and asserted to contain:

```
ASSERT JxlEncoderProcessOutput OK
ASSERT JxlDecoderProcessInput OK
ASSERT JxlEncoderAddBox OK
```

## Static libraries (`lib/`) — SHA-256

| File | SHA-256 |
|---|---|
| `jxl.lib` | `59342327c2e659e08551c5d814e4c0729504f8ba2d3e651874d11fa81675b793`[^len] |
| `jxl_cms.lib` | `4019c351986e17abd6a2653f738e0951ef52ad47bf7b70e0f1881b464d882fc8`[^len] |
| `jxl_threads.lib` | `6b1756d7d1c4373c941de7ad3d05b6e71087ff5203595bf24cc0890fd079d30d`[^len] |
| `hwy.lib` | `6a8139645057436663b9fcbc5d5c3e0b48524725cba702aa1741d42c8d3a9e56`[^len] |
| `brotlicommon.lib` | `4e8364c9814b11bf2381cd694ac0420e38fb0efd04131c4cc2481257942a0ad6`[^len] |
| `brotlidec.lib` | `7620d2a60a939d9dac8d22886711b74a8dd7da853b92892215ce577c0ba632e3`[^len] |
| `brotlienc.lib` | `e1b01c91bfcd0a84ed2ec3ed42fc0914a142809b888556d6febf7de18a466a49`[^len] |
| `include/jxl/encode.h` | `dcef8b08c430d4c15b10268eb9f6d328a8145ba8ea27435826908622f3cbead5`[^len] |

[^len]: computed via `shasum -a 256` on the artifact downloaded from the CI
run above (`native/scripts/tmp/jxl-dist-download/`), each digest verified 64
hex characters.

## Licence files (`share/licenses/`) — SHA-256

| File | SHA-256 |
|---|---|
| `share/licenses/skcms/LICENSE` | `e59bb5c5c6ba426a9ac4ba9fe667ad14c5166b12aa25be8af1d122b14fbe2e36` |

Computed via `shasum -a 256 native/third_party/libjxl-dist-windows/share/licenses/skcms/LICENSE`
(64 hex characters). This file is a copy of the macOS `libjxl-dist`'s
`share/licenses/skcms/LICENSE`, sourced from the pinned `third_party/skcms`
submodule commit `96d9171c94b937a1b5f0293de7309ac16311b722` (tag `v0.12.0`
checkout) — see "Licence and linkage" above.

All release-built with `CMAKE_BUILD_TYPE=Release`. Windows archives are
MSVC-style `.lib` (not `lib*.a`), matching the Windows toolchain's naming
convention — `cmake/jxl.cmake` looks up these exact filenames as static
inputs.
