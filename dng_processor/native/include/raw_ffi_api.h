#pragma once

/* RAW (LibRaw) route FFI surface.
 *
 * Extracted from dng_ffi_api.h on 2026-08-25. The RAW route returns the same
 * DngResult as the DNG route and reuses dng_free_result/dng_free_rgba_buffer
 * for teardown, so this header includes dng_ffi_api.h rather than duplicating
 * the struct: the ABI is shared by design, and duplicating it would create two
 * definitions to keep in sync with dng_processor_ffi/lib/src/dng_bindings.dart.
 *
 * The full definition of RawDecodeDiagnostics lives in raw_pipeline_contract.h;
 * this header only forward-declares it, exactly as dng_ffi_api.h used to.
 */
#include <stdint.h>

#include "dng_ffi_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Generic (non-DNG) RAW decode. Returns the SAME DngResult layout as
 * dng_decode_and_process, so no Dart struct change is needed; free it with
 * dng_free_result(). max_dim <= 0 means full resolution.
 * error_code carries a RawErrorCode (<= -201) on failure. */
struct RawDecodeDiagnostics;
DngResult *raw_decode_and_process(const char *file_path, int32_t max_dim);

/* Diagnostics for the calling thread's most recent raw_decode_and_process.
 * Returns 0 on success, -1 when out is null or no decode has run. */
int32_t raw_last_diagnostics(struct RawDecodeDiagnostics *out);

#ifdef __cplusplus
}
#endif
