import 'dart:ffi';

import 'package:ceyx/src/decode_pool.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:ffi/ffi.dart' show malloc;
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('an oversize acquire is pool-owned and releasable by address', () async {
    final freed = <int>[];
    CeyxNativeBufferPool.debugFreeHook = freed.add;
    addTearDown(() => CeyxNativeBufferPool.debugFreeHook = null);

    final pool = CeyxNativeBufferPool(maxBuffers: 2, maxBufferBytes: 1024);
    final buf = await pool.acquire(4096); // > maxBufferBytes -> unpooled

    expect(
      pool.ownsAddress(buf.address),
      isTrue,
      reason: 'an unpooled buffer the pool itself allocated must be findable',
    );
    expect(pool.tryReleaseByAddress(buf.address), isTrue);
    expect(freed, <int>[buf.address]);
    expect(pool.debugIdleCount, 0);
    // A second release is a no-op, not a double free.
    expect(pool.tryReleaseByAddress(buf.address), isFalse);
    expect(freed.length, 1);

    malloc.free(Pointer<Uint8>.fromAddress(buf.address));
  });

  test('adoptUnpooled takes ownership of a foreign address', () {
    final freed = <int>[];
    CeyxNativeBufferPool.debugFreeHook = freed.add;
    addTearDown(() => CeyxNativeBufferPool.debugFreeHook = null);

    final pool = CeyxNativeBufferPool(maxBuffers: 2);
    final ptr = malloc<Uint8>(64);
    final adopted = pool.adoptUnpooled(ptr.address, 64);

    expect(adopted.pooled, isFalse);
    expect(pool.ownsAddress(ptr.address), isTrue);
    expect(pool.debugAdoptions, 1);
    expect(pool.tryReleaseByAddress(ptr.address), isTrue);
    expect(freed, <int>[ptr.address]);
    expect(pool.debugIdleCount, 0);
    malloc.free(ptr); // the hook intercepted the pool's free; reclaim for real
  });

  test('CeyxDecodePool.nativeBufferPool defaults to the shared pool', () {
    expect(CeyxDecodePool.nativeBufferPool, same(CeyxNativeBufferPool.shared));
  });
}
