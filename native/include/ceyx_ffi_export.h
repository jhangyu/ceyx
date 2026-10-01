// ceyx_ffi_export.h -- the ONE definition of the C-ABI export attribute.
//
// T5a (2026-10-02 techdebt campaign): this attribute used to be defined eight
// times under three names (FFI_EXPORT, RAW_FFI_EXPORT, CEYX_FFI_EXPORT).
// Self-contained on purpose: the public header ceyx_orient.h includes it with
// a quoted sibling include, which resolves next to ceyx_orient.h itself for
// every consumer (generators, tests, the decode-into TU) without any extra
// include path.
#ifndef CEYX_FFI_EXPORT_H
#define CEYX_FFI_EXPORT_H

#if defined(_WIN32)
#define CEYX_FFI_EXPORT __declspec(dllexport)
#else
#define CEYX_FFI_EXPORT __attribute__((visibility("default"))) __attribute__((used))
#endif

#endif  // CEYX_FFI_EXPORT_H
