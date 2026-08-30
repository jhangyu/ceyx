# HEIF distribution — provenance

Built by `native/scripts/fetch_heif_deps.sh`. Nothing under this directory is
tracked except this file.

| Component | Version | Source | SHA-256 |
|---|---|---|---|
| libheif | 1.23.2 | https://github.com/strukturag/libheif/releases/download/v1.23.2/libheif-1.23.2.tar.gz | `8bd5d41d19dc84536d118b04774709f244df6104ef66d623dad5fa4650143405` |
| libde265 | 1.1.1 | https://github.com/strukturag/libde265/releases/download/v1.1.1/libde265-1.1.1.tar.gz | `fd48a927e94ed74fc7ce8829d222b9d8599fcbfe8b6448ba66705babc56ab219` |
| kvazaar | 2.3.1 | https://github.com/ultravideo/kvazaar/releases/download/v2.3.1/kvazaar-2.3.1.tar.gz | `2510b8ecc2bf384bbc7b8fc2756bbfa8a8c173b57634c8dfdd8bea6733e56c46` |
| libaom  | 3.12.1 | https://storage.googleapis.com/aom-releases/libaom-3.12.1.tar.gz | `9e9775180dec7dfd61a79e00bda3809d43891aee6b2e331ff7f26986207ea22e` |

Verified against the GitHub release API on 2026-08-28 (libheif/libde265) and
against the GitHub release API / aomedia.googlesource.com tag list on
2026-08-30 (kvazaar/aom). The design spec's 1.19.x / 1.0.15 were explicitly
unverified targets; these are the versions that actually exist upstream.
kvazaar 2.3.1 and aom 3.12.1 (the plan's target versions) were both confirmed
to exist upstream before pinning — kvazaar's latest release is 2.3.2, but the
plan's target 2.3.1 exists and was kept; aom's tag list confirms v3.12.1
exists (newer tags up to v3.15.0 also exist upstream but were not adopted,
since 3.12.1 was the plan's verified target).

## Licence and linkage

libheif and libde265 are **LGPL-3.0-or-later**. They are built as **separate
shared libraries** and loaded dynamically, which satisfies LGPL-3 section
4(d)(1) outright: a user can replace `libheif.1.dylib` / `libde265.0.dylib`
(`libheif.so.1` / `libde265.so.0` on Linux) inside
`<App>.app/Contents/Frameworks/` (or the equivalent Linux install location).
Static linking of libheif/libde265 into `libdng_decoder_native` is
deliberately NOT done, because it would trigger the 4(d)(0) duty to ship
relinkable object files with every release.

kvazaar (BSD-3-Clause) and libaom (BSD-2-Clause **plus the separate Alliance
for Open Media Patent License 1.0**) are built as STATIC archives and linked
INTO libheif, because ENABLE_PLUGIN_LOADING=OFF. Neither is copyleft, so this
creates no source-availability obligation beyond the existing LGPL-3 one for
libheif and libde265. Licence and patent files are vendored under
`share/licenses/{libheif,libde265,kvazaar,aom}/`. x265 stays OFF: it is
GPL-2.0.

The corresponding source for any shipped binary is the tarball at the URL and
hash above, plus the exact configure flags recorded in the fetch script.

## Encode-enabled build

`WITH_LIBDE265=ON` (HEVC decode), `WITH_KVAZAAR=ON` (HEVC encode),
`WITH_AOM_DECODER=ON` and `WITH_AOM_ENCODER=ON` (AV1 decode + encode, i.e.
AVIF import and export) are all on. `WITH_X265=OFF` (x265 is GPL-2.0),
`WITH_DAV1D=OFF` and `WITH_RAV1E=OFF` (ruling D2) stay off, along with every
other libheif codec plugin. Plugin loading is off, so every enabled codec is
compiled into `libheif` rather than dlopen-ed from a plugin directory that
would not survive app-bundle packaging.

The fetch script asserts all of the following mechanically after the build:
`heif_decode_image` and `heif_context_get_encoder_for_format` are exported, a
`libde265` dependency is present (a libheif built without a working HEVC
decoder installs perfectly happily and then decodes nothing), `kvz_api_get`
is present (HEVC encoder actually compiled in), `aom_codec_av1_cx` AND
`aom_codec_av1_dx` are both present (AV1 encode and decode are independent
flags and one can silently be off while the other is on), and no
`x265_encoder` symbol exists anywhere in the output.

## Linux support (ruling Q3)

The script runs on `ubuntu-latest` as well as macOS. Platform differences
(`shasum` vs `sha256sum`, `otool -L` vs `ldd`, `nm -gU` vs `nm -D`, the
`.dylib` vs `.so.N` suffix, the `lipo -archs` universal-binary arch proof vs a
`file`-based 64-bit-ELF proof, and the macOS-only `CMAKE_OSX_*` args) are
gated behind a single `HOST_OS="$(uname -s)"` check near the top of the
script. No committed Linux dist exists; Linux always builds from source at
CI/build time.
