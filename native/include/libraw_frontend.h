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
    // LibRaw's own colorimetry: `float rgb_cam[3][4]` (libraw_types.h:811),
    // 3 output sRGB rows x 4 camera-channel columns, i.e. linear-sRGB-FROM-
    // camera. Built by cam_xyz_coeff (utils_dcraw.cpp:285-312), which
    // row-normalises the forward sRGB->camera matrix so that it maps (1,1,1)
    // to (1,1,1,...) and only THEN pseudo-inverts it -- so rgb_cam is
    // white-preserving by construction, with no bolted-on rescale. That is why
    // the adapter prefers it over Invert(cam_xyz): normalise-then-invert is not
    // the same matrix as invert-then-normalise.
    const float* rgb_cam = nullptr;         // 12 entries (LibRaw 3x4)
    // color.as_shot_wb_applied (libraw_types.h:829). Non-zero means LibRaw has
    // ALREADY applied the as-shot white balance to the pixels it handed us
    // (Nikon sRAW / small-raw). Multiplying by cam_mul again would double-apply
    // the correction and swing the render hard magenta. Plumbed here in Stage 1;
    // the guard that consumes it is Stage 2.
    uint32_t as_shot_wb_applied = 0;
    // internal_output_params.raw_color (libraw_types.h:139), copied into
    // rawdata.ioparams at unpack (unpack.cpp:508). 1 means LibRaw found NO
    // colour data for this camera and left rgb_cam at the identity
    // (identify.cpp:509 seeds it to 1; a matched camera clears it to 0).
    // Defaults to 1 -- the conservative value: a view that never populated this
    // field must not be read as "rgb_cam is trustworthy".
    uint32_t raw_color = 1;
    const uint32_t* black_pattern = nullptr;
    uint32_t black_repeat_width = 0;
    uint32_t black_repeat_height = 0;
    uint32_t black_scalar = 0;
    const uint32_t* black_channel = nullptr;   // color.cblack[0..3], per-CFA-channel black
    uint32_t white_level = 0;
    int32_t flip = 0;
    const char* cdesc = nullptr;            // e.g. "RGBG"
    // 1 for a single-sample mosaic (rawdata.raw_image), 3 for full-colour
    // interleaved pixels (rawdata.color3_image, the Foveon X3F case). The
    // adapter reads this instead of inferring component count from `colors`,
    // which describes the sensor, not the buffer.
    uint32_t components_per_pixel = 1;
    // Round 2 Task 2.4 (vendor-curve double-apply investigation). 1 when
    // imgdata.color.curve is non-identity at the point this view is built
    // (immediately after unpack(), before any LibRaw postprocessing call --
    // this project never invokes LibRaw's CPU render path in production,
    // spec section 13.1). Measured, not assumed: color.curve is LibRaw-internal
    // and NOT populated by every vendor decoder at unpack() time (grep of
    // third_party/libraw/src/decoders shows only decoders_libraw_dcrdefs.cpp's
    // Nikon Coolscan path and decoders_libraw.cpp's Kodak path write it during
    // load_raw; RAF and X3F do not touch it there). So a 0 here for RAF/X3F is
    // NOT evidence against a vendor-curve effect -- it only rules out THIS
    // specific mechanism (utils_libraw.cpp:341's is_curve_linear() pattern)
    // for whichever decoder actually ran. See
    // native/scripts/tmp/round2_vendor_curve.md for the measured verdict.
    uint32_t vendor_curve_applied = 0;
};

// Pure, LibRaw-instance-free predicate mirroring LibRaw::is_curve_linear()
// (utils_libraw.cpp:338-343): returns true iff curve[i] != i for ANY i in
// [0, count). Factored out here so it is unit-testable with a synthetic
// array (test_libraw_frontend.cpp) without needing a live LibRaw processor
// -- the same shape as raw_frontend_pixels_live_in_raw_image above. A null
// curve returns false (nothing to detect).
bool raw_frontend_vendor_curve_applied(const uint16_t* curve, size_t count);

// Ceiling handed to LibRaw before open_file (spec section 10.2).
#define kRawMaxRawMemoryMb 4096

// Acceptance gate for "the U16 mosaic pixels really live in rawdata.raw_image"
// (round-3 review finding F2). Mirrors LibRaw's own allocation predicate in
// third_party/libraw/src/decoders/unpack.cpp:382. Declared here (not a static
// helper) because no in-repo corpus file exercises the sRAW / legacy alias
// case, so the truth table is what the test can actually cover.
//
// Arguments are the raw values of imgdata.rawdata.raw_alloc,
// imgdata.rawdata.raw_image, imgdata.idata.filters and imgdata.idata.colors.
bool raw_frontend_pixels_live_in_raw_image(const void* raw_alloc,
                                           const void* raw_image,
                                           uint32_t filters,
                                           uint32_t colors);

// Acceptance gate for "the pixels really live in rawdata.color3_image": the
// Foveon X3F case, where LibRaw::x3f_load_raw() allocates raw_alloc, points
// color3_image at it, and never assigns raw_image at all
// (third_party/libraw/src/x3f/x3f_parse_process.cpp:588-640).
//
// A SIBLING of the function above, not a widening of it. That one's clause 3 is
// deliberately a contrapositive guarding LibRaw's sRAW/legacy decoders, which
// alias raw_image onto the 4-component imgdata.image; relaxing it to tolerate a
// null raw_image would re-open exactly that case.
//
// True only when: color3_image is non-null, filters == 0 (a CFA word with a
// 3-component buffer is a self-contradictory imgdata state), colors == 3
// (colors == 4 is the color4_image/Quattro case this phase does not handle),
// and any recorded raw store IS this buffer.
//
// Arguments are the raw values of imgdata.rawdata.raw_alloc,
// imgdata.rawdata.color3_image, imgdata.idata.filters and imgdata.idata.colors.
bool raw_frontend_pixels_live_in_color3_image(const void* raw_alloc,
                                              const void* color3_image,
                                              uint32_t filters,
                                              uint32_t colors);

class LibRawFrontendContext {
 public:
    LibRawFrontendContext();
    ~LibRawFrontendContext();
    LibRawFrontendContext(const LibRawFrontendContext&) = delete;
    LibRawFrontendContext& operator=(const LibRawFrontendContext&) = delete;

    void set_forced_backend(RawForcedBackend backend);

    // Cancellation poll (spec section 10.4). A non-zero return from `poll`
    // aborts the decode; open_and_unpack then reports kRawErrKernelFailed with
    // the processor recycled and nothing borrowed.
    //
    // Deliberately a plain function pointer rather than the GPU layer's
    // RawCancelToken: that type lives in raw_gpu_pipeline.h, which already
    // includes THIS header, so sharing the struct would close an include cycle.
    // A bare pointer also keeps the promise that the poll adds no lock to the
    // hot path.
    void set_cancel_hook(int (*poll)(void* user_data), void* user_data);

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
