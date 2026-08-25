# ceyx_example

Flutter UI and Dart FFI frontend for the native DNG decoder.

## Build

Use the native build watchdog as the single build entry point:

```bash
python3 native/scripts/build_native_watchdog.py \
  --target dng_decoder_native \
  --build-macos-app \
  --macos-mode debug
```

Android and web use the same entry point:

```bash
python3 native/scripts/build_native_watchdog.py \
  --target dng_decoder_native \
  --build-android-app \
  --android-mode debug

python3 native/scripts/build_native_watchdog.py \
  --target none \
  --build-web-app
```

The Flutter/Xcode build cache remains under `build/`, but the useful artifacts
are published to a shallow `dist/` tree:

```text
dist/
├── ceyx_example.app
├── libdng_decoder_native.dylib
├── ceyx_example.apk
└── web/
```

Direct `flutter build macos --debug` is still supported. The Xcode project calls
`native/scripts/build_native_watchdog.py --embed-macos-dylib-only` during the
build so the app bundle receives the current
`native/build/libdng_decoder_native.dylib`.
