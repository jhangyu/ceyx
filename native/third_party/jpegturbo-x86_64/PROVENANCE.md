# Vendored jpeg-turbo (libjpeg.8.dylib) — x86_64 macOS

Committed so the x86_64 (Intel) macOS leg has a dynamic JPEG library
companion without depending on a third party serving this exact binary at
build time. New vendored directory (no arm64 vendored jpeg-turbo predates
this — the arm64 leg installs it via a plain, unpinned
`brew install ... jpeg-turbo` at CI runtime, see
`.github/workflows/macos_build.yml:253`).

## Source

| | |
|---|---|
| Upstream project | libjpeg-turbo — https://www.libjpeg-turbo.org/ |
| Version | **3.2.0** — current Homebrew stable at the time this was sourced. Same rolling-vs-pinned situation as lcms2 (see "Version pinning note" below). |
| Obtained from | Homebrew bottle `jpeg-turbo` 3.2.0, `sonoma` platform tag (Intel), via `ghcr.io/homebrew/core/jpeg-turbo` |
| Formula | https://github.com/Homebrew/homebrew-core/blob/HEAD/Formula/j/jpeg-turbo.rb |
| Version-tagged manifest index | tag `3.2.0` |
| Platform sub-manifest digest | `sha256:a24c2c088138a7a28d45f1dee4b68387e4ebc5beb020e974e71690b4f476a425` (ref name `3.2.0.sonoma`) |
| Blob (bottle tarball) digest | `sha256:c1a02c5e74d687402700645d60f7045485d88ed9f2f615d301d1b081ad1e1f66` |
| Direct blob URL | `https://ghcr.io/v2/homebrew/core/jpeg-turbo/blobs/sha256:c1a02c5e74d687402700645d60f7045485d88ed9f2f615d301d1b081ad1e1f66` |
| Date sourced | 2026-09-01 |

## LOAD-BEARING FILENAME — symlink dereference decision

The release set names `libjpeg.8.dylib`. In the raw Homebrew bottle, that
exact name is a **symlink** to the real file `libjpeg.8.3.2.dylib`
(patch-versioned). Confirmed the SONAME baked into the binary itself
(`otool -l`'s `LC_ID_DYLIB`) is `@@HOMEBREW_PREFIX@@/opt/jpeg-turbo/lib/libjpeg.8.dylib`
— the unpatch-versioned name, matching the release set's expected filename
exactly. This is what makes the dereference below safe: the file is not
being renamed to a name that contradicts its own recorded identity.

**Deliberate deviation from raw bottle layout**: rather than commit the
symlink + its target as two files (fragile through zip/tar packaging steps
that may not preserve symlinks, and after this round's
`liblcms2.dylib`-vs-`liblcms2.2.dylib` naming boundary defect, a second
naming-mismatch risk not worth re-running), the symlink was dereferenced
(`cp -L`) and the REAL bytes staged directly under the name `libjpeg.8.dylib`.
The committed file here is a genuine Mach-O dylib, not a symlink.

## Version pinning note — same open escalation as lcms2, now a second instance

Same acquisition model as lcms2, not the same as x86_64 libomp (which is
pinned to match an existing arm64-vendored version): the arm64 leg installs
jpeg-turbo unpinned every CI run, so it always gets whatever Homebrew's
current stable is. Vendoring x86_64 at a fixed 3.2.0 while arm64 continues to
float means the two halves of one shipped six-dylib release asset could
drift to different jpeg-turbo versions over time — same class of risk
already escalated for lcms2 in
`native/third_party/lcms2-x86_64/PROVENANCE.md`. Not re-escalating as a new
issue; recording that this now applies to 2 of 3 sourced x86_64 companions
(lcms2, jpeg-turbo), with only libomp explicitly version-matched. Remains
open with the user/team lead, unresolved here.

## SHA256 verification (independently-published digest, not self-vs-self)

| | value |
|---|---|
| Publisher, source 1 (formulae.brew.sh API `bottle.stable.files.sonoma.sha256`) | `c1a02c5e74d687402700645d60f7045485d88ed9f2f615d301d1b081ad1e1f66` |
| Publisher, source 2 (ghcr manifest's own `sh.brew.bottle.digest` annotation) | `c1a02c5e74d687402700645d60f7045485d88ed9f2f615d301d1b081ad1e1f66` |
| Self-computed (`shasum -a 256` on downloaded tarball) | `c1a02c5e74d687402700645d60f7045485d88ed9f2f615d301d1b081ad1e1f66` |
| **Match** | **YES** |

Extra corroboration: this exact digest independently appeared inside
lcms2's bottle SBOM (as its declared build-time jpeg-turbo dependency),
discovered during a completely separate sourcing task (#16) — two unrelated
fetches agree.

## Contents and digests (this vendored copy)

| File | sha256 |
|---|---|
| `lib/libjpeg.8.dylib` (dereferenced from bottle symlink, real file) | `29aa4e836a00325001d37e518d62ac04fc102b2ab653a887bbb10bd1283098c8` |
| `LICENSE.md` | `ba6bceebcba0fdd35488477c2cca8c4632ce82c74dbfbc87d886ce6fc4433579` |

## License

IJG AND Zlib AND BSD-3-Clause (per `brew info jpeg-turbo` and the bottle's
own `sh.brew.license` annotation — both agree).

## Mechanical verification (all PASS)

| Check | Command | Result |
|---|---|---|
| Architecture | `lipo -info lib/libjpeg.8.dylib` | `x86_64`, non-fat, single-arch |
| Identity | `otool -L lib/libjpeg.8.dylib` | Deps: self + `/usr/lib/libSystem.B.dylib` only — zero other runtime deps (confirms the bottle manifest's own `runtime_dependencies: []`) |
| Symbol sanity | `nm -gU lib/libjpeg.8.dylib` | RC=0; `_jpeg_read_header` present |
| SHA | see above | Two independent publisher digests match self-computed, plus incidental third-party corroboration via lcms2's SBOM |

## Known difference from a `brew install`ed copy: unresolved install name

Same situation as the other two vendored x86_64 companions: `LC_ID_DYLIB`
carries Homebrew's raw bottle relocation placeholder token
`@@HOMEBREW_PREFIX@@/opt/jpeg-turbo/lib/libjpeg.8.dylib`, unresolved, because
this is a raw ghcr bottle blob, not a `brew install`ed copy. Left as-is to
keep this file's digest matching the independently-verified bottle digest
above.

**IMPORTANT CORRECTION recorded here per team lead's instruction**: earlier
in this task, "the build rewrites the install name regardless, so this does
not block" was stated as fact for all three vendored companions on the
strength of what the build's bundling step is documented to do — but this
was never verified against an actual built artifact. It turned out true for
one library and **false for two** (including this one, until fixed):
the rewrite step the earlier claim relied on is never reached on the Intel
leg for direct-linked companions, and only one library had a second rewrite
opportunity on its own consumption path. This has since been fixed at the
build-system layer (separate task, not this sourcing task) for all three
vendored companions, so this file lands as-shipped from the bottle need not
change. Recording the correction so a future reader does not repeat the
unverified assumption: **when a claim rests on "another component is
documented to do X", say explicitly whether that has been checked against a
built artifact, not just against documentation.**

## Reliability class

Per team lead's direct instruction (`ci-t13-local-vs-ci-verifiability.md`
confirmed nonexistent in this repo): source class is a Homebrew bottle, so
CI-ONLY claims are not supported by local Homebrew state (this sourcing
bypassed local `brew install`/`brew fetch` entirely, fetched by ghcr digest
instead). Once staged, this file's correctness (arch, version, untampered)
is durably locally re-verifiable via the digests above; that does not
guarantee ghcr.io keeps serving this digest indefinitely — vendoring this
file converts that availability question into a fact of this repo's own
history.
