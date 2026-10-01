// Shared native-fixture paths for the plugin tests. `flutter test` runs with
// cwd == package root (plugin/), so every path is relative to it. This file
// has no `_test.dart` suffix, so the runner never collects it as a suite.
import 'dart:io';

/// The dylib shipped to the macOS app bundle.
const String shippedDylibPath = 'macos/Libraries/libdng_decoder_native.dylib';

/// In-repo lossless DNG sample used by several suites.
const String losslessDngSamplePath = '../image_samples/lossless_dng_sample.dng';

/// In-repo ARW sample used by several suites.
const String rawArwSamplePath = '../image_samples/raw_sample.arw';

/// A freshly built dylib: the [environmentVariable] override when set,
/// otherwise the default native/build output.
String freshBuildDylibPath(String environmentVariable) =>
    Platform.environment[environmentVariable] ??
    '../native/build/libdng_decoder_native.dylib';

/// Skip reason when the dylib or the sample is absent, otherwise null.
/// The two strings are the exact reasons the suites printed before.
String? missingFixtureReason({required String dylib, required String sample}) {
  if (!File(dylib).existsSync()) {
    return 'no built dylib at $dylib';
  }
  if (!File(sample).existsSync()) return 'no sample at $sample';
  return null;
}
