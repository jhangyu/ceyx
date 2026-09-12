import 'dart:ffi' as ffi;

/// Remaining live pool-debug typedefs. The generic-RAW diagnostics contract
/// (struct/enums/getter) that used to live in this file has been removed —
/// it had zero consumers outside ceyx's own tests. The native
/// `raw_last_diagnostics` symbol and its C-side coverage are unaffected.

// dng_debug_pool_checked_out returns C size_t -> ffi.Size.
typedef DngDebugPoolCheckedOutNative = ffi.Size Function();
typedef DngDebugPoolCheckedOutDart = int Function();
