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
| Windows | CMake `<pkg>_bundled_libraries` (`windows/CMakeLists.txt`) | `windows/Libraries/dng_decoder_native.dll` → next to `<App>.exe` |

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
- As of this writing, `windows/Libraries/dng_decoder_native.dll` does not
  exist yet -- it can only be produced by building `native/` on a Windows
  machine (see the Halcyon repo's
  `docs/logs/2026-08-21/windows-ffi-build-runbook.md`). Until it is committed,
  `flutter build windows` for any host app will succeed but the resulting exe
  will fail to `dlopen` the library at runtime.

## Refreshing the binaries

After rebuilding the native library in `native`:

```bash
cp native/build/libdng_decoder_native.dylib \
   plugin/macos/Libraries/
cp native/build-android/android-arm64/libdng_decoder_native.so \
   plugin/android/src/main/jniLibs/arm64-v8a/
copy native\build-windows\dng_decoder_native.dll ^
   plugin\windows\Libraries\
```

The macOS dylib must keep the install name `@rpath/libdng_decoder_native.dylib`
(check with `otool -D`) and must not link anything under `/opt/homebrew`
(check with `otool -L`), or sandboxed hosts will fail to load it.

The Windows DLL must be named exactly `dng_decoder_native.dll` -- the Dart
bindings open it by that bare filename
(`DynamicLibrary.open('dng_decoder_native.dll')`,
`plugin/lib/src/dng_bindings.dart:338-339`), relying on Windows'
"application directory first" DLL search order rather than an explicit path
list (unlike the macOS side's `_openFirst` candidate search).
