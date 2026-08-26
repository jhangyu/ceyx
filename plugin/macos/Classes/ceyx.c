// Placeholder translation unit.
//
// The pod exists only to vendor libdng_decoder_native.dylib, but a pod with no
// source files produces an empty module that CocoaPods/Xcode handle
// inconsistently. This one symbol keeps the pod target a normal, buildable
// target; nothing calls it.
const char* ceyx_packaging_tag(void) {
  return "ceyx-0.1.0";
}
