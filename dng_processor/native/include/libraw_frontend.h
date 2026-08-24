#ifndef LIBRAW_FRONTEND_H_
#define LIBRAW_FRONTEND_H_

// The single generic (non-DNG) decoder owner (spec section 2.3).
//
// This class owns exactly one LibRaw processor per decode request and calls
// open_file() + unpack(). RawSpeed3 is LibRaw's *internal* first choice inside
// that unpack() call; the project never sees a RawSpeed handle, never catches a
// RawSpeed error to dispatch load_raw itself, and never reopens the file.
//
// No LibRaw type appears in this header: callers get a plain-C view. That is
// what keeps the GPU boundary decoder-agnostic (spec section 2.5).

#include <cstdint>
#include <memory>

#include "raw_pipeline_contract.h"

// Test-only override. Production always leaves this at kAuto.
enum class RawForcedBackend {
    kAuto = 0,
    kRawSpeed3 = 1,
    kLibRawNative = 2
};

// Borrowed view over LibRaw's imgdata. Every pointer aliases the processor's
// memory and is invalidated by recycle() or destruction.
struct LibRawRawView {
    RawPlaneView plane{};
    uint32_t raw_width = 0;
    uint32_t raw_height = 0;
    uint32_t visible_left = 0;
    uint32_t visible_top = 0;
    uint32_t visible_width = 0;
    uint32_t visible_height = 0;
    uint32_t colors = 0;
    uint32_t filters = 0;
    const char* xtrans_pattern = nullptr;   // 36 bytes row-major, or null
    const float* cam_mul = nullptr;         // 4 entries
    const float* pre_mul = nullptr;         // 4 entries
    const float* cam_xyz = nullptr;         // 12 entries (LibRaw 4x3)
    const uint32_t* black_pattern = nullptr;
    uint32_t black_repeat_width = 0;
    uint32_t black_repeat_height = 0;
    uint32_t black_scalar = 0;
    uint32_t white_level = 0;
    int32_t flip = 0;
    const char* cdesc = nullptr;            // e.g. "RGBG"
};

// Ceiling handed to LibRaw before open_file (spec section 10.2).
#define kRawMaxRawMemoryMb 4096

class LibRawFrontendContext {
 public:
    LibRawFrontendContext();
    ~LibRawFrontendContext();
    LibRawFrontendContext(const LibRawFrontendContext&) = delete;
    LibRawFrontendContext& operator=(const LibRawFrontendContext&) = delete;

    void set_forced_backend(RawForcedBackend backend);

    // Steps 1-6 of the normative sequence in spec section 6.2.
    RawErrorCode open_and_unpack(const char* file_path);

    bool is_open() const;

    // Valid only while is_open(). The owning context must outlive every GPU
    // command that reads these pixels (spec section 5.1.5).
    const LibRawRawView& raw_view() const;

    const RawDecodeDiagnostics& diagnostics() const;

    // Explicit early release. Safe to call twice. Callers must ensure no GPU
    // command is still reading the borrowed pixels first.
    void recycle();

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // LIBRAW_FRONTEND_H_
