# DEVIATIONS — overlay ports vs `native/deps/manifest.toml`

Acceptance for the OD-2 spike is *carrier-neutral*: same versions, same SHA pins, same flag
values. Everything below is a place where this overlay-port implementation is **not**
byte-for-byte the manifest. Read this before treating a green CI run as an equivalence proof.

Severity legend: **[S]** semantic — could change the shipped artefact; **[M]** mechanical —
same end state by a different route; **[R]** sanctioned by a recorded ruling.

---

## D1 [S] libheif carries three upstream vcpkg patches the shell script does not

`ports/libheif/portfile.cmake` applies `cxx-linkage-pkgconfig.diff`, `find-modules.diff` and
`symbol-exports.diff`, taken verbatim from upstream vcpkg `ports/libheif`. All three were
verified to apply cleanly to the **1.23.2** release tarball (upstream's port is at 1.23.1).

- `find-modules.diff` only adds `NAMES_PER_DIR` to three `find_library` calls — search-order
  only, no artefact change.
- `cxx-linkage-pkgconfig.diff` and `symbol-exports.diff` **do** affect the produced library
  (pkg-config `Libs.private`/C++ linkage, and the exported-symbol set).

This is the largest honest gap in the equivalence claim. `gdk-pixbuf.patch` and
`cmake-project-include.cmake` are deliberately **not** carried — they serve `WITH_GDK_PIXBUF`
and `WITH_X265`, both hard `OFF`.

## D2 [M] kvazaar is fetched as the git TAG ARCHIVE on every platform

`manifest.toml` uses the release tarball on macOS/Linux and a `git clone --branch v2.3.1` on
Windows, because the release tarball omits `src/threadwrapper/src/pthread.cpp`. The overlay
port uses `vcpkg_from_github(REF v2.3.1)` everywhere, which *is* the tagged tree — a strict
superset of the release tarball. Same tag, same upstream commit, one mechanism instead of two.

## D3 [S] Windows compile-flag lists are shorter, because vcpkg APPENDS rather than REPLACES

`scripts/toolchains/windows.cmake:79-80` seeds `CMAKE_C_FLAGS` / `CMAKE_CXX_FLAGS` with
`/nologo /DWIN32 /D_WINDOWS /utf-8` (plus `/GR /EHsc` for C++) and then appends
`VCPKG_C_FLAGS` / `VCPKG_CXX_FLAGS`. On the shell-script carrier, `-DCMAKE_CXX_FLAGS=...`
**replaces** CMake's defaults, which is why the script hand-restores `-DWIN32 -D_WINDOWS -EHsc
-GR` (pitfall N16). Resulting effective sets:

| Port | manifest.toml `CMAKE_C_FLAGS` | overlay effective C flags |
|---|---|---|
| kvazaar | `/clang:-msse4.1 /clang:-mavx2` | `/nologo /DWIN32 /D_WINDOWS /utf-8` + `/clang:-msse4.1 /clang:-mavx2` |
| libde265 | `-DWIN32 -D_WINDOWS /clang:-msse4.1` | `/nologo /DWIN32 /D_WINDOWS /utf-8` + `/clang:-msse4.1` |
| libheif | `-DWIN32 -D_WINDOWS -W3 -DKVZ_STATIC_LIB` | `/nologo /DWIN32 /D_WINDOWS /utf-8` + `-DKVZ_STATIC_LIB -W3` |

Every manifest token is present. **Additions** the carrier makes and the manifest does not:
`/nologo`, `/utf-8` on all three; `/DWIN32 /D_WINDOWS` on kvazaar (the manifest passes none
there); `/GR /EHsc` on kvazaar's and libde265's C++ where the manifest passes them only for
libde265. `/utf-8` is the one that could in principle change compilation (source charset).

Also from the toolchain, not from us: `/MT /O2 /Oi /Gy /DNDEBUG /Z7` in `*_FLAGS_RELEASE`.
The literal `/MT` there is *why* pitfall N7 (CMP0091 ignored below `cmake_minimum_required`
3.15) cannot recur on this carrier; the portfiles still pass
`-DCMAKE_POLICY_DEFAULT_CMP0091=NEW` as belt-and-braces (N8).

## D4 [M] Dependency paths are discovered, not pre-seeded

`manifest.toml` hard-codes `LIBDE265_LIBRARY`, `KVAZAAR_LIBRARY`, `AOM_LIBRARY`,
`*_INCLUDE_DIR`, `CMAKE_PREFIX_PATH`, `CMAKE_INSTALL_PREFIX`. Under vcpkg these are the
carrier's job: the installed prefix is on `CMAKE_PREFIX_PATH` and each port installs a
`.pc` file, so libheif's `Find*.cmake` modules resolve them. `CMAKE_IGNORE_PREFIX_PATH`
(N12, Homebrew `aom` shadowing) **is** still passed explicitly on macOS — the carrier does
not protect against that one.

## D5 [R] aom is the registry port at 3.15.0, not the self-built 3.12.1

Sanctioned by Spec §3.2 (Ruling 1: version bumps allowed; aom "joins the registry" at
3.15.0). Consequently aom's own option set is upstream vcpkg's, not `manifest.toml`'s
`[component.aom.cmake.*]`, and `AOM_TARGET_CPU` (K16) is not passed — vcpkg's triplet
supplies the target architecture instead. Pinned explicitly via `vcpkg.json`'s `overrides`
so it cannot float with the baseline (N30).

## D6 [M] Release-only build

All three triplets set `VCPKG_BUILD_TYPE release`. The dist ships no debug artefacts, and a
debug pass roughly doubles wall-clock, which matters under the one-round spike cap.

## D7 [S] macOS install names are the carrier's default, not `CMAKE_INSTALL_NAME_DIR=@rpath`

`manifest.toml` passes `CMAKE_INSTALL_NAME_DIR=@rpath` on macOS/Linux. The overlay ports do
not; vcpkg does its own macOS install-name handling. **Not yet verified on an artefact** —
if the produced `libheif.dylib` has an absolute install name, downstream packaging breaks.
Listed as an open uncertainty rather than a claim.

## D8 [M] libde265's `dec265` tool

`manifest.toml` sets `ENABLE_DECODER=OFF` on Windows only (it gates the CLI tool
subdirectory, not the decode library). The fork reproduces exactly that split, and never
ships the tool on any platform (upstream's port ships it via `vcpkg_copy_tools`).

## D9 [M] The `hevc` (x265) feature was deleted from the libheif fork

Upstream's port declares it. Removing it rather than leaving it unselected means the GPL-2.0
path cannot be requested by accident from this overlay at all (K3).
