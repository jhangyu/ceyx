import 'package:ceyx/ceyx.dart';
import 'package:flutter_test/flutter_test.dart';

/// WP6 Task 6.1: `CeyxNativeBufferPool.warmUpFor` replaces the native
/// warmup's step-2 page pre-commit (`warmPipelinePoolsForSize`, deleted in
/// WP5) by pre-committing the pages of a pooled buffer on the Dart side.
///
/// Written BEFORE the implementation exists; the recorded red is a compile
/// error naming `warmUpFor`.
void main() {
  test(
    'TC-1100: warmUpFor leaves one idle, zero checked-out buffer, and a '
    'subsequent acquire reuses the same address',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(pool.debugDisposeIdle);

      await pool.warmUpFor(1024);

      expect(pool.debugIdleCount, 1);
      expect(pool.debugCheckedOut, 0);

      final buffer = await pool.acquire(1024);
      final warmedAddress = pool.debugLiveAddresses.single;
      expect(buffer.address, warmedAddress);
      pool.release(buffer);
    },
  );

  test(
    'TC-1101: a second warmUpFor at the same size performs no additional '
    'allocation',
    () async {
      final pool = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(pool.debugDisposeIdle);

      await pool.warmUpFor(1024);
      final allocationsAfterFirst = pool.debugAllocations;

      await pool.warmUpFor(1024);

      expect(pool.debugAllocations, allocationsAfterFirst);
      expect(pool.debugIdleCount, 1);
      expect(pool.debugCheckedOut, 0);
    },
  );
}
