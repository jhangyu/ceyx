# HEIF decode distribution — provenance

Built by `native/scripts/fetch_heif_deps.sh`. Nothing under this directory is
tracked except this file.

| Component | Version | Source | SHA-256 |
|---|---|---|---|
| libheif | 1.23.2 | https://github.com/strukturag/libheif/releases/download/v1.23.2/libheif-1.23.2.tar.gz | `8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405` |
| libde265 | 1.1.1 | https://github.com/strukturag/libde265/releases/download/v1.1.1/libde265-1.1.1.tar.gz | `fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219` |

Verified against the GitHub release API on 2026-08-28. The design spec's
1.19.x / 1.0.15 were explicitly unverified targets; these are the versions that
actually exist upstream.

## Licence and linkage

Both are **LGPL-3.0-or-later**. They are built as **separate shared libraries**
and loaded dynamically, which satisfies LGPL-3 section 4(d)(1) outright: a user
can replace `libheif.1.dylib` / `libde265.0.dylib` inside
`<App>.app/Contents/Frameworks/`. Static linking into `libdng_decoder_native`
is deliberately NOT done, because it would trigger the 4(d)(0) duty to ship
relinkable object files with every release.

The corresponding source for any shipped binary is the tarball at the URL and
hash above, plus the exact configure flags recorded in the fetch script.

## Decode-only build

No encoder is built. `WITH_X265=OFF` (x265 is GPL-2.0), `WITH_AOM_ENCODER=OFF`,
`ENABLE_ENCODER=OFF` for libde265, and every other libheif codec plugin off
except `WITH_LIBDE265=ON`. Plugin loading is off, so the HEVC decoder is
compiled into `libheif` rather than dlopen-ed from a plugin directory that
would not survive app-bundle packaging.

The fetch script asserts all three of these mechanically after the build:
`heif_decode_image` is exported, a `libde265` dependency is present (a libheif
built without a working HEVC decoder installs perfectly happily and then
decodes nothing), and no `x265_encoder` symbol exists.
