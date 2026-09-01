# jniLibs — placed-at-build/fetch policy

`libdng_decoder_native.so` under each ABI directory (e.g. `arm64-v8a/`) is
**not committed** to this repository (ruling 2026-08-31, option a, A-T12).

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
