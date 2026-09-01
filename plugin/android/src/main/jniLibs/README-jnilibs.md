# jniLibs — placed-at-build/fetch policy

`libdng_decoder_native.so` and its runtime companions under each ABI
directory (e.g. `arm64-v8a/`) are **not committed** to this repository
(ruling 2026-08-31, option a, A-T12).

## The full .so set (A-T8-FIX, 2026-09-01)

As of the HEIF Android leg going live, `arm64-v8a/` needs FOUR files, not
one — Gradle's `jniLibs.srcDirs` packs every `.so` it finds, but a partial
set fails to load at runtime with an error naming only the first missing
dependency:

- `libdng_decoder_native.so` — the decoder itself.
- `libheif.so`, `libde265.so` — HEIC/AVIF decode route (dynamically linked,
  unversioned names on Android — see
  `native/third_party/heif-dist-android-arm64-v8a/PROVENANCE.md`).
- `libc++_shared.so` — the NDK's shared C++ runtime; the decoder is built
  with `ANDROID_STL=c++_shared` (`native/CMakePresets.json`) regardless of
  which codecs are enabled, so this file is required unconditionally.

`native/cmake/heif.cmake`'s `ANDROID` branches stage all four files next to
each other in the build output directory (`build-android/android-arm64/`)
via `POST_BUILD` copy commands, so copying every `*.so` from that directory
into `arm64-v8a/` picks up the complete set. `.github/workflows/android_build.yml`
does the equivalent copy + a completeness assertion for the CI artifact.

libjxl is NOT yet part of this set: no Android libjxl dist has been
committed (only `native/third_party/libjxl-dist-android-arm64-v8a/PROVENANCE.md`
exists), so `-DCEYX_ENABLE_JXL=OFF` on this platform today.

A committed `.so` here inevitably goes stale relative to the actual release
pin — there is no mechanism that keeps a tracked binary in sync with the
native build it's supposed to represent. Instead, this directory is treated
the same way `plugin/windows/Libraries/*.dll` is: the binary is placed here
by a build or fetch step, never checked in.

## How the `.so` gets here

- Local development: build the native target
  (`native/scripts/build_native_watchdog.py --target dng_decoder_native`,
  Android cross-compile variant) and copy the resulting `.so` into the
  matching ABI directory under `jniLibs/`.
- Downstream consumers (e.g. Halcyon): fetch the pinned release artifact via
  their own build scripts, same pattern as the Windows `.dll` fetch.

## Why the directory itself is still tracked

`plugin/android/src/main/jniLibs` must survive a clean checkout because
downstream projects (Halcyon) consume this plugin **by path** — the
directory structure itself is part of the interface, even though the binary
content is not. Each ABI subdirectory keeps a `.gitkeep` file for this
reason. Do not delete the ABI directories or their `.gitkeep` files.

## .gitignore

See the `plugin/android/src/main/jniLibs/*/*.so` rule (with the
`.gitkeep` negation) in the repository root `.gitignore`.
