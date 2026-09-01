# Vendored little-cms2 (lcms2) runtime — arm64 macOS

Committed as the symmetric-pin counterpart to `native/third_party/lcms2-x86_64/`.
Both architectures were previously mismatched (x86_64 vendored at a fixed
version, arm64 installed unpinned via `brew install` at CI time — see
`native/third_party/lcms2-x86_64/PROVENANCE.md`'s "Version pinning note").
This escalation was ruled in favor of pinning both sides to the same version.

## Pinned version — matches the x86_64 companion exactly

| | |
|---|---|
| x86_64 vendored version | **2.19.1** |
| arm64 vendored version (this directory) | **2.19.1** |
| **Match** | **YES** |

Requested version 2.19.1 was available as an arm64 macOS bottle — no
substitution, no BLOCKED condition.

## Source

| | |
|---|---|
| Upstream project | Little CMS (lcms2) — https://www.littlecms.com/ |
| Version | 2.19.1 |
| Obtained from | Homebrew bottle `little-cms2` 2.19.1, `arm64_sonoma` platform tag, via `ghcr.io/homebrew/core/little-cms2` |
| Formula | https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/l/little-cms2.rb |
| Platform-tag reasoning | `arm64_sonoma` chosen (not the newer `arm64_sequoia`/`arm64_tahoe`, also bottled at this version) because CI's arm64 leg runner is pinned to `macos-14` = Sonoma (`.github/workflows/macos_build.yml:91`/`:105`) — matching the machine that actually consumes this artifact, not the newest available. |
| Version-tagged manifest index | tag `2.19.1` |
| Platform sub-manifest digest | `sha256:e66bb27e21d3cb6072977929f33a12a2099301ddb5a79e16ee29bf12e167fb53` (ref name `2.19.1.arm64_sonoma`) |
| Blob (bottle tarball) digest | `sha256:6657dccb4ec6c9a6d99255fdc226299c883b75d1a9cfe9b1650720721c1af626` |
| Direct blob URL | `https://ghcr.io/v2/homebrew/core/little-cms2/blobs/sha256:6657dccb4ec6c9a6d99255fdc226299c883b75d1a9cfe9b1650720721c1af626` |
| Date sourced | 2026-09-01 |

## SHA256 verification (independently-published digest, not self-vs-self)

| | value |
|---|---|
| Publisher-published (ghcr manifest `sh.brew.bottle.digest`) | `6657dccb4ec6c9a6d99255fdc226299c883b75d1a9cfe9b1650720721c1af626` |
| Self-computed (`shasum -a 256` on downloaded tarball) | `6657dccb4ec6c9a6d99255fdc226299c883b75d1a9cfe9b1650720721c1af626` |
| **Match** | **YES** |

## Same-upstream-version-by-content evidence (stronger than the version string)

| File | This arm64 copy sha256 | x86_64 vendored copy sha256 |
|---|---|---|
| `include/lcms2.h` | `67f73413d7168a0cf7fa94ff3eb0d795fb75668b07d02e6ff583110166ca0f38` | `67f73413d7168a0cf7fa94ff3eb0d795fb75668b07d02e6ff583110166ca0f38` — **IDENTICAL** |
| `LICENSE` | `6dbd60437f8ef91d8de1f08ad75882547fd4931bfcc3566a0735f28db1484d31` | `6dbd60437f8ef91d8de1f08ad75882547fd4931bfcc3566a0735f28db1484d31` — **IDENTICAL** |
| `lib/liblcms2.2.dylib` | `f4c2fe0d303e63e65e627271e0bceada3ca664486f9bb9f07077083dd0ff2540` | different (expected — different arch slice) |

Byte-identical headers/license across architectures is a stronger claim than
matching version numbers: a version string is a label that could in
principle be applied to different bytes; an identical hash cannot.

## License
MIT (per `LICENSE` file — matches `brew info little-cms2`).

## Symlink question — asked again for this package, answer: no dereference needed
Checked whether this bottle ships the expected filename (`liblcms2.2.dylib`)
as a symlink, same question asked for the x86_64 companion and for the
arm64 jpeg-turbo companion below. Answer for lcms2 on **both** architectures:
**no** — `liblcms2.2.dylib` is a real file, not a symlink. (Contrast with
jpeg-turbo, where the answer is yes on both architectures — see the sibling
`jpegturbo-arm64/PROVENANCE.md`. Different libraries, different answers;
checked independently rather than assumed from the other library's shape.)

## Mechanical verification (all PASS)

| Check | Command | Result |
|---|---|---|
| Architecture | `lipo -info lib/liblcms2.2.dylib` | `arm64`, non-fat, single-arch |
| Identity | `otool -L lib/liblcms2.2.dylib` | Deps: self + `/usr/lib/libSystem.B.dylib` only |
| Symbol sanity | `nm -gU lib/liblcms2.2.dylib` | RC=0; `_cmsOpenProfileFromFile` present |
| SHA | see above | Independently-published digest matches self-computed |

## Known difference from a `brew install`ed copy: unresolved install name

`LC_ID_DYLIB` carries the raw bottle relocation placeholder
`@@HOMEBREW_PREFIX@@/opt/little-cms2/lib/liblcms2.2.dylib`, unresolved — same
cause as all previously vendored companions (raw ghcr bottle blob, not a
`brew install`ed copy). Per the team lead, this is no longer treated as a
blocking concern: the build system now rewrites install names at link time
for this class of vendored package, and that fix is CI-proven (see
`native/third_party/jpegturbo-x86_64/PROVENANCE.md`'s "IMPORTANT CORRECTION"
section for the history of why this was previously mis-assessed as
non-blocking without verification, and how it was subsequently fixed and
proven).

## Reliability class
Same rule as all other vendored companions (team lead's direct instruction,
`ci-t13-local-vs-ci-verifiability.md` confirmed nonexistent in this repo):
source class is Homebrew bottle → CI-ONLY claims not supported by local
state; once staged, LOGIC durably re-verifiable via digests above; CI-time
availability is separate and is what vendoring settles.
