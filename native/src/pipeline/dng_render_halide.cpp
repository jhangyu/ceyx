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
    description: "呼叫 full Stage4 Halide AOT kernel（host-side src buffer 路徑）。P15 W2：Android 改 zero-copy wrap SDK interleaved RGB 成 flat-1D src 直餵 kernel（src_rgb gather + src_row_stride_px scalar），刪除 host repack_src。G2（Round 2）：Android kernel 直接輸出 interleaved RGBA8（Probe-A 驗證構造），退役 planar D2H + repackPlanarToRGBAMT/repackPlanarToInterleavedMT + RepackThreadPool；legacy RGB8 caller 走 stripRgbaToRgbMT 去 alpha。"
    lines: "1286-1588"
  - name: "runRenderStage4HalideAotFromDevice"
    description: "Phase 8.2.2/8.2.3 — Stage4 AOT kernel，src 來自 GPU device buffer。G1（Round 2）：Android 改為 zero-copy device alias——用 shallow halide_buffer_t 把 producer 的 Vulkan device allocation 以 offset-0 flat-1D view 直餵 kernel（crop 由 crop_l/crop_t scalar 吸收），刪除 W4-4 時代的全幀 copy_to_host + device_deallocate + 重上傳；成功後經原始 struct halide_device_free（owner destructor 因 device==0 不會 double-free），失敗路徑保留 device data 供 fallback。其他平台保留原 Metal device handoff。G2（Round 2）：Android dst 改 kernel 直出 interleaved RGBA8，fused 路徑 D2H 直落 caller RGBA buffer（零 host repack）。"
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
  - name: "LazyZeroBuf"
    description: "T9 (Gotcha #62)：Android S4 prewarm dummy buffer 的 mmap MAP_ANON lazy-zero RAII 容器（calloc fallback），消除 ~410MB eager memset。"
    lines: "1632-1655"
  - name: "dng_render_stage4_prewarm_for_size"
    description: "W7-E Android-only：以 actual-size LazyZeroBuf identity-params dummy 預建 Stage4 render Vulkan pipeline；per-size cache、[Warmup] s4 markers。macOS no-op。"
    lines: "1659-1749"
---
*/
#include "dng_render_halide.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

// W7b: which Stage4 AOT kernel variant this host bridge talks to. The 3-channel
// split kernel (dng_render_stage4_split) exists to dodge the Halide v21
// SPIR-V Tuple R==G bug, so every Vulkan target needs it — Android and Windows
// today. Mirrors CMake's DNG_STAGE4_SPLIT_KERNEL option (CMakeLists.txt:460).
// Guards that are about the *kernel variant* (buffer shapes, RGBA scratch, D2H,
// call sites) key off this macro; guards that are genuinely platform-specific
// (arm_neon.h, mmap, Android-only prewarm) stay on __ANDROID__.
#if defined(__ANDROID__) || defined(_WIN32) || defined(__linux__)
#define DNG_STAGE4_SPLIT_KERNEL 1
#endif

#if defined(__ANDROID__)
// W7-E S4 prewarm per-size cache + crash-attribution markers (Android-only).
// T9: mmap MAP_ANON lazy-zero dummy buffers (Gotcha #62).
#include <sys/mman.h>
#include <unordered_set>
#endif

#include "HalideBuffer.h"
#include "concurrent_dng_host.h"
#include "dng_1d_function.h"
#include "dng_1d_table.h"
#include "dng_camera_profile.h"
#include "dng_color_space.h"
#include "dng_color_spec.h"
#include "dng_hue_sat_map.h"
#include "dng_matrix.h"
#include "dng_pixel_buffer.h"
#include "dng_pipeline_config.h"
#include "dng_render_params.h"
#include "dng_rect.h"
#include "dng_render_stage4.h"
#if defined(DNG_STAGE4_SPLIT_KERNEL)
#include "dng_render_stage4_split.h"
#else
// R2 sized decode: pre-average (Variant A) scaled Stage4 kernel. macOS/Metal
// only — the split (Android/Vulkan) branch has no scaled AOT and refuses
// sized requests instead (see runRenderStage4HalideAotFromDevice).
#include "dng_render_stage4_scaled_preavg.h"
#endif
#if defined(__ANDROID__)
#include <arm_neon.h>
#endif
#include "dng_resample.h"

// NOTE: G2 (Round 2) — the Android/Vulkan Stage4 kernel now writes interleaved
// RGBA8 directly (same dst layout as macOS; construct re-verified 0-error on
// Halide v21 Vulkan by the G2 pre-check Probe A). The planar-output workaround
// (planar D2H + repackPlanarToRGBAMT / repackPlanarToInterleavedMT +
// RepackThreadPool) is retired. Legacy RGB8 callers get a host alpha-strip
// pass (mirrors the macOS strip path); the production fused path writes the
// caller's RGBA buffer with no host repack at all.

namespace {

using Halide::Runtime::Buffer;

// Q2 (Round 2 perf diag): runtime gate for this file's per-decode diagnostic
// stderr prints (and the chrono::now() calls that exist only to time them).
// Mirrors the pipelineVerbose() lazy-cache pattern in dng_pipeline.cpp
// (same DNG_PIPELINE_VERBOSE env; a separate TU-local static since that
// function lives in dng_pipeline.cpp's own anonymous namespace and isn't
// exported via a header).
bool pipelineVerbose() {
    static const bool cached = []() {
        const char *v = std::getenv("DNG_PIPELINE_VERBOSE");
        return v && v[0] != '0';
    }();
    return cached;
}

#if defined(DNG_STAGE4_SPLIT_KERNEL)
// G2: the split Stage4 generator emits interleaved RGBA8 directly (macOS
// layout; Probe-A verified). The only remaining host pass is the alpha-strip
// for legacy RGB8 callers below (stripRgbaToRgbMT); the fused production path
// D2Hs straight into the caller's RGBA buffer.

// W4-1: persistent, non-zero-initialised scratch for the Stage4 strip path. The
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

using Stage4DstScratch = Stage4ScratchPool<uint8_t>;   // RGBA8 kernel output for legacy RGB8 callers

Stage4DstScratch& stage4DstScratch() {
    static Stage4DstScratch instance;
    return instance;
}

// G2: RGBA8 -> RGB8 alpha-strip for legacy RGB8 callers (vector overloads /
// matrix test path). The production fused path (fuse_rgba=true) needs no host
// pass at all — the kernel's D2H lands directly in the caller's RGBA buffer.
// On Android: NEON vld4q_u8 + vst3q_u8 (16 px/iter). W7b: other split-kernel
// platforms (Windows) take the plain scalar loop — deliberately boring, no SSE
// intrinsics. Range-split across a few short-lived
// std::threads. Ad-hoc spawn is deliberate: this runs at most once per legacy
// decode (~0.5 ms spawn cost vs a ~168 MB memory pass). The former persistent
// RepackThreadPool (Q3b) existed for the retired per-decode planar repacks
// (repackPlanarToRGBAMT / repackPlanarToInterleavedMT) and was removed with
// them — which also removes the Q3b exit-teardown hazard it had to work
// around (bionic FORTIFY abort on destroyed cv/mutex with parked workers).
void stripRgbaToRgbMT(const uint8_t* rgba, uint8_t* rgb, int total_px) {
    auto strip_range = [rgba, rgb](int i_begin, int i_end) {
        int i = i_begin;
#if defined(__ANDROID__)
        for (; i + 16 <= i_end; i += 16) {
            uint8x16x4_t v = vld4q_u8(rgba + static_cast<size_t>(i) * 4);
            uint8x16x3_t o = {v.val[0], v.val[1], v.val[2]};
            vst3q_u8(rgb + static_cast<size_t>(i) * 3, o);
        }
#endif
        for (; i < i_end; ++i) {
            rgb[static_cast<size_t>(i) * 3 + 0] = rgba[static_cast<size_t>(i) * 4 + 0];
            rgb[static_cast<size_t>(i) * 3 + 1] = rgba[static_cast<size_t>(i) * 4 + 1];
            rgb[static_cast<size_t>(i) * 3 + 2] = rgba[static_cast<size_t>(i) * 4 + 2];
        }
    };

    const unsigned hw = std::thread::hardware_concurrency();
    const int workers = static_cast<int>(hw == 0 ? 4u : std::min(hw, 8u));
    // Chunks 16-aligned so every worker keeps the NEON fast path and writes
    // disjoint dst regions. Small frames: single-threaded.
    const int base = workers > 1 ? (total_px / workers) & ~15 : 0;
    if (workers <= 1 || base < 16) {
        strip_range(0, total_px);
        return;
    }
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers));
    int start = 0;
    for (int w = 0; w < workers && start < total_px; ++w) {
        const int end = (w == workers - 1) ? total_px
                                           : std::min(total_px, start + base);
        threads.emplace_back(strip_range, start, end);
        start = end;
    }
    for (auto& t : threads) {
        t.join();
    }
}
#endif

#if !defined(DNG_STAGE4_SPLIT_KERNEL)
// W7 (M-11): persistent scratch for the macOS RGBA→RGB strip path (legacy
// vector callers). Avoids a fresh 96MB allocation + first-touch page-fault per
// decode (same Gotcha #62 pattern). Grow-only; safe because all decodes are
// serialized by pipelineSingleFlightMutex in dng_pipeline.cpp.
// ponytail: simple static, upgrade to Stage4ScratchPool pattern if concurrent decode needed.
static std::unique_ptr<uint8_t[]> s_rgba_strip_scratch;
static size_t s_rgba_strip_scratch_cap = 0;

static uint8_t* acquireRgbaStripScratch(size_t bytes) {
    if (bytes > s_rgba_strip_scratch_cap) {
        s_rgba_strip_scratch.reset(new (std::nothrow) uint8_t[bytes]);
        s_rgba_strip_scratch_cap = s_rgba_strip_scratch ? bytes : 0;
    }
    return s_rgba_strip_scratch.get();
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


// struct RenderParams moved to include/dng_render_params.h (Phase 17 Task 8,
// extract-only: no member added, removed, reordered or re-defaulted).

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

}  // namespace
// T8 extract-only: moved out of the anonymous namespace so the LibRaw builder
// can make "identity" explicit (spec 7.1.4). Body unchanged.

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

namespace {  // T8: reopen the file-local namespace.

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

}  // namespace
// T8 extract-only: moved out of the anonymous namespace. Body unchanged.

void toIdentityCurve(std::vector<float>& table) {
    table.resize(dng_1d_table::kTableSize + 2);
    for (int i = 0; i <= dng_1d_table::kTableSize; ++i) {
        table[static_cast<size_t>(i)] =
            static_cast<float>(i) / static_cast<float>(dng_1d_table::kTableSize);
    }
    table[static_cast<size_t>(dng_1d_table::kTableSize + 1)] = 1.0f;
}

namespace {  // T8: reopen the file-local namespace.

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

}  // namespace
// T8 extract-only: buildRenderParams, runRenderStage4HalideAot and
// runRenderStage4HalideAotFromDevice are one contiguous block moved out of the
// anonymous namespace so the LibRaw frontend can reach THE shared Stage4 core.
// Bodies unchanged; the `fuse_rgba` default arguments now live on the
// declarations in include/dng_render_params.h (a default may be given only once
// per TU). The namespace reopens after the last of the three.

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

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // Gotcha #92: the split kernel reads the hue/sat maps as one flat,
    // component-major table and always samples the 3D path, so the shape must be
    // sanitized + promoted here. The Tuple kernel takes an (n,3) 2D buffer and
    // does its own 2D/3D selection — kernel-variant concern, not platform.
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
                              uint8_t* dst,
                              bool fuse_rgba) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0 || src_p < 3) {
        return false;
    }

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // W2: zero-copy wrap the SDK interleaved RGB buffer as one flat 1D plane and
    // let the GPU gather src_rgb(base + c) directly (Gotcha #95, probe GO). The
    // former host repack_src (interleaved->3 dense planar) is eliminated — the
    // bytes uploaded are identical, only the host CPU repack is removed.
    // Q2: these timepoints only feed the verbose-gated [Stage4-Perf] print
    // below; skip the clock reads entirely when not verbose.
    const bool verbose_timing = pipelineVerbose();
    auto t0 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};
    // src_row_step is the interleaved row stride in u16 elements (= pixels*3
    // incl. any SDK tile padding). Per-pixel row stride therefore = /3.
    const int src_row_stride_px = src_row_step / 3;
    const int flat_len = src_row_step * src_h;  // elements in the flat plane
    Buffer<uint16_t> src_rgb_buf(const_cast<uint16_t*>(src), flat_len);
    auto t1 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};  // repack_src now ~0
#else
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] non-split-kernel interleaved path ACTIVE (runRenderStage4HalideAot)\n");
    }
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
#if defined(DNG_STAGE4_SPLIT_KERNEL)
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
#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // G2: the kernel writes interleaved RGBA8 directly (macOS layout; Probe-A
    // verified). fuse_rgba callers hand in an RGBA8 (W*H*4) buffer — the D2H
    // lands there with zero host repack. Legacy RGB8 callers render into
    // persistent RGBA scratch, then stripRgbaToRgbMT drops alpha below.
    uint8_t* dst_rgba = dst;
    std::optional<Stage4DstScratch::Lease> dst_lease;
    if (!fuse_rgba) {
        dst_lease.emplace(stage4DstScratch().acquire(
            static_cast<size_t>(dst_w) * dst_h * 4));
        dst_rgba = dst_lease->data();
    }
    Buffer<uint8_t> dst_rgba_buf =
        Buffer<uint8_t>::make_interleaved(dst_rgba, dst_w, dst_h, 4);
#else
    // W7 (M-11): macOS generator outputs RGBA8 (4 channels, alpha=255 in-kernel).
    // When fuse_rgba, the caller's buffer is already RGBA8 (W*H*4) — write directly.
    // When !fuse_rgba (legacy vector callers), use persistent scratch for the
    // 4-channel kernel output, then strip alpha to RGB8 in the caller's buffer.
    uint8_t* dst_rgba = dst;
    if (!fuse_rgba) {
        dst_rgba = acquireRgbaStripScratch(static_cast<size_t>(dst_w) * dst_h * 4);
        if (!dst_rgba) return false;
    }
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst_rgba, dst_w, dst_h, 4);
#endif

#if defined(DNG_STAGE4_SPLIT_KERNEL)
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
#if defined(DNG_STAGE4_SPLIT_KERNEL)
    dst_rgba_buf.set_host_dirty(false);
#else
    dst_buf.set_host_dirty(false);
#endif

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    const int32_t huesat_entry_count = static_cast<int32_t>(params.huesat_table.size() / 3);
    const int32_t look_entry_count = static_cast<int32_t>(params.look_table.size() / 3);
    // G2: dst_width scalar retired — the RGBA dst buffer extents carry the
    // output geometry.
    const int result = dng_render_stage4_split(
        src_rgb_buf.raw_buffer(),
        src_w,
        src_h,
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
        dst_rgba_buf.raw_buffer());
    auto t2 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};
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
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] kernel result=%d (runRenderStage4HalideAot)\n", result);
    }
    if (result != 0) {
        return false;
    }

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // G2: one D2H copy of the interleaved RGBA output. On the fused path this
    // lands directly in the caller's RGBA buffer — no host repack at all.
    if (dst_rgba_buf.copy_to_host() != 0) return false;
    auto t3 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};

    // Legacy RGB8 callers: strip alpha from the RGBA scratch (mirrors the
    // macOS strip path, NEON + short-lived threads).
    if (!fuse_rgba) {
        stripRgbaToRgbMT(dst_rgba, dst, dst_w * dst_h);
    }
    auto t4 = verbose_timing ? std::chrono::high_resolution_clock::now()
                             : std::chrono::high_resolution_clock::time_point{};
    if (verbose_timing) {
        fprintf(stderr, "[Stage4-Perf] repack_src=%.1f ms dispatch=%.1f ms copy_host=%.1f ms repack_dst=%.1f ms total=%.1f ms\n",
            std::chrono::duration<double, std::milli>(t1 - t0).count(),
            std::chrono::duration<double, std::milli>(t2 - t1).count(),
            std::chrono::duration<double, std::milli>(t3 - t2).count(),
            std::chrono::duration<double, std::milli>(t4 - t3).count(),
            std::chrono::duration<double, std::milli>(t4 - t0).count());
    }
#else
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    // W7 (M-11): strip alpha when legacy caller expects RGB8.
    if (!fuse_rgba) {
        const size_t total_px = static_cast<size_t>(dst_w) * dst_h;
        for (size_t i = 0; i < total_px; ++i) {
            dst[i * 3 + 0] = dst_rgba[i * 4 + 0];
            dst[i * 3 + 1] = dst_rgba[i * 4 + 1];
            dst[i * 3 + 2] = dst_rgba[i * 4 + 2];
        }
    }
#endif

    return true;
}

// Phase 8.2.2 - Stage3->Stage4 device handoff.
// Android/Vulkan uses a host-side planar repack before Stage4. Other targets
// keep the original device buffer handoff path.
// R2 sized decode: src_w/src_h are the SOURCE crop extent (DefaultCropArea),
// dst_w/dst_h the requested OUTPUT extent. When they differ the pre-average
// scaled kernel runs; when equal the path is bit-identical to before.
//
// The distinction is load-bearing: the unscaled path crops the source to the
// DESTINATION extent, so feeding a small dst_w/dst_h without this split would
// silently emit a top-left CROP at exactly the requested size rather than a
// downscale.
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
                                         bool fuse_rgba) {
    if (!stage3_device_buf || stage3_device_buf->dimensions < 3 ||
        !dst || dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) {
        return false;
    }

    const bool scaled = (src_w != dst_w || src_h != dst_h);
#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // Android/Vulkan has no scaled AOT (Gotcha #93/#96 unverifiable without a
    // real device). Refuse rather than crop; the caller falls back to the host
    // path, which resamples correctly via the SDK.
    if (scaled) {
        return false;
    }
#endif

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // Q2: these timepoints only feed the verbose-gated [Stage4-Perf] print
    // below; skip the clock reads entirely when not verbose. The real work
    // (copy_to_host / device_deallocate below) always runs regardless.
    const bool verbose_timing_fd = pipelineVerbose();
    auto t0_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};
    // G1 (Round 2): zero-copy device alias — bind the producer's Vulkan device
    // allocation directly as the kernel's flat-1D src, eliminating the W4-4-era
    // full-frame copy_to_host + device_deallocate + re-upload round trip.
    // Halide v21's Vulkan runtime is synchronous per dispatch (vkQueueWaitIdle
    // inside halide_vulkan_run), so the Stage3/Stage2 producer kernel has
    // already finished — no extra sync needed. Evidence + patch plan:
    // docs/logs/2026-07-04/Task_g1_vulkan_handoff_spike.md.
    const int sw = stage3_device_buf->dim[0].extent;
    const int sh = stage3_device_buf->dim[1].extent;
    const int sp = stage3_device_buf->dim[2].extent;
    if (sp < 3) {
        return false;
    }
    const int s_row = stage3_device_buf->dim[1].stride;  // = row_stride_px * 3
    // Row-dense interleaved contract (same assumption the old host flatten made,
    // now guarded explicitly).
    if (s_row <= 0 || (s_row % 3) != 0 || sh <= 0) {
        return false;
    }
    const int src_row_stride_px = s_row / 3;
    const int flat_len = s_row * sh;          // elements in the flat plane
    // Shallow flat-1D view borrowing the device handle at offset 0 — a pure
    // 3D→1D reshape of the same allocation; the crop is absorbed by the
    // kernel's crop_l/crop_t scalars, so no device_crop offset (and none of
    // Vulkan's descriptor-offset alignment constraints) is involved.
    // Deliberately a raw halide_buffer_t, NOT a Runtime::Buffer — Buffer's
    // decref() would device_free the borrowed handle on destruction.
    // ponytail: borrowed handle without retain — decodes are single-flight
    // through this path; switch to halide_device_crop/release_crop if
    // concurrent decodes ever share producer buffers.
    halide_dimension_t flat_dim(0, flat_len, 1);
    halide_buffer_t src_flat = *stage3_device_buf;  // copies device + flags (device_dirty)
    src_flat.dimensions = 1;
    src_flat.dim = &flat_dim;
    if (src_flat.device == 0) {
        // Producer fell back to host memory — upload path (pre-G1 behavior
        // minus the redundant D2H, which is a no-op without a device buffer).
        if (!src_flat.host) {
            return false;
        }
        src_flat.set_host_dirty(true);
    }
    auto tcopy_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                      : std::chrono::high_resolution_clock::time_point{};  // d2h_src now ~0 (alias)
    auto t1_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};  // repack_src ~0 (O(1) reshape)
#else
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] non-split-kernel interleaved path ACTIVE (runRenderStage4HalideAotFromDevice)\n");
    }
    // Non-owning wrapper — do NOT call set_host_dirty; data is on the GPU.
    Buffer<uint16_t> src_buf(*stage3_device_buf);
    if (scaled) {
        // Sized path: crop to the SOURCE extent (not dst) — the box geometry is
        // derived from the source extent against out_w/out_h, so the full source
        // area must be visible to the kernel. Cropped unconditionally, including
        // when crop_l/crop_t are 0, so the ratio matches DefaultCropArea exactly
        // rather than whatever the producer buffer happens to be sized to.
        src_buf.crop(0, crop_l, src_w);
        src_buf.crop(1, crop_t, src_h);
        halide_buffer_t* raw = src_buf.raw_buffer();
        raw->dim[0].min = 0;
        raw->dim[1].min = 0;
    } else if (crop_l > 0 || crop_t > 0) {
        // 8.2.3 fix: physically shift the read pointer via crop(), then mutate
        // dim[i].min back to 0 so src/dst share the [0..dst_w-1] coordinate system.
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
#if defined(DNG_STAGE4_SPLIT_KERNEL)
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
#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // G2: interleaved RGBA8 dst (macOS layout; Probe-A verified). Fused path
    // writes the caller's RGBA buffer directly; legacy RGB8 callers go through
    // RGBA scratch + alpha strip.
    uint8_t* dst_rgba_and = dst;
    std::optional<Stage4DstScratch::Lease> dst_lease;
    if (!fuse_rgba) {
        dst_lease.emplace(stage4DstScratch().acquire(
            static_cast<size_t>(dst_w) * dst_h * 4));
        dst_rgba_and = dst_lease->data();
    }
    Buffer<uint8_t> dst_rgba_buf =
        Buffer<uint8_t>::make_interleaved(dst_rgba_and, dst_w, dst_h, 4);
#else
    // W7 (M-11): macOS generator outputs RGBA8. Persistent scratch for strip path.
    uint8_t* dst_rgba_fd = dst;
    if (!fuse_rgba) {
        dst_rgba_fd = acquireRgbaStripScratch(static_cast<size_t>(dst_w) * dst_h * 4);
        if (!dst_rgba_fd) return false;
    }
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst_rgba_fd, dst_w, dst_h, 4);
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
#if defined(DNG_STAGE4_SPLIT_KERNEL)
    dst_rgba_buf.set_host_dirty(false);
#else
    dst_buf.set_host_dirty(false);
#endif

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    const int32_t huesat_entry_count = static_cast<int32_t>(params.huesat_table.size() / 3);
    const int32_t look_entry_count = static_cast<int32_t>(params.look_table.size() / 3);
    // G2: dst_width scalar retired — the RGBA dst buffer extents carry the
    // output geometry.
    const int result = dng_render_stage4_split(
        &src_flat,
        sw,
        sh,
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
        dst_rgba_buf.raw_buffer());
    auto t2_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};
#else
    // R2 sized decode: identical argument tail, two kernels. The scaled kernel
    // takes dst_w/dst_h as explicit scalars so its box geometry never depends
    // on output bounds inference. scaled==false reproduces the previous call
    // exactly.
    const int result =
        scaled
        ? dng_render_stage4_scaled_preavg(src_buf.raw_buffer(),
                                         src_scale,
                                         dst_w,
                                         dst_h,
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
                                         dst_buf.raw_buffer())
        : dng_render_stage4(src_buf.raw_buffer(),
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
    if (pipelineVerbose()) {
        fprintf(stderr, "[Stage4-Diag] kernel result=%d (runRenderStage4HalideAotFromDevice)\n", result);
    }
    if (result != 0) {
        return false;
    }

#if defined(DNG_STAGE4_SPLIT_KERNEL)
    // G2: one D2H copy of the interleaved RGBA output. On the fused path this
    // lands directly in the caller's RGBA buffer — no host repack at all.
    if (dst_rgba_buf.copy_to_host() != 0) return false;
    // G1: src device allocation fully consumed — free it through the ORIGINAL
    // struct so the owner's halide_buffer_t sees device==0 and its Buffer
    // destructor (lossless: impl->dst_buf via halide_cancel; lossy:
    // DeviceHandoffState::buffer via clear_device_handoff) does not
    // double-free a stale handle. On every failure return above we
    // deliberately do NOT free: the lossless fallback
    // (demosaic_warp_rectilinear_halide_finish) and the lossy restore
    // (halide_stage2_ol2_device_handoff_copy_to_host) still need the data.
    if (stage3_device_buf->device != 0) {
        halide_device_free(nullptr, stage3_device_buf);
    }
    auto t3_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};

    // Legacy RGB8 callers: strip alpha from the RGBA scratch. Fused path:
    // nothing left to do on the host.
    if (!fuse_rgba) {
        stripRgbaToRgbMT(dst_rgba_and, dst, dst_w * dst_h);
    }
    auto t4_fd = verbose_timing_fd ? std::chrono::high_resolution_clock::now()
                                   : std::chrono::high_resolution_clock::time_point{};
    // G1: both repack_src and d2h_src are ~0 (O(1) device-alias reshape, no
    // host round trip). G2: repack_dst is the legacy alpha-strip time and ~0
    // on the fused production path. The keys are kept so the [Stage4-Perf]
    // parser and baseline comparisons keep resolving; they directly show the
    // eliminated transfers vs the pre-G1/pre-G2 baselines.
    if (verbose_timing_fd) {
        fprintf(stderr, "[Stage4-Perf] FromDevice: repack_src=%.1f ms d2h_src=%.1f ms dispatch=%.1f ms copy_host=%.1f ms repack_dst=%.1f ms total=%.1f ms\n",
            std::chrono::duration<double, std::milli>(t1_fd - tcopy_fd).count(),
            std::chrono::duration<double, std::milli>(tcopy_fd - t0_fd).count(),
            std::chrono::duration<double, std::milli>(t2_fd - t1_fd).count(),
            std::chrono::duration<double, std::milli>(t3_fd - t2_fd).count(),
            std::chrono::duration<double, std::milli>(t4_fd - t3_fd).count(),
            std::chrono::duration<double, std::milli>(t4_fd - t0_fd).count());
    }
#else
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    // W7 (M-11): strip alpha when legacy caller expects RGB8.
    if (!fuse_rgba) {
        const size_t total_px = static_cast<size_t>(dst_w) * dst_h;
        for (size_t i = 0; i < total_px; ++i) {
            dst[i * 3 + 0] = dst_rgba_fd[i * 4 + 0];
            dst[i * 3 + 1] = dst_rgba_fd[i * 4 + 1];
            dst[i * 3 + 2] = dst_rgba_fd[i * 4 + 2];
        }
    }
#endif

    return true;
}

namespace {  // T8: reopen the file-local namespace after the extracted core.

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
    // W7 (M-11): macOS generator outputs RGBA8 but the caller's vector stays RGB (W*H*3).
    // The kernel writes into the persistent RGBA scratch (via runRenderStage4HalideAot's
    // fuse_rgba=false path), which strips to RGB in the caller's vector — no 4-channel
    // resize of the caller's vector (that realloc + page-fault was costing ~150ms).
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
            // W7 (M-11): on macOS, runRenderStage4HalideAot already stripped
            // RGBA→RGB via the persistent scratch (fuse_rgba=false). The caller's
            // vector is W*H*3 throughout — no resize needed.
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

    // Pool path: no resize, no page fault. W7-B: fused path outputs RGBA8.
    const size_t needed_out_size =
        static_cast<size_t>(out_w) * out_h * (config.fuse_rgba_output ? 4 : 3);
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
                                                         out_rgb_ptr,
                                                         config.fuse_rgba_output);
        if (render_ok) {
            return true;
        }
    }

    // W7-B: the SDK fallback below writes interleaved RGB8; it cannot satisfy an
    // RGBA8 fused buffer. On the fused path the GPU render is mandatory (callers
    // already refuse SDK CPU fallback), so fail rather than emit RGB-in-RGBA.
    if (config.fuse_rgba_output) {
        return false;
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

// R2 sized decode: public accessor so dng_pipeline.cpp can size the Stage4
// output buffer by OUTPUT extent before the render runs. Same computation the
// render path itself uses — deliberately not duplicated there. Defined outside
// the anonymous namespace above so it has external linkage.
void dng_render_stage4_output_size(const dng_negative& negative,
                                   const dng_render& renderer,
                                   uint32_t& out_w,
                                   uint32_t& out_h) {
    const dng_point dst_size = computeOutputSize(negative, renderer);
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);
}

const char* renderHalideModeName(RenderHalideMode mode) {
    switch (mode) {
        case RenderHalideMode::SDK: return "sdk";
        case RenderHalideMode::HALIDE_METAL: return "halide-metal";
        case RenderHalideMode::HALIDE_GPU: return "HALIDE_GPU";
        case RenderHalideMode::AUTO: return "auto";
    }
    return "unknown";
}

#if defined(__ANDROID__)
namespace {
// T9 (Gotcha #62): lazy-zero scratch for warmup dummy buffers. mmap MAP_ANON
// hands back zero-pages committed only on first touch (the Halide H2D upload),
// avoiding std::vector(N,0)'s eager full-buffer memset (~2s for the ~410MB of
// S4 dummy src+dst at 6048x4024). munmap (or free for the calloc fallback)
// releases on scope exit.
struct LazyZeroBuf {
    void* ptr = nullptr;
    size_t bytes = 0;
    bool mmaped = false;
    explicit LazyZeroBuf(size_t n) : bytes(n) {
        ptr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_ANON | MAP_PRIVATE, -1, 0);
        if (ptr == MAP_FAILED) {
            ptr = std::calloc(1, bytes);  // OS-zeroed fallback
            mmaped = false;
        } else {
            mmaped = true;
        }
    }
    ~LazyZeroBuf() {
        if (mmaped && ptr != nullptr) {
            ::munmap(ptr, bytes);
        } else if (ptr != nullptr) {
            std::free(ptr);
        }
    }
    LazyZeroBuf(const LazyZeroBuf&) = delete;
    LazyZeroBuf& operator=(const LazyZeroBuf&) = delete;
};
}  // namespace
#endif  // __ANDROID__

// G3: shared Stage4 prewarm body — called by both the (width, height) and the
// (width, height, dst_rgba_ptr, dst_rgba_size) overloads. When dst_rgba_ptr
// is non-null and large enough, the kernel writes there instead of a dummy
// buffer, warming the GPU DMA path to the exact host pages production's
// copy_to_host targets.
static void prewarm_stage4_impl(int width, int height,
                                uint8_t* dst_rgba_ptr, size_t dst_rgba_size) {
#if defined(__ANDROID__)
    // W7-E: build the Stage4 render GPU pipeline at the actual image size so the
    // first real decode skips the Vulkan pipeline-creation + first large
    // dispatch cold tax (matrix S4 ~440ms vs warm ~192ms). Mirrors the S3
    // prewarm: per-size cache + one identity-params dispatch on zeroed dummy
    // buffers, result discarded. Drives the same dng_render_stage4_split AOT
    // entry production uses (host-src vs device-src do not change the compiled
    // kernel / pipeline state).
    if (width <= 0 || height <= 0) {
        return;
    }
    static std::mutex cache_mu;
    static std::unordered_set<uint64_t> warmed;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(width)) << 32) |
                         static_cast<uint64_t>(static_cast<uint32_t>(height));
    {
        std::lock_guard<std::mutex> lock(cache_mu);
        if (warmed.count(key)) {
            return;
        }
    }

    // Identity RenderParams via the exact toIdentity*/ensureSafeHueSatMap path
    // buildRenderParams uses for the linear/no-profile case (Gotcha #92 shape
    // sanitization). The AOT kernel only reads the float members + dims/flags;
    // the SDK-object scaffolding is unused. Values are irrelevant (output
    // discarded) — only buffer shapes drive the Vulkan pipeline specialization.
    RenderParams params;
    toIdentityCurve(params.exp_ramp);
    toIdentityCurve(params.tone_curve);
    toIdentityCurve(params.encode_gamma);
    params.camera_white[0] = params.camera_white[1] = params.camera_white[2] = 1.0f;
    params.camera_to_rgb[0] = params.camera_to_rgb[4] = params.camera_to_rgb[8] = 1.0f;
    params.rgb_to_final[0] = params.rgb_to_final[4] = params.rgb_to_final[8] = 1.0f;
    toIdentityHueSatMap(params.huesat_table, params.huesat_hue_div,
                        params.huesat_sat_div, params.huesat_val_div,
                        params.huesat_has_table);
    toIdentityHueSatMap(params.look_table, params.look_hue_div,
                        params.look_sat_div, params.look_val_div,
                        params.look_has_table);
    toIdentityCurve(params.huesat_encode);
    toIdentityCurve(params.huesat_decode);
    toIdentityCurve(params.look_encode);
    toIdentityCurve(params.look_decode);
    ensureSafeHueSatMap(params.huesat_table, params.huesat_hue_div,
                        params.huesat_sat_div, params.huesat_val_div,
                        params.huesat_has_table);
    ensureSafeHueSatMap(params.look_table, params.look_hue_div,
                        params.look_sat_div, params.look_val_div,
                        params.look_has_table);

    // Dense interleaved RGB16 dummy src + RGBA8 dst at actual size.
    // G2: fuse_rgba=true — the kernel now renders interleaved RGBA8 directly,
    // so the prewarm writes the buffer without the legacy alpha-strip pass
    // (same AOT entry + pipeline specialization production uses; fuse_rgba only
    // changes host-side buffer wiring).
    // G3: when a pool RGBA buffer is provided and large enough, the kernel
    // writes there instead of a dummy. This warms the GPU DMA → host-page
    // mapping so copy_to_host during production doesn't pay a first-touch
    // staging penalty (~63ms cold on SM8650).
    const size_t srcElems =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 3u;
    const size_t dstBytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
    LazyZeroBuf dummy_src(srcElems * sizeof(uint16_t));

    uint8_t* dst_ptr = nullptr;
    std::optional<LazyZeroBuf> dummy_dst_holder;
    if (dst_rgba_ptr != nullptr && dst_rgba_size >= dstBytes) {
        dst_ptr = dst_rgba_ptr;
    } else {
        dummy_dst_holder.emplace(dstBytes);
        dst_ptr = static_cast<uint8_t*>(dummy_dst_holder->ptr);
    }

    std::fprintf(stderr, "[Warmup] s4 begin %dx%d pool_dst=%d\n",
                 width, height, (dst_rgba_ptr != nullptr && dst_rgba_size >= dstBytes) ? 1 : 0);
    std::fflush(stderr);
    if (dummy_src.ptr != nullptr && dst_ptr != nullptr) {
        (void)runRenderStage4HalideAot(static_cast<uint16_t*>(dummy_src.ptr),
                                       width, height, /*src_p=*/3,
                                       /*src_row_step=*/width * 3,
                                       /*src_col_step=*/3,
                                       /*src_plane_step=*/1,
                                       /*src_scale=*/1.0f / 65535.0f,
                                       /*dst_w=*/width, /*dst_h=*/height,
                                       params, dst_ptr,
                                       /*fuse_rgba=*/true);
    }
    std::fprintf(stderr, "[Warmup] s4 done\n");
    std::fflush(stderr);

    {
        std::lock_guard<std::mutex> lock(cache_mu);
        warmed.insert(key);
    }
#else
    (void)width;
    (void)height;
    (void)dst_rgba_ptr;
    (void)dst_rgba_size;
#endif
}

void dng_render_stage4_prewarm_for_size(int width, int height) {
    prewarm_stage4_impl(width, height, nullptr, 0);
}

void dng_render_stage4_prewarm_for_size(int width, int height,
                                         uint8_t* dst_rgba_ptr,
                                         size_t dst_rgba_size) {
    prewarm_stage4_impl(width, height, dst_rgba_ptr, dst_rgba_size);
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

    // R2 sized decode: a size mismatch no longer bails out — the source extent
    // is passed through and the scaled kernel handles the downscale. The split
    // (Android) branch still refuses inside the runner and falls back to host.
    const dng_rect src_area = negative.DefaultCropArea();

    // W7 (M-11): caller's vector stays RGB (W*H*3). macOS kernel writes RGBA into
    // persistent scratch; runRenderStage4HalideAotFromDevice strips to the caller's
    // buffer (fuse_rgba=false default).
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
        static_cast<int>(src_area.W()), static_cast<int>(src_area.H()),
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

    // R2 sized decode: see the vector overload above — size mismatch dispatches
    // the scaled kernel instead of bailing out.
    const dng_rect src_area = negative.DefaultCropArea();

    // Pool path: buffer is already committed — no resize, no page fault.
    // W7-B: fused path outputs RGBA8.
    const size_t needed =
        static_cast<size_t>(out_w) * out_h * (config.fuse_rgba_output ? 4 : 3);
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
        static_cast<int>(src_area.W()), static_cast<int>(src_area.H()),
        static_cast<int>(out_w), static_cast<int>(out_h),
        params, out_rgb_ptr, config.fuse_rgba_output);
}
