#pragma once

// mem8 v3 T12 — THE destination sizing arithmetic for CeyxOutputFormat.
//
// raw_ffi_api.h declares `ceyx_output_format_byte_count` as the contract's
// sizing function and states that a consumer which open-codes the arithmetic
// instead is a DEFECT BY CONTRACT. That exported symbol lives in
// native/src/ffi/raw_ffi_api.cpp, which is NOT linked into the many test
// targets that compile the Stage-4 pipeline sources directly
// (native/cmake/tests.cmake lists src/pipeline/dng_render_halide.cpp in a
// dozen targets). So the arithmetic itself lives HERE, inline, and the
// exported entry is a one-line forward to it: one implementation, reachable
// from both the FFI surface and the pipeline internals, with no new
// translation unit to add to every target.
//
// raw_ffi_api.h is FROZEN by T12.0 and is deliberately NOT edited to hold
// this; this header is additive and nothing in the frozen block changes.
//
// The chroma extent comes from ceyx::yuv420::chroma_extent (the oracle), not
// from a second `(x + 1) / 2` written here — the odd-dimension case is exactly
// where a naive w/2 diverges, and one oracle means one place to be right.

#include <cstdint>

#include "ceyx_yuv420_oracle.h"

namespace ceyx {

// Bytes a decode of width x height occupies in `output_format`
// (a CeyxOutputFormat value: 0 = rgba8, 1 = yuv420).
// Returns -1 for an unknown format or a non-positive extent — same contract as
// the exported ceyx_output_format_byte_count that forwards here.
inline int64_t output_format_byte_count(int32_t output_format, int32_t width,
                                        int32_t height) {
    if (width <= 0 || height <= 0) return -1;
    const int64_t w = static_cast<int64_t>(width);
    const int64_t h = static_cast<int64_t>(height);
    switch (output_format) {
        case 0:  // kCeyxOutputFormatRgba8
            return w * h * 4;
        case 1: {  // kCeyxOutputFormatYuv420
            const int64_t cw =
                static_cast<int64_t>(ceyx::yuv420::chroma_extent(width));
            const int64_t ch =
                static_cast<int64_t>(ceyx::yuv420::chroma_extent(height));
            return w * h + 2 * cw * ch;
        }
        default:
            return -1;
    }
}

}  // namespace ceyx
