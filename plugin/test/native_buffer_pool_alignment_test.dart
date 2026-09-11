import 'dart:ffi';
import 'dart:io';

import 'package:ceyx/src/dng_bindings.dart';
import 'package:ceyx/src/native_buffer_pool.dart';
import 'package:flutter_test/flutter_test.dart';

/// R4 (gpu-copy-elimination campaign), Round 4 acceptance item 1: every
/// POOLED allocation must satisfy `address % 16384 == 0 && capacity % 16384
/// == 0` — the same alignment probe `ceyxDecodeIntoPrepare`
/// (native/src/ffi/ceyx_decode_into_ffi.cpp) checks before engaging the C2
/// zero-copy wrap.
///
/// Loads a REAL freshly-built dylib (never the vendored
/// plugin/macos/Libraries copy, which is not guaranteed to include this
/// round's native additions) — same convention as
/// wp10_activation_proof_test.dart. Skips when no such build is present
/// rather than failing, since building native/ is outside `flutter test`'s
/// own responsibility.
///
/// flutter test runs with cwd == package root (plugin/), so paths below are
/// resolved relative to Directory.current.
void main() {
  const int alignmentBytes = 16384;

  final dylibPath = File(
    Platform.environment['CEYX_R4_DYLIB'] ??
        '../native/build/libdng_decoder_native.dylib',
  ).absolute.path;

  late bool skip;
  String skipReason = '';

  setUp(() {
    skip = false;
    CeyxNativeBufferPool.debugNativeBindingsOverride = null;
    CeyxNativeBufferPool.debugResetNativeBindingsCache();
    if (!File(dylibPath).existsSync()) {
      skip = true;
      skipReason =
          'reason: no freshly-built dylib at $dylibPath — set '
          'CEYX_R4_DYLIB or build native/build via cmake first';
      return;
    }
    final bindings = DngNativeBindings.fromPath(dylibPath);
    if (!bindings.poolAlignedAllocatorAvailable) {
      skip = true;
      skipReason =
          'reason: dylib at $dylibPath does not export '
          'ceyx_pool_aligned_alloc/free — point CEYX_R4_DYLIB at a build '
          'that includes Round 4';
      return;
    }
    CeyxNativeBufferPool.debugNativeBindingsOverride = bindings;
  });

  tearDown(() {
    CeyxNativeBufferPool.debugNativeBindingsOverride = null;
    CeyxNativeBufferPool.debugResetNativeBindingsCache();
  });

  test(
    'AC1: a pooled allocation is page-aligned on both pointer and capacity, '
    'for a size that is NOT itself a multiple of the page size',
    () async {
      if (skip) {
        markTestSkipped(skipReason);
        return;
      }
      final pool = CeyxNativeBufferPool(maxBuffers: 2);
      addTearDown(pool.debugDisposeIdle);

      // Deliberately not a multiple of 16384, so a pass here proves the pool
      // rounds UP rather than merely being lucky.
      const requested = 97 * 1024 * 1024 + 37;
      final buffer = await pool.acquire(requested);
      addTearDown(() => pool.release(buffer));

      expect(
        buffer.address % alignmentBytes,
        0,
        reason: 'pointer must be a page-alignment multiple',
      );
      expect(
        buffer.capacity % alignmentBytes,
        0,
        reason: 'capacity must be a page-alignment multiple',
      );
      expect(
        buffer.capacity,
        greaterThanOrEqualTo(requested),
        reason: 'rounding up must never under-serve the request',
      );
      expect(buffer.alignedAllocated, isTrue);
    },
  );

  test(
    'AC1: a small pooled allocation is still page-aligned (rounds up from a '
    'tiny request)',
    () async {
      if (skip) {
        markTestSkipped(skipReason);
        return;
      }
      final pool = CeyxNativeBufferPool(maxBuffers: 1);
      addTearDown(pool.debugDisposeIdle);

      final buffer = await pool.acquire(64);
      addTearDown(() => pool.release(buffer));

      expect(buffer.address % alignmentBytes, 0);
      expect(buffer.capacity % alignmentBytes, 0);
      expect(buffer.capacity, greaterThanOrEqualTo(64));
    },
  );

  test(
    'AC4: a mismatched-allocator free is impossible by construction — the '
    'aligned free is what actually reclaims a pooled buffer, observed via '
    'the aligned-free native entry rather than assumed',
    () async {
      if (skip) {
        markTestSkipped(skipReason);
        return;
      }
      final bindings = DngNativeBindings.fromPath(dylibPath);
      final freedAddresses = <int>[];
      final realFree = bindings.ceyxPoolAlignedFree!;
      CeyxNativeBufferPool.debugFreeHook = (address) {
        freedAddresses.add(address);
        realFree(Pointer<Uint8>.fromAddress(address));
      };
      addTearDown(() => CeyxNativeBufferPool.debugFreeHook = null);

      final pool = CeyxNativeBufferPool(maxBuffers: 1);
      final buffer = await pool.acquire(4096);
      expect(buffer.alignedAllocated, isTrue);
      pool.release(buffer);
      pool.debugDisposeIdle();

      expect(freedAddresses, contains(buffer.address));
    },
  );
}
