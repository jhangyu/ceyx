/*
---
file_summary: >
  Stage4 render 橋接層。負責把 DNG SDK 的 Stage3 `dng_image` 與 color/tone/profile
  參數整理成 Halide AOT kernel 可用的 Buffer，並在 full Halide / SDK fallback
  兩條路徑間 dispatch。

notes:
  - `render_stage4_halide()` 是唯一正式入口；其餘多為資料萃取、參數建構與 full AOT 包裝。
  - Phase 8.3 後只保留正式 full Stage4 kernel 與 SDK fallback/quality-lock。
  - Round 1 P1 (commit 1d1726e) sweep 移除了 `renderHalideTimingEnabled` /
    `renderHalideTryFullEnabled` / `renderHalideForceFullKernelEnabled` /
    `renderHalideBitExactModeEnabled` 四個 env getter 與對應的 `DNG_RENDER_*`
    env，全部退役；本檔不再直接呼叫 `std::getenv`。env 分類請見
    `include/dng_pipeline_config.h` 頂部的 RouteConfig/DiagnosticConfig/
    ResearchConfig 目錄。
  - Phase 10 Sprint E 加入 RGB pool 版本的 `render_stage4_halide` 多載
    （ptr-based），供 mmap pool zero-copy 路徑直接寫入 caller-owned 記憶體。

structs:
  - name: "RenderParams"
    description: "Stage4 所需矩陣、1D/3D table、encoding table 與 SDK reference 物件快取。"
    lines: "125-163"
  - name: "CachedRenderHost"
    description: "quality-lock SDK Render 專用的 reusable host cache。"
    lines: "175-193"

functions:
  - name: "copyRenderSettings"
    description: "把既有 `dng_render` 參數複製到另一個 renderer（供 host 分離時重建）。"
    lines: "165-173"
  - name: "qualityLockRenderHostCache"
    description: "回傳 quality-lock SDK Render 專用的快取 host。"
    lines: "195-198"
  - name: "extractStage3Interleaved"
    description: "把 Stage3 `dng_image` 抽成 float interleaved buffer。"
    lines: "200-222"
  - name: "extractStage3Interleaved16"
    description: "把 Stage3 `dng_image` 抽成 uint16 interleaved buffer。"
    lines: "224-246"
  - name: "borrowStage3Interleaved16"
    description: "嘗試直接借用 Stage3 tile buffer，避免額外拷貝。"
    lines: "248-287"
  - name: "packBorrowedStage3Interleaved16"
    description: "將 borrowed Stage3 tile 重新整理成緊密 interleaved 版面。"
    lines: "289-322"
  - name: "borrowImageInterleaved8"
    description: "嘗試直接借用最終 8-bit image tile buffer。"
    lines: "324-362"
  - name: "packBorrowedInterleaved8"
    description: "將 borrowed 8-bit tile 重新整理成緊密 interleaved 輸出。"
    lines: "364-397"
  - name: "matrixToRowMajor3x3"
    description: "將 `dng_matrix` 轉成 Halide 端使用的 row-major 3x3 array。"
    lines: "399-405"
  - name: "toIdentityHueSat"
    description: "建立 identity HueSat / Look table。"
    lines: "407-413"
  - name: "toIdentityCurve"
    description: "建立 identity 1D curve。"
    lines: "415-422"
  - name: "copyHueSatMap"
    description: "將 SDK HueSatMap 轉成 Halide 期望的 planar layout。"
    lines: "424-450"
  - name: "buildRenderParams"
    description: "從 `dng_negative`/`dng_render` 與 centralized config 萃取 Stage4 所需矩陣、tone/gamma/table 與 profile map。"
    lines: "452-589"
  - name: "runRenderStage4HalideAot"
    description: "呼叫 full Stage4 Halide AOT kernel（host-side src buffer 路徑）。P15 W2：Android 改 zero-copy wrap SDK interleaved RGB 成 flat-1D src 直餵 kernel（src_rgb gather + src_row_stride_px scalar），刪除 host repack_src。"
    lines: "1286-1588"
  - name: "runRenderStage4HalideAotFromDevice"
    description: "Phase 8.2.2/8.2.3 — Stage4 AOT kernel，src 來自 GPU device buffer。P15 W2：Android 保留 copy_to_host + device_deallocate（W4-4 device-alias 範疇排除），但刪除 host-side crop() 與 dense-planar repack，改把 host-side 3D interleaved buffer 攤平成 flat-1D 直餵 kernel，crop 由 crop_l/crop_t scalar 吸收；其他平台保留原 device handoff。"
    lines: "1590-1888"
  - name: "runHalideFullOrSdkFallback (host src overload)"
    description: "執行正式 full Stage4 kernel（host src），失敗時回退 SDK render。"
    lines: "798-924"
  - name: "runHalideFullOrSdkFallback (device src overload)"
    description: "Stage3-on-device 路徑的 full Stage4 kernel + SDK fallback。"
    lines: "931-1063"
  - name: "renderHalideModeName"
    description: "列舉值轉字串。"
    lines: "1067-1074"
  - name: "render_stage4_halide (host vector overload)"
    description: "Stage4 正式入口；處理 Stage3 取得、kernel dispatch、fallback 與 timing。"
    lines: "1076-1099"
  - name: "render_stage4_halide_from_device_buffer (host vector overload)"
    description: "Phase 8.2.2 device handoff 入口；傳入 Stage3 device buffer，計算 DefaultCropArea 並呼叫 runRenderStage4HalideAotFromDevice。"
    lines: "1101-1158"
  - name: "render_stage4_halide (pool ptr overload)"
    description: "Phase 10 Sprint E RGB pool — 以 caller-supplied (mmap pool) 指標直接寫入；包裝 vector overload 並驗證 data() 未搬家。"
    lines: "1196-1244"
  - name: "render_stage4_halide_from_device_buffer (pool ptr overload)"
    description: "Phase 10 Sprint E RGB pool — device handoff + caller pool ptr 版本。"
    lines: "1246-1303"
---
*/
#include "dng_render_halide.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "HalideBuffer.h"
#include "ConcurrentDngHost.h"
#include "dng_1d_function.h"
#include "dng_1d_table.h"
#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_color_spec.h"
#include "dng_hue_sat_map.h"
#include "dng_matrix.h"
#include "dng_pixel_buffer.h"
#include "dng_pipeline_config.h"
#include "dng_rect.h"
#include "dng_render_stage4.h"
#if defined(__ANDROID__)
#include "dng_render_stage4_android.h"
#include <arm_neon.h>
#if defined(DNG_STAGE4_INTERLEAVED_SRC_PROBE)
// P14-W4-4 go/no-go probe: isolated interleaved flat-1D src-read kernel.
#include "dng_render_stage4_android_probe.h"
#endif
#endif
#include "dng_resample.h"

// NOTE: Android/Vulkan Stage4 avoids multi-dimensional RGB buffers. The Android
// AOT generator declares one dense 1D plane per channel, so runtime code repacks
// the first three RGB channels before dispatch. Other targets keep the original
// interleaved RGB input/output contract for performance.

namespace {

using Halide::Runtime::Buffer;

#if defined(__ANDROID__)
// W4-0: Android Stage4 verbose diagnostics (per-decode CPU oracle mirror +
// [Stage4-Diag] dumps) are only meaningful while bisecting codegen issues with
// an explicit diag_stage in [0..7]. The default production build keeps
// DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE = -1 (full render): in that mode we
// suppress the oracle mirror and verbose dumps so a clean decode only emits the
// [Stage4-Perf] timing instrument. constexpr lets the compiler dead-strip the
// guarded blocks entirely.
#if defined(DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE)
constexpr bool kStage4AndroidVerboseDiag = (DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE >= 0);
#else
constexpr bool kStage4AndroidVerboseDiag = false;
#endif

// W4-3 (b): the Android Stage4 generator now emits a single 2D planar output
// dst(i, c) — three contiguous channel planes (size = N*3) — dispatched as ONE
// Vulkan kernel (vs the former three dst_r/g/b kernels each recomputing the full
// pipeline). The host still repacks the planar output to interleaved RGB8 via
// the W4-1 multithreaded NEON repack_dst. Plan (a) (single interleaved 1D
// dst_rgb(j), j=i*3+c, no repack_dst) was attempted first but mis-lowered on
// Vulkan (~8 dB border/coverage corruption); (b) keeps the single-dispatch +
// compute-1x win without the interleaved-index codegen hazard.

// W4-1: persistent, non-zero-initialised scratch for the Stage4 repacks. The
// previous `std::vector<T>(N)` re-allocated and zero-filled large buffers on
// every decode (a wasted full-buffer memset plus thousands of fresh page
// faults). Stage4ScratchPool hands out reusable, uninitialised storage and only
// grows when a larger frame arrives. A mutex-guarded free-list keeps this
// race-free under the concurrent-decode convention used elsewhere in this file
// (ConcurrentDngHost). Every consumer writes each element before reading it, so
// skipping zero-init is safe.
template <typename T>
class Stage4ScratchPool {
public:
    // Checked-out lease: owns a buffer for the duration of one decode and
    // returns it to the pool on destruction.
    class Lease {
    public:
        Lease(Stage4ScratchPool* owner, std::unique_ptr<T[]> buf, size_t cap)
            : owner_(owner), buf_(std::move(buf)), cap_(cap) {}
        Lease(Lease&& other) noexcept
            : owner_(other.owner_), buf_(std::move(other.buf_)), cap_(other.cap_) {
            other.owner_ = nullptr;
        }
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        ~Lease() {
            if (owner_) {
                owner_->release(std::move(buf_), cap_);
            }
        }
        T* data() const { return buf_.get(); }

    private:
        Stage4ScratchPool* owner_;
        std::unique_ptr<T[]> buf_;
        size_t cap_;
    };

    Lease acquire(size_t count) {
        std::unique_ptr<T[]> buf;
        size_t cap = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!free_.empty() && free_.back().cap >= count) {
                buf = std::move(free_.back().buf);
                cap = free_.back().cap;
                free_.pop_back();
            }
        }
        if (!buf || cap < count) {
            buf.reset(new T[count]);  // no value-init -> no memset
            cap = count;
        }
        return Lease(this, std::move(buf), cap);
    }

private:
    struct Slot {
        std::unique_ptr<T[]> buf;
        size_t cap;
    };
    void release(std::unique_ptr<T[]> buf, size_t cap) {
        std::lock_guard<std::mutex> lock(mutex_);
        free_.push_back({std::move(buf), cap});
    }
    std::mutex mutex_;
    std::vector<Slot> free_;
};

using Stage4Scratch = Stage4ScratchPool<uint16_t>;     // interleaved->planar src
using Stage4DstScratch = Stage4ScratchPool<uint8_t>;   // planar dst -> interleaved

Stage4Scratch& stage4SrcScratch() {
    static Stage4Scratch instance;
    return instance;
}

Stage4DstScratch& stage4DstScratch() {
    static Stage4DstScratch instance;
    return instance;
}

// W4-1: row-partitioned multithreaded interleaved(u16)->planar(u16) repack.
// The transform is embarrassingly parallel across rows, so we split the row
// range into a handful of chunks and run NEON deinterleave on worker threads.
// col_step==3 && plane_step==1 hits the vld3q_u16 fast path; otherwise a scalar
// fallback handles arbitrary strides. Bit-exact with the original serial loop.
void repackInterleavedToPlanarMT(const uint16_t* src,
                                 int src_w,
                                 int src_h,
                                 int src_row_step,
                                 int src_col_step,
                                 int src_plane_step,
                                 uint16_t* planar_r,
                                 uint16_t* planar_g,
                                 uint16_t* planar_b) {
    const int plane_size = src_w * src_h;
    const bool neon_fast = (src_col_step == 3 && src_plane_step == 1);

    auto repack_rows = [&](int y_begin, int y_end) {
        if (neon_fast) {
            for (int y = y_begin; y < y_end; ++y) {
                const uint16_t* src_row = src + y * src_row_step;
                const int dst_row = y * src_w;
                int x = 0;
                for (; x + 8 <= src_w; x += 8) {
                    uint16x8x3_t rgb = vld3q_u16(src_row + x * 3);
                    vst1q_u16(planar_r + dst_row + x, rgb.val[0]);
                    vst1q_u16(planar_g + dst_row + x, rgb.val[1]);
                    vst1q_u16(planar_b + dst_row + x, rgb.val[2]);
                }
                for (; x < src_w; ++x) {
                    const int dst_idx = dst_row + x;
                    const uint16_t* px = src_row + x * 3;
                    planar_r[dst_idx] = px[0];
                    planar_g[dst_idx] = px[1];
                    planar_b[dst_idx] = px[2];
                }
            }
        } else {
            for (int y = y_begin; y < y_end; ++y) {
                const uint16_t* src_row = src + y * src_row_step;
                const int dst_row = y * src_w;
                for (int x = 0; x < src_w; ++x) {
                    const uint16_t* src_px = src_row + x * src_col_step;
                    const int dst_idx = dst_row + x;
                    planar_r[dst_idx] = src_px[0];
                    planar_g[dst_idx] = src_px[src_plane_step];
                    planar_b[dst_idx] = src_px[2 * src_plane_step];
                }
            }
        }
    };

    (void)plane_size;
    unsigned hw = std::thread::hardware_concurrency();
    int chunks = static_cast<int>(hw == 0 ? 4u : std::min<unsigned>(hw, 8u));
    if (chunks < 1) chunks = 1;
    if (chunks > src_h) chunks = std::max(1, src_h);
    if (chunks <= 1) {
        repack_rows(0, src_h);
        return;
    }
    const int rows_per_chunk = (src_h + chunks - 1) / chunks;
    std::vector<std::thread> workers;
    workers.reserve(chunks - 1);
    for (int c = 1; c < chunks; ++c) {
        const int yb = c * rows_per_chunk;
        if (yb >= src_h) break;
        const int ye = std::min(src_h, yb + rows_per_chunk);
        workers.emplace_back(repack_rows, yb, ye);
    }
    // Run the first chunk on the calling thread.
    repack_rows(0, std::min(src_h, rows_per_chunk));
    for (auto& w : workers) w.join();
}

// W4-1: row/element-partitioned multithreaded planar(u8 x3)->interleaved(u8)
// repack of the GPU output. Mirrors the NEON vst3q_u8 fast path. Bit-exact.
// W4-3 (b) keeps this: the kernel writes 3 contiguous u8 planes; this converts
// them to the caller's interleaved RGB8 (pr/pg/pb = plane0/1/2 base pointers).
void repackPlanarToInterleavedMT(const uint8_t* pr,
                                 const uint8_t* pg,
                                 const uint8_t* pb,
                                 uint8_t* dst,
                                 int total_px) {
    auto repack_range = [&](int i_begin, int i_end) {
        int i = i_begin;
        for (; i + 16 <= i_end; i += 16) {
            uint8x16_t r = vld1q_u8(pr + i);
            uint8x16_t g = vld1q_u8(pg + i);
            uint8x16_t b = vld1q_u8(pb + i);
            uint8x16x3_t rgb = {r, g, b};
            vst3q_u8(dst + i * 3, rgb);
        }
        for (; i < i_end; ++i) {
            dst[i * 3 + 0] = pr[i];
            dst[i * 3 + 1] = pg[i];
            dst[i * 3 + 2] = pb[i];
        }
    };

    unsigned hw = std::thread::hardware_concurrency();
    int chunks = static_cast<int>(hw == 0 ? 4u : std::min<unsigned>(hw, 8u));
    if (chunks < 1) chunks = 1;
    if (chunks > total_px) chunks = std::max(1, total_px);
    if (chunks <= 1) {
        repack_range(0, total_px);
        return;
    }
    // Align chunk boundaries to 16 so each worker keeps the vst3q_u8 fast path
    // and writes disjoint 48-byte-aligned dst regions.
    int base = (total_px / chunks) & ~15;
    if (base < 16) base = 16;
    std::vector<std::thread> workers;
    workers.reserve(chunks);
    int start = 0;
    while (start < total_px) {
        int end = std::min(total_px, start + base);
        // Last remaining region folds into the final worker.
        if (total_px - end < base) end = total_px;
        workers.emplace_back(repack_range, start, end);
        start = end;
    }
    for (auto& w : workers) w.join();
}
#endif

// W6-2 / TD-20: shared output size computation used by all four
// render_stage4_halide overloads.  Mirrors the MaximumSize / AspectRatio
// clamp logic that was previously duplicated verbatim at four call sites.
// Pure refactor — must remain bit-exact.
dng_point computeOutputSize(const dng_negative& negative,
                            const dng_render& renderer) {
    dng_point dst_size;
    dst_size.h = static_cast<int32>(negative.DefaultFinalWidth());
    dst_size.v = static_cast<int32>(negative.DefaultFinalHeight());
    if (renderer.MaximumSize()) {
        if (Max_uint32(static_cast<uint32>(dst_size.h),
                       static_cast<uint32>(dst_size.v)) >
            renderer.MaximumSize()) {
            const real64 ratio = negative.AspectRatio();
            if (ratio >= 1.0) {
                dst_size.h = static_cast<int32>(renderer.MaximumSize());
                dst_size.v = static_cast<int32>(Max_uint32(1,
                    Round_uint32(static_cast<real64>(dst_size.h) / ratio)));
            } else {
                dst_size.v = static_cast<int32>(renderer.MaximumSize());
                dst_size.h = static_cast<int32>(Max_uint32(1,
                    Round_uint32(static_cast<real64>(dst_size.v) * ratio)));
            }
        }
    }
    return dst_size;
}

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

void copyRenderSettings(const dng_render& src, dng_render& dst) {
    dst.SetWhiteXY(src.WhiteXY());
    dst.SetExposure(src.Exposure());
    dst.SetShadows(src.Shadows());
    dst.SetToneCurve(src.ToneCurve());
    dst.SetFinalSpace(src.FinalSpace());
    dst.SetFinalPixelType(src.FinalPixelType());
    dst.SetMaximumSize(src.MaximumSize());
}

class CachedRenderHost {
public:
    dng_host* acquire(uint32_t requested_threads) {
        if (requested_threads == 0) {
            return nullptr;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!host_ || threads_ != requested_threads) {
            host_ = std::make_unique<ConcurrentDngHost>(requested_threads);
            threads_ = requested_threads;
        }
        return host_.get();
    }

private:
    std::mutex mutex_;
    std::unique_ptr<ConcurrentDngHost> host_;
    uint32_t threads_ = 0;
};

CachedRenderHost& qualityLockRenderHostCache() {
    static CachedRenderHost cache;
    return cache;
}

void extractStage3Interleaved(dng_image* image,
                              const dng_rect& area,
                              std::vector<float>& out,
                              uint32_t& w,
                              uint32_t& h,
                              uint32_t& p) {
    w = area.W();
    h = area.H();
    p = image->Planes();
    out.resize(static_cast<size_t>(w) * h * p);

    dng_pixel_buffer buffer;
    buffer.fArea = area;
    buffer.fPlane = 0;
    buffer.fPlanes = p;
    buffer.fPixelType = ttFloat;
    buffer.fPixelSize = sizeof(float);
    buffer.fData = out.data();
    buffer.fRowStep = static_cast<int32>(w * p);
    buffer.fColStep = static_cast<int32>(p);
    buffer.fPlaneStep = 1;
    image->Get(buffer);
}

void extractStage3Interleaved16(dng_image* image,
                                const dng_rect& area,
                                std::vector<uint16_t>& out,
                                uint32_t& w,
                                uint32_t& h,
                                uint32_t& p) {
    w = area.W();
    h = area.H();
    p = image->Planes();
    out.resize(static_cast<size_t>(w) * h * p);

    dng_pixel_buffer buffer;
    buffer.fArea = area;
    buffer.fPlane = 0;
    buffer.fPlanes = p;
    buffer.fPixelType = ttShort;
    buffer.fPixelSize = sizeof(uint16_t);
    buffer.fData = out.data();
    buffer.fRowStep = static_cast<int32>(w * p);
    buffer.fColStep = static_cast<int32>(p);
    buffer.fPlaneStep = 1;
    image->Get(buffer);
}

bool borrowStage3Interleaved16(dng_image* image,
                               const dng_rect& area,
                               std::unique_ptr<dng_const_tile_buffer>& borrowedTile,
                               const uint16_t*& borrowedPtr,
                               uint32_t& w,
                               uint32_t& h,
                               uint32_t& p,
                               int32_t& rowStep,
                               int32_t& colStep,
                               int32_t& planeStep) {
    borrowedTile.reset();
    borrowedPtr = nullptr;
    rowStep = 0;
    colStep = 0;
    planeStep = 0;

    if (!image || area.IsEmpty() || image->PixelType() != ttShort) {
        return false;
    }

    w = area.W();
    h = area.H();
    p = image->Planes();
    auto tile = std::make_unique<dng_const_tile_buffer>(*image, area);

    if (tile->fPixelType != ttShort || tile->fPlanes != static_cast<int32>(p)) {
        return false;
    }

    borrowedPtr = tile->ConstPixel_uint16(area.t, area.l, 0);
    if (!borrowedPtr) {
        return false;
    }

    rowStep = tile->fRowStep;
    colStep = tile->fColStep;
    planeStep = tile->fPlaneStep;
    borrowedTile = std::move(tile);
    return true;
}

void packBorrowedStage3Interleaved16(const uint16_t* srcBase,
                                     uint32_t w,
                                     uint32_t h,
                                     uint32_t p,
                                     int32_t rowStep,
                                     int32_t colStep,
                                     int32_t planeStep,
                                     std::vector<uint16_t>& out) {
    out.resize(static_cast<size_t>(w) * h * p);
    if (!srcBase) {
        return;
    }

    const size_t rowElements = static_cast<size_t>(w) * p;
    if (colStep == static_cast<int32>(p) && planeStep == 1) {
        for (uint32_t y = 0; y < h; ++y) {
            const uint16_t* srcRow = srcBase + static_cast<int64_t>(y) * rowStep;
            uint16_t* dstRow = out.data() + static_cast<size_t>(y) * rowElements;
            std::memcpy(dstRow, srcRow, rowElements * sizeof(uint16_t));
        }
        return;
    }

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t dstBase = (static_cast<size_t>(y) * w + x) * p;
            const int64_t srcBaseIdx = static_cast<int64_t>(y) * rowStep +
                                       static_cast<int64_t>(x) * colStep;
            for (uint32_t c = 0; c < p; ++c) {
                out[dstBase + c] = srcBase[srcBaseIdx + static_cast<int64_t>(c) * planeStep];
            }
        }
    }
}

bool borrowImageInterleaved8(dng_image* image,
                             const dng_rect& area,
                             std::unique_ptr<dng_const_tile_buffer>& borrowedTile,
                             const uint8_t*& borrowedPtr,
                             uint32_t& w,
                             uint32_t& h,
                             uint32_t& p,
                             int32_t& rowStep,
                             int32_t& colStep,
                             int32_t& planeStep) {
    borrowedTile.reset();
    borrowedPtr = nullptr;
    rowStep = 0;
    colStep = 0;
    planeStep = 0;

    if (!image || area.IsEmpty() || image->PixelType() != ttByte) {
        return false;
    }

    w = area.W();
    h = area.H();
    p = image->Planes();
    auto tile = std::make_unique<dng_const_tile_buffer>(*image, area);
    if (tile->fPixelType != ttByte || tile->fPlanes != static_cast<int32>(p)) {
        return false;
    }

    borrowedPtr = tile->ConstPixel_uint8(area.t, area.l, 0);
    if (!borrowedPtr) {
        return false;
    }

    rowStep = tile->fRowStep;
    colStep = tile->fColStep;
    planeStep = tile->fPlaneStep;
    borrowedTile = std::move(tile);
    return true;
}

void packBorrowedInterleaved8(const uint8_t* srcBase,
                              uint32_t w,
                              uint32_t h,
                              uint32_t p,
                              int32_t rowStep,
                              int32_t colStep,
                              int32_t planeStep,
                              std::vector<uint8_t>& out) {
    out.resize(static_cast<size_t>(w) * h * p);
    if (!srcBase) {
        return;
    }

    const size_t rowElements = static_cast<size_t>(w) * p;
    if (colStep == static_cast<int32_t>(p) && planeStep == 1) {
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* srcRow = srcBase + static_cast<int64_t>(y) * rowStep;
            uint8_t* dstRow = out.data() + static_cast<size_t>(y) * rowElements;
            std::memcpy(dstRow, srcRow, rowElements * sizeof(uint8_t));
        }
        return;
    }

    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const size_t dstBase = (static_cast<size_t>(y) * w + x) * p;
            const int64_t srcBaseIdx = static_cast<int64_t>(y) * rowStep +
                                       static_cast<int64_t>(x) * colStep;
            for (uint32_t c = 0; c < p; ++c) {
                out[dstBase + c] = srcBase[srcBaseIdx + static_cast<int64_t>(c) * planeStep];
            }
        }
    }
}

void matrixToRowMajor3x3(const dng_matrix& m, float out9[9]) {
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out9[r * 3 + c] = static_cast<float>(m[r][c]);
        }
    }
}

void toIdentityHueSat(std::vector<float>& table) {
    // Halide Buffer(width=count, height=3) expects planar-by-component:
    // table(entry, comp) == data[entry + comp * count].
    // Use the minimum safe 2x2x2 map shape. Android Vulkan Stage4 avoids
    // runtime 2D/3D selection and always samples the 3D path.
    table = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
    };
}

void toIdentityHueSatMap(std::vector<float>& table,
                         int32_t& hue_div,
                         int32_t& sat_div,
                         int32_t& val_div,
                         int32_t& has_table) {
    toIdentityHueSat(table);
    hue_div = 2;
    sat_div = 2;
    val_div = 2;
    has_table = 0;
}

void promoteHueSatMapTo3D(std::vector<float>& table,
                          int32_t& val_div) {
    if (val_div >= 2) {
        return;
    }

    const size_t entry_count = table.size() / 3;
    std::vector<float> promoted(entry_count * 2u * 3u);
    for (int comp = 0; comp < 3; ++comp) {
        const size_t old_base = static_cast<size_t>(comp) * entry_count;
        const size_t new_base = static_cast<size_t>(comp) * entry_count * 2u;
        for (size_t i = 0; i < entry_count; ++i) {
            const float value = table[old_base + i];
            promoted[new_base + i] = value;
            promoted[new_base + entry_count + i] = value;
        }
    }
    table.swap(promoted);
    val_div = 2;
}

bool isSafeHueSatMap(const std::vector<float>& table,
                     int32_t hue_div,
                     int32_t sat_div,
                     int32_t val_div,
                     int32_t has_table) {
    if (table.size() < 3 || (table.size() % 3) != 0) {
        return false;
    }
    if (has_table == 0) {
        return true;
    }

    if (hue_div <= 0 || sat_div <= 0 || val_div <= 0) {
        return false;
    }

    const size_t entry_count = table.size() / 3;
    const size_t min_entries =
        static_cast<size_t>(hue_div) *
        static_cast<size_t>(sat_div) *
        static_cast<size_t>(val_div);
    return entry_count >= min_entries;
}

void ensureSafeHueSatMap(std::vector<float>& table,
                         int32_t& hue_div,
                         int32_t& sat_div,
                         int32_t& val_div,
                         int32_t& has_table) {
    if (!isSafeHueSatMap(table, hue_div, sat_div, val_div, has_table)) {
        toIdentityHueSatMap(table, hue_div, sat_div, val_div, has_table);
    }
    promoteHueSatMapTo3D(table, val_div);
}

void toIdentityCurve(std::vector<float>& table) {
    table.resize(dng_1d_table::kTableSize + 2);
    for (int i = 0; i <= dng_1d_table::kTableSize; ++i) {
        table[static_cast<size_t>(i)] =
            static_cast<float>(i) / static_cast<float>(dng_1d_table::kTableSize);
    }
    table[static_cast<size_t>(dng_1d_table::kTableSize + 1)] = 1.0f;
}

void copyHueSatMap(const dng_hue_sat_map& map,
                   std::vector<float>& out,
                   int32_t& hue_div,
                   int32_t& sat_div,
                   int32_t& val_div) {
    uint32_t h = 0, s = 0, v = 0;
    map.GetDivisions(h, s, v);
    hue_div = static_cast<int32_t>(h);
    sat_div = static_cast<int32_t>(s);
    val_div = static_cast<int32_t>(v);

    const uint32_t count = map.DeltasCount();
    out.resize(static_cast<size_t>(count) * 3u);
    if (count == 0) {
        return;
    }

    const auto* deltas = map.GetConstDeltas();
    // Convert SDK interleaved HSBModify array into planar layout expected by
    // Halide Buffer(count, 3): [all hue][all sat][all val].
    const size_t n = static_cast<size_t>(count);
    for (size_t i = 0; i < n; ++i) {
        out[i] = deltas[i].fHueShift;
        out[n + i] = deltas[i].fSatScale;
        out[2 * n + i] = deltas[i].fValScale;
    }
}

#if defined(__ANDROID__)
struct Stage4OracleRgb8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

uint8_t oracleLinear8(float v) {
    return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
}

float oracleTableInterp(const std::vector<float>& table, float v) {
    const int max_idx = static_cast<int>(table.size()) - 2;
    const float yv = std::clamp(v, 0.0f, 1.0f) * static_cast<float>(max_idx);
    const int idx = std::clamp(static_cast<int>(std::floor(yv)), 0, max_idx);
    const float frac = yv - static_cast<float>(idx);
    return table[static_cast<size_t>(idx)] * (1.0f - frac) +
           table[static_cast<size_t>(idx + 1)] * frac;
}

uint8_t oracleGamma8(const RenderParams& params, float v) {
    return oracleLinear8(oracleTableInterp(params.encode_gamma, v));
}

void oracleRgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    v = std::max(r, std::max(g, b));
    const float mn = std::min(r, std::min(g, b));
    const float gap = v - mn;
    const float gap_den = gap > 0.0f ? gap : 1.0f;
    float h_r = (g - b) / gap_den;
    h_r = h_r < 0.0f ? h_r + 6.0f : h_r;
    const float h_g = 2.0f + (b - r) / gap_den;
    const float h_b = 4.0f + (r - g) / gap_den;
    h = gap > 0.0f ? ((r == v) ? h_r : ((g == v) ? h_g : h_b)) : 0.0f;
    s = gap > 0.0f ? gap / v : 0.0f;
}

void oracleHsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    if (s <= 0.0f) {
        r = v;
        g = v;
        b = v;
        return;
    }

    float hh = h < 0.0f ? h + 6.0f : h;
    hh = hh >= 6.0f ? hh - 6.0f : hh;
    const int i = static_cast<int>(hh);
    const float f = hh - static_cast<float>(i);
    const float p = v * (1.0f - s);
    const float q = v * (1.0f - s * f);
    const float t = v * (1.0f - s * (1.0f - f));
    const int cc = std::clamp(i, 0, 5);
    r = (cc == 0) ? v : ((cc == 1) ? q : ((cc == 2) ? p : ((cc == 3) ? p : ((cc == 4) ? t : v))));
    g = (cc == 0) ? t : ((cc == 1) ? v : ((cc == 2) ? v : ((cc == 3) ? q : ((cc == 4) ? p : p))));
    b = (cc == 0) ? p : ((cc == 1) ? p : ((cc == 2) ? t : ((cc == 3) ? v : ((cc == 4) ? v : q))));
}

float oracleHsvMapTval(const std::vector<float>& table, int idx, int comp) {
    const int entry_count = static_cast<int>(table.size()) / 3;
    const int entry = std::clamp(idx, 0, entry_count - 1);
    return table[static_cast<size_t>(comp * entry_count + entry)];
}

void oracleSampleHsvMap(const std::vector<float>& table,
                        const std::vector<float>& encode_table,
                        const std::vector<float>& decode_table,
                        int32_t hue_div,
                        int32_t sat_div,
                        int32_t val_div,
                        int32_t has_table,
                        int32_t has_encoding,
                        float r,
                        float g,
                        float b,
                        float& out_r,
                        float& out_g,
                        float& out_b) {
    if (has_table == 0) {
        out_r = r;
        out_g = g;
        out_b = b;
        return;
    }

    float h = 0.0f, s = 0.0f, v = 0.0f;
    oracleRgbToHsv(r, g, b, h, s, v);
    const int hue_div_safe = std::max<int32_t>(hue_div, 2);
    const int sat_div_safe = std::max<int32_t>(sat_div, 2);
    const int val_div_safe = std::max<int32_t>(val_div, 1);
    const float hue_scale = static_cast<float>(hue_div_safe) * (1.0f / 6.0f);
    const float sat_scale = static_cast<float>(sat_div_safe - 1);
    const float val_scale = static_cast<float>(val_div_safe - 1);
    const int max_hue_index0 = hue_div_safe - 1;
    const int max_sat_index0 = sat_div_safe - 2;
    const int max_val_index0 = val_div_safe - 2;
    const int hue_step = sat_div_safe;
    const int val_step = hue_div_safe * sat_div_safe;
    const bool use_encode = (has_encoding != 0) && (val_div_safe >= 2);
    const float v_encoded = use_encode ? oracleTableInterp(encode_table, std::clamp(v, 0.0f, 1.0f)) : v;

    const float h_scaled = h * hue_scale;
    const float s_scaled = s * sat_scale;
    const float v_scaled = v_encoded * val_scale;
    const int h_index0_raw = static_cast<int>(std::floor(h_scaled));
    const int s_index0 = std::clamp(static_cast<int>(std::floor(s_scaled)), 0, max_sat_index0);
    const int v_index0 = std::clamp(static_cast<int>(std::floor(v_scaled)), 0, max_val_index0);
    const int h_index0 = std::clamp(h_index0_raw, 0, max_hue_index0);
    const int h_index1 = h_index0_raw >= max_hue_index0 ? 0 : h_index0 + 1;
    const float h_fract1 = h_scaled - static_cast<float>(h_index0);
    const float s_fract1 = s_scaled - static_cast<float>(s_index0);
    const float v_fract1 = v_scaled - static_cast<float>(v_index0);
    const float h_fract0 = 1.0f - h_fract1;
    const float s_fract0 = 1.0f - s_fract1;
    const float v_fract0 = 1.0f - v_fract1;

    const int base2d0 = h_index0 * hue_step + s_index0;
    const int base2d1 = h_index1 * hue_step + s_index0;
    const float hs_hue0 = h_fract0 * oracleHsvMapTval(table, base2d0, 0) + h_fract1 * oracleHsvMapTval(table, base2d1, 0);
    const float hs_sat0 = h_fract0 * oracleHsvMapTval(table, base2d0, 1) + h_fract1 * oracleHsvMapTval(table, base2d1, 1);
    const float hs_val0 = h_fract0 * oracleHsvMapTval(table, base2d0, 2) + h_fract1 * oracleHsvMapTval(table, base2d1, 2);
    const float hs_hue1 = h_fract0 * oracleHsvMapTval(table, base2d0 + 1, 0) + h_fract1 * oracleHsvMapTval(table, base2d1 + 1, 0);
    const float hs_sat1 = h_fract0 * oracleHsvMapTval(table, base2d0 + 1, 1) + h_fract1 * oracleHsvMapTval(table, base2d1 + 1, 1);
    const float hs_val1 = h_fract0 * oracleHsvMapTval(table, base2d0 + 1, 2) + h_fract1 * oracleHsvMapTval(table, base2d1 + 1, 2);
    const float hue_shift_2d = s_fract0 * hs_hue0 + s_fract1 * hs_hue1;
    const float sat_scale_2d = s_fract0 * hs_sat0 + s_fract1 * hs_sat1;
    const float val_scale_2d = s_fract0 * hs_val0 + s_fract1 * hs_val1;

    const int base3d00 = v_index0 * val_step + h_index0 * hue_step + s_index0;
    const int base3d01 = v_index0 * val_step + h_index1 * hue_step + s_index0;
    const int base3d10 = base3d00 + val_step;
    const int base3d11 = base3d01 + val_step;
    auto lerp_hv = [&](int comp, int off) {
        return v_fract0 * (h_fract0 * oracleHsvMapTval(table, base3d00 + off, comp) +
                           h_fract1 * oracleHsvMapTval(table, base3d01 + off, comp)) +
               v_fract1 * (h_fract0 * oracleHsvMapTval(table, base3d10 + off, comp) +
                           h_fract1 * oracleHsvMapTval(table, base3d11 + off, comp));
    };
    const float hue_shift_3d = s_fract0 * lerp_hv(0, 0) + s_fract1 * lerp_hv(0, 1);
    const float sat_scale_3d = s_fract0 * lerp_hv(1, 0) + s_fract1 * lerp_hv(1, 1);
    const float val_scale_3d = s_fract0 * lerp_hv(2, 0) + s_fract1 * lerp_hv(2, 1);
    const bool use_2d = val_div_safe < 2;
    const float hue_shift = use_2d ? hue_shift_2d : hue_shift_3d;
    const float sat_mult = use_2d ? sat_scale_2d : sat_scale_3d;
    const float val_mult = use_2d ? val_scale_2d : val_scale_3d;
    const float hh = h + hue_shift * (6.0f / 360.0f);
    const float ss = std::min(s * sat_mult, 1.0f);
    const float ve = std::clamp(v_encoded * val_mult, 0.0f, 1.0f);
    const float vv = use_encode ? oracleTableInterp(decode_table, ve) : ve;
    oracleHsvToRgb(hh, ss, vv, out_r, out_g, out_b);
}

void oracleRgbTone(const RenderParams& params, float r, float g, float b, float& rr, float& gg, float& bb) {
    const float tr = oracleTableInterp(params.tone_curve, r);
    const float tg = oracleTableInterp(params.tone_curve, g);
    const float tb = oracleTableInterp(params.tone_curve, b);
    const float rr1 = tr;
    const float den1 = ((r >= g) && (g > b)) ? (r - b) : 1.0f;
    const float gg1 = tb + ((tr - tb) * (g - b) / den1);
    const float bb1 = tb;
    const float bb2 = tb;
    const float gg2 = tg;
    const float den2 = ((r >= g) && !(g > b) && (b > r)) ? (b - g) : 1.0f;
    const float rr2 = gg2 + ((bb2 - gg2) * (r - g) / den2);
    const float rr3 = tr;
    const float gg3 = tg;
    const float den3 = ((r >= g) && !(g > b) && !(b > r) && (b > g)) ? (r - g) : 1.0f;
    const float bb3 = gg3 + ((rr3 - gg3) * (b - g) / den3);
    const float rr4 = tr;
    const float gg4 = tg;
    const float bb4 = tg;
    const float gg5 = tg;
    const float bb5 = tb;
    const float den5 = (!(r >= g) && (r >= b)) ? (g - b) : 1.0f;
    const float rr5 = bb5 + ((gg5 - bb5) * (r - b) / den5);
    const float bb6 = tb;
    const float rr6 = tr;
    const float den6 = (!(r >= g) && !(r >= b) && (b > g)) ? (b - r) : 1.0f;
    const float gg6 = rr6 + ((bb6 - rr6) * (g - r) / den6);
    const float gg7 = tg;
    const float rr7 = tr;
    const float den7 = (!(r >= g) && !(r >= b) && !(b > g)) ? (g - r) : 1.0f;
    const float bb7 = rr7 + ((gg7 - rr7) * (b - r) / den7);
    const bool c1 = (r >= g) && (g > b);
    const bool c2 = (r >= g) && !(g > b) && (b > r);
    const bool c3 = (r >= g) && !(g > b) && !(b > r) && (b > g);
    const bool c4 = (r >= g) && !(g > b) && !(b > r) && !(b > g);
    const bool c5 = !(r >= g) && (r >= b);
    const bool c6 = !(r >= g) && !(r >= b) && (b > g);
    rr = c1 ? rr1 : (c2 ? rr2 : (c3 ? rr3 : (c4 ? rr4 : (c5 ? rr5 : (c6 ? rr6 : rr7)))));
    gg = c1 ? gg1 : (c2 ? gg2 : (c3 ? gg3 : (c4 ? gg4 : (c5 ? gg5 : (c6 ? gg6 : gg7)))));
    bb = c1 ? bb1 : (c2 ? bb2 : (c3 ? bb3 : (c4 ? bb4 : (c5 ? bb5 : (c6 ? bb6 : bb7)))));
}

Stage4OracleRgb8 computeStage4OraclePixel(const RenderParams& params,
                                          const uint16_t* planar_r,
                                          const uint16_t* planar_g,
                                          const uint16_t* planar_b,
                                          int src_w,
                                          int src_h,
                                          int dst_w,
                                          int dst_x,
                                          int dst_y,
                                          float src_scale,
                                          int diag_stage) {
    (void)dst_w;
    const int sx = std::clamp(dst_x, 0, src_w - 1);
    const int sy = std::clamp(dst_y, 0, src_h - 1);
    const int src_idx = sy * src_w + sx;
    const float s_r = static_cast<float>(planar_r[src_idx]) * src_scale;
    const float s_g = static_cast<float>(planar_g[src_idx]) * src_scale;
    const float s_b = static_cast<float>(planar_b[src_idx]) * src_scale;
    if (diag_stage == 0) return {oracleLinear8(s_r), oracleLinear8(s_g), oracleLinear8(s_b)};

    const float wb_r = std::min(s_r, params.camera_white[0]);
    const float wb_g = std::min(s_g, params.camera_white[1]);
    const float wb_b = std::min(s_b, params.camera_white[2]);
    if (diag_stage == 1) return {oracleLinear8(wb_r), oracleLinear8(wb_g), oracleLinear8(wb_b)};

    auto matrix3 = [](const float* matrix, int col, int row) { return matrix[row * 3 + col]; };
    const float p_r0 = std::clamp(wb_r * matrix3(params.camera_to_rgb, 0, 0) +
                                  wb_g * matrix3(params.camera_to_rgb, 1, 0) +
                                  wb_b * matrix3(params.camera_to_rgb, 2, 0), 0.0f, 1.0f);
    const float p_g0 = std::clamp(wb_r * matrix3(params.camera_to_rgb, 0, 1) +
                                  wb_g * matrix3(params.camera_to_rgb, 1, 1) +
                                  wb_b * matrix3(params.camera_to_rgb, 2, 1), 0.0f, 1.0f);
    const float p_b0 = std::clamp(wb_r * matrix3(params.camera_to_rgb, 0, 2) +
                                  wb_g * matrix3(params.camera_to_rgb, 1, 2) +
                                  wb_b * matrix3(params.camera_to_rgb, 2, 2), 0.0f, 1.0f);
    if (diag_stage == 2) return {oracleLinear8(p_r0), oracleLinear8(p_g0), oracleLinear8(p_b0)};

    float p_r1 = 0.0f, p_g1 = 0.0f, p_b1 = 0.0f;
    oracleSampleHsvMap(params.huesat_table, params.huesat_encode, params.huesat_decode,
                       params.huesat_hue_div, params.huesat_sat_div, params.huesat_val_div,
                       params.huesat_has_table, params.huesat_has_encoding,
                       p_r0, p_g0, p_b0, p_r1, p_g1, p_b1);
    if (diag_stage == 3) return {oracleLinear8(p_r1), oracleLinear8(p_g1), oracleLinear8(p_b1)};

    const float e_r = oracleTableInterp(params.exp_ramp, p_r1);
    const float e_g = oracleTableInterp(params.exp_ramp, p_g1);
    const float e_b = oracleTableInterp(params.exp_ramp, p_b1);
    if (diag_stage == 4) return {oracleLinear8(e_r), oracleLinear8(e_g), oracleLinear8(e_b)};

    float p_r2 = 0.0f, p_g2 = 0.0f, p_b2 = 0.0f;
    oracleSampleHsvMap(params.look_table, params.look_encode, params.look_decode,
                       params.look_hue_div, params.look_sat_div, params.look_val_div,
                       params.look_has_table, params.look_has_encoding,
                       e_r, e_g, e_b, p_r2, p_g2, p_b2);
    if (diag_stage == 5) return {oracleLinear8(p_r2), oracleLinear8(p_g2), oracleLinear8(p_b2)};

    float t_r = 0.0f, t_g = 0.0f, t_b = 0.0f;
    oracleRgbTone(params, p_r2, p_g2, p_b2, t_r, t_g, t_b);
    if (diag_stage == 6) return {oracleLinear8(t_r), oracleLinear8(t_g), oracleLinear8(t_b)};

    const float f_r = std::clamp(t_r * matrix3(params.rgb_to_final, 0, 0) +
                                 t_g * matrix3(params.rgb_to_final, 1, 0) +
                                 t_b * matrix3(params.rgb_to_final, 2, 0), 0.0f, 1.0f);
    const float f_g = std::clamp(t_r * matrix3(params.rgb_to_final, 0, 1) +
                                 t_g * matrix3(params.rgb_to_final, 1, 1) +
                                 t_b * matrix3(params.rgb_to_final, 2, 1), 0.0f, 1.0f);
    const float f_b = std::clamp(t_r * matrix3(params.rgb_to_final, 0, 2) +
                                 t_g * matrix3(params.rgb_to_final, 1, 2) +
                                 t_b * matrix3(params.rgb_to_final, 2, 2), 0.0f, 1.0f);
    if (diag_stage == 7) return {oracleLinear8(f_r), oracleLinear8(f_g), oracleLinear8(f_b)};
    return {oracleGamma8(params, f_r), oracleGamma8(params, f_g), oracleGamma8(params, f_b)};
}

void logStage4OracleSamples(const char* label,
                            const RenderParams& params,
                            const uint16_t* planar_r,
                            const uint16_t* planar_g,
                            const uint16_t* planar_b,
                            int src_w,
                            int src_h,
                            int dst_w,
                            int dst_h,
                            float src_scale,
                            const uint8_t* gpu_dst) {
#if defined(DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE)
    if (!kStage4AndroidVerboseDiag) {
        (void)label; (void)params; (void)planar_r; (void)planar_g; (void)planar_b;
        (void)src_w; (void)src_h; (void)dst_w; (void)dst_h; (void)src_scale; (void)gpu_dst;
        return;
    }
    const int diag_stage = DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE;
    fprintf(stderr,
            "[Stage4-Oracle] %s huesat dims=%d/%d/%d has=%d encoding=%d entries=%zu\n",
            label,
            params.huesat_hue_div,
            params.huesat_sat_div,
            params.huesat_val_div,
            params.huesat_has_table,
            params.huesat_has_encoding,
            params.huesat_table.size() / 3);
    fprintf(stderr,
            "[Stage4-Oracle] %s look dims=%d/%d/%d has=%d encoding=%d entries=%zu\n",
            label,
            params.look_hue_div,
            params.look_sat_div,
            params.look_val_div,
            params.look_has_table,
            params.look_has_encoding,
            params.look_table.size() / 3);
    const int xs[4] = {0, std::min(1, dst_w - 1), dst_w / 2, std::max(0, dst_w - 2)};
    const int ys[4] = {0, 0, dst_h / 2, std::max(0, dst_h - 2)};
    for (int sample = 0; sample < 4; ++sample) {
        const int x = xs[sample];
        const int y = ys[sample];
        const Stage4OracleRgb8 cpu = computeStage4OraclePixel(params, planar_r, planar_g, planar_b,
                                                              src_w, src_h, dst_w, x, y,
                                                              src_scale, diag_stage);
        const int gpu_idx = (y * dst_w + x) * 3;
        const int gr = gpu_dst[gpu_idx + 0];
        const int gg = gpu_dst[gpu_idx + 1];
        const int gb = gpu_dst[gpu_idx + 2];
        fprintf(stderr,
                "[Stage4-Oracle] %s diag_stage=%d pixel=(%d,%d) gpu=R%d G%d B%d cpu=R%u G%u B%u diff=R%d G%d B%d\n",
                label, diag_stage, x, y, gr, gg, gb, cpu.r, cpu.g, cpu.b,
                gr - static_cast<int>(cpu.r),
                gg - static_cast<int>(cpu.g),
                gb - static_cast<int>(cpu.b));
    }
#else
    (void)label; (void)params; (void)planar_r; (void)planar_g; (void)planar_b;
    (void)src_w; (void)src_h; (void)dst_w; (void)dst_h; (void)src_scale; (void)gpu_dst;
#endif
}
#endif

bool buildRenderParams(dng_host& host,
                       dng_negative& negative,
                       const dng_render& renderer,
                       const PipelineConfig& config,
                       RenderParams& params) {
    dng_camera_profile_id profileID;
    AutoPtr<dng_color_spec> spec(negative.MakeColorSpec(profileID));
    if (!spec.Get()) {
        return false;
    }

    if (renderer.WhiteXY().IsValid()) {
        spec->SetWhiteXY(renderer.WhiteXY());
    } else if (negative.HasCameraNeutral()) {
        spec->SetWhiteXY(spec->NeutralToXY(negative.CameraNeutral()));
    } else if (negative.HasCameraWhiteXY()) {
        spec->SetWhiteXY(negative.CameraWhiteXY());
    } else {
        spec->SetWhiteXY(D55_xy_coord());
    }

    const dng_matrix camera_to_rgb =
        dng_space_ProPhoto::Get().MatrixFromPCS() * spec->CameraToPCS();
    const dng_matrix rgb_to_final =
        renderer.FinalSpace().MatrixFromPCS() * dng_space_ProPhoto::Get().MatrixToPCS();
    params.camera_to_rgb_mat = camera_to_rgb;
    params.rgb_to_final_mat = rgb_to_final;
    params.camera_white_vec = spec->CameraWhite();

    matrixToRowMajor3x3(camera_to_rgb, params.camera_to_rgb);
    matrixToRowMajor3x3(rgb_to_final, params.rgb_to_final);
    const dng_vector& cw = spec->CameraWhite();
    if (cw.Count() >= 3) {
        params.camera_white[0] = static_cast<float>(cw[0]);
        params.camera_white[1] = static_cast<float>(cw[1]);
        params.camera_white[2] = static_cast<float>(cw[2]);
    }

    const real64 exposure =
        renderer.Exposure() +
        negative.TotalBaselineExposure(profileID) -
        (std::log(negative.Stage3Gain()) / std::log(2.0));

    const real64 white = 1.0 / std::pow(2.0, std::max<real64>(0.0, exposure));
    real64 black =
        renderer.Shadows() * negative.ShadowScale() * negative.Stage3Gain() * 0.001;
    black = std::min<real64>(black, 0.99 * white);

    dng_function_exposure_ramp ramp_fn(white, black, black);
    dng_1d_table exp_table;
    exp_table.Initialize(host.Allocator(), ramp_fn);
    params.exp_table_ref.Initialize(host.Allocator(), ramp_fn);

    dng_function_exposure_tone exposure_tone(exposure);
    dng_1d_concatenate total_tone(exposure_tone, renderer.ToneCurve());
    dng_1d_table tone_table;
    tone_table.Initialize(host.Allocator(), total_tone);
    params.tone_table_ref.Initialize(host.Allocator(), total_tone);

    dng_1d_table gamma_table;
    gamma_table.Initialize(host.Allocator(), renderer.FinalSpace().GammaFunction());
    params.gamma_table_ref.Initialize(host.Allocator(), renderer.FinalSpace().GammaFunction());

    params.exp_ramp.assign(exp_table.Table(), exp_table.Table() + dng_1d_table::kTableSize + 2);
    params.tone_curve.assign(tone_table.Table(), tone_table.Table() + dng_1d_table::kTableSize + 2);
    params.encode_gamma.assign(gamma_table.Table(), gamma_table.Table() + dng_1d_table::kTableSize + 2);

    toIdentityHueSatMap(params.huesat_table,
                        params.huesat_hue_div,
                        params.huesat_sat_div,
                        params.huesat_val_div,
                        params.huesat_has_table);
    toIdentityHueSatMap(params.look_table,
                        params.look_hue_div,
                        params.look_sat_div,
                        params.look_val_div,
                        params.look_has_table);
    toIdentityCurve(params.huesat_encode);
    toIdentityCurve(params.huesat_decode);
    toIdentityCurve(params.look_encode);
    toIdentityCurve(params.look_decode);

    const dng_camera_profile* profile = negative.ProfileByID(profileID);
    if (profile) {
        AutoPtr<dng_hue_sat_map> hs_map(profile->HueSatMapForWhite(spec->WhiteXY()));
        if (hs_map.Get() && hs_map->IsValid()) {
            params.huesat_map_ref.Reset(new dng_hue_sat_map(*hs_map.Get()));
            copyHueSatMap(*hs_map.Get(),
                          params.huesat_table,
                          params.huesat_hue_div,
                          params.huesat_sat_div,
                          params.huesat_val_div);
            params.huesat_has_table = 1;
        }

        if (profile->HasLookTable() && profile->LookTable().IsValid()) {
            params.look_map_ref.Reset(new dng_hue_sat_map(profile->LookTable()));
            copyHueSatMap(profile->LookTable(),
                          params.look_table,
                          params.look_hue_div,
                          params.look_sat_div,
                          params.look_val_div);
            params.look_has_table = 1;
        }

        if (profile->HueSatMapEncoding() != encoding_Linear) {
            AutoPtr<dng_1d_table> encode_table;
            AutoPtr<dng_1d_table> decode_table;
            BuildHueSatMapEncodingTable(host.Allocator(),
                                        profile->HueSatMapEncoding(),
                                        encode_table,
                                        decode_table,
                                        false);
            if (encode_table.Get() && decode_table.Get()) {
                params.huesat_encode.assign(encode_table->Table(),
                                            encode_table->Table() + dng_1d_table::kTableSize + 2);
                params.huesat_decode.assign(decode_table->Table(),
                                            decode_table->Table() + dng_1d_table::kTableSize + 2);
                params.huesat_encode_ref.Reset(encode_table.Release());
                params.huesat_decode_ref.Reset(decode_table.Release());
                params.huesat_has_encoding = 1;
            }
        }

        if (profile->LookTableEncoding() != encoding_Linear) {
            AutoPtr<dng_1d_table> encode_table;
            AutoPtr<dng_1d_table> decode_table;
            BuildHueSatMapEncodingTable(host.Allocator(),
                                        profile->LookTableEncoding(),
                                        encode_table,
                                        decode_table,
                                        false);
            if (encode_table.Get() && decode_table.Get()) {
                params.look_encode.assign(encode_table->Table(),
                                          encode_table->Table() + dng_1d_table::kTableSize + 2);
                params.look_decode.assign(decode_table->Table(),
                                          decode_table->Table() + dng_1d_table::kTableSize + 2);
                params.look_encode_ref.Reset(encode_table.Release());
                params.look_decode_ref.Reset(decode_table.Release());
                params.look_has_encoding = 1;
            }
        }
    }

#if defined(__ANDROID__)
    ensureSafeHueSatMap(params.huesat_table,
                        params.huesat_hue_div,
                        params.huesat_sat_div,
                        params.huesat_val_div,
                        params.huesat_has_table);
    ensureSafeHueSatMap(params.look_table,
                        params.look_hue_div,
                        params.look_sat_div,
                        params.look_val_div,
                        params.look_has_table);
#endif

    return true;
}

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
                              uint8_t* dst) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || src_p < 3) {
        return false;
    }

#if defined(__ANDROID__) && defined(DNG_STAGE4_INTERLEAVED_SRC_PROBE)
    // P14-W4-4 GO/NO-GO PROBE (host path only, isolated, reversible).
    // Dispatch the interleaved flat-1D src-read kernel: zero-copy wrap the SDK
    // interleaved RGB buffer as one flat 1D plane and let the GPU gather
    // src_rgb(base + c). Output goes through the verified 2D-planar dst(i,c),
    // then the existing MT repack to interleaved. linear8 passthrough only.
    {
        // src_row_step is the interleaved row stride in u16 elements (= pixels*3
        // incl. any SDK tile padding). Per-pixel row stride therefore = /3.
        const int src_row_stride_px = src_row_step / 3;
        const int flat_len = src_row_step * src_h;  // elements in the flat plane
        Buffer<uint16_t> src_rgb_buf(const_cast<uint16_t*>(src), flat_len);
        src_rgb_buf.set_host_dirty();

        const int dst_pixel_count = dst_w * dst_h;
        Stage4DstScratch::Lease dst_lease =
            stage4DstScratch().acquire(static_cast<size_t>(dst_pixel_count) * 3);
        halide_dimension_t dst_shape[2] = {
            {0, dst_pixel_count, 1, 0},
            {0, 3, dst_pixel_count, 0},
        };
        Buffer<uint8_t> dst_planar_buf(dst_lease.data(), 2, dst_shape);
        dst_planar_buf.set_host_dirty(false);

        const int probe_result = dng_render_stage4_android_probe(
            src_rgb_buf.raw_buffer(),
            src_w,
            src_h,
            dst_w,
            src_row_stride_px,
            /*crop_l=*/0,
            /*crop_t=*/0,
            src_scale,
            dst_planar_buf.raw_buffer());
        fprintf(stderr, "[Stage4-Probe] interleaved-src kernel result=%d row_stride_px=%d flat_len=%d\n",
                probe_result, src_row_stride_px, flat_len);
        if (probe_result != 0) {
            return false;
        }
        if (dst_planar_buf.copy_to_host() != 0) return false;
        {
            const uint8_t* planar = dst_planar_buf.data();
            repackPlanarToInterleavedMT(planar, planar + dst_pixel_count,
                                        planar + 2 * dst_pixel_count, dst,
                                        dst_pixel_count);
        }
        // Reuse the existing oracle mirror to compare GPU vs CPU passthrough.
        // The oracle expects three planar src pointers; build them from the
        // interleaved src so the diff is apples-to-apples (diag_stage forced 0
        // via the probe's linear8-passthrough semantics).
        if (kStage4AndroidVerboseDiag) {
            const int sp_sz = src_w * src_h;
            Stage4Scratch::Lease mirror =
                stage4SrcScratch().acquire(static_cast<size_t>(sp_sz) * 3);
            uint16_t* pr = mirror.data();
            uint16_t* pg = pr + sp_sz;
            uint16_t* pb = pg + sp_sz;
            repackInterleavedToPlanarMT(src, src_w, src_h, src_row_step,
                                        src_col_step, src_plane_step, pr, pg, pb);
            logStage4OracleSamples("probe", params, pr, pg, pb, src_w, src_h,
                                   dst_w, dst_h, src_scale, dst);
        }
        return true;
    }
#endif

#if defined(__ANDROID__)
    // W2: zero-copy wrap the SDK interleaved RGB buffer as one flat 1D plane and
    // let the GPU gather src_rgb(base + c) directly (Gotcha #95, probe GO). The
    // former host repack_src (interleaved->3 dense planar) is eliminated — the
    // bytes uploaded are identical, only the host CPU repack is removed.
    if (kStage4AndroidVerboseDiag) {
        fprintf(stderr, "[Stage4-Diag] ANDROID interleaved direct-feed ACTIVE (runRenderStage4HalideAot): src %dx%d col_step=%d row_step=%d plane_step=%d\n",
                src_w, src_h, src_col_step, src_row_step, src_plane_step);
#if defined(DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE)
        fprintf(stderr, "[Stage4-Diag] Android AOT compile diag_stage=%d (runRenderStage4HalideAot)\n",
                DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE);
#endif
    }
    auto t0 = std::chrono::high_resolution_clock::now();
    // src_row_step is the interleaved row stride in u16 elements (= pixels*3
    // incl. any SDK tile padding). Per-pixel row stride therefore = /3.
    const int src_row_stride_px = src_row_step / 3;
    const int flat_len = src_row_step * src_h;  // elements in the flat plane
    Buffer<uint16_t> src_rgb_buf(const_cast<uint16_t*>(src), flat_len);
    auto t1 = std::chrono::high_resolution_clock::now();  // repack_src now ~0
#else
    fprintf(stderr, "[Stage4-Diag] NON-ANDROID interleaved path ACTIVE (runRenderStage4HalideAot)\n");
    halide_dimension_t src_shape[3] = {
        {0, src_w, src_col_step, 0},
        {0, src_h, src_row_step, 0},
        {0, src_p, src_plane_step, 0},
    };
    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(src), 3, src_shape);
#endif
    Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                          static_cast<int>(params.exp_ramp.size()));
    Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                           static_cast<int>(params.tone_curve.size()));
    Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                            static_cast<int>(params.encode_gamma.size()));
    Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
#if defined(__ANDROID__)
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 9);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 9);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size()));
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size()));
#else
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size() / 3), 3);
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size() / 3), 3);
#endif
    Buffer<float> hs_encode_buf(const_cast<float*>(params.huesat_encode.data()),
                                static_cast<int>(params.huesat_encode.size()));
    Buffer<float> hs_decode_buf(const_cast<float*>(params.huesat_decode.data()),
                                static_cast<int>(params.huesat_decode.size()));
    Buffer<float> look_encode_buf(const_cast<float*>(params.look_encode.data()),
                                  static_cast<int>(params.look_encode.size()));
    Buffer<float> look_decode_buf(const_cast<float*>(params.look_decode.data()),
                                  static_cast<int>(params.look_decode.size()));
#if defined(__ANDROID__)
    // W4-3 (b): single 2D planar dst output (dim0=i stride1, dim1=c stride N).
    // Persistent non-zero-init scratch holds the 3 contiguous channel planes;
    // the host repacks them to the caller's interleaved RGB8 below.
    const int dst_pixel_count = dst_w * dst_h;
    Stage4DstScratch::Lease dst_lease =
        stage4DstScratch().acquire(static_cast<size_t>(dst_pixel_count) * 3);
    halide_dimension_t dst_shape[2] = {
        {0, dst_pixel_count, 1, 0},
        {0, 3, dst_pixel_count, 0},
    };
    Buffer<uint8_t> dst_planar_buf(dst_lease.data(), 2, dst_shape);
#else
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst, dst_w, dst_h, 3);
#endif

#if defined(__ANDROID__)
    src_rgb_buf.set_host_dirty();
#else
    src_buf.set_host_dirty();
#endif
    exp_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();
    c2r_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();
    hs_encode_buf.set_host_dirty();
    hs_decode_buf.set_host_dirty();
    look_table_buf.set_host_dirty();
    look_encode_buf.set_host_dirty();
    look_decode_buf.set_host_dirty();
#if defined(__ANDROID__)
    dst_planar_buf.set_host_dirty(false);
#else
    dst_buf.set_host_dirty(false);
#endif

#if defined(__ANDROID__)
    const int32_t huesat_entry_count = static_cast<int32_t>(params.huesat_table.size() / 3);
    const int32_t look_entry_count = static_cast<int32_t>(params.look_table.size() / 3);
    const int result = dng_render_stage4_android(
        src_rgb_buf.raw_buffer(),
        src_w,
        src_h,
        dst_w,
        src_row_stride_px,
        /*crop_l=*/0,
        /*crop_t=*/0,
        src_scale,
        exp_buf.raw_buffer(),
        tone_buf.raw_buffer(),
        gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(),
        c2r_buf.raw_buffer(),
        r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(),
        hs_encode_buf.raw_buffer(),
        hs_decode_buf.raw_buffer(),
        huesat_entry_count,
        params.huesat_hue_div,
        params.huesat_sat_div,
        params.huesat_val_div,
        params.huesat_has_table,
        params.huesat_has_encoding,
        look_table_buf.raw_buffer(),
        look_encode_buf.raw_buffer(),
        look_decode_buf.raw_buffer(),
        look_entry_count,
        params.look_hue_div,
        params.look_sat_div,
        params.look_val_div,
        params.look_has_table,
        params.look_has_encoding,
        dst_planar_buf.raw_buffer());
    auto t2 = std::chrono::high_resolution_clock::now();
#else
    const int result = dng_render_stage4(src_buf.raw_buffer(),
                                         src_scale,
                                         exp_buf.raw_buffer(),
                                         tone_buf.raw_buffer(),
                                         gamma_buf.raw_buffer(),
                                         cw_buf.raw_buffer(),
                                         c2r_buf.raw_buffer(),
                                         r2f_buf.raw_buffer(),
                                         hs_table_buf.raw_buffer(),
                                         hs_encode_buf.raw_buffer(),
                                         hs_decode_buf.raw_buffer(),
                                         params.huesat_hue_div,
                                         params.huesat_sat_div,
                                         params.huesat_val_div,
                                         params.huesat_has_table,
                                         params.huesat_has_encoding,
                                         look_table_buf.raw_buffer(),
                                         look_encode_buf.raw_buffer(),
                                         look_decode_buf.raw_buffer(),
                                         params.look_hue_div,
                                         params.look_sat_div,
                                         params.look_val_div,
                                         params.look_has_table,
                                         params.look_has_encoding,
                                         dst_buf.raw_buffer());
#endif
    fprintf(stderr, "[Stage4-Diag] kernel result=%d (runRenderStage4HalideAot)\n", result);
    if (result != 0) {
        return false;
    }

#if defined(__ANDROID__)
    // W4-3 (b): one D2H copy of the planar output, then MT repack to interleaved.
    if (dst_planar_buf.copy_to_host() != 0) return false;
    auto t3 = std::chrono::high_resolution_clock::now();

    // The 3 channel planes are contiguous: plane0=base, plane1=base+N, plane2=base+2N.
    // W4-1: multithreaded NEON vld1q_u8 + vst3q_u8 (16 px/iter).
    {
        const uint8_t* planar = dst_planar_buf.data();
        const int total_px = dst_w * dst_h;
        repackPlanarToInterleavedMT(planar, planar + total_px, planar + 2 * total_px,
                                    dst, total_px);
    }
    auto t4 = std::chrono::high_resolution_clock::now();
    // W2: the production hot path no longer materialises dense planar src. The
    // oracle still needs dense planar pointers; build a verbose-only temporary
    // mirror so the diag_stage gate keeps comparing GPU vs CPU (off the hot path).
    if (kStage4AndroidVerboseDiag) {
        const int sp_sz = src_w * src_h;
        Stage4Scratch::Lease mirror =
            stage4SrcScratch().acquire(static_cast<size_t>(sp_sz) * 3);
        uint16_t* pr = mirror.data();
        uint16_t* pg = pr + sp_sz;
        uint16_t* pb = pg + sp_sz;
        repackInterleavedToPlanarMT(src, src_w, src_h, src_row_step,
                                    src_col_step, src_plane_step, pr, pg, pb);
        logStage4OracleSamples("host", params, pr, pg, pb, src_w, src_h,
                               dst_w, dst_h, src_scale, dst);
    }
    fprintf(stderr, "[Stage4-Perf] repack_src=%.1f ms dispatch=%.1f ms copy_host=%.1f ms repack_dst=%.1f ms total=%.1f ms\n",
        std::chrono::duration<double, std::milli>(t1 - t0).count(),
        std::chrono::duration<double, std::milli>(t2 - t1).count(),
        std::chrono::duration<double, std::milli>(t3 - t2).count(),
        std::chrono::duration<double, std::milli>(t4 - t3).count(),
        std::chrono::duration<double, std::milli>(t4 - t0).count());
    if (kStage4AndroidVerboseDiag) {
        fprintf(stderr, "[Stage4-Diag] Android 3-ch split dst first 6: R=%u G=%u B=%u R=%u G=%u B=%u\n",
                dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
        const int mid_idx = ((dst_h / 2) * dst_w + (dst_w / 2)) * 3;
        fprintf(stderr, "[Stage4-Diag] Android 3-ch split dst mid pixel: R=%u G=%u B=%u\n",
                dst[mid_idx], dst[mid_idx + 1], dst[mid_idx + 2]);
    }
#else
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
#endif

    return true;
}

// Phase 8.2.2 - Stage3->Stage4 device handoff.
// Android/Vulkan uses a host-side planar repack before Stage4. Other targets
// keep the original device buffer handoff path.
bool runRenderStage4HalideAotFromDevice(halide_buffer_t* stage3_device_buf,
                                         float src_scale,
                                         int crop_l,
                                         int crop_t,
                                         int dst_w,
                                         int dst_h,
                                         const RenderParams& params,
                                         uint8_t* dst) {
    if (!stage3_device_buf || stage3_device_buf->dimensions < 3 ||
        !dst || dst_w <= 0 || dst_h <= 0) {
        return false;
    }

#if defined(__ANDROID__)
    if (kStage4AndroidVerboseDiag) {
        fprintf(stderr, "[Stage4-Diag] ANDROID planar repack ACTIVE (runRenderStage4HalideAotFromDevice): dst_w=%d dst_h=%d crop_l=%d crop_t=%d\n",
                dst_w, dst_h, crop_l, crop_t);
#if defined(DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE)
        fprintf(stderr, "[Stage4-Diag] Android AOT compile diag_stage=%d (runRenderStage4HalideAotFromDevice)\n",
                DNG_RENDER_STAGE4_ANDROID_DIAG_STAGE);
#endif
    }
    auto t0_fd = std::chrono::high_resolution_clock::now();
    // Non-owning wrapper of the 3D device buffer.
    Buffer<uint16_t> src3d(*stage3_device_buf);

    // We still copy the Stage3 result to host (device-alias = W4-4 scope, kept
    // out of W2). On Metal, the serial command queue guarantees Stage3 is done.
    if (src3d.copy_to_host() != 0) {
        return false;
    }
    src3d.device_deallocate();
    auto tcopy_fd = std::chrono::high_resolution_clock::now();  // end of D2H copy

    // W2: no host repack and no host-side crop(). The host-side 3D buffer is
    // already interleaved (x, y, c) with channel stride 1; flatten it to a 1D
    // plane and feed the kernel directly. crop_l/crop_t are absorbed by the
    // kernel's index arithmetic (src_row_stride_px = dim(1).stride()/3).
    const int sw = src3d.dim(0).extent();
    const int sh = src3d.dim(1).extent();
    const int sp = src3d.dim(2).extent();
    if (sp < 3) {
        return false;
    }
    const int s_row = src3d.dim(1).stride();  // = row_stride_px * 3
    const int src_row_stride_px = s_row / 3;
    const int flat_len = s_row * sh;          // elements in the flat plane
    uint16_t* src_data = reinterpret_cast<uint16_t*>(src3d.data());
    Buffer<uint16_t> src_rgb_buf(src_data, flat_len);
    src_rgb_buf.set_host_dirty();
    auto t1_fd = std::chrono::high_resolution_clock::now();  // repack_src now ~0
    if (kStage4AndroidVerboseDiag) {
        fprintf(stderr, "[Stage4-Diag] FromDevice interleaved direct-feed: %dx%d s_row=%d row_stride_px=%d flat_len=%d crop_l=%d crop_t=%d\n",
                sw, sh, s_row, src_row_stride_px, flat_len, crop_l, crop_t);
    }
#else
    fprintf(stderr, "[Stage4-Diag] NON-ANDROID interleaved path ACTIVE (runRenderStage4HalideAotFromDevice)\n");
    // Non-owning wrapper — do NOT call set_host_dirty; data is on the GPU.
    Buffer<uint16_t> src_buf(*stage3_device_buf);
    // 8.2.3 fix: physically shift the read pointer via crop(), then mutate
    // dim[i].min back to 0 so src/dst share the [0..dst_w-1] coordinate system.
    if (crop_l > 0 || crop_t > 0) {
        src_buf.crop(0, crop_l, dst_w);
        src_buf.crop(1, crop_t, dst_h);
        halide_buffer_t* raw = src_buf.raw_buffer();
        raw->dim[0].min = 0;
        raw->dim[1].min = 0;
    }
#endif

    Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                          static_cast<int>(params.exp_ramp.size()));
    Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                           static_cast<int>(params.tone_curve.size()));
    Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                            static_cast<int>(params.encode_gamma.size()));
    Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
#if defined(__ANDROID__)
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 9);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 9);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size()));
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size()));
#else
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size() / 3), 3);
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size() / 3), 3);
#endif
    Buffer<float> hs_encode_buf(const_cast<float*>(params.huesat_encode.data()),
                                static_cast<int>(params.huesat_encode.size()));
    Buffer<float> hs_decode_buf(const_cast<float*>(params.huesat_decode.data()),
                                static_cast<int>(params.huesat_decode.size()));
    Buffer<float> look_encode_buf(const_cast<float*>(params.look_encode.data()),
                                  static_cast<int>(params.look_encode.size()));
    Buffer<float> look_decode_buf(const_cast<float*>(params.look_decode.data()),
                                  static_cast<int>(params.look_decode.size()));
#if defined(__ANDROID__)
    // W4-3 (b): single 2D planar dst output (dim0=i stride1, dim1=c stride N).
    const int dst_pixel_count = dst_w * dst_h;
    Stage4DstScratch::Lease dst_lease =
        stage4DstScratch().acquire(static_cast<size_t>(dst_pixel_count) * 3);
    halide_dimension_t dst_shape[2] = {
        {0, dst_pixel_count, 1, 0},
        {0, 3, dst_pixel_count, 0},
    };
    Buffer<uint8_t> dst_planar_buf(dst_lease.data(), 2, dst_shape);
#else
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst, dst_w, dst_h, 3);
#endif

    exp_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();
    c2r_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();
    hs_encode_buf.set_host_dirty();
    hs_decode_buf.set_host_dirty();
    look_table_buf.set_host_dirty();
    look_encode_buf.set_host_dirty();
    look_decode_buf.set_host_dirty();
#if defined(__ANDROID__)
    dst_planar_buf.set_host_dirty(false);
#else
    dst_buf.set_host_dirty(false);
#endif

#if defined(__ANDROID__)
    const int32_t huesat_entry_count = static_cast<int32_t>(params.huesat_table.size() / 3);
    const int32_t look_entry_count = static_cast<int32_t>(params.look_table.size() / 3);
    const int result = dng_render_stage4_android(
        src_rgb_buf.raw_buffer(),
        sw,
        sh,
        dst_w,
        src_row_stride_px,
        crop_l,
        crop_t,
        src_scale,
        exp_buf.raw_buffer(),
        tone_buf.raw_buffer(),
        gamma_buf.raw_buffer(),
        cw_buf.raw_buffer(),
        c2r_buf.raw_buffer(),
        r2f_buf.raw_buffer(),
        hs_table_buf.raw_buffer(),
        hs_encode_buf.raw_buffer(),
        hs_decode_buf.raw_buffer(),
        huesat_entry_count,
        params.huesat_hue_div,
        params.huesat_sat_div,
        params.huesat_val_div,
        params.huesat_has_table,
        params.huesat_has_encoding,
        look_table_buf.raw_buffer(),
        look_encode_buf.raw_buffer(),
        look_decode_buf.raw_buffer(),
        look_entry_count,
        params.look_hue_div,
        params.look_sat_div,
        params.look_val_div,
        params.look_has_table,
        params.look_has_encoding,
        dst_planar_buf.raw_buffer());
    auto t2_fd = std::chrono::high_resolution_clock::now();
#else
    const int result = dng_render_stage4(src_buf.raw_buffer(),
                                         src_scale,
                                         exp_buf.raw_buffer(),
                                         tone_buf.raw_buffer(),
                                         gamma_buf.raw_buffer(),
                                         cw_buf.raw_buffer(),
                                         c2r_buf.raw_buffer(),
                                         r2f_buf.raw_buffer(),
                                         hs_table_buf.raw_buffer(),
                                         hs_encode_buf.raw_buffer(),
                                         hs_decode_buf.raw_buffer(),
                                         params.huesat_hue_div,
                                         params.huesat_sat_div,
                                         params.huesat_val_div,
                                         params.huesat_has_table,
                                         params.huesat_has_encoding,
                                         look_table_buf.raw_buffer(),
                                         look_encode_buf.raw_buffer(),
                                         look_decode_buf.raw_buffer(),
                                         params.look_hue_div,
                                         params.look_sat_div,
                                         params.look_val_div,
                                         params.look_has_table,
                                         params.look_has_encoding,
                                         dst_buf.raw_buffer());
#endif
    fprintf(stderr, "[Stage4-Diag] kernel result=%d (runRenderStage4HalideAotFromDevice)\n", result);
    if (result != 0) {
        return false;
    }

#if defined(__ANDROID__)
    // W4-3 (b): one D2H copy of the planar output, then MT repack to interleaved.
    if (dst_planar_buf.copy_to_host() != 0) return false;
    auto t3_fd = std::chrono::high_resolution_clock::now();

    {
        const uint8_t* planar = dst_planar_buf.data();
        const int total_px = dst_w * dst_h;
        repackPlanarToInterleavedMT(planar, planar + total_px, planar + 2 * total_px,
                                    dst, total_px);
    }
    auto t4_fd = std::chrono::high_resolution_clock::now();
    // W2: build a verbose-only dense-planar mirror over the cropped region so the
    // oracle (which indexes dst_x/dst_y against a src_w-stride dense buffer with
    // no crop) compares apples-to-apples against the GPU's crop-scalar gather.
    if (kStage4AndroidVerboseDiag) {
        const int sp_sz = dst_w * dst_h;
        Stage4Scratch::Lease mirror =
            stage4SrcScratch().acquire(static_cast<size_t>(sp_sz) * 3);
        uint16_t* pr = mirror.data();
        uint16_t* pg = pr + sp_sz;
        uint16_t* pb = pg + sp_sz;
        for (int y = 0; y < dst_h; ++y) {
            const int syc = std::clamp(y + crop_t, 0, sh - 1);
            for (int x = 0; x < dst_w; ++x) {
                const int sxc = std::clamp(x + crop_l, 0, sw - 1);
                const long base = (static_cast<long>(syc) * src_row_stride_px + sxc) * 3;
                const int di = y * dst_w + x;
                pr[di] = src_data[base + 0];
                pg[di] = src_data[base + 1];
                pb[di] = src_data[base + 2];
            }
        }
        logStage4OracleSamples("from_device", params, pr, pg, pb,
                               dst_w, dst_h, dst_w, dst_h, src_scale, dst);
    }
    // W2: repack_src now measures only the (eliminated) host repack — the flatten
    // wrap is O(1), so it prints ~0. The kept Stage3->host D2H copy (W4-4 scope,
    // not removed in W2) is broken out separately as d2h_src so the parser's
    // repack_src= key still resolves and the total stays comparable to baseline.
    fprintf(stderr, "[Stage4-Perf] FromDevice: repack_src=%.1f ms d2h_src=%.1f ms dispatch=%.1f ms copy_host=%.1f ms repack_dst=%.1f ms total=%.1f ms\n",
        std::chrono::duration<double, std::milli>(t1_fd - tcopy_fd).count(),
        std::chrono::duration<double, std::milli>(tcopy_fd - t0_fd).count(),
        std::chrono::duration<double, std::milli>(t2_fd - t1_fd).count(),
        std::chrono::duration<double, std::milli>(t3_fd - t2_fd).count(),
        std::chrono::duration<double, std::milli>(t4_fd - t3_fd).count(),
        std::chrono::duration<double, std::milli>(t4_fd - t0_fd).count());
    if (kStage4AndroidVerboseDiag) {
        fprintf(stderr, "[Stage4-Diag] Android 3-ch split FromDevice dst first 6: R=%u G=%u B=%u R=%u G=%u B=%u\n",
                dst[0], dst[1], dst[2], dst[3], dst[4], dst[5]);
    }
#else
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
#endif

    return true;
}

bool runHalideFullOrSdkFallback(dng_host& host,
                                dng_negative& negative,
                                dng_image* stage3,
                                const dng_render& renderer,
                                const PipelineConfig& config,
                                std::vector<uint8_t>& out_rgb,
                                uint32_t& out_w,
                                uint32_t& out_h) {
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    // Use resize instead of assign(N, 0): the Halide kernel overwrites every byte,
    // so the zero-fill is wasted work. resize() is a no-op when out_rgb is already sized
    // by the caller (which avoids the 250ms first-touch page-fault cost on a 72MB buffer).
    const size_t needed_out_size = static_cast<size_t>(out_w) * out_h * 3;
    if (out_rgb.size() != needed_out_size) {
        out_rgb.resize(needed_out_size);
    }

    dng_rect src_area = negative.DefaultCropArea();

    dng_image* source_image = stage3;
    dng_rect source_area = src_area;
    AutoPtr<dng_image> resized_stage3;
    const bool need_resample = src_area.Size() != dst_size;
    if (need_resample) {
        resized_stage3.Reset(host.Make_dng_image(dst_size, stage3->Planes(), stage3->PixelType()));
        if (!resized_stage3.Get()) {
            return false;
        }
        ResampleImage(host,
                      *stage3,
                      *resized_stage3.Get(),
                      src_area,
                      resized_stage3->Bounds(),
                      dng_resample_bicubic::Get());
        source_image = resized_stage3.Get();
        source_area = resized_stage3->Bounds();
    }

    uint32_t src_w = source_area.W();
    uint32_t src_h = source_area.H();
    uint32_t src_p = source_image->Planes();
    std::vector<uint16_t> stage3_data16;
    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    const bool can_use_u16_stage3 = source_image->PixelType() == ttShort &&
                                    source_image->PixelRange() != 0;
    if (can_use_u16_stage3) {
        const uint16_t* stage3_u16_ptr = nullptr;
        std::unique_ptr<dng_const_tile_buffer> stage3_borrowed_tile;
        int32_t stage3_row_step = static_cast<int32_t>(src_w * src_p);
        int32_t stage3_col_step = static_cast<int32_t>(src_p);
        int32_t stage3_plane_step = 1;
        if (borrowStage3Interleaved16(source_image,
                                      source_area,
                                      stage3_borrowed_tile,
                                      stage3_u16_ptr,
                                      src_w,
                                      src_h,
                                      src_p,
                                      stage3_row_step,
                                      stage3_col_step,
                                      stage3_plane_step)) {
            // Borrowed path keeps source strides and avoids O(WxH) repack.
        } else {
            extractStage3Interleaved16(source_image, source_area, stage3_data16, src_w, src_h, src_p);
            stage3_u16_ptr = stage3_data16.data();
        }
        const float src_scale = 1.0f / static_cast<float>(source_image->PixelRange());
        const bool render_ok = runRenderStage4HalideAot(stage3_u16_ptr,
                                                         static_cast<int>(src_w),
                                                         static_cast<int>(src_h),
                                                         static_cast<int>(src_p),
                                                         stage3_row_step,
                                                         stage3_col_step,
                                                         stage3_plane_step,
                                                         src_scale,
                                                         static_cast<int>(out_w),
                                                         static_cast<int>(out_h),
                                                         params,
                                                         out_rgb.data());
        if (render_ok) {
            return true;
        }
    }

    AutoPtr<dng_image> final_image(const_cast<dng_render&>(renderer).Render());
    if (!final_image.Get()) {
        return false;
    }
    out_w = final_image->Width();
    out_h = final_image->Height();
    out_rgb.resize(static_cast<size_t>(out_w) * out_h * 3);
    dng_pixel_buffer buffer;
    buffer.fArea = final_image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = 3;
    buffer.fPixelType = ttByte;
    buffer.fPixelSize = 1;
    buffer.fData = out_rgb.data();
    buffer.fRowStep = static_cast<int32>(out_w * 3);
    buffer.fColStep = 3;
    buffer.fPlaneStep = 1;
    final_image->Get(buffer);
    return true;
}

// Pool-backed overload: out_rgb_ptr/out_rgb_size supplied by RgbOutputPool.
// No resize, no page-fault cost.  SDK fallback path (renderer.Render()) is
// not available here since we cannot safely write into an arbitrary pointer
// whose capacity may not match the resized output; return false in that case
// so the caller falls back to the vector overload.
bool runHalideFullOrSdkFallback(dng_host& host,
                                dng_negative& negative,
                                dng_image* stage3,
                                const dng_render& renderer,
                                const PipelineConfig& config,
                                uint8_t* out_rgb_ptr,
                                size_t out_rgb_size,
                                uint32_t& out_w,
                                uint32_t& out_h) {
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    // Pool path: no resize, no page fault.
    const size_t needed_out_size = static_cast<size_t>(out_w) * out_h * 3;
    if (out_rgb_size < needed_out_size || !out_rgb_ptr) {
        return false;
    }

    dng_rect src_area = negative.DefaultCropArea();

    dng_image* source_image = stage3;
    dng_rect source_area = src_area;
    AutoPtr<dng_image> resized_stage3;
    const bool need_resample = src_area.Size() != dst_size;
    if (need_resample) {
        resized_stage3.Reset(host.Make_dng_image(dst_size, stage3->Planes(), stage3->PixelType()));
        if (!resized_stage3.Get()) {
            return false;
        }
        ResampleImage(host,
                      *stage3,
                      *resized_stage3.Get(),
                      src_area,
                      resized_stage3->Bounds(),
                      dng_resample_bicubic::Get());
        source_image = resized_stage3.Get();
        source_area = resized_stage3->Bounds();
    }

    uint32_t src_w = source_area.W();
    uint32_t src_h = source_area.H();
    uint32_t src_p = source_image->Planes();
    std::vector<uint16_t> stage3_data16;
    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    const bool can_use_u16_stage3 = source_image->PixelType() == ttShort &&
                                    source_image->PixelRange() != 0;
    if (can_use_u16_stage3) {
        const uint16_t* stage3_u16_ptr = nullptr;
        std::unique_ptr<dng_const_tile_buffer> stage3_borrowed_tile;
        int32_t stage3_row_step   = static_cast<int32_t>(src_w * src_p);
        int32_t stage3_col_step   = static_cast<int32_t>(src_p);
        int32_t stage3_plane_step = 1;
        if (borrowStage3Interleaved16(source_image,
                                      source_area,
                                      stage3_borrowed_tile,
                                      stage3_u16_ptr,
                                      src_w,
                                      src_h,
                                      src_p,
                                      stage3_row_step,
                                      stage3_col_step,
                                      stage3_plane_step)) {
            // Borrowed path.
        } else {
            extractStage3Interleaved16(source_image, source_area, stage3_data16, src_w, src_h, src_p);
            stage3_u16_ptr = stage3_data16.data();
        }
        const float src_scale = 1.0f / static_cast<float>(source_image->PixelRange());
        const bool render_ok = runRenderStage4HalideAot(stage3_u16_ptr,
                                                         static_cast<int>(src_w),
                                                         static_cast<int>(src_h),
                                                         static_cast<int>(src_p),
                                                         stage3_row_step,
                                                         stage3_col_step,
                                                         stage3_plane_step,
                                                         src_scale,
                                                         static_cast<int>(out_w),
                                                         static_cast<int>(out_h),
                                                         params,
                                                         out_rgb_ptr);
        if (render_ok) {
            return true;
        }
    }

    // SDK fallback with pool pointer: use fData directly.
    AutoPtr<dng_image> final_image(const_cast<dng_render&>(renderer).Render());
    if (!final_image.Get()) {
        return false;
    }
    const uint32_t fw = static_cast<uint32_t>(final_image->Width());
    const uint32_t fh = static_cast<uint32_t>(final_image->Height());
    const size_t fallback_size = static_cast<size_t>(fw) * fh * 3;
    if (fallback_size > out_rgb_size) {
        // Pool buffer too small for fallback size — should not happen in practice.
        return false;
    }
    out_w = fw;
    out_h = fh;
    dng_pixel_buffer buffer;
    buffer.fArea = final_image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = 3;
    buffer.fPixelType = ttByte;
    buffer.fPixelSize = 1;
    buffer.fData = out_rgb_ptr;
    buffer.fRowStep = static_cast<int32>(out_w * 3);
    buffer.fColStep = 3;
    buffer.fPlaneStep = 1;
    final_image->Get(buffer);
    return true;
}

}  // namespace

const char* renderHalideModeName(RenderHalideMode mode) {
    switch (mode) {
        case RenderHalideMode::SDK: return "sdk";
        case RenderHalideMode::HALIDE_METAL: return "halide-metal";
        case RenderHalideMode::HALIDE_GPU: return "HALIDE_GPU";
        case RenderHalideMode::AUTO: return "auto";
    }
    return "unknown";
}

bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          const PipelineConfig& config,
                          std::vector<uint8_t>& out_rgb,
                          uint32_t& out_w,
                          uint32_t& out_h) {
    if (mode == RenderHalideMode::SDK) {
        return false;
    }

    dng_image* stage3 = const_cast<dng_image*>(negative.Stage3Image());
    if (!stage3 || stage3->Planes() < 3) {
        return false;
    }

    return runHalideFullOrSdkFallback(host,
                                      negative,
                                      stage3,
                                      renderer,
                                      config,
                                      out_rgb,
                                      out_w,
                                      out_h);
}

bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              const PipelineConfig& config,
                                              std::vector<uint8_t>& out_rgb,
                                              uint32_t& out_w,
                                              uint32_t& out_h) {
    if (!stage3_device_buf) {
        return false;
    }

    // W6-2 / TD-20: shared dst_size helper (mirrors runHalideFullOrSdkFallback).
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    // Device handoff only works without resample.
    const dng_rect src_area = negative.DefaultCropArea();
    if (src_area.W() != out_w || src_area.H() != out_h) {
        return false;
    }

    const size_t needed = static_cast<size_t>(out_w) * out_h * 3;
    if (out_rgb.size() != needed) {
        out_rgb.resize(needed);
    }

    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    return runRenderStage4HalideAotFromDevice(
        stage3_device_buf, src_scale,
        static_cast<int>(src_area.l), static_cast<int>(src_area.t),
        static_cast<int>(out_w), static_cast<int>(out_h),
        params, out_rgb.data());
}

// ---------------------------------------------------------------------------
// Pool-backed overloads (Phase 10 Sprint E-rgb-pool)
// These wrap the vector overloads using a non-owning std::vector that points
// at caller-allocated (mmap pool) memory.  The vector's allocator is bypassed
// by constructing it from an existing pointer; instead we build a minimal
// wrapper that satisfies the internal API without re-allocating.
//
// Implementation note: we cannot construct a std::vector from a raw pointer
// without ownership.  The cleanest zero-copy path is to delegate to the
// internal helpers directly.  For render_stage4_halide we call
// runHalideFullOrSdkFallback via a local vector that we swap-in after the
// render call.  But that still page-faults on resize.
//
// Instead, we use a thin shim std::vector that wraps the pool pointer:
// since std::vector does not support non-owning pointers, we reuse the
// existing vector overloads by passing a vector whose .data() already points
// to the pool memory AND whose .size() already equals the needed bytes.
// We achieve this by assigning into a local vector<uint8_t> that is
// move-constructed from the pool-sized data, then after the call we confirm
// the pointer has not changed (render path only ever writes, never reallocates
// when size is correct).
//
// Actually the simplest correct approach: build a local std::vector backed by
// a custom allocator.  That's too complex.  The real simplest approach:
// replicate the render body but pass out_rgb_ptr directly.  We factor out the
// core logic via an internal ptr-based helper defined in the same TU, which
// the vector overloads already call indirectly.  We add ptr-based internal
// variants of runHalideFullOrSdkFallback and the device-handoff path.
//
// For this sprint we use the straightforward solution: allocate a
// std::vector<uint8_t> that is already pre-sized to exactly the pool buffer
// size, call the existing overload, then verify data() has not moved (it
// won't as long as size == needed on entry).  The vector resize inside the
// callee is a no-op (size already correct), so there is zero page-fault cost.
// ---------------------------------------------------------------------------

bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
                          const PipelineConfig& config,
                          uint8_t* out_rgb_ptr,
                          size_t out_rgb_size,
                          uint32_t& out_w,
                          uint32_t& out_h) {
    if (mode == RenderHalideMode::SDK) {
        return false;
    }

    dng_image* stage3 = const_cast<dng_image*>(negative.Stage3Image());
    if (!stage3 || stage3->Planes() < 3) {
        return false;
    }

    // Wrap the pool pointer in a non-owning vector alias.
    // We build a std::vector by aliasing the pool memory: since the pool
    // buffer is already exactly out_rgb_size bytes, the resize() inside
    // runHalideFullOrSdkFallback is a no-op (no realloc, no page fault).
    // We use the assign-from-pointer trick: default-construct, then
    // swap with a vector that owns a copy — but that still copies.
    //
    // Correct zero-copy path: use a std::vector whose internal buffer IS the
    // pool pointer.  This requires calling runHalideFullOrSdkFallback with a
    // vector whose .data() == out_rgb_ptr.  We achieve this by calling the
    // existing vector overload with a vector pre-sized to out_rgb_size that
    // wraps the pool memory via placement:
    //   1. Construct a vector of size out_rgb_size (may page-fault on first
    //      call — but on the first FFI call the pool has already been
    //      committed by the caller before passing here, so this is a no-op
    //      resize).
    //   2. memcpy of 72 MB defeats the purpose.
    //
    // The ONLY zero-copy approach without changing the internal helper
    // signature is to use a custom allocator or to duplicate the helper.
    // We duplicate the minimal part: re-use the existing implementation path
    // by routing through runHalideFullOrSdkFallback directly.
    // Since it lives in the same TU (anonymous namespace) we call it here.
    return runHalideFullOrSdkFallback(host,
                                      negative,
                                      stage3,
                                      renderer,
                                      config,
                                      out_rgb_ptr,
                                      out_rgb_size,
                                      out_w,
                                      out_h);
}

bool render_stage4_halide_from_device_buffer(dng_host& host,
                                              dng_negative& negative,
                                              const dng_render& renderer,
                                              halide_buffer_t* stage3_device_buf,
                                              float src_scale,
                                              const PipelineConfig& config,
                                              uint8_t* out_rgb_ptr,
                                              size_t out_rgb_size,
                                              uint32_t& out_w,
                                              uint32_t& out_h) {
    if (!stage3_device_buf) {
        return false;
    }

    // W6-2 / TD-20: shared dst_size helper.
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);

    const dng_rect src_area = negative.DefaultCropArea();
    if (src_area.W() != out_w || src_area.H() != out_h) {
        return false;
    }

    // Pool path: buffer is already committed — no resize, no page fault.
    const size_t needed = static_cast<size_t>(out_w) * out_h * 3;
    if (out_rgb_size < needed || !out_rgb_ptr) {
        return false;
    }

    RenderParams params;
    if (!buildRenderParams(host, negative, renderer, config, params)) {
        return false;
    }

    return runRenderStage4HalideAotFromDevice(
        stage3_device_buf, src_scale,
        static_cast<int>(src_area.l), static_cast<int>(src_area.t),
        static_cast<int>(out_w), static_cast<int>(out_h),
        params, out_rgb_ptr);
}
