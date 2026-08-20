/// DNG/RAW decoding through the bundled `dng_decoder_native` library.
///
/// This package is both the public Dart API and the Flutter FFI plugin that
/// makes the host app's build system bundle the native library:
///
/// * macOS   — CocoaPods embeds `macos/Libraries/libdng_decoder_native.dylib`
///             into `<App>.app/Contents/Frameworks/`.
/// * Android — Gradle packs `android/src/main/jniLibs/<abi>/
///             libdng_decoder_native.so` into the APK.
///
/// Host apps should depend on this package rather than on `dng_processor`:
/// `dng_processor` is a Flutter *app* project whose harness dependencies
/// (file_picker, path_provider) leak into any app that depends on it.
library;

export 'src/dng_decoder_service.dart'
    show DngImage, DngErrorCode, DngDecodeException, DngDecoderService;

/// Marker for the vendored binaries, so a build can be traced back to a
/// specific drop of the native library.
const String dngNativeLibraryTag = 'dng_processor_ffi-0.0.1';
