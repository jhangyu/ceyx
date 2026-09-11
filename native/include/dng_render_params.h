#ifndef DNG_RENDER_PARAMS_H_
#define DNG_RENDER_PARAMS_H_

// Extracted from dng_render_halide.cpp's anonymous namespace so a second
// translation unit (the LibRaw parameter builder) can reach the SHARED Stage4
// core. Nothing here is new logic: the struct body and the function signatures
// are byte-for-byte what dng_render_halide.cpp already defined.
//
// This is one shared Stage4 core, not a per-decoder copy (spec section 13.1):
// exactly two run* declarations exist below, and both frontends call them.

#include <cstdint>
#include <vector>

#include "dng_1d_table.h"
#include "dng_auto_ptr.h"
#include "dng_host.h"
#include "dng_hue_sat_map.h"
#include "dng_matrix.h"
#include "dng_negative.h"
#include "dng_render.h"

#include "dng_pipeline_config.h"

struct halide_buffer_t;

// Transcribed unchanged from dng_render_halide.cpp:369-407.
struct RenderParams {
    dng_vector camera_white_vec;
    dng_matrix camera_to_rgb_mat;
    dng_matrix rgb_to_final_mat;
    dng_1d_table exp_table_ref;
    dng_1d_table tone_table_ref;
    dng_1d_table gamma_table_ref;
    AutoPtr<dng_hue_sat_map> huesat_map_ref;
    AutoPtr<dng_hue_sat_map> look_map_ref;
    AutoPtr<dng_1d_table> huesat_encode_ref;
    AutoPtr<dng_1d_table> huesat_decode_ref;
    AutoPtr<dng_1d_table> look_encode_ref;
    AutoPtr<dng_1d_table> look_decode_ref;

    float camera_white[3] = {1.0f, 1.0f, 1.0f};
    float camera_to_rgb[9] = {};
    float rgb_to_final[9] = {};
    std::vector<float> exp_ramp;
    std::vector<float> tone_curve;
    std::vector<float> encode_gamma;

    std::vector<float> huesat_table;
    std::vector<float> huesat_encode;
    std::vector<float> huesat_decode;
    int32_t huesat_hue_div = 0;
    int32_t huesat_sat_div = 0;
    int32_t huesat_val_div = 0;
    int32_t huesat_has_table = 0;
    int32_t huesat_has_encoding = 0;

    std::vector<float> look_table;
    std::vector<float> look_encode;
    std::vector<float> look_decode;
    int32_t look_hue_div = 0;
    int32_t look_sat_div = 0;
    int32_t look_val_div = 0;
    int32_t look_has_table = 0;
    int32_t look_has_encoding = 0;
};

// DNG-side parameter builder (unchanged behaviour).
// Signature transcribed from dng_render_halide.cpp:768-772.
bool buildRenderParams(dng_host& host,
                       dng_negative& negative,
                       const dng_render& renderer,
                       const PipelineConfig& config,
                       RenderParams& params);

// Mutex rework (plan Task 4): per-decode state container. Forward declaration
// only — the definition lives in src/pipeline/decode_context.h, which is not on
// every consumer's include path. A null ctx means "no decode frame" (the plain
// dng_host harness paths); see the two Stage-4 entry points below.
struct DecodeContext;

// Productionization plan section 1.6 — Stage4 failure-reason channel
// (Task 2 fix cycle 1, review blocker B-1).
//
// The two Stage4 runners below return bool, which cannot distinguish an
// overlap refusal from a kernel/copy failure. The FFI layer needs exactly that
// distinction to report -402 (kCeyxOrientErrOverlap) versus -403
// (kCeyxOrientErrKernel), so the runners publish the reason here instead of
// the caller having to guess from a bare false.
//
// STALENESS CONTRACT (tightened in fix cycle 2, review blocker B-2):
// READ THIS VALUE ONLY AFTER AN EXPLICIT dngRenderStage4ResetFailureReason()
// FOLLOWED BY THE CALL UNDER TEST. The reader must own the reset.
//
// Why the runners resetting themselves is NOT sufficient, which is exactly what
// B-2 caught: a consumer such as the FFI layer reads this after a WHOLE
// PIPELINE, not after a runner. Plenty of phase-3 failures return before any
// runner executes at all — parse failure, stage3 failure, OpcodeList2 failure,
// a dst-too-small refusal on the RAW route. On those paths no runner runs, so
// nothing resets, and a reader that trusted a bare "last reason" would pick up
// a kKernel or kOverlap left by a PREVIOUS decode on the same thread and
// overwrite the real upstream error with -403/-402. That would, among other
// things, silently destroy the dst-too-small bounded-retry contract by turning
// a recoverable size error into a kernel failure.
//
// The value is thread-local (decodes overlap under a shared_lock since Task 8,
// so a plain static would let concurrent decodes clobber each other). The two
// runners additionally reset on entry, which is retained as defence in depth
// for direct runner callers — but it is NOT the contract, and no consumer may
// rely on it in place of its own reset.
//
// After a true return the value is always kNone.
//
// kNone on a false return is legitimate and means "failed for a reason that is
// not orientation-specific" (bad arguments, scratch allocation, SDK fallback
// refusal). The FFI layer must keep its existing generic error for that case
// and must not translate kNone into -402 or -403.
//
// Deliberately declared HERE and not in ceyx_orient.h: Phase 4 (Task 9)
// removes ceyx_orient.cpp from the shipping dylib, and this channel has to
// outlive that deletion. It also carries no PipelineConfig dependency —
// PipelineConfig is env/route settings and this is per-call data.
enum class Stage4FailureReason : int32_t {
    kNone    = 0,
    kOverlap = 1,  // src/dst alias — FFI maps to kCeyxOrientErrOverlap (-402)
    kKernel  = 2,  // kernel or copy_to_host returned non-zero — maps to -403
};

// Clears this thread's Stage4 failure reason to kNone. Call this IMMEDIATELY
// BEFORE the operation whose failure you intend to classify — the reader owns
// the reset (see the staleness contract above). Cheap: one thread-local store.
void dngRenderStage4ResetFailureReason();

// Reads the current thread's Stage4 failure reason. Meaningful only when paired
// with a preceding dngRenderStage4ResetFailureReason(); without that pairing the
// value may belong to an earlier call on this thread.
Stage4FailureReason dngRenderStage4LastFailureReason();

// THE shared Stage4 core. Plain buffers + RenderParams, no decoder state.
// Signature transcribed from dng_render_halide.cpp:932-944.
//
// WP1 phase 3: the fuse-mode parameter (and the RGB8 strip path it gated)
// is deleted — every caller hard-passed the fused arm; RGB8 output no
// longer exists.
bool runRenderStage4HalideAot(const uint16_t* src,
                              int src_w,
                              int src_h,
                              int src_p,
                              int src_row_step,
                              int src_col_step,
                              int src_plane_step,
                              float src_scale,
                              int dst_w,
                              int dst_h,
                              const RenderParams& params,
                              uint8_t* dst,
                              DecodeContext* ctx = nullptr,
                              // Productionization plan section 1.3 (Task 2).
                              // EXIF 1..8; anything else is treated as 1. dst_w/dst_h
                              // stay the UNORIENTED extent; this runner is the ONLY
                              // place that converts them to the oriented extent.
                              int32_t exif_orientation = 1);

// Device-handoff form. Signature transcribed from dng_render_halide.cpp:1182-1192
// (note the crop_l/crop_t/src_w/src_h parameters the plan placeholder omitted).
bool runRenderStage4HalideAotFromDevice(halide_buffer_t* stage3_device_buf,
                                        float src_scale,
                                        int crop_l,
                                        int crop_t,
                                        int src_w,
                                        int src_h,
                                        int dst_w,
                                        int dst_h,
                                        const RenderParams& params,
                                        uint8_t* dst,
                                        DecodeContext* ctx = nullptr,
                                        // Productionization plan section 1.3 (Task 2).
                                        // See runRenderStage4HalideAot above.
                                        int32_t exif_orientation = 1);

// Lead-assigned scope addition (2026-09-11, plan §6.2 item 1 — C4
// device->host copy bracket). Owned by impl-2-sonnet alongside
// dng_render_halide.cpp's runRenderStage4HalideAotFromDevice, which is the
// sole writer. Reset to 0.0 at entry of every call to that function; set to
// the measured elapsed ms only when its copy_to_host actually runs.
// impl-4-sonnet reads this from raw_gpu_pipeline.cpp right after the Stage4
// call to fill out.timing.device_to_host_copy_ms.
double runRenderStage4LastDeviceToHostCopyMilliseconds();

// Needed by the LibRaw builder so "identity" is explicit, never uninitialised
// (spec section 7.1.4). Signatures transcribed from dng_render_halide.cpp:663-667
// and :731 — note all four ints are by reference; the helper itself sets the
// divisions to 2/2/2 and has_table to 0.
void toIdentityHueSatMap(std::vector<float>& table,
                         int32_t& hue_div,
                         int32_t& sat_div,
                         int32_t& val_div,
                         int32_t& has_table);
void toIdentityCurve(std::vector<float>& table);

#endif  // DNG_RENDER_PARAMS_H_
