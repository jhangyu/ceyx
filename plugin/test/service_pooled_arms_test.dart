// WP2 Task 2.3: the service's own decode arms stop calling the dylib's
// allocating decode entries. Every public entry still has its exact signature,
// but the buffer underneath now comes from this isolate's CeyxNativeBufferPool,
// so every live full-resolution RGBA address is pool-owned.

import 'package:ceyx/src/dng_decoder_service.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:flutter_test/flutter_test.dart';
import 'support/native_fixtures.dart';

/// The shipped dylib is the only built library in this tree (native/build is
/// not populated here). Tests that need a real decode are skipped without it —
/// stated explicitly rather than silently, so a skip is never mistaken for a
/// pass.

void main() {
  tearDown(() {
    // WP2 Task 2.5: nothing may still be checked out anywhere on this isolate
    // when a test ends — the Dart-side replacement for the native
    // checked-out-count leak assertion.
    expect(CeyxNativeBufferPool.debugTotalLiveAddresses, 0);
  });

  tearDown(() {
    CeyxNativeBufferPool.shared.debugDisposeIdle();
  });

  test('decode() yields an address owned by the shared pool', () {
    final service = DngDecoderService(libraryPath: shippedDylibPath)..initialize();
    final image = service.decode(losslessDngSamplePath);
    expect(image.width, greaterThan(0));
    expect(
      CeyxNativeBufferPool.shared.ownsAddress(image.nativeAddress),
      isTrue,
      reason: 'decode() must allocate through the Dart pool, not the dylib',
    );
    // Round-1 review F1: the explicit release must DETACH the safety net, or a
    // later collection of this typed list would reclaim a buffer that has since
    // been handed to someone else.
    final detachesBefore = CeyxNativeBufferPool.debugSafetyNetDetaches;
    image.releaseToPool();
    expect(
      CeyxNativeBufferPool.debugSafetyNetDetaches,
      detachesBefore + 1,
      reason: 'releaseToPool must disarm the safety net for this buffer',
    );
    // A POOLED buffer stays pool-owned after release — it returns to the free
    // list rather than being freed — so the reclaim is asserted on the checkout
    // count, not on ownership.
    expect(
      CeyxNativeBufferPool.shared.debugCheckedOut,
      0,
      reason: 'releaseToPool must return the buffer to the pool',
    );
    expect(CeyxNativeBufferPool.shared.debugFinalizerReleases, 0);
  }, skip: missingFixtureReason(dylib: shippedDylibPath, sample: losslessDngSamplePath));

  test('decodeForPointerTransfer() yields an address owned by the shared pool', () {
    final service = DngDecoderService(libraryPath: shippedDylibPath)..initialize();
    final wire = service.decodeForPointerTransfer(losslessDngSamplePath);
    final address = wire[0] as int;
    expect(address, isNot(0));
    expect(CeyxNativeBufferPool.shared.ownsAddress(address), isTrue);
    // The pointer route ships ownership onward, so the caller releases.
    expect(CeyxNativeBufferPool.shared.tryReleaseByAddress(address), isTrue);
  }, skip: missingFixtureReason(dylib: shippedDylibPath, sample: losslessDngSamplePath));

  test('decodeOnWorker() copies out and leaves nothing checked out', () async {
    final service = DngDecoderService(libraryPath: shippedDylibPath)..initialize();
    final image = await service.decodeOnWorker(losslessDngSamplePath);
    expect(image.width, greaterThan(0));
    // The transferable route copies into Dart-owned bytes on the worker
    // isolate and returns its pool buffer there, so this isolate's pool holds
    // nothing.
    expect(CeyxNativeBufferPool.shared.debugCheckedOut, 0);
  }, skip: missingFixtureReason(dylib: shippedDylibPath, sample: losslessDngSamplePath));
}
