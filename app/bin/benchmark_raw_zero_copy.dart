// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:ceyx/ceyx.dart';

/// Phase 18 acceptance harness (spec §5 criterion 1) for the generic RAW
/// route: dimensions, first-pixel checksum, and RGBA pool accounting.
///
/// The per-decode "native diagnostics" phase this harness used to print was
/// removed with the Dart-side RAW diagnostics surface itself (5f602e7e); the
/// getter it read no longer exists, so the claim is dropped here rather than
/// left advertising output the tool cannot produce.
///
/// Run order is deliberate. The worker path frees its native buffer in a
/// `finally`, so the pool assertion after it is DETERMINISTIC and is the hard
/// gate. The zero-copy path hands its buffer to a NativeFinalizer, which only
/// runs on GC — Dart offers no way to force it, so that number is reported,
/// never gated (see the plan's Deviation 4).
void _fail(String what) {
  print('[ASSERT FAIL] $what');
  exit(1);
}

int _firstPixelChecksum(Uint8List rgba) =>
    rgba[0] + rgba[1] + rgba[2] + rgba[3];

void _assertValidImage(DngImage image, String label) {
  if (image.width <= 0 || image.height <= 0) {
    _fail('$label: width=${image.width} height=${image.height} must be > 0');
  }
  final expectedSize = image.width * image.height * 4;
  if (image.rgbaData.length != expectedSize) {
    _fail(
      '$label: rgbaData.length=${image.rgbaData.length} expected '
      '$expectedSize (${image.width}x${image.height}x4)',
    );
  }
  print(
    '[ASSERT PASS] $label: ${image.width}x${image.height} '
    'rgba=${image.rgbaData.length} bytes '
    'decode_ms=${image.decodeMs.toStringAsFixed(2)} '
    'process_ms=${image.processMs.toStringAsFixed(2)}',
  );
  print('[CHECKSUM] first_pixel=${_firstPixelChecksum(image.rgbaData)}');
}

int _parseGcTimeoutMs(List<String> args) {
  for (final arg in args) {
    if (arg.startsWith('--gc-timeout-ms=')) {
      final value = int.tryParse(arg.split('=').last);
      if (value == null || value <= 0) {
        _fail('invalid --gc-timeout-ms value in "$arg"');
      }
      return value!;
    }
  }
  return 10000;
}

Future<void> main(List<String> args) async {
  final workerOnly = args.contains('--worker-only');
  final gcTimeoutMs = _parseGcTimeoutMs(args);
  final positional = args.where((a) => !a.startsWith('--')).toList();
  if (positional.isEmpty) {
    print('Usage: dart run bin/benchmark_raw_zero_copy.dart <raw-file> '
        '[--worker-only] [--gc-timeout-ms=N]');
    exit(1);
  }
  final path = positional.first;
  if (!File(path).existsSync()) {
    print('Error: $path not found');
    exit(1);
  }

  final service = DngDecoderService();
  service.initialize();
  if (!service.rawDecodeAvailable) {
    _fail('raw symbols missing from the loaded dylib');
  }

  print('--- Benchmarking generic RAW route ---');
  print('Loading: $path');

  try {
    // 1) Worker decode: deterministic native free in its finally block.
    final workerSw = Stopwatch()..start();
    final workerImage = await service.decodeOnWorker(path);
    workerSw.stop();
    print('[WORKER] dart_total_ms=${workerSw.elapsedMilliseconds}');
    _assertValidImage(workerImage, 'worker decode');

    // WP5: was service.poolCheckedOut, the native pool gauge, which is deleted
    // along with the pool it counted. CeyxNativeBufferPool.debugTotalLiveAddresses
    // is the user-designated equivalent-strength replacement and has more reach
    // (it counts live checkouts across every pool on the isolate, not one
    // native counter). Note this is now never null, so the "symbol missing"
    // bail-out below is gone rather than converted into a skip.
    final afterWorker = CeyxNativeBufferPool.debugTotalLiveAddresses;
    if (afterWorker != 0) {
      print('[POOL FAIL] live_addresses=$afterWorker');
      exit(1);
    }
    print('[POOL PASS] live_addresses=0');

    if (workerOnly) {
      print('[INFO] --worker-only: skipping the zero-copy and GC phases');
      return;
    }

    // 2) Zero-copy decode, scoped so the only reference is dropped on return.
    void zeroCopyPhase() {
      final sw = Stopwatch()..start();
      final image = service.decode(path);
      sw.stop();
      print('[ZEROCOPY] dart_total_ms=${sw.elapsedMilliseconds}');
      _assertValidImage(image, 'zero-copy decode');
    }

    zeroCopyPhase();

    // 3) Best-effort GC drive. Reported, never gated.
    final gcSw = Stopwatch()..start();
    for (var i = 0; i < 64; i++) {
      final garbage = Uint8List(4 * 1024 * 1024);
      garbage[0] = i;
    }
    var checkedOut = CeyxNativeBufferPool.debugTotalLiveAddresses;
    while (checkedOut != 0 && gcSw.elapsedMilliseconds < gcTimeoutMs) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
      checkedOut = CeyxNativeBufferPool.debugTotalLiveAddresses;
    }
    gcSw.stop();
    print(
      '[POOL GC] live_addresses=$checkedOut after ${gcSw.elapsedMilliseconds}ms '
      '(informational: NativeFinalizer runs only on GC)',
    );
  } on RawUnavailableException catch (e) {
    _fail('raw symbols missing from the loaded dylib: $e');
  } on RawDecodeException catch (e) {
    _fail('RAW decode failed: $e');
  } on DngDecodeException catch (e) {
    _fail('decode rejected: $e');
  }
}
