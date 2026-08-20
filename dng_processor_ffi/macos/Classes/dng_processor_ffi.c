// Placeholder translation unit.
//
// The pod exists only to vendor libdng_decoder_native.dylib, but a pod with no
// source files produces an empty module that CocoaPods/Xcode handle
// inconsistently. This one symbol keeps the pod target a normal, buildable
// target; nothing calls it.
const char* dng_processor_ffi_packaging_tag(void) {
  return "dng_processor_ffi-0.0.1";
}
