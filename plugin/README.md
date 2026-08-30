# ceyx

Packaging-only Flutter FFI plugin. It ships **no Dart API**; it exists so that a
host app's build system bundles the prebuilt `dng_decoder_native` library.

Why it is a separate package: `ceyx_example` is a Flutter *app* project (its
`android/` is an app Gradle root with `include(":app")`, its `macos/` is a
Runner project). A Flutter plugin's platform directories must be library-shaped,
so `ceyx_example` cannot declare `flutter: plugin: platforms:` without breaking
any host app that depends on it. This package carries the plugin declaration
instead, and `ceyx_example` depends on it — Flutter resolves plugins across the
whole transitive package graph, so host apps get the bundling automatically.

| Platform | Mechanism | Artifact |
|---|---|---|
| macOS | CocoaPods `vendored_libraries` | `macos/Libraries/libdng_decoder_native.dylib` → `<App>.app/Contents/Frameworks/` |
| Android | Gradle `jniLibs` | `android/src/main/jniLibs/arm64-v8a/libdng_decoder_native.so` → APK `lib/arm64-v8a/` |
| Windows | CMake `<pkg>_bundled_libraries` (`windows/CMakeLists.txt`) | `windows/Libraries/{dng_decoder_native,heif,libde265}.dll` → next to `<App>.exe` |

## Known limitations

- **Android is arm64-v8a only.** The native build currently emits no x86_64
  slice, so RAW decode does not work on x86_64 emulators.
- **Windows requires a Vulkan 1.1+ driver at runtime.** The native library's
  GPU backend on Windows is Vulkan (see
  `../Halcyon/docs/logs/2026-08-21/windows-ffi-upgrade-findings.md` section 2); there
  is no CPU fallback path. Machines without a compatible GPU driver cannot
  decode full-size RAW on Windows.
- The binaries are **committed prebuilts**, not built during the host app's
  build. Building `native/CMakeLists.txt` (Halide AOT generators, Adobe DNG SDK,
  vendored libjpeg-turbo) inside every host build is not viable.
- `windows/Libraries/*.dll` are **not committed**. They are fetched on demand
  from a pinned ceyx GitHub Release by the Halcyon repo's
  `scripts/build_apps.py` (run `python3 scripts/build_apps.py --fetch-native`
  from Halcyon, or just build `linux`/`windows` there — the fetch happens
  automatically when a destination library is absent). The pin (tag +
  per-asset SHA-256) lives in Halcyon's `scripts/ceyx_release_pin.json`.
  Without a fetch, `flutter build windows` for any host app will succeed but
  the resulting exe will fail to `dlopen` the libraries at runtime.

## Refreshing the binaries

After rebuilding the native library in `native` (macOS/Android — these are
committed prebuilts):

```bash
cp native/build/libdng_decoder_native.dylib \
   plugin/macos/Libraries/
cp native/build-android/android-arm64/libdng_decoder_native.so \
   plugin/android/src/main/jniLibs/arm64-v8a/
```

Windows and Linux libraries (`dng_decoder_native`, `heif`, `libde265`) are
never committed here or copied by hand — they are fetched from the pinned
ceyx release via Halcyon's `scripts/build_apps.py --fetch-native`. To move
the pin itself to a newer ceyx release, run
`python3 scripts/build_apps.py --ceyx-release latest` from Halcyon; it
records the new per-asset SHA-256s and stops without building so the diff
can be reviewed and committed.

The macOS dylib must keep the install name `@rpath/libdng_decoder_native.dylib`
(check with `otool -D`) and must not link anything under `/opt/homebrew`
(check with `otool -L`), or sandboxed hosts will fail to load it.

The Windows DLL must be named exactly `dng_decoder_native.dll` -- the Dart
bindings open it by that bare filename
(`DynamicLibrary.open('dng_decoder_native.dll')`,
`plugin/lib/src/dng_bindings.dart:338-339`), relying on Windows'
"application directory first" DLL search order rather than an explicit path
list (unlike the macOS side's `_openFirst` candidate search).
