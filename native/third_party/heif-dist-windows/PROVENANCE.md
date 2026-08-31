# HEIF decode distribution (Windows x86-64) — provenance

Built by `native/scripts/build_heif_dist_windows.sh` (deleted, D9; the
workflow now runs the Python carrier — see
`.github/workflows/heif_dist_windows.yml`), run on a `windows-latest`
GitHub Actions runner via `.github/workflows/heif_dist_windows.yml`. Unlike the
macOS dist (which is produced locally and untracked), **this tree is committed**:
no contributor machine in this project can build Windows binaries, so the built
bytes are a reviewed input, pinned once and changed only by a visible diff.

| Component | Version | Source | SHA-256 (upstream tarball) |
|---|---|---|---|
| libheif | 1.23.2 | https://github.com/strukturag/libheif/releases/download/v1.23.2/libheif-1.23.2.tar.gz | `8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405` |
| libde265 | 1.1.1 | https://github.com/strukturag/libde265/releases/download/v1.1.1/libde265-1.1.1.tar.gz | `fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219` |

Same versions and same hashes as the macOS dist
(`native/third_party/heif-dist/PROVENANCE.md`) — deliberately, so the two
platforms decode with identical library code.

Built artifacts committed here:

| File | SHA-256 |
|---|---|
| `bin/heif.dll` | `e8c84ae257941c85f05a8236cb012f3090484d93ed541cd78139083e982e7d4a` |
| `bin/libde265.dll` | `339ac370586d78f5ba1550f4a1ffbb39f7076f897c41c5b75e0e80daec41466d` |
| `lib/heif.lib` | `46ed10b6b906758440873f11b28148b6137da0e7a8530d6d824c1b52f6876ce2` |
| `lib/de265.lib` | `d606afc41621282d8f8f2b8754029b36a316fe46a0dd5ca7b06139f5915d3e9a` |

Produced by run https://github.com/jhangyu/ceyx/actions/runs/33294406360
(branch `ci/heif-dist-windows`), which logged `HEIF_DIST_WINDOWS_RC=0`,
`ASSERT heif_decode_image RC=0`, `ASSERT de265 dependency RC=0`,
`ASSERT no-x265 RC=1 (expected 1 = absent)` and `ASSERT PE32+ x86-64 RC=0`.

**Runtime DLL naming (asymmetric, upstream's choice, not ours):** the libde265
runtime installs as `bin/libde265.dll` while its import library is
`lib/de265.lib`. The DLL is deliberately NOT renamed to `de265.dll`:
`heif.dll`'s import table names `libde265.dll`, so a renamed copy would never
be loaded. Anything that stages or ships these files must use `libde265.dll`.

## Licence and linkage

Both are **LGPL-3.0-or-later**. They are built as **separate shared libraries**
and loaded dynamically, which satisfies LGPL-3 section 4(d)(1) outright: a user
can replace `heif.dll` / `de265.dll` next to the installed application. Static
linking into `dng_decoder_native` is deliberately NOT done, because it would
trigger the 4(d)(0) duty to ship relinkable object files with every release.

The corresponding source for any shipped binary is the tarball at the URL and
hash above, plus the exact configure flags recorded in the now-deleted
`native/scripts/build_heif_dist_windows.sh` (git tag `r5-pre-d9-legacy-scripts`
still has the file, for historical reference).

## Decode-only build

No encoder is built. `WITH_X265=OFF` (x265 is GPL-2.0), `WITH_AOM_ENCODER=OFF`,
`ENABLE_ENCODER=OFF` for libde265, and every other libheif codec plugin off
except `WITH_LIBDE265=ON`. Plugin loading is off, so the HEVC decoder is
compiled into `heif.dll` rather than dlopen-ed from a plugin directory.

The build script asserts all three mechanically, reading tool output from files
rather than pipes: `heif_decode_image` is exported, a `de265` dependency is
present (a libheif built without a working HEVC decoder installs perfectly
happily and then decodes nothing), and no `x265` symbol exists.

## Toolchain deltas from the macOS dist

- clang-cl + Ninja, `CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded` (static CRT), so
  these DLLs carry no `vcruntime140.dll` dependency of their own. Safe because
  no CRT-allocated object crosses the libheif ABI: every buffer libheif hands
  out is released through a libheif entry point.
- Install layout is CMake's Windows default: runtime DLLs under `bin/`, import
  libraries under `lib/`.
- **`/clang:-msse4.1` is passed to the libde265 configure.** libde265's
  `CMakeLists.txt` reads `if(MSVC)` as "cl.exe", which enables the SSE4.1
  kernels while adding no `-m` flag at all, because cl.exe accepts intrinsics
  unconditionally. clang-cl also sets `MSVC=1` but is clang underneath and
  rejects them (`always_inline function '_mm_mullo_epi32' requires target
  feature 'sse4.1'`). The flag is the clang-cl spelling of what upstream passes
  on every non-MSVC compiler, so it restores exactly the code paths upstream
  intended to build; it is a build-mechanism fix, not a feature change.
  Consequence: `de265.dll` requires an SSE4.1-capable CPU (Intel Penryn 2008+,
  AMD Bulldozer 2011+; Windows 11 already mandates SSE4.2). Setting
  `ENABLE_SIMD=OFF` would have made the build green by building less and was
  deliberately not done.
- **Explicit `/EHsc /GR` is passed on the command line.** clang-cl in
  `MultiThreaded` static-CRT configurations does not always carry over MSVC's
  default exception-handling and RTTI flags, so both are restored explicitly
  to match what a normal MSVC build would produce. Without them, libheif's C++
  exception-based error path and RTTI-dependent code would silently compile
  with different semantics than upstream expects.
- **`ENABLE_DECODER=OFF` is passed to the libde265 configure.** This gates
  only the standalone `dec265` command-line decoding tool that upstream
  builds alongside the library; the `de265` library itself (the thing
  `heif.dll` links against for HEVC decode) is built unconditionally by this
  flag. No decode capability is lost — nothing in this project uses the
  `dec265` CLI tool.
