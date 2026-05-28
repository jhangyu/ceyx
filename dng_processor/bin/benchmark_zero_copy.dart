// ignore_for_file: avoid_print

import 'dart:async';
import 'dart:io';
import 'package:dng_processor/src/dng_decoder_service.dart';

void main(List<String> args) async {
  final service = DngDecoderService();
  service.initialize();

  final useWorker = args.contains('--worker');
  final filteredArgs = args.where((arg) => arg != '--worker').toList();
  final dngPath = filteredArgs.isNotEmpty ? filteredArgs[0] : '../sample.dng';
  if (!File(dngPath).existsSync()) {
    print('Error: $dngPath not found');
    exit(1);
  }

  print('--- Benchmarking ${useWorker ? 'Worker decode' : 'Zero-copy'} ---');
  print('Loading: $dngPath');

  if (useWorker) {
    final previewSw = Stopwatch()..start();
    final preview = service.getPreviewJpeg(dngPath);
    previewSw.stop();
    print(
      'Preview extraction: ${preview?.length ?? 0} bytes '
      'in ${previewSw.elapsedMilliseconds} ms',
    );

    var ticks = 0;
    final ticker = Timer.periodic(
      const Duration(milliseconds: 50),
      (_) => ticks += 1,
    );
    final stopwatch = Stopwatch()..start();
    final image = await service.decodeOnWorker(dngPath);
    stopwatch.stop();
    ticker.cancel();

    print('Dart-side total time: ${stopwatch.elapsedMilliseconds} ms');
    print('Event-loop ticks during worker decode: $ticks');
    print(
      'C++ internal process time: ${image.processMs.toStringAsFixed(2)} ms',
    );
    print('Image dimensions: ${image.width}x${image.height}');
    print('Buffer size: ${image.rgbaData.length} bytes');

    final sum = image.rgbaData[0] + image.rgbaData[1];
    print('Sample pixel sum: $sum');
    return;
  }

  // Warm up
  service.decode(dngPath);

  final stopwatch = Stopwatch()..start();
  final image = service.decode(dngPath);
  stopwatch.stop();

  print('Dart-side total time: ${stopwatch.elapsedMilliseconds} ms');
  print('C++ internal process time: ${image.processMs.toStringAsFixed(2)} ms');
  print('Image dimensions: ${image.width}x${image.height}');
  print('Buffer size: ${image.rgbaData.length} bytes');

  // Verify data is accessible
  final sum = image.rgbaData[0] + image.rgbaData[1];
  print('Sample pixel sum: $sum');
}
