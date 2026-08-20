# dng_processor_ffi

Packaging-only Flutter FFI plugin. It ships **no Dart API**; it exists so that a
host app's build system bundles the prebuilt `dng_decoder_native` library.

Why it is a separate package: `dng_processor` is a Flutter *app* project (its
`android/` is an app Gradle root with `include(":app")`, its `macos/` is a
Runner project). A Flutter plugin's platform directories must be library-shaped,
so `dng_processor` cannot declare `flutter: plugin: platforms:` without breaking
any host app that depends on it. This package carries the plugin declaration
instead, and `dng_processor` depends on it — Flutter resolves plugins across the
whole transitive package graph, so host apps get the bundling automatically.

| Platform | Mechanism | Artifact |
|---|---|---|
| macOS | CocoaPods `vendored_libraries` | `macos/Libraries/libdng_decoder_native.dylib` → `<App>.app/Contents/Frameworks/` |
| Android | Gradle `jniLibs` | `android/src/main/jniLibs/arm64-v8a/libdng_decoder_native.so` → APK `lib/arm64-v8a/` |

## Known limitations

- **Android is arm64-v8a only.** The native build currently emits no x86_64
  slice, so RAW decode does not work on x86_64 emulators.
- The binaries are **committed prebuilts**, not built during the host app's
  build. Building `native/CMakeLists.txt` (Halide AOT generators, Adobe DNG SDK,
  vendored libjpeg-turbo) inside every host build is not viable.

## Refreshing the binaries

After rebuilding the native library in `dng_processor/native`:

```bash
cp dng_processor/native/build/libdng_decoder_native.dylib \
   dng_processor_ffi/macos/Libraries/
cp dng_processor/native/build-android/android-arm64/libdng_decoder_native.so \
   dng_processor_ffi/android/src/main/jniLibs/arm64-v8a/
```

The macOS dylib must keep the install name `@rpath/libdng_decoder_native.dylib`
(check with `otool -D`) and must not link anything under `/opt/homebrew`
(check with `otool -L`), or sandboxed hosts will fail to load it.
