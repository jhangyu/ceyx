# Vendored little-cms2 (lcms2) runtime — x86_64 macOS

Committed so the x86_64 (Intel) macOS leg has an ICC colour-management
companion without depending on a third party serving this exact binary at
build time. New vendored directory (no arm64 vendored copy predates this —
the arm64 leg installs lcms2 via a plain, unpinned `brew install ... little-cms2`
at CI runtime, see `.github/workflows/macos_build.yml:253`).

## Source

| | |
|---|---|
| Upstream project | Little CMS (lcms2) — https://www.littlecms.com/ |
| Version | **2.19.1** — current Homebrew stable at the time this was sourced. See "Version pinning note" below: this is deliberately NOT the same acquisition model as the x86_64 libomp companion. |
| Obtained from | Homebrew bottle `little-cms2` 2.19.1, `sonoma` platform tag (Homebrew's non-`arm64_`-prefixed macOS tag = Intel x86_64), via `ghcr.io/homebrew/core/little-cms2` |
| Formula | https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/l/little-cms2.rb |
| Version-tagged manifest index | tag `2.19.1` |
| Platform sub-manifest digest | `sha256:ae94a3fb9c5201e3645535ce07e0feebd93643525427b24d61f1938f12ce8f1a` (ref name `2.19.1.sonoma`) |
| Blob (bottle tarball) digest | `sha256:b6a008c02dff9c51ddee68a8a4cbf2b031f9ab2e6c8554d92ffbbf982a31f1ed` |
| Direct blob URL | `https://ghcr.io/v2/homebrew/core/little-cms2/blobs/sha256:b6a008c02dff9c51ddee68a8a4cbf2b031f9ab2e6c8554d92ffbbf982a31f1ed` |
| Date sourced | 2026-09-01 |

## Version pinning note — escalated, not resolved here

Unlike x86_64 libomp (pinned to match an existing arm64-vendored version
exactly), lcms2 has no arm64-vendored counterpart to match: the arm64 leg
does a bare, unpinned `brew install little-cms2` every CI run, so it always
gets whatever Homebrew's current stable is at that moment. Vendoring x86_64
at a fixed 2.19.1 while arm64 continues to float means the two halves of one
shipped six-dylib release asset could drift to different lcms2 versions over
time — the same class of cross-architecture version-skew risk the libomp
companion was deliberately pinned to avoid. This also sits against
MACOS-DEPS-DETERMINISM's (task #13) stated goal of deterministic six-dylib
production: an unpinned arm64 install is not fully deterministic regardless
of what x86_64 does.

**This has been escalated to the team lead for a decision on pinning both
architectures symmetrically. Not resolved by this task — flagging so a
future reader does not assume the asymmetry was overlooked.**

## SHA256 verification (independently-published digest, not self-vs-self)

| | value |
|---|---|
| Publisher-published, source 1 (formulae.brew.sh API `bottle.stable.files.sonoma.sha256`) | `b6a008c02dff9c51ddee68a8a4cbf2b031f9ab2e6c8554d92ffbbf982a31f1ed` |
| Publisher-published, source 2 (ghcr manifest's own `sh.brew.bottle.digest` annotation) | `b6a008c02dff9c51ddee68a8a4cbf2b031f9ab2e6c8554d92ffbbf982a31f1ed` |
| Self-computed (`shasum -a 256` on downloaded tarball) | `b6a008c02dff9c51ddee68a8a4cbf2b031f9ab2e6c8554d92ffbbf982a31f1ed` |
| **Match** | **YES** — two independent publisher sources agree with each other and with the self-computed digest |

## Contents and digests (this vendored copy)

| File | sha256 |
|---|---|
| `lib/liblcms2.2.dylib` | `1f95583cf9dd7d064d9c2d89beaeb088323746f0a72f0af0468b7edc558edc47` |
| `include/lcms2.h` | `67f73413d7168a0cf7fa94ff3eb0d795fb75668b07d02e6ff583110166ca0f38` |
| `LICENSE` | `6dbd60437f8ef91d8de1f08ad75882547fd4931bfcc3566a0735f28db1484d31` |

## License

MIT (per `LICENSE` file, "Copyright (c) 2023 Marti Maria Saguer" — matches
`brew info little-cms2`'s reported "License: MIT", no discrepancy).

## Mechanical verification (all PASS)

| Check | Command | Result |
|---|---|---|
| Architecture | `lipo -info lib/liblcms2.2.dylib` | `x86_64`, non-fat, single-arch |
| Identity | `otool -L lib/liblcms2.2.dylib` | Deps: self + `/usr/lib/libSystem.B.dylib` only |
| Symbol sanity | `nm -gU lib/liblcms2.2.dylib` | RC=0; `_cmsOpenProfileFromFile` present |
| SHA | see above | Two independent publisher digests match self-computed |

### Finding: bottle SBOM lists build-time deps the library does not runtime-link

The ghcr bottle manifest's SBOM lists lcms2 as built against jpeg-turbo,
giflib, libpng, webp, xz, lz4, zstd, and libtiff. Taking that at face value
would suggest this library pulls in seven more runtime dependencies.
Direct inspection of the actual `.dylib`'s load commands (`otool -L`) shows
none of them present — only self and libSystem. Those SBOM entries belong to
the bottle's `bin/` CLI tools (`jpgicc`, `tificc`, `linkicc`, `psicc`,
`transicc`), which are not part of what is vendored here (only `lib/` and
`include/` were extracted from the bottle). Confirmed by inspecting the
artifact directly, not inferred from the manifest.

## Known difference from a `brew install`ed copy: unresolved install name

Same situation as the x86_64 libomp companion: `lib/liblcms2.2.dylib`'s
`LC_ID_DYLIB` carries Homebrew's raw bottle relocation placeholder token
`@@HOMEBREW_PREFIX@@/opt/little-cms2/lib/liblcms2.2.dylib`, unresolved,
because this file is a raw ghcr bottle blob, not a `brew install`ed copy.
Deliberately left as-is to keep this file's digest matching the
independently-verified bottle digest above.

**Why this does not block usage**: the project's `bundle_macos_dylib_deps.py`
POST_BUILD step already handles rewriting arbitrary Homebrew-style dependency
install names uniformly (this is the same mechanism that already processes
the native arm64 leg's lcms2, which IS resolved because it comes from a live
`brew install`) — per task #14's acquisition table, this mechanism is
unmodified by that fix and expected to apply the same rewrite here. This has
NOT been end-to-end verified by an actual build from this vendored copy (no
build attempted as part of this sourcing task).

## Reliability class

Per team lead's direct instruction (`ci-t13-local-vs-ci-verifiability.md`
confirmed nonexistent in this repo by both the sourcing agent and the lead):
- **Homebrew rows are CI-ONLY**: source class is a Homebrew bottle. Local
  Homebrew state proves nothing about a clean CI runner — this sourcing
  bypassed local `brew install`/`brew fetch` and fetched the version-tagged
  bottle directly from ghcr.io by digest.
- **Vendored/SHA-pinned rows are locally verifiable for LOGIC only**: this
  file's correctness is durably re-verifiable via the digests above; it does
  not guarantee ghcr.io keeps serving this digest indefinitely — vendoring
  converts that availability question into a fact of this repo's history.
