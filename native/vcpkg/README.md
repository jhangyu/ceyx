# `native/vcpkg/` — overlay ports, overlay triplets, chainload toolchains

Round-2 OD-2 spike (one-round cap). This directory is the **overlay-port** answer to the
three registry-blocked components identified in
`docs/logs/2026-08-30/Spec_build_rewrite.md` §3.2:

| Component | Why the registry cannot supply it | What lives here |
|---|---|---|
| kvazaar | `absent` — no vcpkg port at any version | `ports/kvazaar/` (new port) |
| libheif | `feature` — upstream port's only HEVC encoder is GPL-2.0 x265; no `WITH_KVAZAAR` | `ports/libheif/` (fork, adds a `kvazaar` feature) |
| libde265 (Windows) | `feature` — three Windows blockers (dec265 tool, no clang-cl SIMD flag, no dynamic-lib+static-CRT triplet) | `ports/libde265/` (fork) + `triplets/x64-windows-heif.cmake` |

**Layout choice.** Spec §3.5 does not fix a layout. `native/vcpkg/` was chosen so the whole
vcpkg surface (ports + triplets + toolchains + the consuming manifest) sits under `native/`
with the rest of the build system, rather than at the repo root.

## Carrier neutrality (values traceable to `native/deps/manifest.toml`)

Every option below is the manifest value. Deviations are enumerated in
`DEVIATIONS.md` — read that before assuming this build is equivalent to the shell scripts.

## Two structural wins the overlay route gets for free (verified against vcpkg master)

1. **Layer 6 / pitfall N7 (CMP0091) cannot recur inside a vcpkg build.**
   `scripts/toolchains/windows.cmake:88-90` writes `${VCPKG_CRT_LINK_FLAG_PREFIX}` (`/MT`,
   `/MTd`, `/MD`, …) **literally into `CMAKE_C_FLAGS_RELEASE`/`_DEBUG`**, in addition to
   setting `CMAKE_MSVC_RUNTIME_LIBRARY` at line 3. A port whose `cmake_minimum_required()`
   is below 3.15 therefore still gets the right CRT — the flag never depends on the policy.
   Our chainload toolchain (`toolchains/clang-cl.cmake`) reproduces this belt-and-braces
   behaviour deliberately: it sets the policy **and** puts `/MT` on the flag line.
2. **Pitfall N16 (flag replacement) cannot recur.** The same toolchain seeds
   `CMAKE_C_FLAGS` with `/nologo /DWIN32 /D_WINDOWS` and then **appends** `VCPKG_C_FLAGS`
   (line 79). Triplet-supplied flags are additive, so `-EHsc`/`-GR`/`-DWIN32` do not have to
   be hand-restored the way the shell scripts must restore them.

Pitfalls that are **not** solved by the carrier and are reproduced by hand here: N9
(`-DKVZ_STATIC_LIB` on libheif's compile line, not kvazaar's), N12 (Homebrew `aom`
shadowing), N27 (build order — vcpkg's dependency graph enforces it), N30 (explicit version
pins, in `vcpkg.json` + `vcpkg-configuration.json`).
