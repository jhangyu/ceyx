import 'dart:ffi';
void main() {
  try {
    DynamicLibrary.open('/Users/jhangyu/Documents/flutter_dng_decoder/dng_processor/native/build/libdng_decoder_native.dylib');
    print("Loaded successfully");
  } catch (e) {
    print(e);
  }
}
