# Vendored LLVM OpenMP runtime (libomp) — x86_64 macOS

Committed so the x86_64 (Intel) macOS cross leg produces the full six-dylib
companion set without depending on a third party serving this exact binary
at build time. Mirrors the existing `native/third_party/libomp/` (arm64)
vendored copy's convention and version.

## Source

| | |
|---|---|
| Upstream project | LLVM OpenMP runtime — https://openmp.llvm.org/ |
| Version | **22.1.8** — matches the arm64 vendored copy exactly (`native/third_party/libomp/lib/libomp.dylib`, see its own PROVENANCE.md). Deliberately pinned to this version, not "latest" (current Homebrew stable is 23.1.0) — architecture skew within the same shipped companion set would produce a bug nobody could reproduce on their own machine. |
| Obtained from | Homebrew bottle `libomp` 22.1.8, `sonoma` platform tag (Homebrew's non-`arm64_`-prefixed macOS tag = Intel x86_64), via `ghcr.io/homebrew/core/libomp` |
| Formula | https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/lib/libomp.rb |
| Version-tagged manifest index | tag `22.1.8` |
| Platform sub-manifest digest | `sha256:b2a2e9e1d5b2ab22e07a3d90777d5f7ecf8684b300629fcf20e9645840411633` (ref name `22.1.8.sonoma`) |
| Blob (bottle tarball) digest | `sha256:569a93ca1ac3c3674c56055baddd0f9697a95a32cc2f3c485da3d7c8a53711f4` |
| Direct blob URL | `https://ghcr.io/v2/homebrew/core/libomp/blobs/sha256:569a93ca1ac3c3674c56055baddd0f9697a95a32cc2f3c485da3d7c8a53711f4` |
| Date sourced | 2026-09-01 |

## SHA256 verification (independently-published digest, not self-vs-self)

| | value |
|---|---|
| Publisher-published (ghcr manifest `sh.brew.bottle.digest` / content-addressed layer digest) | `569a93ca1ac3c3674c56055baddd0f9697a95a32cc2f3c485da3d7c8a53711f4` |
| Self-computed (`shasum -a 256` on downloaded tarball) | `569a93ca1ac3c3674c56055baddd0f9697a95a32cc2f3c485da3d7c8a53711f4` |
| **Match** | **YES** |

## Contents and digests (this vendored copy)

| File | sha256 |
|---|---|
| `lib/libomp.dylib` | `921dd032f2c9853b5bd4bb9d4a6e5a8c192cfea6a1ce9cc05dc4c39b9f6364f9` |
| `include/omp.h` | `5974470842520cea4bc50136e2329bbf4e36ba928d317e86f7def2ba1752d3d4` |
| `LICENSE.TXT` | `8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee` |

`include/omp.h` and `LICENSE.TXT` are **byte-identical** to the arm64 vendored
copy's corresponding files (same sha256) — independent corroboration by
content, not just by matching version string, that this is genuinely the
same upstream release as the arm64 companion.

`lib/libomp.dylib` differs from the arm64 copy's digest as expected — it is
the x86_64 slice, not the arm64 slice, of the same upstream version.

## Mechanical verification (all PASS)

| Check | Command | Result |
|---|---|---|
| Architecture | `lipo -info lib/libomp.dylib` | `x86_64`, non-fat, single-arch |
| Identity | `otool -L lib/libomp.dylib` | Deps: self + `/usr/lib/libSystem.B.dylib` only |
| Symbol sanity | `nm -gU lib/libomp.dylib` | RC=0; `_omp_get_num_threads` present |
| SHA | see above | Independently-published digest matches self-computed |

## Known difference from the arm64 vendored copy: unresolved install name

`lib/libomp.dylib`'s `LC_ID_DYLIB` still carries Homebrew's raw bottle
relocation placeholder token `@@HOMEBREW_PREFIX@@/opt/libomp/lib/libomp.dylib`,
**unresolved** — because this file was fetched directly from the ghcr bottle
blob, bypassing `brew install`'s relocation step (which would normally
rewrite it to `/usr/local/opt/libomp/lib/libomp.dylib` for an Intel prefix).

This is deliberately left as-is (not rewritten) to keep this file's digest
matching the independently-verified bottle digest above — rewriting it here
would break that verification chain for anyone re-checking it later.

**Why this does not block usage**: per the arm64 copy's own PROVENANCE.md
(§"How the build consumes this"), the build always copies the chosen
`libomp.dylib` into the build directory and rewrites ITS install name to
`@rpath/libomp.dylib` before linking (`native/cmake/tests.cmake`), so neither
this placeholder token nor a resolved absolute path is ever consumed by any
executable directly — the build-directory copy is what matters, not this
vendored source copy's own install name.

**If that rewrite assumption is ever found to be wrong**: this is the file
to check first — `lipo -info` / `otool -L` this file directly to confirm the
placeholder is still present and unresolved as documented here.

## License

Apache License v2.0 with LLVM Exceptions (per `LICENSE.TXT`'s own first
line). Note `brew info libomp` reports "License: MIT", which does not match
the shipped license text — the file is authoritative, same discrepancy noted
in the arm64 copy's PROVENANCE.md.

## Reliability class

Per team lead's direct instruction (the file this convention would normally
reference, `ci-t13-local-vs-ci-verifiability.md`, does not exist anywhere in
this repo — confirmed independently by both the sourcing agent and the
lead):
- **Homebrew rows are CI-ONLY**: this binary's source class is a Homebrew
  bottle. Local Homebrew state on any given machine proves nothing about
  what a clean CI runner has — this sourcing deliberately bypassed local
  `brew install`/`brew fetch` and fetched the specific version-tagged bottle
  directly from ghcr.io by digest instead.
- **Vendored/SHA-pinned rows are locally verifiable for LOGIC only**: this
  file's correctness (right arch, right version, untampered) is durably
  re-verifiable at any time via the digests above. That does not, by itself,
  guarantee `ghcr.io` would keep serving this digest indefinitely — vendoring
  this file into the repo is precisely what converts that availability
  question into a settled fact of this repo's own history instead.

## How the build consumes this

Same mechanism as the arm64 copy (`native/cmake/tests.cmake`): the x86_64
cross-leg configure logic searches this arch-suffixed vendored directory
(`native/third_party/libomp-x86_64/`), verifies the found dylib's
architecture via `lipo -archs` before accepting it (the same honesty check
that closed the false-cross-build-guard bug — see `OMP-CROSS-FIX`/task #14
in the project's task log for the guard-narrowing fix this vendored copy
depends on), copies it into the build directory, rewrites its install name
to `@rpath/libomp.dylib`, and ad-hoc re-signs it.
