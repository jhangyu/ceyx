import 'dart:io';
import 'dart:ffi' as ffi;
import 'package:ffi/ffi.dart';
import 'package:dng_processor/src/dng_bindings.dart';

void main(List<String> args) {
  if (args.isEmpty) {
    print('Usage: dart benchmark_preview.dart <path_to_dng>');
    exit(1);
  }

  final dngPath = args[0];
  print('--- Benchmarking Fast Preview Extraction ---');
  print('Target: $dngPath');

  final bindings = DngNativeBindings();

  final outBufferPtr = calloc<ffi.Pointer<ffi.Uint8>>();
  final outSizePtr = calloc<ffi.Int32>();
  final pathPtr = dngPath.toNativeUtf8();

  try {
    final sw = Stopwatch()..start();
    final result = bindings.extractPreviewJpeg(
      pathPtr,
      outBufferPtr,
      outSizePtr,
    );
    sw.stop();

    if (result == 0) {
      final size = outSizePtr.value;
      print('Success! Extracted $size bytes in ${sw.elapsedMilliseconds} ms');

      // We can also verify it's a JPEG by checking magic bytes
      if (size >= 2) {
        final data = outBufferPtr.value.asTypedList(size);
        if (data[0] == 0xFF && data[1] == 0xD8) {
          print('Valid JPEG magic bytes found (FF D8)');
        }
      }

      // Save it out for proof
      final outFile = File('preview_out.jpg');
      outFile.writeAsBytesSync(outBufferPtr.value.asTypedList(size));
      print('Saved preview to ${outFile.path}');

      bindings.freeBuffer(outBufferPtr.value);
    } else {
      print('Failed with error code: $result');
    }
  } finally {
    calloc.free(outBufferPtr);
    calloc.free(outSizePtr);
    calloc.free(pathPtr);
  }
}
