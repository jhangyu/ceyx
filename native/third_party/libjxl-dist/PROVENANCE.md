# libjxl distribution — provenance

Built by `native/scripts/fetch_libjxl_dist.sh`. Nothing under this directory
is tracked except this file.

## Pin mechanism (commit-SHA based, not a tarball SHA-256)

Upstream libjxl has never published a release asset that is a source tarball
including the `third_party/highway` and `third_party/brotli` git
submodules the static build requires (checked via the GitHub Releases API
across every tag from v0.6 through v0.12.0, 2026-08-30). Only prebuilt binary
archives and a submodule-free `archive/refs/tags` snapshot exist. So the pin
here is `git clone` at a tagged commit plus selective `git submodule
update --init` for only the submodules the static core library links
(brotli, highway, skcms), with the full submodule SHA set recorded below —
equivalent verifiability to a tarball hash, and the only mechanism that
preserves "bundled submodules, not system copies" for highway and brotli.

- Tag: `v0.12.0` (latest stable per the GitHub Releases API as of
  2026-08-30; deviates from the design spec's target `v0.11.1`, which the
  spec gave no stated reason to prefer over latest).
- Commit: `a7a9c787341cf703dede03c2009fa460cae5e5df`
- Arch: `arm64`

### Submodule pins (`git submodule status` for the submodules linked into
the static core library: brotli, highway, skcms)

```
 028fb5a23661f123017c060daa546b55cf4bde29 third_party/brotli (028fb5a)
 457c891775a7397bdb0376bb1031e6e027af1c48 third_party/highway (457c891)
 96d9171c94b937a1b5f0293de7309ac16311b722 third_party/skcms (96d9171)
```

## Licence and linkage

libjxl and brotli are BSD-3-Clause; highway (Google) is Apache-2.0. All three
are linked **statically** into `libdng_decoder_native` — BSD-3/Apache-2.0
carry no relink duty, unlike the LGPL-3 heif-dist, which is why this dist is
static where heif-dist is dynamic (see cmake/jxl.cmake). Licence files for all
three are vendored under `share/licenses/{libjxl,highway,brotli}/` and must
ship alongside any distributed build that includes JXL support.

## Static libraries

`libjxl.a`, `libjxl_threads.a`, `libhwy.a`, `libbrotlicommon.a`,
`libbrotlidec.a`, `libbrotlienc.a` — all release-built with
`CMAKE_BUILD_TYPE=Release` and stripped of debug symbols (ruling Q5) before
being written here.
