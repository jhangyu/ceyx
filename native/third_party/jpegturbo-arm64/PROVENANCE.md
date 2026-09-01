# Vendored jpeg-turbo (libjpeg.8.dylib) — arm64 macOS

Committed as the symmetric-pin counterpart to
`native/third_party/jpegturbo-x86_64/`. Both architectures were previously
mismatched (x86_64 vendored at a fixed version, arm64 installed unpinned via
`brew install` at CI time). This escalation was ruled in favor of pinning
both sides to the same version.

## Pinned version — matches the x86_64 companion exactly

| | |
|---|---|
| x86_64 vendored version | **3.2.0** |
| arm64 vendored version (this directory) | **3.2.0** |
| **Match** | **YES** |

Requested version 3.2.0 was available as an arm64 macOS bottle — no
substitution, no BLOCKED condition.

## Source

| | |
|---|---|
| Upstream project | libjpeg-turbo — https://www.libjpeg-turbo.org/ |
| Version | 3.2.0 |
| Obtained from | Homebrew bottle `jpeg-turbo` 3.2.0, `arm64_sonoma` platform tag, via `ghcr.io/homebrew/core/jpeg-turbo` |
| Formula | https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/j/jpeg-turbo.rb |
| Platform-tag reasoning | `arm64_sonoma` chosen (not the newer `arm64_sequoia`/`arm64_tahoe`, also bottled at this version) because CI's arm64 leg runner is pinned to `macos-14` = Sonoma (`.github/workflows/macos_build.yml:91`/`:105`) — matching the machine that actually consumes this artifact, not the newest available. |
| Version-tagged manifest index | tag `3.2.0` |
| Platform sub-manifest digest | `sha256:263955632511fc8554cd13297fa3dbe7449713e89e0759e720b82c6d97c31217` (ref name `3.2.0.arm64_sonoma`) |
| Blob (bottle tarball) digest | `sha256:0d248d272a2e9d4f3442ce8d82c2df322079e77a76011cf75cb18d7114e78655` |
| Direct blob URL | `https://ghcr.io/v2/homebrew/core/jpeg-turbo/blobs/sha256:0d248d272a2e9d4f3442ce8d82c2df322079e77a76011cf75cb18d7114e78655` |
| Date sourced | 2026-09-01 |

## SHA256 verification (independently-published digest, not self-vs-self)

| | value |
|---|---|
| Publisher-published (ghcr manifest `sh.brew.bottle.digest`) | `0d248d272a2e9d4f3442ce8d82c2df322079e77a76011cf75cb18d7114e78655` |
| Self-computed (`shasum -a 256` on downloaded tarball) | `0d248d272a2e9d4f3442ce8d82c2df322079e77a76011cf75cb18d7114e78655` |
| **Match** | **YES** |

## LOAD-BEARING FILENAME — symlink question asked again, same answer as x86_64

The release set names `libjpeg.8.dylib`. This arm64 bottle, like the x86_64
one, ships that exact name as a **symlink** to the real file
`libjpeg.8.3.2.dylib`. Confirmed the SONAME baked into the binary itself
(`otool -l`'s `LC_ID_DYLIB`) is
`@@HOMEBREW_PREFIX@@/opt/jpeg-turbo/lib/libjpeg.8.dylib` — matching the
release set's expected filename exactly, same as x86_64. This is what makes
the dereference below safe.

**Same deliberate deviation as x86_64**: dereferenced the symlink (`cp -L`)
and vendored the REAL bytes directly under the name `libjpeg.8.dylib`
(genuine Mach-O file, not a symlink).

## Same-upstream-version-by-content evidence (stronger than the version string)

| File | This arm64 copy sha256 | x86_64 vendored copy sha256 |
|---|---|---|
| `LICENSE.md` | `ba6bceebcba0fdd35488477c2cca8c4632ce82c74dbfbc87d886ce6fc4433579` | `ba6bceebcba0fdd35488477c2cca8c4632ce82c74dbfbc87d886ce6fc4433579` — **IDENTICAL** |
| `lib/libjpeg.8.dylib` | `eff366d3f3d417a7344d96b2a6e735ed6d147607041d20fb7f7c265c00392181` | different (expected — different arch slice) |

Byte-identical license file across architectures corroborates same upstream
version 3.2.0 by content, not just by matching version label.

## License
IJG AND Zlib AND BSD-3-Clause (matches `brew info jpeg-turbo`).

## Mechanical verification (all PASS)

| Check | Command | Result |
|---|---|---|
| Architecture | `lipo -info lib/libjpeg.8.dylib` | `arm64`, non-fat, single-arch |
| Identity | `otool -L lib/libjpeg.8.dylib` | Deps: self + `/usr/lib/libSystem.B.dylib` only, zero other runtime deps |
| Symbol sanity | `nm -gU lib/libjpeg.8.dylib` | RC=0; `_jpeg_read_header` present |
| SHA | see above | Independently-published digest matches self-computed |

## Known difference from a `brew install`ed copy: unresolved install name

`LC_ID_DYLIB` carries the raw bottle relocation placeholder
`@@HOMEBREW_PREFIX@@/opt/jpeg-turbo/lib/libjpeg.8.dylib`, unresolved — same
cause as all previously vendored companions. Per the team lead, no longer
treated as blocking: the build system now rewrites install names at link
time for this class of vendored package, CI-proven (see
`native/third_party/jpegturbo-x86_64/PROVENANCE.md`'s "IMPORTANT CORRECTION"
section for the full history).

## Reliability class
Same rule as all other vendored companions (team lead's direct instruction,
`ci-t13-local-vs-ci-verifiability.md` confirmed nonexistent in this repo):
source class is Homebrew bottle → CI-ONLY claims not supported by local
state; once staged, LOGIC durably re-verifiable via digests above; CI-time
availability is separate and is what vendoring settles.
