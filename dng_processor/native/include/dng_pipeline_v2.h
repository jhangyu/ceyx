#pragma once

#include <cstdint>
#include <vector>

class dng_host;
class dng_negative;

struct DngPipelineV2Result {
  // Pool-backed RGB buffer from the checkout-style RgbOutputPool. Populated
  // on the fuse_rgba_output=false path (test harness / rollback); null when
  // rgba_ptr is set. Must be returned via dng_rgb_output_release when freed.
  uint8_t* rgb_ptr = nullptr;
  size_t   rgb_size = 0;
  // W7-B (P15): fused interleaved RGBA8 output (alpha=255). Populated only on
  // the Android Vulkan fused path; the buffer comes from the checkout-style
  // RGBA output pool (dng_rgba_output_acquire) and MUST be returned via
  // dng_rgba_output_release when freed. When set, rgb_ptr is null and the FFI
  // layer skips its own rgb_to_rgba pass.
  uint8_t* rgba_ptr = nullptr;
  size_t   rgba_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  double decode_ms = 0.0;
  double process_ms = 0.0;
  int32_t error_code = 0;
};

// W7-B (P15): shared, process-scoped, checkout-style pool for the returned
// RGBA output buffer. Used by the fused Stage4 bridge (acquire) and by
// the FFI layer (release on free). Hands out distinct per-decode buffers so a
// buffer handed to Dart via zero-copy NativeFinalizer is never overwritten by
// a subsequent decode. acquire() returns uninitialised storage (every byte is
// overwritten before read); release() returns true if the pointer was
// pool-owned (reclaimed) or false if the caller must delete[] it.
uint8_t* dng_rgba_output_acquire(size_t bytes);
bool dng_rgba_output_release(uint8_t* ptr);
// W5-#15: debug accessor — number of RGBA buffers currently checked out.
// Zero after every dng_free_result cycle on a correct run.
size_t dng_rgba_output_checked_out_count();

// 7.1: checkout-model RGB output pool accessors (mirrors RGBA pool above).
// Used by the fuse_rgba_output=false fallback path. release() returns true if
// the pointer was pool-owned (reclaimed); unknown pointers are absorbed with a
// diagnostic log to prevent double-free.
bool dng_rgb_output_release(uint8_t* ptr);
size_t dng_rgb_output_checked_out_count();

struct DngPipelineStage3Timing {
  double extract_stage2_ms = 0.0;
  double make_image_ms = 0.0;
  double workspace_acquire_ms = 0.0;
  double demosaic_ms = 0.0;
  double fused_demosaic_warp_ms = 0.0;
  double fast_warp_setup_ms = 0.0;
  double inject_put_ms = 0.0;
  double apply_opcode3_ms = 0.0;
  double sdk_build_ms = 0.0;
  double total_ms = 0.0;
};

bool dng_pipeline_v2_decode_to_rgb(const char *file_path,
                                   DngPipelineV2Result &result);

bool dng_pipeline_v2_warmup_for_size(int32_t width, int32_t height);

bool dng_pipeline_v2_run_stage3(dng_host &host,
                                dng_negative &negative,
                                bool use_halide_bayer,
                                DngPipelineStage3Timing *timing,
                                std::vector<uint16_t> *stage3_workspace = nullptr);
