import 'package:flutter_test/flutter_test.dart';

import 'package:ceyx/ceyx.dart';

void main() {
  test('a dylib without the HEIF symbols degrades instead of throwing', () {
    // Constructing and querying must NEVER throw: an older dylib that predates
    // the HEIF route has to leave the rest of the decoder fully working. The
    // failure mode this pins is a constructor-time lookupFunction throw, which
    // would kill ALL decoding, not just HEIC.
    final service = HeifDecoderService(
      libraryPath: 'definitely-not-a-dylib-${DateTime.now().microsecond}',
    );
    expect(service.heifAvailable, isFalse);
  });

  test('probeOnWorker returns null rather than throwing when unavailable',
      () async {
    final service = HeifDecoderService(libraryPath: 'no-such-library');
    // The loader calls this and is documented as never throwing, so a null is
    // the only acceptable answer here.
    expect(await service.probeOnWorker('/tmp/whatever.heic'), isNull);
  });

  test('decodeOnWorker throws HeifUnavailableException when unavailable',
      () async {
    final service = HeifDecoderService(libraryPath: 'no-such-library');
    // The DECODE path is allowed to throw — the dispatcher's arm turns any
    // throw into the uniform permanent miss. What it must not do is crash or
    // return a bogus image.
    await expectLater(
      service.decodeOnWorker('/tmp/whatever.heic'),
      throwsA(isA<HeifUnavailableException>()),
    );
  });
}
