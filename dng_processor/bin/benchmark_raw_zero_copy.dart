// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:dng_processor/src/dng_decoder_service.dart';

/// Phase 18 acceptance harness (spec §5 criterion 1) for the generic RAW
/// route: dimensions, first-pixel checksum, native diagnostics, and RGBA pool
/// accounting.
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

    final afterWorker = service.poolCheckedOut;
    if (afterWorker == null) {
      _fail('dng_debug_pool_checked_out missing from the loaded dylib');
    }
    if (afterWorker != 0) {
      print('[POOL FAIL] checked_out=$afterWorker');
      exit(1);
    }
    print('[POOL PASS] checked_out=0');

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
      final diag = service.lastRawDiagnostics;
      if (diag == null) {
        _fail('lastRawDiagnostics returned null after a same-isolate decode');
      }
      print(
        '[DIAG] frontend=${diag!.frontend} '
        'unpack_backend=${diag.unpackBackend} '
        'gpu=${diag.gpuBackend} '
        'sample_model=${diag.sampleModel} '
        'cfa=${diag.cfaRepeatWidth}x${diag.cfaRepeatHeight} '
        'unpack_ms=${diag.rawUnpackMs.toStringAsFixed(2)} '
        'gpu_ms=${diag.gpuProcessMs.toStringAsFixed(2)} '
        'total_ms=${diag.totalMs.toStringAsFixed(2)} '
        'repack_bytes=${diag.rawRepackBytes}',
      );
    }

    zeroCopyPhase();

    // 3) Best-effort GC drive. Reported, never gated.
    final gcSw = Stopwatch()..start();
    for (var i = 0; i < 64; i++) {
      final garbage = Uint8List(4 * 1024 * 1024);
      garbage[0] = i;
    }
    var checkedOut = service.poolCheckedOut ?? -1;
    while (checkedOut != 0 && gcSw.elapsedMilliseconds < gcTimeoutMs) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
      checkedOut = service.poolCheckedOut ?? -1;
    }
    gcSw.stop();
    print(
      '[POOL GC] checked_out=$checkedOut after ${gcSw.elapsedMilliseconds}ms '
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
