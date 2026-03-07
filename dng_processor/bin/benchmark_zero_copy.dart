import 'dart:io';
import 'package:dng_processor/src/dng_decoder_service.dart';

void main() async {
  final service = DngDecoderService();
  service.initialize();

  final dngPath = '../sample.dng';
  if (!File(dngPath).existsSync()) {
    print('Error: $dngPath not found');
    exit(1);
  }

  print('--- Benchmarking Zero-copy ---');
  print('Loading: $dngPath');

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
