// T12.0 (mem8 v3, Phase P0) — the FROZEN format contract's shape.
//
// These are shape tests, not behaviour tests: T12.0 ships declarations only.
// Their job is to make an accidental drift in the frozen shape fail loudly,
// because four tasks in two repos and two languages encode this shape
// (T12 kernel arms, T13 converter, T14 binding, T15a Halcyon seam).
//
// Both checks read the OTHER side from disk and compare mechanically rather
// than restating it — a mirror asserted against a hand-copied literal is a
// second source of truth, which is the drift it is supposed to catch.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/src/codec_format.dart';

/// Repo-relative from `plugin/`, which is `flutter test`'s cwd here.
File _repoFile(String relative) {
  final direct = File(relative);
  if (direct.existsSync()) return direct;
  return File('plugin/$relative'.replaceFirst('plugin/../', ''));
}

void main() {
  test('CeyxOutputFormat mirrors enum CeyxOutputFormat in raw_ffi_api.h', () {
    final header = _repoFile('../native/include/raw_ffi_api.h');
    expect(header.existsSync(), isTrue,
        reason: 'contract header not found at ${header.absolute.path}');
    final text = header.readAsStringSync();

    final block = RegExp(r'enum\s+CeyxOutputFormat\s*\{(.*?)\}', dotAll: true)
        .firstMatch(text);
    expect(block, isNotNull,
        reason: 'enum CeyxOutputFormat is absent from raw_ffi_api.h');

    // `kCeyxOutputFormatRgba8 = 0` -> ('Rgba8', 0), in declaration order.
    final native = <String, int>{};
    for (final m in RegExp(r'kCeyxOutputFormat(\w+)\s*=\s*(-?\d+)')
        .allMatches(block!.group(1)!)) {
      native[m.group(1)!.toLowerCase()] = int.parse(m.group(2)!);
    }

    final dart = {
      for (final v in CeyxOutputFormat.values) v.name.toLowerCase(): v.value,
    };

    // Same names, same values, same count — an enumerator added on one side
    // only, or renumbered on either side, fails here.
    expect(dart, equals(native));
    expect(native.keys.toList(), equals(dart.keys.toList()),
        reason: 'declaration order must match; values are append-only');
    expect(CeyxOutputFormat.rgba8.value, 0,
        reason: 'R-B: rgba8 is enumerator 0 and stays the C-side default');
  });

  test('ceyxOutputFormatByteCount implements the frozen 2a formula', () {
    // Literals, deliberately not recomputed with the expression under test.
    expect(ceyxOutputFormatByteCount(CeyxOutputFormat.rgba8, 4, 4), 64);
    expect(ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 4, 4), 24);
    // ODD extents: a naive w/2 gives 1x1 chroma planes (16 + 2 = 18) instead
    // of ceil(w/2) = 2x2 (9 + 2*4 = 17 for 3x3). This is the case that
    // catches a half-plane miscalculation, and an under-count here is a heap
    // overrun, not a miscount.
    expect(ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 3, 3), 17);
    expect(ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 5, 3), 27);
    expect(() => ceyxOutputFormatByteCount(CeyxOutputFormat.yuv420, 0, 4),
        throwsArgumentError);
  });

  test('CeyxFormatUnsupportedException declares EXACTLY three fields', () {
    // T12.0.2's freeze. Parsed from source because dart:mirrors is
    // unavailable under flutter_test — and because the point is to fail when
    // a FOURTH field is added, which no constructed instance would reveal.
    final source = _repoFile('lib/src/codec_format.dart').readAsStringSync();
    final body = RegExp(
      r'class\s+CeyxFormatUnsupportedException\s+implements\s+Exception\s*\{(.*?)\n\}',
      dotAll: true,
    ).firstMatch(source);
    expect(body, isNotNull,
        reason: 'CeyxFormatUnsupportedException is absent from codec_format.dart');

    // Everything before `@override` is the constructor + the field block.
    // Matching ANY member declaration (not only `final T name;`) is
    // deliberate: an added `final String x = '';` carries an initialiser and
    // a `var`/`late` field carries a different modifier, and a probe that
    // only recognised the frozen spelling would pass on exactly the drift it
    // exists to catch — observed, M1 in tmp/verify/t120/t120-red-probes.txt.
    final declarations = body!.group(1)!.split('@override').first;
    final fields = RegExp(
      r'^\s*(?:static\s+)?(?:final\s+|const\s+|late\s+|var\s+)?'
      r'[A-Za-z_][\w<>?.]*[\w>?]?\s+(\w+)\s*(?:=[^;]*)?;\s*$',
      multiLine: true,
    ).allMatches(declarations).map((m) => m.group(1)!).toList();
    expect(fields, equals(['format', 'missingSymbol', 'libraryPath']),
        reason: 'the exception shape is FROZEN at three fields (T12.0.2); a '
            'fourth — notably the expected pin digest — is ruled out');

    final required = RegExp(r'required\s+this\.(\w+)')
        .allMatches(body.group(1)!)
        .map((m) => m.group(1)!)
        .toList();
    expect(required, equals(fields),
        reason: 'every frozen field is required; none may be optional');

    const e = CeyxFormatUnsupportedException(
      format: CeyxOutputFormat.yuv420,
      missingSymbol: 'ceyx_decode_into_buffer_format',
      libraryPath: '/abs/path/libdng_decoder_native.dylib',
    );
    expect(e.toString(), contains('/abs/path/libdng_decoder_native.dylib'));
    expect(e.toString(), contains('ceyx_decode_into_buffer_format'));
    expect(e.toString(), contains('yuv420'));
  });

  test('the frozen FFI entry points exist as declarations in raw_ffi_api.h',
      () {
    final text = _repoFile('../native/include/raw_ffi_api.h').readAsStringSync();
    for (final symbol in const [
      'ceyx_output_format_byte_count',
      'ceyx_probe_output_size_format',
      'ceyx_decode_into_buffer_format',
      'ceyx_decode_into_buffer_oriented_format',
      'ceyx_yuv420_to_rgba8',
    ]) {
      expect(RegExp('\\b$symbol\\s*\\(').hasMatch(text), isTrue,
          reason: '$symbol is missing from the frozen contract header');
    }
    expect(text, contains('CeyxYuv420PlaneDescriptor'));
    // The layout ruling must be stated in the header, not only in the plan.
    expect(text, contains('w*h + 2*(ceil(w/2) * ceil(h/2))'));
    expect(text.contains('TIGHTLY PACKED'), isTrue);
  });
}
