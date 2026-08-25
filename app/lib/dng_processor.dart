/// Public API surface for the `dng_processor` package.
///
/// Downstream consumers (e.g. Halcyon) should import this barrel instead of
/// reaching into `package:dng_processor/src/...`, which trips the
/// `implementation_imports` lint and exposes internals that are not part of
/// the stable contract.
library;

export 'package:dng_processor_ffi/dng_processor_ffi.dart';

export 'src/dng_image_widget.dart' show dngImageToUiImage, DngImageWidget;
