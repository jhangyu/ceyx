// Pure-Dart routing contract. Loads no dylib, so it runs anywhere.
import 'package:flutter_test/flutter_test.dart';

import 'package:dng_processor_ffi/src/raw_route.dart';

void main() {
  group('decodeExtensionOf', () {
    test('lowercases and strips the dot', () {
      expect(decodeExtensionOf('/a/b/RAW.ARW'), 'arw');
      expect(decodeExtensionOf('c:\\photos\\Shot.NEF'), 'nef');
      expect(decodeExtensionOf('file.dng'), 'dng');
    });

    test('returns empty string when the basename has no usable dot', () {
      expect(decodeExtensionOf('noext'), '');
      expect(decodeExtensionOf('trailing.'), '');
      expect(decodeExtensionOf('/a/b.dng/file'), '');
      expect(decodeExtensionOf(''), '');
    });
  });

  group('decodeRouteForPath', () {
    test('every RAW allowlist extension routes to DecodeRoute.raw', () {
      for (final ext in kRawExtensions) {
        expect(
          decodeRouteForPath('/samples/photo.$ext'),
          DecodeRoute.raw,
          reason: 'extension .$ext must route generic',
        );
        expect(
          decodeRouteForPath('/samples/photo.${ext.toUpperCase()}'),
          DecodeRoute.raw,
          reason: 'uppercase .$ext must route generic',
        );
      }
    });

    test('x3f routes to the RAW entry point', () {
      // Foveon X3F. Correct only because raw_file_router.cpp routes the
      // FOVb magic (P19 T4) — this list's documented invariant is that it
      // matches the native router.
      expect(decodeRouteForPath('sigma_sd_quattro.x3f'), DecodeRoute.raw);
      expect(decodeRouteForPath('SIGMA_SD_QUATTRO.X3F'), DecodeRoute.raw,
          reason: 'extension matching is case-insensitive');
    });

    test('dng routes to the existing DNG entry', () {
      expect(decodeRouteForPath('a.dng'), DecodeRoute.dng);
      expect(decodeRouteForPath('A.DNG'), DecodeRoute.dng);
    });

    test('anything else is unsupported', () {
      expect(decodeRouteForPath('a.txt'), DecodeRoute.unsupported);
      expect(decodeRouteForPath('a.jpg'), DecodeRoute.unsupported);
      expect(decodeRouteForPath('noext'), DecodeRoute.unsupported);
      expect(decodeRouteForPath('trailing.'), DecodeRoute.unsupported);
      expect(decodeRouteForPath('/a/b.dng/file'), DecodeRoute.unsupported);
    });

    test('kSupportedDecodeExtensions is dng plus the RAW allowlist', () {
      expect(kSupportedDecodeExtensions.first, 'dng');
      expect(kSupportedDecodeExtensions.length, kRawExtensions.length + 1);
      expect(kSupportedDecodeExtensions.toSet().length,
          kSupportedDecodeExtensions.length);
    });
  });
}
