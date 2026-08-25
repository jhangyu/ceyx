#include "raw_gpu_pipeline.h"

#include <chrono>
#include <cstdio>

#include "HalideBuffer.h"
#include "dng_ffi_api.h"
#include "dng_halide_device.h"
#include "dng_pipeline_v2.h"
#include "dng_render_params.h"
#include "libraw_gpu_input_adapter.h"
#include "raw_bayer_demosaic.h"
#include "raw_contract_validate.h"
#include "raw_file_router.h"
#include "raw_render_params_builder.h"

namespace {

double nowMs() {
    // There is no dng_now_ms() in this tree; include/dng_timing_utils.h only
    // exposes an elapsed helper, so the clock read is spelled out here.
    using Clock = std::chrono::high_resolution_clock;
    return std::chrono::duration<double, std::milli>(
               Clock::now().time_since_epoch())
        .count();
}

// Returns the pool buffer on every path, including the error paths, so no
// checkout can leak (spec section 5.2.4).
class RgbaCheckoutGuard {
 public:
    explicit RgbaCheckoutGuard(size_t bytes)
        : ptr_(dng_rgba_output_acquire(bytes)), bytes_(bytes) {}
    ~RgbaCheckoutGuard() { if (ptr_) dng_rgba_output_release(ptr_); }
    RgbaCheckoutGuard(const RgbaCheckoutGuard&) = delete;
    RgbaCheckoutGuard& operator=(const RgbaCheckoutGuard&) = delete;
    uint8_t* get() const { return ptr_; }
    size_t bytes() const { return bytes_; }
    uint8_t* release() { uint8_t* p = ptr_; ptr_ = nullptr; return p; }
 private:
    uint8_t* ptr_;
    size_t bytes_;
};

RawGpuBackend currentGpuBackend() {
#if defined(__APPLE__)
    return kRawGpuBackendMetal;
#else
    return kRawGpuBackendVulkan;
#endif
}

// Single black/white scale for the fused normalize expression.
float computeInvRange(const RawGpuInput& input) {
    const uint32_t bw = input.black.repeat_width ? input.black.repeat_width : 1;
    const uint32_t bh = input.black.repeat_height ? input.black.repeat_height : 1;
    float black_max = 0.0f;
    for (uint32_t i = 0; i < bw * bh; ++i) {
        if (input.black.values[i] > black_max) black_max = input.black.values[i];
    }
    const float range = input.white_level[0] - black_max;
    return range > 0.0f ? 65535.0f / range : 1.0f;
}

RawErrorCode runBayerBranch(const RawGpuInput& input,
                            const RawDevelopParams& develop,
                            RawPipelineResult& out) {
    int32_t red_x = 0, red_y = 0;
    if (!raw_bayer_phase_from_pattern(&input.layout, &red_x, &red_y)) {
        // No RGGB fallback on purpose: a guessed phase silently mis-colours the
        // image (spec section 3.3.5).
        return kRawErrLayoutUnsupported;
    }

    const RawPlaneView& plane = input.planes[0];
    const uint32_t w = plane.width;
    const uint32_t h = plane.height;
    if (w == 0 || h == 0 || !plane.data) return kRawErrMetadataInvalid;

    // The Stage4 crop is expressed in plane coordinates, exactly like the DNG
    // route's DefaultCropArea against its Stage3 buffer
    // (src/dng_render_halide.cpp:1988-2008).
    const RawRect& crop = input.default_crop;
    if (crop.width == 0 || crop.height == 0 ||
        crop.x < 0 || crop.y < 0 ||
        static_cast<uint32_t>(crop.x) + crop.width > w ||
        static_cast<uint32_t>(crop.y) + crop.height > h) {
        return kRawErrMetadataInvalid;
    }

    // Borrowed, stride-aware wrap: no host copy (spec section 5.2.1).
    halide_dimension_t src_dims[2] = {
        {0, static_cast<int32_t>(w), 1, 0},
        {0, static_cast<int32_t>(h),
         static_cast<int32_t>(plane.row_stride_bytes / 2), 0}};
    Halide::Runtime::Buffer<const uint16_t> src_buf(
        static_cast<const uint16_t*>(plane.data), 2, src_dims);

    const uint32_t bw = input.black.repeat_width ? input.black.repeat_width : 1;
    const uint32_t bh = input.black.repeat_height ? input.black.repeat_height : 1;
    Halide::Runtime::Buffer<const float> black_buf(
        input.black.values, static_cast<int>(bw), static_cast<int>(bh));

    // Interleaved RGB16 intermediate, byte-identical in shape to the DNG
    // route's device-handoff buffer (src/dng_warp_halide.cpp:1123). Left
    // device-dirty for the handoff, so there is no GPU->host->GPU round trip
    // (spec section 5.2.3). The host allocation is never read on the success
    // path; it exists because the shared Stage4 entry accepts a host-backed
    // halide_buffer_t and Halide device-mallocs on first dispatch.
    // ponytail: host side stays resident for the whole decode, same as the DNG
    // route's Stage3 workspace; swap to a device-only allocation only if a
    // measurement shows this footprint matters.
    Halide::Runtime::Buffer<uint16_t> stage3 =
        Halide::Runtime::Buffer<uint16_t>::make_interleaved(
            static_cast<int>(w), static_cast<int>(h), 3);

    // GPU targets only upload an input whose host_dirty flag is set; without
    // these the kernel reads freshly device-malloc'd memory. Same handshake as
    // src/raw_demosaic_reference.cpp:139-141.
    src_buf.set_host_dirty();
    black_buf.set_host_dirty();
    stage3.set_host_dirty(false);

    const double gpu_t0 = nowMs();
    if (raw_bayer_demosaic(src_buf, red_x, red_y, black_buf,
                           computeInvRange(input), stage3) != 0) {
        return kRawErrKernelFailed;
    }

    RenderParams params;
    if (!buildRenderParamsFromRaw(input, develop, params)) {
        return kRawErrMetadataInvalid;
    }

    const uint32_t out_w = crop.width;
    const uint32_t out_h = crop.height;
    const size_t rgba_bytes = static_cast<size_t>(out_w) * out_h * 4;

    RgbaCheckoutGuard rgba(rgba_bytes);
    if (!rgba.get()) return kRawErrAllocationFailed;

    // Unscaled crop form: src extent == dst extent, so the shared Stage4 takes
    // the crop branch at src/dng_render_halide.cpp:1261-1269, which does the
    // raw->dim[i].min = 0 mutation itself (never set_min/translate, which would
    // trigger device_deallocate — memory.md Key Gotchas).
    if (!runRenderStage4HalideAotFromDevice(stage3.raw_buffer(),
                                            1.0f / 65535.0f,
                                            crop.x, crop.y,
                                            static_cast<int>(out_w),
                                            static_cast<int>(out_h),
                                            static_cast<int>(out_w),
                                            static_cast<int>(out_h),
                                            params, rgba.get(),
                                            /*fuse_rgba=*/true)) {
        return kRawErrKernelFailed;
    }

    out.diag.gpu_process_ms = nowMs() - gpu_t0;
    out.width = out_w;
    out.height = out_h;
    out.rgba_size = rgba_bytes;
    out.rgba_ptr = rgba.release();   // ownership moves to the caller
    return kRawSuccess;
}

}  // namespace

RawErrorCode raw_pipeline_decode_to_rgba(const RawGpuInput& input,
                                         const RawDevelopParams& develop,
                                         RawPipelineResult& out) {
    char reason[256] = {0};
    const RawErrorCode validated = raw_validate_gpu_input(&input, reason, sizeof(reason));
    raw_contract_print("RawGpuPipeline", &input, validated, reason, stdout);
    if (validated != kRawSuccess) {
        std::fprintf(stderr, "[RawPipeline] contract FAIL (%s: %s)\n",
                     raw_error_name(validated), reason);
        out.error = validated;
        return validated;
    }

    out.diag.sample_model = input.layout.sample_model;
    out.diag.cfa_repeat_width = input.layout.cfa_repeat_width;
    out.diag.cfa_repeat_height = input.layout.cfa_repeat_height;
    out.diag.gpu_backend = currentGpuBackend();

    // No CPU render fallback exists (spec section 2.6), so an unavailable GPU
    // is an explicit error rather than a slower path.
    if (!dng_halide_gpu_available()) {
        std::fprintf(stderr, "[RawPipeline] GPU capability gate FAILED: "
                             "backend=%s\n", dng_halide_gpu_backend_name());
        out.diag.gpu_backend = kRawGpuBackendNone;
        out.error = kRawErrGpuUnavailable;
        return out.error;
    }

    // Dispatch is a switch with NO fallthrough to a kernel: an unlisted class
    // can never reach the Bayer path by accident (spec section 13.1).
    const RawLayoutClass cls = raw_classify_layout(&input.layout);
    RawErrorCode rc = kRawErrLayoutUnsupported;
    switch (cls) {
        case kRawLayoutClassBayer2x2:
            rc = runBayerBranch(input, develop, out);
            break;
        case kRawLayoutClassXTrans6x6:
            // Task 12 wires the X-Trans kernel. Until then this is an explicit,
            // tested failure - never a Bayer misroute.
            std::fprintf(stderr,
                         "[RawPipeline] layout class 'xtrans6x6' has no kernel yet\n");
            rc = kRawErrLayoutUnsupported;
            break;
        default:
            std::fprintf(stderr, "[RawPipeline] layout class '%s' unsupported\n",
                         raw_layout_class_name(cls));
            rc = kRawErrLayoutUnsupported;
            break;
    }

    out.error = rc;
    return rc;
}

namespace {

RawErrorCode decodeFileImpl(const char* file_path,
                            const RawDevelopParams& develop,
                            RawForcedBackend forced,
                            RawPipelineResult& out) {
    out = RawPipelineResult{};
    const double t0 = nowMs();

    if (!file_path || file_path[0] == '\0') {
        out.error = kRawErrNullPath;
        return out.error;
    }

    RawRoute route = kRawRouteUnknown;
    const RawErrorCode probe_rc = raw_probe_file(file_path, &route);
    if (probe_rc != kRawSuccess) {
        out.error = probe_rc;
        return out.error;
    }
    if (route == kRawRouteUnknown) {
        out.error = kRawErrProbeFailed;
        return out.error;
    }

    if (route == kRawRouteDng) {
        // The DNG route is untouched: delegate to the existing public entry.
        DngResult* dng = dng_decode_and_process_sized(
            file_path, static_cast<int32_t>(develop.max_output_long_edge));
        if (!dng) {
            out.error = kRawErrAllocationFailed;
            return out.error;
        }
        out.diag.frontend = kRawFrontendDngSdk;
        out.diag.unpack_backend = kRawDecoderBackendDngSdk;
        out.diag.gpu_backend = currentGpuBackend();
        out.diag.raw_unpack_ms = dng->decode_ms;
        out.diag.gpu_process_ms = dng->process_ms;
        out.width = static_cast<uint32_t>(dng->width);
        out.height = static_cast<uint32_t>(dng->height);
        out.rgba_ptr = dng->rgba_data;
        out.rgba_size = static_cast<size_t>(dng->width) * dng->height * 4;
        out.error = dng->error_code == 0 ? kRawSuccess
                                         : static_cast<RawErrorCode>(dng->error_code);
        dng->rgba_data = nullptr;   // ownership moved; avoid the double free
        dng_free_result(dng);
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    // The context is a local of THIS function on purpose: its scope extends
    // past the GPU wait below, which is the ownership guarantee of spec 5.1.5.
    LibRawFrontendContext ctx;
    ctx.set_forced_backend(forced);
    const RawErrorCode unpack_rc = ctx.open_and_unpack(file_path);
    out.diag = ctx.diagnostics();
    out.diag.gpu_backend = currentGpuBackend();
    if (unpack_rc != kRawSuccess) {
        out.error = unpack_rc;
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    LibRawGpuInputAdapter adapter;
    RawGpuInput input{};
    RawDevelopParams effective = develop;
    char reason[256] = {0};
    const RawErrorCode build_rc =
        adapter.build(ctx, &input, &effective, reason, sizeof(reason));
    // The adapter owns the metadata half of RawDevelopParams; the develop knobs
    // stay the caller's.
    effective.max_output_long_edge = develop.max_output_long_edge;
    effective.exposure_ev = develop.exposure_ev;
    effective.tone_curve_strength = develop.tone_curve_strength;
    effective.output_space = develop.output_space;
    if (build_rc != kRawSuccess) {
        std::fprintf(stderr, "[RawPipeline] contract FAIL (%s: %s)\n",
                     raw_error_name(build_rc), reason);
        out.error = build_rc;
        out.diag.total_ms = nowMs() - t0;
        return out.error;
    }

    const RawDecodeDiagnostics unpack_diag = out.diag;
    const RawErrorCode rc = raw_pipeline_decode_to_rgba(input, effective, out);
    out.diag.frontend = unpack_diag.frontend;
    out.diag.unpack_backend = unpack_diag.unpack_backend;
    out.diag.rawspeed_flags = unpack_diag.rawspeed_flags;
    out.diag.rawspeed_warning_bits = unpack_diag.rawspeed_warning_bits;
    out.diag.raw_unpack_ms = unpack_diag.raw_unpack_ms;
    out.diag.raw_repack_bytes = unpack_diag.raw_repack_bytes;
    out.diag.total_ms = nowMs() - t0;

    // ctx destructs here, AFTER Stage4 completed and copied to host. Moving
    // this earlier would free pixels a GPU command may still be reading.
    return rc;
}

}  // namespace

RawErrorCode raw_pipeline_decode_file(const char* file_path,
                                      const RawDevelopParams& develop,
                                      RawPipelineResult& out) {
    return decodeFileImpl(file_path, develop, RawForcedBackend::kAuto, out);
}

RawErrorCode raw_pipeline_decode_file_forced(const char* file_path,
                                             const RawDevelopParams& develop,
                                             RawForcedBackend forced,
                                             RawPipelineResult& out) {
    return decodeFileImpl(file_path, develop, forced, out);
}
