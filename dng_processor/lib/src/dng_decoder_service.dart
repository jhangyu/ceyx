/// Compatibility shim.
///
/// 2026-08-21 (D1): the decoder API and its FFI bindings moved to
/// `package:dng_processor_ffi`, which is the plugin package that also ships the
/// native library. Downstream apps should depend on `dng_processor_ffi`
/// directly — depending on `dng_processor` drags in this project's app-harness
/// dependencies (file_picker, path_provider), which broke host Android builds.
///
/// This file stays so the in-repo harness (`lib/main.dart`, `bin/benchmark_*`)
/// and any existing `package:dng_processor/src/dng_decoder_service.dart`
/// imports keep working unchanged.
library;

export 'package:dng_processor_ffi/dng_processor_ffi.dart';
