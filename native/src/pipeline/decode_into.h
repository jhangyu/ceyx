// decode_into.h -- INTERNAL to the pipeline layer. Not installed, not part of
// the C ABI; the exported entries are in include/ceyx_decode_into.h.
//
// T5b (2026-10-02 techdebt campaign): the decode-into body every
// ceyx_decode_into_* entry shares, callable from the pipeline layer without
// going through an exported FFI symbol.
#pragma once

#include <cstddef>
#include <cstdint>

#include "dng_ffi_api.h"      // DngResult
#include "raw_file_router.h"  // RawRoute

namespace ceyx {

// Body of ceyx_probe_output_size: metadata-only output extent on whichever
// route owns the file. Out-params are zeroed first; 0 on success.
int32_t probeOutputSize(const char *file_path, int32_t max_dim,
                        int32_t *out_width, int32_t *out_height);

// Phases 1-2 (route probe, extent probe, capacity refusal, alignment probe).
// Returns true when the caller may proceed to decodeIntoPhase3.
bool decodeIntoPrepare(const char *file_path, int32_t max_dim,
                       const uint8_t *dst, size_t dst_capacity,
                       int32_t output_format, RawRoute *route,
                       DngResult *result,
                       bool *out_destination_is_page_aligned);

// Phase 3: decode into dst through the route's caller-buffer sibling.
void decodeIntoPhase3(const char *file_path, int32_t max_dim, RawRoute route,
                      uint8_t *dst, size_t dst_capacity,
                      int32_t exif_orientation,
                      bool destination_is_page_aligned,
                      int32_t output_format, DngResult *result);

// Body of ceyx_decode_into_buffer (rgba8, orientation 1).
DngResult *decodeIntoBuffer(const char *file_path, int32_t max_dim,
                            uint8_t *dst, size_t dst_capacity);

}  // namespace ceyx
