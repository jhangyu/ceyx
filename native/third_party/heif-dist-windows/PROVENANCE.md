# HEIF encode+decode distribution (Windows x86-64) — provenance

Built by the Python carrier (`native/scripts/deps/win_heif_dist.py`, invoked
via `native/scripts/build_deps.py build heif-stack --platform windows
--arch x86_64`), run on a `windows-latest` GitHub Actions runner via
`.github/workflows/heif_dist_windows.yml`. Unlike the macOS dist (produced
locally and untracked), **this tree is committed**: no contributor machine in
this project can build Windows binaries, so the built bytes are a reviewed
input, pinned once and changed only by a visible diff.

**Encode-capable since 2026-08-31** (docs/logs/2026-08-31/spec-windows-codec-full-green.md,
in-scope item 1): the Windows dist was decode-only until this rebuild. kvazaar
(HEVC encode) and aom (AV1 encode+decode) are now built/acquired and merged
STATICALLY into `heif.dll` (`ENABLE_PLUGIN_LOADING=OFF`), exactly as on
macOS/Linux (`native/third_party/heif-dist/PROVENANCE.md`). No new shipped
DLL: the DLL count in the release archive stays 3.

## Component table

| Component | Version | Source (Windows) | Pin |
|---|---|---|---|
| libheif | 1.23.2 | tarball, https://github.com/strukturag/libheif/releases/download/v1.23.2/libheif-1.23.2.tar.gz | SHA-256 `8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405` |
| libde265 | 1.1.1 | tarball, https://github.com/strukturag/libde265/releases/download/v1.1.1/libde265-1.1.1.tar.gz (self-built from source on Windows permanently — see `[component.libde265.source.windows]`'s three durable blockers against the vcpkg port) | SHA-256 `fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219` |
| kvazaar | 2.3.1 | git clone, https://github.com/ultravideo/kvazaar (self-built from source on Windows: the v2.3.1 release tarball omits `src/threadwrapper/src/pthread.cpp`, which kvazaar's CMakeLists adds unconditionally when `WIN32` is true) | tag `v2.3.1` |
| aom | 3.15.0 | vcpkg registry (D1-a), triplet `x64-windows-heif`, copied out of the vcpkg install prefix — nothing compiled by this dist's own build (`build_aom()` in `win_heif_dist.py`) | version `3.15.0` (`native/vcpkg/vcpkg.json` `overrides`, resolved against pinned baseline `abb6dda5cc32914d2e64d7d72b974dc301d1fc8a`) |

`.pins` (committed alongside): `libheif=1.23.2:8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405 libde265=1.1.1:fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219 kvazaar=2.3.1:v2.3.1 aom=3.15.0:3.15.0 platform=windows-x86_64`

## Configure flags (confirmed from the run log, `native/deps/manifest.toml [component.libheif.cmake.windows]`)

- `WITH_KVAZAAR=ON` — confirmed: `-- Found kvazaar: .../include` and libheif's own configure summary "Compiling 'kvazaar' as built-in backend".
- `WITH_AOM_DECODER=ON` / `WITH_AOM_ENCODER=ON` — confirmed: `-- Found AOM` / `-- Found AOM: .../include` and "Compiling 'aomdec' as built-in backend" / "Compiling 'aomenc' as built-in backend".
- `WITH_X265=OFF` — confirmed: "Not compiling 'x265' backend" (GPL-2.0, excluded by name).
- `ENABLE_PLUGIN_LOADING=OFF`, `WITH_REDUCED_VISIBILITY=ON` — inherited from `[component.libheif.cmake.base]`, unchanged by this rebuild.

## Filename spelling note (IMPORTANT — do not "fix")

kvazaar's CMake target is named `kvazaar`, but clang-cl+Ninja installed its
static archive as **`lib/libkvazaar.lib`** (the `lib`-prefixed spelling), not
the nominal `lib/kvazaar.lib` the manifest's `KVAZAAR_LIBRARY` hint states.
This is the same by-toolchain naming asymmetry `win_heif_dist.py` already
handles for libde265's import library (`de265.lib` vs `libde265.lib`); it now
also resolves kvazaar's spelling by presence
(`win_heif_dist.resolve_kvazaar_library`). **The committed file is named
`lib/libkvazaar.lib`, not `lib/kvazaar.lib`.** Do not rename it to match an
older plan document's fixed-name acceptance line — the carrier resolves
either spelling by presence, and renaming would only reintroduce a stale
assumption the carrier was written to avoid.

## Built artifacts committed here (SHA-256, computed from the committed files)

| File | SHA-256 |
|---|---|
| `bin/heif.dll` | `58b723c407035ea490221494e150d0f10431d176528d200df3165a04723223e1` |
| `bin/libde265.dll` | `7974e31e2eec2dcce16d88122f3a44e239aa0fbb4e787965c7735ad25c09e765` |
| `lib/heif.lib` | `46ed10b6b906758440873f11b28148b6137da0e7a8530d6d824c1b52f6876ce2` |
| `lib/de265.lib` | `d606afc41621282d8f8f2b8754029b36a316fe46a0dd5ca7b06139f5915d3e9a` |
| `lib/libkvazaar.lib` | `92ef74d5b294600244d6d521fc91638437fdbd525d38286f4062ebdf5082ded5` |
| `lib/aom.lib` | `8fb525f3300512fc565d3188d2e7af3ee397740a57185ce363ffcbae64ab51e5` |
| `include/aom/aom.h` | `b25cc7277ade68ba9c43903c0139b8aad081a301d085f48390439ce6ebe76982` |
| `include/kvazaar.h` | `fc875992577bdd54eb20cb28057e673293f769246cc8a1d6f0b1d176178df361` |
| `share/licenses/aom/copyright` | `797958a3c220f1fd96ab1be1ecf61f57615593d74b0b170941ae1a309ae554a0` |
| `share/licenses/kvazaar/LICENSE` | `3c1dc3d7f8a3d08c14f4fbe9942f45764d8a21a3296e8d83446f33fd65f21c38` |
| `share/licenses/kvazaar/LICENSE.EXT.greatest` | `16f569c87d5ec20b7474b55ee0a8877b8f8b4dc13f9567ebe0b8fe8afdeb34d2` |
| `share/licenses/libde265/COPYING` | `02cc1585a20677992e0ba578fa692635dc193735f2691dc81de924b51c4e8020` |
| `share/licenses/libheif/COPYING` | `fa81ce652315b013359d6e8e4744335f31a50c7c192907176d3632f78a3b4596` |

(`include/libheif/**`, `include/libde265/**`, `include/aom/**` beyond `aom.h`,
`lib/cmake/**`, `lib/pkgconfig/**`, `share/man/**` are the rest of the
installed headers/CMake config/pkg-config/man page trees; every file above is
the one the digest loop below recomputes and checks against this table.)

## Licences vendored (`share/licenses/`)

- `libheif/COPYING` — LGPL-3.0-or-later.
- `libde265/COPYING` — LGPL-3.0-or-later.
- `kvazaar/LICENSE`, `kvazaar/LICENSE.EXT.greatest` — BSD-3-Clause (+ the
  bundled `greatest` test framework's licence).
- `aom/copyright` — BSD-2-Clause **AND the Alliance for Open Media Patent
  License 1.0** (vendored from the vcpkg install prefix's
  `share/aom/copyright`, which is strictly more complete than a source-tree
  glob: it carries LICENSE, **PATENTS**, and three third-party licences
  (fastfeat, vector, x86inc)). Confirmed present: `grep -ci patent
  share/licenses/aom/copyright` → 10 matches.

libheif/libde265 (LGPL-3.0-or-later) are built as SEPARATE SHARED LIBRARIES
and linked dynamically — LGPL-3 §4(d)(1) is satisfied outright, a user can
replace `heif.dll` / `libde265.dll` next to the application. kvazaar
(BSD-3-Clause) and aom (BSD-2-Clause + AOM Patent License 1.0) are STATIC
archives merged INTO `heif.dll` (`ENABLE_PLUGIN_LOADING=OFF`); their permissive
licences carry no source-availability duty for static linking. `WITH_X265`
stays OFF — x265 is GPL-2.0 and is excluded by name.

## Excluded from this commit

**Removed 2026-09-01 (ruling 3):** `bin/kvazaar.exe` (the kvazaar CLI tool)
and `share/man/man1/kvazaar.1` (its manpage) were dropped from this tree.
Neither has any consumer — the build links only `lib/libkvazaar.lib` as a
static archive merged into `heif.dll`; nothing in `plugin/`, `native/`, or
`.github/` invokes `kvazaar.exe` as a subprocess or references the manpage
(verified by grep before removal). `win_heif_dist.py`'s
`prune_unconsumed_cli_tools()` now deletes both from any future dist
assembly before it lands on disk, so a rebuild does not resurrect them.

Per Nit-5 ruling (round-2 review): dot-prefixed instrument outputs the
carrier writes during assertion (`.heif_exports.txt`, `.heif_deps.txt`) are
NOT committed into the dist tree — they are working scratch the carrier
regenerates on every run, not shipped artefacts. Neither was present in the
uploaded artifact for this run (the carrier writes them under `dist/`, but
`actions/upload-artifact@v4` did not surface them or the `.pins` stamp file
in the downloaded archive for this run; `.pins` above was reproduced locally
from `win_heif_dist.want_pins()`, which is a pure function of
`native/deps/manifest.toml` at the same commit and therefore byte-identical
to what the runner wrote).

## Build proof

Produced by run https://github.com/jhangyu/ceyx/actions/runs/33416381354
(head commit `4c53e81c3787728245248739d5fe83969275a556`), which logged
`HEIF_DIST_WINDOWS_RC=0` and the following ASSERT lines:

```
[heif-win] ASSERT heif_decode_image OK (466 exports seen)
[heif-win] ASSERT de265 dependency OK
[heif-win] ASSERT no-x265 OK
[heif-win] ASSERT static archive lib/libkvazaar.lib OK (1312404 bytes)
[heif-win] ASSERT static archive lib/aom.lib OK (43199828 bytes)
[heif-win] ASSERT no dynamic aom/kvazaar OK (import table: libde265.dll, KERNEL32.dll only)
[heif-win] ASSERT PE32+ x86-64 OK
[heif-win] ASSERT aom licence (incl. PATENTS) vendored from vcpkg prefix OK
[heif-win] ASSERT libheif licence vendored (1 file(s)) OK
[heif-win] ASSERT libde265 licence vendored (1 file(s)) OK
[heif-win] ASSERT kvazaar licence vendored (2 file(s)) OK
HEIF_DIST_WINDOWS_RC=0
```

This run followed one prior failed attempt (run 33415312766, same commit
lineage minus one fix): kvazaar's static archive installed as
`lib/libkvazaar.lib`, not the manifest's nominal `lib/kvazaar.lib`, causing
`ninja: error: '.../lib/kvazaar.lib', needed by 'libheif/heif.dll', missing
and no known rule to make it`. Fixed by `win_heif_dist.resolve_kvazaar_library()`
(by-presence resolution, mirroring the existing libde265 import-library
resolver) — see commit `4c53e81`.

## Round-trip / functional proof (accepted gap)

There is NO end-to-end proof that the kvazaar/aom encoders actually WORK: the
round-trip gate was removed by the 2026-08-31 compile-only ruling. The
strongest surviving claim is libheif's own `heif_have_encoder_for_format` /
`heif_have_decoder_for_format` runtime answer, queried by `probe_codecs`
(CI-T3) in the product build leg.
