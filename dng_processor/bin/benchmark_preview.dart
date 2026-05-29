// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:io';
import 'package:dng_processor/src/dng_decoder_service.dart';

/// Benchmark: preview JPEG extraction via DngDecoderService worker isolate.
///
/// Uses [DngDecoderService.getPreviewJpegOnWorker] so the event loop keeps
/// ticking while the native FFI call runs on a background isolate.
///
/// Usage:
///   dart run bin/benchmark_preview.dart `<path_to_dng>`
void main(List<String> args) async {
  if (args.isEmpty) {
    print('Usage: dart benchmark_preview.dart <path_to_dng>');
    exit(1);
  }

  final dngPath = args[0];
  if (!File(dngPath).existsSync()) {
    print('Error: $dngPath not found');
    exit(1);
  }

  print('--- Benchmarking Fast Preview Extraction (worker isolate) ---');
  print('Target: $dngPath');

  final decoder = DngDecoderService()..initialize();

  // Track event-loop ticks while the worker runs — verifies the UI thread
  // remains unblocked during native preview extraction.
  // Preview extraction is typically only a few ms, so use a sub-millisecond
  // ticker to give the event loop a chance to fire several times before the
  // worker isolate finishes.
  var ticks = 0;
  final ticker = Timer.periodic(
    const Duration(milliseconds: 1),
    (_) => ticks++,
  );

  final sw = Stopwatch()..start();
  final previewBytes = await decoder.getPreviewJpegOnWorker(dngPath);
  sw.stop();
  ticker.cancel();

  if (previewBytes != null) {
    print(
      'Success! Extracted ${previewBytes.length} bytes '
      'in ${sw.elapsedMilliseconds} ms',
    );

    // Verify JPEG magic bytes (FF D8)
    if (previewBytes.length >= 2 &&
        previewBytes[0] == 0xFF &&
        previewBytes[1] == 0xD8) {
      print('Valid JPEG magic bytes found (FF D8)');
    }

    // Save preview for visual inspection
    final outFile = File('preview_out.jpg');
    outFile.writeAsBytesSync(previewBytes);
    print('Saved preview to ${outFile.path}');

    print('Event-loop ticks during worker preview: $ticks');
  } else {
    print('Failed to extract preview JPEG');
    exit(1);
  }
}
