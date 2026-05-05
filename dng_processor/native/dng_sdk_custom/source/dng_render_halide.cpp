/*
---
file_summary: >
  Stage4 render 橋接層。負責把 DNG SDK 的 Stage3 `dng_image` 與 color/tone/profile
  參數整理成 Halide AOT kernel 可用的 Buffer，並在 full / no-map / maps-no-encode /
  tone-tail / reference / SDK quality-lock 路徑間 dispatch，同時提供 debug 與 timing。

notes:
  - `render_stage4_halide()` 是唯一正式入口；其餘多為資料萃取、參數建構、AOT 包裝與 reference/debug helper。
  - `DNG_RENDER_BIT_EXACT` 或 `DNG_WARP_BIT_EXACT` 啟用時，會走 SDK render quality-lock 路徑。
  - 目前仍保留 prefix/tail/reference 路徑作為除錯與 fallback，後續 Phase 6.4.3 預計收斂。

structs:
  - name: "RenderParams"
    description: "Stage4 所需矩陣、1D/3D table、encoding table 與 SDK reference 物件快取。"
    lines: "156-194"
  - name: "DiffChannelStats"
    description: "debug 模式下每個 RGB channel 的差異統計與 sample 座標。"
    lines: "1239-1250"

functions:
  - name: "parsePositiveEnvU32"
    description: "解析正整數環境變數（≤0 視為未設定）。"
    lines: "198-208"
  - name: "copyRenderSettings"
    description: "把既有 `dng_render` 參數複製到另一個 renderer（供 host 分離時重建）。"
    lines: "210-219"
  - name: "qualityLockRenderHostCache"
    description: "回傳 quality-lock SDK Render 專用的快取 host。"
    lines: "240-243"
  - name: "extractStage3Interleaved"
    description: "把 Stage3 `dng_image` 抽成 float interleaved buffer。"
    lines: "245-267"
  - name: "extractStage3Interleaved16"
    description: "把 Stage3 `dng_image` 抽成 uint16 interleaved buffer。"
    lines: "220-242"
  - name: "borrowStage3Interleaved16"
    description: "嘗試直接借用 Stage3 tile buffer，避免額外拷貝。"
    lines: "244-283"
  - name: "packBorrowedStage3Interleaved16"
    description: "將 borrowed Stage3 tile 重新整理成緊密 interleaved 版面。"
    lines: "285-318"
  - name: "borrowImageInterleaved8"
    description: "嘗試直接借用最終 8-bit image tile buffer。"
    lines: "320-358"
  - name: "packBorrowedInterleaved8"
    description: "將 borrowed 8-bit tile 重新整理成緊密 interleaved 輸出。"
    lines: "360-393"
  - name: "matrixToRowMajor3x3"
    description: "將 `dng_matrix` 轉成 Halide 端使用的 row-major 3x3 array。"
    lines: "395-401"
  - name: "toIdentityHueSat"
    description: "建立 identity HueSat / Look table。"
    lines: "403-409"
  - name: "toIdentityCurve"
    description: "建立 identity 1D curve。"
    lines: "411-418"
  - name: "copyHueSatMap"
    description: "將 SDK HueSatMap 轉成 Halide 期望的 planar layout。"
    lines: "420-446"
  - name: "buildRenderParams"
    description: "從 `dng_negative`/`dng_render` 萃取 Stage4 所需矩陣、tone/gamma/table 與 profile map。"
    lines: "448-602"
  - name: "runRenderStage4HalideAot"
    description: "呼叫 full Stage4 Halide AOT kernel。"
    lines: "604-688"
  - name: "runRenderStage4NoMapHalideAot"
    description: "呼叫無 HueSat/Look map 的簡化 Stage4 kernel。"
    lines: "690-740"
  - name: "runRenderStage4MapsNoEncodingHalideAot"
    description: "呼叫有 map 但無 encoding table 的 Stage4 kernel。"
    lines: "742-806"
  - name: "runRenderTailHalideAot"
    description: "只做 RGB-to-final + gamma 的 tail AOT kernel。"
    lines: "808-841"
  - name: "runRenderToneTailHalideAot"
    description: "做 tone curve + RGB-to-final + gamma 的 tail AOT kernel。"
    lines: "843-882"
  - name: "runRenderStage4Reference"
    description: "逐 scanline 呼叫 SDK reference helper，產生完整 Stage4 參考輸出。"
    lines: "884-983"
  - name: "runRenderPrefix"
    description: "在 CPU 上先做 ABC->RGB / HueSat / Exposure / Look / optional Tone，供 tail fallback 使用。"
    lines: "985-1112"
  - name: "runRenderPrefixToTone"
    description: "包裝 `runRenderPrefix(..., apply_tone=true)`。"
    lines: "1114-1121"
  - name: "runRenderPrefixToPreTone"
    description: "包裝 `runRenderPrefix(..., apply_tone=false)`。"
    lines: "1123-1130"
  - name: "runRenderTailReference"
    description: "用 SDK reference helper 完成 tail 端 RGB-to-final + gamma。"
    lines: "1132-1173"
  - name: "runRenderTailScalar"
    description: "純 CPU scalar tail 實作，用於矩陣/encoding 行為比對。"
    lines: "1175-1221"
  - name: "computePSNR8"
    description: "計算 8-bit RGB buffer PSNR。"
    lines: "1223-1237"
  - name: "printRgbDiffStats"
    description: "輸出 RGB channel 的 MAE / maxAbs / sample mismatch。"
    lines: "1252-1305"
  - name: "renderHalideDebugEnabled"
    description: "讀取 `DNG_RENDER_HALIDE_DEBUG`。"
    lines: "1307-1310"
  - name: "renderHalideTimingEnabled"
    description: "讀取 `DNG_RENDER_HALIDE_TIMING`。"
    lines: "1312-1315"
  - name: "renderHalideTryToneTailEnabled"
    description: "讀取 `DNG_RENDER_HALIDE_TRY_TONETAIL`。"
    lines: "1317-1320"
  - name: "renderHalideTryFullEnabled"
    description: "判斷 full kernel 是否允許啟用。"
    lines: "1322-1325"
  - name: "renderHalideForceFullKernelEnabled"
    description: "讀取 `DNG_RENDER_HALIDE_FORCE_FULL`。"
    lines: "1327-1330"
  - name: "renderHalideBitExactModeEnabled"
    description: "判斷是否進入 SDK quality-lock / bit-exact 模式。"
    lines: "1332-1339"
  - name: "renderHalideModeName"
    description: "列舉值轉字串。"
    lines: "1343-1350"
  - name: "render_stage4_halide"
    description: "Stage4 正式入口；處理 quality-lock、resample、borrow/copy、kernel dispatch、fallback 與 debug/timing。"
    lines: "1401-1749"
---
*/
#include "dng_render_halide.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <array>
#include <limits>
#include <iostream>
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
#include "dng_rect.h"
#include "dng_reference.h"
#include "dng_render_nomap_stage4.h"
#include "dng_render_maps_noencode_stage4.h"
#include "dng_render_stage4.h"
#include "dng_render_tail_stage4.h"
#include "dng_render_tonetail_stage4.h"
#include "dng_resample.h"

namespace {

using Halide::Runtime::Buffer;

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

uint32_t parsePositiveEnvU32(const char* key) {
    const char* v = std::getenv(key);
    if (!v || !v[0]) {
        return 0;
    }
    const long parsed = std::strtol(v, nullptr, 10);
    if (parsed <= 0) {
        return 0;
    }
    return static_cast<uint32_t>(parsed);
}

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
    // For 2 identity entries, layout is:
    // hue: [0, 0], sat: [1, 1], val: [1, 1].
    table = {0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f};
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

bool buildRenderParams(dng_host& host,
                       dng_negative& negative,
                       const dng_render& renderer,
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

    toIdentityHueSat(params.huesat_table);
    toIdentityHueSat(params.look_table);
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

    const char* disable_huesat = std::getenv("DNG_RENDER_DISABLE_HUESAT");
    if (disable_huesat && disable_huesat[0] && disable_huesat[0] != '0') {
        params.huesat_map_ref.Reset();
        params.huesat_encode_ref.Reset();
        params.huesat_decode_ref.Reset();
        params.huesat_has_table = 0;
        params.huesat_has_encoding = 0;
    }

    const char* disable_look = std::getenv("DNG_RENDER_DISABLE_LOOK");
    if (disable_look && disable_look[0] && disable_look[0] != '0') {
        params.look_map_ref.Reset();
        params.look_encode_ref.Reset();
        params.look_decode_ref.Reset();
        params.look_has_table = 0;
        params.look_has_encoding = 0;
    }

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

    halide_dimension_t src_shape[3] = {
        {0, src_w, src_col_step, 0},
        {0, src_h, src_row_step, 0},
        {0, src_p, src_plane_step, 0},
    };
    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(src), 3, src_shape);
    Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                          static_cast<int>(params.exp_ramp.size()));
    Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                           static_cast<int>(params.tone_curve.size()));
    Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                            static_cast<int>(params.encode_gamma.size()));
    Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size() / 3), 3);
    Buffer<float> hs_encode_buf(const_cast<float*>(params.huesat_encode.data()),
                                static_cast<int>(params.huesat_encode.size()));
    Buffer<float> hs_decode_buf(const_cast<float*>(params.huesat_decode.data()),
                                static_cast<int>(params.huesat_decode.size()));
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size() / 3), 3);
    Buffer<float> look_encode_buf(const_cast<float*>(params.look_encode.data()),
                                  static_cast<int>(params.look_encode.size()));
    Buffer<float> look_decode_buf(const_cast<float*>(params.look_decode.data()),
                                  static_cast<int>(params.look_decode.size()));
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst, dst_w, dst_h, 3);

    src_buf.set_host_dirty();
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
    dst_buf.set_host_dirty(false);

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
    if (result != 0) {
        return false;
    }
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    return true;
}

[[deprecated("Phase 6.4.3: dispatch converged to Halide Full + SDK fallback")]]
bool runRenderStage4NoMapHalideAot(const uint16_t* src,
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

    halide_dimension_t src_shape[3] = {
        {0, src_w, src_col_step, 0},
        {0, src_h, src_row_step, 0},
        {0, src_p, src_plane_step, 0},
    };
    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(src), 3, src_shape);
    Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                          static_cast<int>(params.exp_ramp.size()));
    Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                           static_cast<int>(params.tone_curve.size()));
    Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                            static_cast<int>(params.encode_gamma.size()));
    Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst, dst_w, dst_h, 3);

    src_buf.set_host_dirty();
    exp_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();
    c2r_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int result = dng_render_nomap_stage4(src_buf.raw_buffer(),
                                               src_scale,
                                               exp_buf.raw_buffer(),
                                               tone_buf.raw_buffer(),
                                               gamma_buf.raw_buffer(),
                                               cw_buf.raw_buffer(),
                                               c2r_buf.raw_buffer(),
                                               r2f_buf.raw_buffer(),
                                               dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    return true;
}

[[deprecated("Phase 6.4.3: dispatch converged to Halide Full + SDK fallback")]]
bool runRenderStage4MapsNoEncodingHalideAot(const uint16_t* src,
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

    halide_dimension_t src_shape[3] = {
        {0, src_w, src_col_step, 0},
        {0, src_h, src_row_step, 0},
        {0, src_p, src_plane_step, 0},
    };
    Buffer<uint16_t> src_buf(const_cast<uint16_t*>(src), 3, src_shape);
    Buffer<float> exp_buf(const_cast<float*>(params.exp_ramp.data()),
                          static_cast<int>(params.exp_ramp.size()));
    Buffer<float> tone_buf(const_cast<float*>(params.tone_curve.data()),
                           static_cast<int>(params.tone_curve.size()));
    Buffer<float> gamma_buf(const_cast<float*>(params.encode_gamma.data()),
                            static_cast<int>(params.encode_gamma.size()));
    Buffer<float> cw_buf(const_cast<float*>(params.camera_white), 3);
    Buffer<float> c2r_buf(const_cast<float*>(params.camera_to_rgb), 3, 3);
    Buffer<float> r2f_buf(const_cast<float*>(params.rgb_to_final), 3, 3);
    Buffer<float> hs_table_buf(const_cast<float*>(params.huesat_table.data()),
                               static_cast<int>(params.huesat_table.size() / 3), 3);
    Buffer<float> look_table_buf(const_cast<float*>(params.look_table.data()),
                                 static_cast<int>(params.look_table.size() / 3), 3);
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst, dst_w, dst_h, 3);

    src_buf.set_host_dirty();
    exp_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    cw_buf.set_host_dirty();
    c2r_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    hs_table_buf.set_host_dirty();
    look_table_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int result = dng_render_maps_noencode_stage4(src_buf.raw_buffer(),
                                                       src_scale,
                                                       exp_buf.raw_buffer(),
                                                       tone_buf.raw_buffer(),
                                                       gamma_buf.raw_buffer(),
                                                       cw_buf.raw_buffer(),
                                                       c2r_buf.raw_buffer(),
                                                       r2f_buf.raw_buffer(),
                                                       hs_table_buf.raw_buffer(),
                                                       params.huesat_hue_div,
                                                       params.huesat_sat_div,
                                                       params.huesat_val_div,
                                                       look_table_buf.raw_buffer(),
                                                       params.look_hue_div,
                                                       params.look_sat_div,
                                                       params.look_val_div,
                                                       dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    return true;
}

[[deprecated("Phase 6.4.3: legacy tail path")]]
bool runRenderTailHalideAot(const float* src,
                            int src_w,
                            int src_h,
                            int src_p,
                            const float rgb_to_final[9],
                            const std::vector<float>& encode_gamma,
                            uint8_t* dst) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || src_p < 3 || encode_gamma.empty()) {
        return false;
    }

    Buffer<float> src_buf = Buffer<float>::make_interleaved(const_cast<float*>(src), src_w, src_h, src_p);
    Buffer<float> r2f_buf(const_cast<float*>(rgb_to_final), 3, 3);
    Buffer<float> gamma_buf(const_cast<float*>(encode_gamma.data()),
                            static_cast<int>(encode_gamma.size()));
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst, src_w, src_h, 3);

    src_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int result = dng_render_tail_stage4(src_buf.raw_buffer(),
                                              r2f_buf.raw_buffer(),
                                              gamma_buf.raw_buffer(),
                                              dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    return true;
}

[[deprecated("Phase 6.4.3: legacy tone-tail path")]]
bool runRenderToneTailHalideAot(const float* src,
                                int src_w,
                                int src_h,
                                int src_p,
                                const std::vector<float>& tone_curve,
                                const float rgb_to_final[9],
                                const std::vector<float>& encode_gamma,
                                uint8_t* dst) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || src_p < 3 ||
        tone_curve.empty() || encode_gamma.empty()) {
        return false;
    }

    Buffer<float> src_buf = Buffer<float>::make_interleaved(const_cast<float*>(src), src_w, src_h, src_p);
    Buffer<float> tone_buf(const_cast<float*>(tone_curve.data()),
                           static_cast<int>(tone_curve.size()));
    Buffer<float> r2f_buf(const_cast<float*>(rgb_to_final), 3, 3);
    Buffer<float> gamma_buf(const_cast<float*>(encode_gamma.data()),
                            static_cast<int>(encode_gamma.size()));
    Buffer<uint8_t> dst_buf = Buffer<uint8_t>::make_interleaved(dst, src_w, src_h, 3);

    src_buf.set_host_dirty();
    tone_buf.set_host_dirty();
    r2f_buf.set_host_dirty();
    gamma_buf.set_host_dirty();
    dst_buf.set_host_dirty(false);

    const int result = dng_render_tonetail_stage4(src_buf.raw_buffer(),
                                                  tone_buf.raw_buffer(),
                                                  r2f_buf.raw_buffer(),
                                                  gamma_buf.raw_buffer(),
                                                  dst_buf.raw_buffer());
    if (result != 0) {
        return false;
    }
    if (dst_buf.copy_to_host() != 0) {
        return false;
    }
    return true;
}

bool runRenderStage4Reference(const float* src,
                              int src_w,
                              int src_h,
                              int src_p,
                              const RenderParams& params,
                              uint8_t* dst) {
    if (!src || !dst || src_w <= 0 || src_h <= 0 || src_p < 3) {
        return false;
    }

    std::vector<real32> a(src_w), b(src_w), c(src_w);
    std::vector<real32> r0(src_w), g0(src_w), b0(src_w);
    std::vector<real32> r1(src_w), g1(src_w), b1(src_w);
    std::vector<real32> r2(src_w), g2(src_w), b2(src_w);
    std::vector<real32> r3(src_w), g3(src_w), b3(src_w);

    for (int y = 0; y < src_h; ++y) {
        const float* row = src + static_cast<size_t>(y) * src_w * src_p;
        for (int x = 0; x < src_w; ++x) {
            const float* px = row + static_cast<size_t>(x) * src_p;
            a[x] = px[0];
            b[x] = px[1];
            c[x] = px[2];
        }

        RefBaselineABCtoRGB(a.data(),
                            b.data(),
                            c.data(),
                            r0.data(),
                            g0.data(),
                            b0.data(),
                            static_cast<uint32>(src_w),
                            params.camera_white_vec,
                            params.camera_to_rgb_mat);

        if (params.huesat_map_ref.Get()) {
            RefBaselineHueSatMap(r0.data(),
                                 g0.data(),
                                 b0.data(),
                                 r0.data(),
                                 g0.data(),
                                 b0.data(),
                                 static_cast<uint32>(src_w),
                                 *params.huesat_map_ref.Get(),
                                 params.huesat_encode_ref.Get(),
                                 params.huesat_decode_ref.Get());
        }

        RefBaseline1DTable(r0.data(), r1.data(), static_cast<uint32>(src_w), params.exp_table_ref);
        RefBaseline1DTable(g0.data(), g1.data(), static_cast<uint32>(src_w), params.exp_table_ref);
        RefBaseline1DTable(b0.data(), b1.data(), static_cast<uint32>(src_w), params.exp_table_ref);

        if (params.look_map_ref.Get()) {
            RefBaselineHueSatMap(r1.data(),
                                 g1.data(),
                                 b1.data(),
                                 r1.data(),
                                 g1.data(),
                                 b1.data(),
                                 static_cast<uint32>(src_w),
                                 *params.look_map_ref.Get(),
                                 params.look_encode_ref.Get(),
                                 params.look_decode_ref.Get());
        }

        RefBaselineRGBTone(r1.data(),
                           g1.data(),
                           b1.data(),
                           r2.data(),
                           g2.data(),
                           b2.data(),
                           static_cast<uint32>(src_w),
                           params.tone_table_ref);

        RefBaselineRGBtoRGB(r2.data(),
                            g2.data(),
                            b2.data(),
                            r3.data(),
                            g3.data(),
                            b3.data(),
                            static_cast<uint32>(src_w),
                            params.rgb_to_final_mat);

        RefBaseline1DTable(r3.data(), r3.data(), static_cast<uint32>(src_w), params.gamma_table_ref);
        RefBaseline1DTable(g3.data(), g3.data(), static_cast<uint32>(src_w), params.gamma_table_ref);
        RefBaseline1DTable(b3.data(), b3.data(), static_cast<uint32>(src_w), params.gamma_table_ref);

        uint8_t* out_row = dst + static_cast<size_t>(y) * src_w * 3;
        for (int x = 0; x < src_w; ++x) {
            const float rr = std::clamp(r3[x], 0.0f, 1.0f);
            const float gg = std::clamp(g3[x], 0.0f, 1.0f);
            const float bbv = std::clamp(b3[x], 0.0f, 1.0f);
            out_row[x * 3 + 0] = static_cast<uint8_t>(rr * 255.0f + 0.5f);
            out_row[x * 3 + 1] = static_cast<uint8_t>(gg * 255.0f + 0.5f);
            out_row[x * 3 + 2] = static_cast<uint8_t>(bbv * 255.0f + 0.5f);
        }
    }

    return true;
}

[[deprecated("Phase 6.4.3: legacy prefix path")]]
bool runRenderPrefix(const float* src,
                     int src_w,
                     int src_h,
                     int src_p,
                     const RenderParams& params,
                     bool apply_tone,
                     std::vector<float>& out_rgb) {
    if (!src || src_w <= 0 || src_h <= 0 || src_p < 3) {
        return false;
    }

    out_rgb.assign(static_cast<size_t>(src_w) * src_h * 3u, 0.0f);
    unsigned int thread_count = std::thread::hardware_concurrency();
    if (thread_count == 0) {
        thread_count = 4;
    }
    const char* thread_env = std::getenv("DNG_RENDER_PREFIX_THREADS");
    if (thread_env && thread_env[0]) {
        const int parsed = std::atoi(thread_env);
        if (parsed > 0) {
            thread_count = static_cast<unsigned int>(parsed);
        }
    }
    if (thread_count > static_cast<unsigned int>(src_h)) {
        thread_count = static_cast<unsigned int>(src_h);
    }
    if (thread_count == 0) {
        thread_count = 1;
    }

    auto worker = [&](int y_begin, int y_end) {
        std::vector<real32> a(src_w), b(src_w), c(src_w);
        std::vector<real32> r0(src_w), g0(src_w), b0(src_w);
        std::vector<real32> r1(src_w), g1(src_w), b1(src_w);
        std::vector<real32> r2(src_w), g2(src_w), b2(src_w);

        for (int y = y_begin; y < y_end; ++y) {
            const float* row = src + static_cast<size_t>(y) * src_w * src_p;
            for (int x = 0; x < src_w; ++x) {
                const float* px = row + static_cast<size_t>(x) * src_p;
                a[x] = px[0];
                b[x] = px[1];
                c[x] = px[2];
            }

            RefBaselineABCtoRGB(a.data(),
                                b.data(),
                                c.data(),
                                r0.data(),
                                g0.data(),
                                b0.data(),
                                static_cast<uint32>(src_w),
                                params.camera_white_vec,
                                params.camera_to_rgb_mat);

            if (params.huesat_map_ref.Get()) {
                RefBaselineHueSatMap(r0.data(),
                                     g0.data(),
                                     b0.data(),
                                     r0.data(),
                                     g0.data(),
                                     b0.data(),
                                     static_cast<uint32>(src_w),
                                     *params.huesat_map_ref.Get(),
                                     params.huesat_encode_ref.Get(),
                                     params.huesat_decode_ref.Get());
            }

            RefBaseline1DTable(r0.data(), r1.data(), static_cast<uint32>(src_w), params.exp_table_ref);
            RefBaseline1DTable(g0.data(), g1.data(), static_cast<uint32>(src_w), params.exp_table_ref);
            RefBaseline1DTable(b0.data(), b1.data(), static_cast<uint32>(src_w), params.exp_table_ref);

            if (params.look_map_ref.Get()) {
                RefBaselineHueSatMap(r1.data(),
                                     g1.data(),
                                     b1.data(),
                                     r1.data(),
                                     g1.data(),
                                     b1.data(),
                                     static_cast<uint32>(src_w),
                                     *params.look_map_ref.Get(),
                                     params.look_encode_ref.Get(),
                                     params.look_decode_ref.Get());
            }

            const float* src_r = r1.data();
            const float* src_g = g1.data();
            const float* src_b = b1.data();
            if (apply_tone) {
                RefBaselineRGBTone(r1.data(),
                                   g1.data(),
                                   b1.data(),
                                   r2.data(),
                                   g2.data(),
                                   b2.data(),
                                   static_cast<uint32>(src_w),
                                   params.tone_table_ref);
                src_r = r2.data();
                src_g = g2.data();
                src_b = b2.data();
            }

            float* out_row = out_rgb.data() + static_cast<size_t>(y) * src_w * 3u;
            for (int x = 0; x < src_w; ++x) {
                out_row[x * 3 + 0] = src_r[x];
                out_row[x * 3 + 1] = src_g[x];
                out_row[x * 3 + 2] = src_b[x];
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(thread_count);
    int y0 = 0;
    for (unsigned int i = 0; i < thread_count; ++i) {
        const int remain_rows = src_h - y0;
        const int remain_threads = static_cast<int>(thread_count - i);
        const int span = remain_rows / remain_threads;
        const int y1 = y0 + span;
        workers.emplace_back(worker, y0, y1);
        y0 = y1;
    }
    for (auto& t : workers) {
        t.join();
    }

    return true;
}

[[deprecated("Phase 6.4.3: legacy prefix path")]]
bool runRenderPrefixToTone(const float* src,
                           int src_w,
                           int src_h,
                           int src_p,
                           const RenderParams& params,
                           std::vector<float>& tone_rgb) {
    return runRenderPrefix(src, src_w, src_h, src_p, params, true, tone_rgb);
}

[[deprecated("Phase 6.4.3: legacy prefix path")]]
bool runRenderPrefixToPreTone(const float* src,
                              int src_w,
                              int src_h,
                              int src_p,
                              const RenderParams& params,
                              std::vector<float>& pre_tone_rgb) {
    return runRenderPrefix(src, src_w, src_h, src_p, params, false, pre_tone_rgb);
}

[[deprecated("Phase 6.4.3: reference-only legacy path")]]
bool runRenderTailReference(const float* src_tone,
                            int src_w,
                            int src_h,
                            const RenderParams& params,
                            std::vector<uint8_t>& out_rgb) {
    if (!src_tone || src_w <= 0 || src_h <= 0) {
        return false;
    }
    out_rgb.assign(static_cast<size_t>(src_w) * src_h * 3u, 0);

    std::vector<real32> r_in(src_w), g_in(src_w), b_in(src_w);
    std::vector<real32> r_lin(src_w), g_lin(src_w), b_lin(src_w);

    for (int y = 0; y < src_h; ++y) {
        const float* row = src_tone + static_cast<size_t>(y) * src_w * 3u;
        for (int x = 0; x < src_w; ++x) {
            r_in[x] = row[x * 3 + 0];
            g_in[x] = row[x * 3 + 1];
            b_in[x] = row[x * 3 + 2];
        }

        RefBaselineRGBtoRGB(r_in.data(),
                            g_in.data(),
                            b_in.data(),
                            r_lin.data(),
                            g_lin.data(),
                            b_lin.data(),
                            static_cast<uint32>(src_w),
                            params.rgb_to_final_mat);
        RefBaseline1DTable(r_lin.data(), r_lin.data(), static_cast<uint32>(src_w), params.gamma_table_ref);
        RefBaseline1DTable(g_lin.data(), g_lin.data(), static_cast<uint32>(src_w), params.gamma_table_ref);
        RefBaseline1DTable(b_lin.data(), b_lin.data(), static_cast<uint32>(src_w), params.gamma_table_ref);

        uint8_t* out_row = out_rgb.data() + static_cast<size_t>(y) * src_w * 3u;
        for (int x = 0; x < src_w; ++x) {
            out_row[x * 3 + 0] = static_cast<uint8_t>(std::clamp(r_lin[x], 0.0f, 1.0f) * 255.0f + 0.5f);
            out_row[x * 3 + 1] = static_cast<uint8_t>(std::clamp(g_lin[x], 0.0f, 1.0f) * 255.0f + 0.5f);
            out_row[x * 3 + 2] = static_cast<uint8_t>(std::clamp(b_lin[x], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return true;
}

[[deprecated("Phase 6.4.3: reference-only legacy path")]]
bool runRenderTailScalar(const float* src_tone,
                         int src_w,
                         int src_h,
                         const float rgb_to_final[9],
                         const std::vector<float>& encode_gamma,
                         bool transposed_matrix,
                         std::vector<uint8_t>& out_rgb) {
    if (!src_tone || src_w <= 0 || src_h <= 0 || encode_gamma.size() < 2) {
        return false;
    }
    out_rgb.assign(static_cast<size_t>(src_w) * src_h * 3u, 0);
    const int max_idx = static_cast<int>(encode_gamma.size()) - 2;

    auto interp = [&](float v) {
        const float xv = std::clamp(v, 0.0f, 1.0f);
        const float yv = xv * static_cast<float>(max_idx);
        int idx = static_cast<int>(std::floor(yv));
        idx = std::clamp(idx, 0, max_idx);
        const float frac = yv - static_cast<float>(idx);
        return encode_gamma[static_cast<size_t>(idx)] * (1.0f - frac) +
               encode_gamma[static_cast<size_t>(idx + 1)] * frac;
    };

    auto m = [&](int row, int col) -> float {
        if (!transposed_matrix) {
            return rgb_to_final[row * 3 + col];
        }
        return rgb_to_final[col * 3 + row];
    };

    for (int y = 0; y < src_h; ++y) {
        const float* row = src_tone + static_cast<size_t>(y) * src_w * 3u;
        uint8_t* out_row = out_rgb.data() + static_cast<size_t>(y) * src_w * 3u;
        for (int x = 0; x < src_w; ++x) {
            const float r = row[x * 3 + 0];
            const float g = row[x * 3 + 1];
            const float b = row[x * 3 + 2];
            const float fr = std::clamp(r * m(0, 0) + g * m(0, 1) + b * m(0, 2), 0.0f, 1.0f);
            const float fg = std::clamp(r * m(1, 0) + g * m(1, 1) + b * m(1, 2), 0.0f, 1.0f);
            const float fb = std::clamp(r * m(2, 0) + g * m(2, 1) + b * m(2, 2), 0.0f, 1.0f);
            out_row[x * 3 + 0] = static_cast<uint8_t>(std::clamp(interp(fr), 0.0f, 1.0f) * 255.0f + 0.5f);
            out_row[x * 3 + 1] = static_cast<uint8_t>(std::clamp(interp(fg), 0.0f, 1.0f) * 255.0f + 0.5f);
            out_row[x * 3 + 2] = static_cast<uint8_t>(std::clamp(interp(fb), 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    return true;
}

double computePSNR8(const std::vector<uint8_t>& ref, const std::vector<uint8_t>& test) {
    if (ref.size() != test.size() || ref.empty()) {
        return 0.0;
    }
    long double mse = 0.0;
    for (size_t i = 0; i < ref.size(); ++i) {
        const long double d = static_cast<long double>(ref[i]) - static_cast<long double>(test[i]);
        mse += d * d;
    }
    mse /= static_cast<long double>(ref.size());
    if (mse <= std::numeric_limits<long double>::epsilon()) {
        return 999.0;
    }
    return 10.0 * std::log10((255.0 * 255.0) / static_cast<double>(mse));
}

struct DiffChannelStats {
    double mae = 0.0;
    uint8_t max_abs = 0;
    uint32_t max_x = 0;
    uint32_t max_y = 0;
    uint32_t non_zero = 0;
    std::array<uint32_t, 8> sample_x{};
    std::array<uint32_t, 8> sample_y{};
    std::array<uint8_t, 8> sample_ref{};
    std::array<uint8_t, 8> sample_test{};
    uint32_t sample_count = 0;
};

void printRgbDiffStats(const std::vector<uint8_t>& ref,
                       const std::vector<uint8_t>& test,
                       uint32_t width,
                       uint32_t height) {
    if (ref.size() != test.size() || ref.empty() || width == 0 || height == 0) {
        std::cerr << "[RenderHalideDiff] skipped (invalid input sizes)\n";
        return;
    }

    DiffChannelStats stats[3];
    const size_t pixels = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixels; ++i) {
        const uint32_t x = static_cast<uint32_t>(i % width);
        const uint32_t y = static_cast<uint32_t>(i / width);
        const size_t base = i * 3;
        for (uint32_t c = 0; c < 3; ++c) {
            const int diff = static_cast<int>(test[base + c]) - static_cast<int>(ref[base + c]);
            const uint8_t abs_diff = static_cast<uint8_t>(std::abs(diff));
            stats[c].mae += static_cast<double>(abs_diff);
            if (abs_diff != 0) {
                stats[c].non_zero++;
                if (stats[c].sample_count < stats[c].sample_x.size()) {
                    const uint32_t s = stats[c].sample_count++;
                    stats[c].sample_x[s] = x;
                    stats[c].sample_y[s] = y;
                    stats[c].sample_ref[s] = ref[base + c];
                    stats[c].sample_test[s] = test[base + c];
                }
            }
            if (abs_diff > stats[c].max_abs) {
                stats[c].max_abs = abs_diff;
                stats[c].max_x = x;
                stats[c].max_y = y;
            }
        }
    }

    const char* names[3] = {"R", "G", "B"};
    for (uint32_t c = 0; c < 3; ++c) {
        stats[c].mae /= static_cast<double>(pixels);
        std::cerr << "[RenderHalideDiff] " << names[c]
                  << " mae=" << stats[c].mae
                  << " maxAbs=" << static_cast<int>(stats[c].max_abs)
                  << " at=(" << stats[c].max_x << "," << stats[c].max_y << ")"
                  << " nonZero=" << stats[c].non_zero
                  << "/" << pixels << "\n";
        for (uint32_t i = 0; i < stats[c].sample_count; ++i) {
            std::cerr << "  [RenderHalideDiffSample] " << names[c]
                      << " (" << stats[c].sample_x[i] << "," << stats[c].sample_y[i] << ")"
                      << " ref=" << static_cast<int>(stats[c].sample_ref[i])
                      << " test=" << static_cast<int>(stats[c].sample_test[i]) << "\n";
        }
    }
}

bool renderHalideDebugEnabled() {
    const char* v = std::getenv("DNG_RENDER_HALIDE_DEBUG");
    return v && v[0] && v[0] != '0';
}

bool renderHalideTimingEnabled() {
    const char* v = std::getenv("DNG_RENDER_HALIDE_TIMING");
    return v && v[0] && v[0] != '0';
}

bool renderHalideTryToneTailEnabled() {
    const char* v = std::getenv("DNG_RENDER_HALIDE_TRY_TONETAIL");
    return v && v[0] && v[0] != '0';
}

bool renderHalideTryFullEnabled() {
    const char* disable = std::getenv("DNG_RENDER_HALIDE_DISABLE_FULL");
    return !(disable && disable[0] && disable[0] != '0');
}

bool renderHalideForceFullKernelEnabled() {
    const char* v = std::getenv("DNG_RENDER_HALIDE_FORCE_FULL");
    return v && v[0] && v[0] != '0';
}

bool renderHalideBitExactModeEnabled() {
    const char* render_exact = std::getenv("DNG_RENDER_BIT_EXACT");
    if (render_exact) {
        return render_exact[0] && render_exact[0] != '0';
    }
    const char* warp_exact = std::getenv("DNG_WARP_BIT_EXACT");
    return warp_exact && warp_exact[0] && warp_exact[0] != '0';
}

bool renderLsbResearchEnabled() {
    const char* v = std::getenv("DNG_RENDER_LSB_RESEARCH");
    return v && v[0] && v[0] != '0';
}

bool renderTailResearchEnabled() {
    const char* v = std::getenv("DNG_RENDER_TAIL_RESEARCH");
    return v && v[0] && v[0] != '0';
}

float tableInterpSdk(const std::vector<float>& table, float x) {
    if (table.size() < 2) {
        return 0.0f;
    }
    const float xv = std::clamp(x, 0.0f, 1.0f);
    const float y = xv * static_cast<float>(dng_1d_table::kTableSize);
    int index = static_cast<int>(y);
    index = std::clamp(index, 0, static_cast<int>(table.size()) - 2);
    const float fract = y - static_cast<float>(index);
    return table[static_cast<size_t>(index)] * (1.0f - fract) +
           table[static_cast<size_t>(index + 1)] * fract;
}

float tableInterpHalideStyle(const std::vector<float>& table, float x) {
    if (table.size() < 2) {
        return 0.0f;
    }
    const float xv = std::clamp(x, 0.0f, 1.0f);
    const int max_idx = static_cast<int>(table.size()) - 2;
    const float y = xv * static_cast<float>(max_idx);
    int index = static_cast<int>(std::floor(y));
    index = std::clamp(index, 0, max_idx);
    const float fract = y - static_cast<float>(index);
    return table[static_cast<size_t>(index)] * (1.0f - fract) +
           table[static_cast<size_t>(index + 1)] * fract;
}

void applyRgbToneSdk(float r,
                     float g,
                     float b,
                     const std::vector<float>& tone_table,
                     float& rr,
                     float& gg,
                     float& bb) {
    auto rgbTone = [&](float ra, float ga, float ba, float& rra, float& gga, float& bba) {
        rra = tableInterpSdk(tone_table, ra);
        bba = tableInterpSdk(tone_table, ba);
        gga = bba + ((rra - bba) * (ga - ba) / (ra - ba));
    };

    if (r >= g) {
        if (g > b) {
            rgbTone(r, g, b, rr, gg, bb);
        } else if (b > r) {
            rgbTone(b, r, g, bb, rr, gg);
        } else if (b > g) {
            rgbTone(r, b, g, rr, bb, gg);
        } else {
            rr = tableInterpSdk(tone_table, r);
            gg = tableInterpSdk(tone_table, g);
            bb = gg;
        }
    } else {
        if (r >= b) {
            rgbTone(g, r, b, gg, rr, bb);
        } else if (b > g) {
            rgbTone(b, g, r, bb, gg, rr);
        } else {
            rgbTone(g, b, r, gg, bb, rr);
        }
    }
}

void applyRgbToneHalideStyle(float r,
                             float g,
                             float b,
                             const std::vector<float>& tone_table,
                             float& rr,
                             float& gg,
                             float& bb) {
    const float tr = tableInterpHalideStyle(tone_table, r);
    const float tg = tableInterpHalideStyle(tone_table, g);
    const float tb = tableInterpHalideStyle(tone_table, b);

    const float den1 = ((r >= g) && (g > b)) ? (r - b) : 1.0f;
    const float rr1 = tr;
    const float gg1 = tb + ((tr - tb) * (g - b) / den1);
    const float bb1 = tb;

    const float den2 = ((r >= g) && !(g > b) && (b > r)) ? (b - g) : 1.0f;
    const float bb2 = tb;
    const float gg2 = tg;
    const float rr2 = gg2 + ((bb2 - gg2) * (r - g) / den2);

    const float den3 = ((r >= g) && !(g > b) && !(b > r) && (b > g)) ? (r - g) : 1.0f;
    const float rr3 = tr;
    const float gg3 = tg;
    const float bb3 = gg3 + ((rr3 - gg3) * (b - g) / den3);

    const float rr4 = tr;
    const float gg4 = tg;
    const float bb4 = tg;

    const float den5 = (!(r >= g) && (r >= b)) ? (g - b) : 1.0f;
    const float gg5 = tg;
    const float bb5 = tb;
    const float rr5 = bb5 + ((gg5 - bb5) * (r - b) / den5);

    const float den6 = (!(r >= g) && !(r >= b) && (b > g)) ? (b - r) : 1.0f;
    const float bb6 = tb;
    const float rr6 = tr;
    const float gg6 = rr6 + ((bb6 - rr6) * (g - r) / den6);

    const float den7 = (!(r >= g) && !(r >= b) && !(b > g)) ? (g - r) : 1.0f;
    const float gg7 = tg;
    const float rr7 = tr;
    const float bb7 = rr7 + ((gg7 - rr7) * (b - r) / den7);

    const bool c1 = (r >= g) && (g > b);
    const bool c2 = (r >= g) && !(g > b) && (b > r);
    const bool c3 = (r >= g) && !(g > b) && !(b > r) && (b > g);
    const bool c4 = (r >= g) && !(g > b) && !(b > r) && !(b > g);
    const bool c5 = !(r >= g) && (r >= b);
    const bool c6 = !(r >= g) && !(r >= b) && (b > g);

    rr = c1 ? rr1 : c2 ? rr2 : c3 ? rr3 : c4 ? rr4 : c5 ? rr5 : c6 ? rr6 : rr7;
    gg = c1 ? gg1 : c2 ? gg2 : c3 ? gg3 : c4 ? gg4 : c5 ? gg5 : c6 ? gg6 : gg7;
    bb = c1 ? bb1 : c2 ? bb2 : c3 ? bb3 : c4 ? bb4 : c5 ? bb5 : c6 ? bb6 : bb7;
}

enum class MatrixReplayMode {
    SdkOrder,
    AssocAlt,
    FmaChain
};

float matrixRowDot(float r,
                   float g,
                   float b,
                   float m0,
                   float m1,
                   float m2,
                   MatrixReplayMode mode) {
    switch (mode) {
        case MatrixReplayMode::SdkOrder:
            return (m0 * r + m1 * g) + m2 * b;
        case MatrixReplayMode::AssocAlt:
            return m0 * r + (m1 * g + m2 * b);
        case MatrixReplayMode::FmaChain:
            return std::fmaf(m2, b, std::fmaf(m1, g, m0 * r));
    }
    return 0.0f;
}

void applyRgbToFinalMatrix(float r,
                           float g,
                           float b,
                           const float rgb_to_final[9],
                           MatrixReplayMode mode,
                           float& fr,
                           float& fg,
                           float& fb) {
    fr = std::clamp(matrixRowDot(r, g, b,
                                 rgb_to_final[0], rgb_to_final[1], rgb_to_final[2],
                                 mode),
                    0.0f, 1.0f);
    fg = std::clamp(matrixRowDot(r, g, b,
                                 rgb_to_final[3], rgb_to_final[4], rgb_to_final[5],
                                 mode),
                    0.0f, 1.0f);
    fb = std::clamp(matrixRowDot(r, g, b,
                                 rgb_to_final[6], rgb_to_final[7], rgb_to_final[8],
                                 mode),
                    0.0f, 1.0f);
}

uint8_t quantizeHalfUp(float v) {
    const float scaled = std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f;
    return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
}

uint8_t quantizeTrunc(float v) {
    const float scaled = std::clamp(v, 0.0f, 1.0f) * 255.0f;
    return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
}

void runLsbResearchReplay(const std::vector<float>& pre_tone_rgb,
                          uint32_t width,
                          uint32_t height,
                          const RenderParams& params,
                          const std::vector<uint8_t>& ref_rgb,
                          const std::vector<uint8_t>& test_rgb) {
    if (pre_tone_rgb.empty() ||
        params.tone_curve.size() < 2 ||
        params.encode_gamma.size() < 2 ||
        width == 0 ||
        height == 0) {
        return;
    }

    const std::array<std::pair<uint32_t, uint32_t>, 24> samples = {
        // R-channel diff samples
        std::make_pair(5338u, 17u),
        std::make_pair(112u, 26u),
        std::make_pair(3163u, 44u),
        std::make_pair(782u, 99u),
        std::make_pair(1017u, 102u),
        std::make_pair(1801u, 106u),
        std::make_pair(846u, 111u),
        std::make_pair(3966u, 158u),
        // G-channel diff samples
        std::make_pair(4773u, 49u),
        std::make_pair(3519u, 55u),
        std::make_pair(4199u, 88u),
        std::make_pair(1086u, 101u),
        std::make_pair(4485u, 126u),
        std::make_pair(4024u, 177u),
        std::make_pair(3118u, 179u),
        std::make_pair(5354u, 218u),
        // B-channel diff samples
        std::make_pair(4211u, 24u),
        std::make_pair(4944u, 42u),
        std::make_pair(2491u, 50u),
        std::make_pair(3393u, 76u),
        std::make_pair(1307u, 101u),
        std::make_pair(2799u, 131u),
        std::make_pair(3725u, 140u),
        std::make_pair(2740u, 218u),
    };

    float max_table_diff = 0.0f;
    float max_tone_diff = 0.0f;
    float max_matrix_assoc_diff = 0.0f;
    float max_matrix_fma_diff = 0.0f;
    float max_encode_interp_diff = 0.0f;
    size_t encode_diff_half_vs_trunc = 0;
    size_t ref_match_sdk_chain = 0;
    size_t ref_match_assoc_chain = 0;
    size_t ref_match_fma_chain = 0;
    size_t ref_match_fma_trunc_chain = 0;
    size_t test_match_sdk_chain = 0;
    size_t test_match_assoc_chain = 0;
    size_t test_match_fma_chain = 0;
    size_t test_match_fma_trunc_chain = 0;
    size_t compared_channels = 0;
    size_t tested = 0;

    const bool have_ref = ref_rgb.size() == static_cast<size_t>(width) * height * 3u;
    const bool have_test = test_rgb.size() == static_cast<size_t>(width) * height * 3u;

    for (const auto& [x, y] : samples) {
        if (x >= width || y >= height) {
            continue;
        }
        const size_t idx = (static_cast<size_t>(y) * width + x) * 3u;
        const float r = pre_tone_rgb[idx + 0];
        const float g = pre_tone_rgb[idx + 1];
        const float b = pre_tone_rgb[idx + 2];

        const float ti_sdk = tableInterpSdk(params.tone_curve, std::clamp(r, 0.0f, 1.0f));
        const float ti_hal = tableInterpHalideStyle(params.tone_curve, std::clamp(r, 0.0f, 1.0f));
        max_table_diff = std::max(max_table_diff, std::abs(ti_sdk - ti_hal));

        float rr_sdk = 0.0f, gg_sdk = 0.0f, bb_sdk = 0.0f;
        float rr_hal = 0.0f, gg_hal = 0.0f, bb_hal = 0.0f;
        applyRgbToneSdk(r, g, b, params.tone_curve, rr_sdk, gg_sdk, bb_sdk);
        applyRgbToneHalideStyle(r, g, b, params.tone_curve, rr_hal, gg_hal, bb_hal);
        max_tone_diff = std::max(max_tone_diff, std::abs(rr_sdk - rr_hal));
        max_tone_diff = std::max(max_tone_diff, std::abs(gg_sdk - gg_hal));
        max_tone_diff = std::max(max_tone_diff, std::abs(bb_sdk - bb_hal));

        float fr_sdk = 0.0f, fg_sdk = 0.0f, fb_sdk = 0.0f;
        float fr_assoc = 0.0f, fg_assoc = 0.0f, fb_assoc = 0.0f;
        float fr_fma = 0.0f, fg_fma = 0.0f, fb_fma = 0.0f;
        applyRgbToFinalMatrix(rr_sdk, gg_sdk, bb_sdk, params.rgb_to_final, MatrixReplayMode::SdkOrder, fr_sdk, fg_sdk, fb_sdk);
        applyRgbToFinalMatrix(rr_sdk, gg_sdk, bb_sdk, params.rgb_to_final, MatrixReplayMode::AssocAlt, fr_assoc, fg_assoc, fb_assoc);
        applyRgbToFinalMatrix(rr_sdk, gg_sdk, bb_sdk, params.rgb_to_final, MatrixReplayMode::FmaChain, fr_fma, fg_fma, fb_fma);

        max_matrix_assoc_diff = std::max(max_matrix_assoc_diff, std::abs(fr_assoc - fr_sdk));
        max_matrix_assoc_diff = std::max(max_matrix_assoc_diff, std::abs(fg_assoc - fg_sdk));
        max_matrix_assoc_diff = std::max(max_matrix_assoc_diff, std::abs(fb_assoc - fb_sdk));
        max_matrix_fma_diff = std::max(max_matrix_fma_diff, std::abs(fr_fma - fr_sdk));
        max_matrix_fma_diff = std::max(max_matrix_fma_diff, std::abs(fg_fma - fg_sdk));
        max_matrix_fma_diff = std::max(max_matrix_fma_diff, std::abs(fb_fma - fb_sdk));

        const float enc_sdk_r = tableInterpSdk(params.encode_gamma, fr_sdk);
        const float enc_sdk_g = tableInterpSdk(params.encode_gamma, fg_sdk);
        const float enc_sdk_b = tableInterpSdk(params.encode_gamma, fb_sdk);
        const float enc_hal_r = tableInterpHalideStyle(params.encode_gamma, fr_sdk);
        const float enc_hal_g = tableInterpHalideStyle(params.encode_gamma, fg_sdk);
        const float enc_hal_b = tableInterpHalideStyle(params.encode_gamma, fb_sdk);
        max_encode_interp_diff = std::max(max_encode_interp_diff, std::abs(enc_sdk_r - enc_hal_r));
        max_encode_interp_diff = std::max(max_encode_interp_diff, std::abs(enc_sdk_g - enc_hal_g));
        max_encode_interp_diff = std::max(max_encode_interp_diff, std::abs(enc_sdk_b - enc_hal_b));

        const uint8_t sdk_u8_r = quantizeHalfUp(enc_sdk_r);
        const uint8_t sdk_u8_g = quantizeHalfUp(enc_sdk_g);
        const uint8_t sdk_u8_b = quantizeHalfUp(enc_sdk_b);
        const uint8_t assoc_u8_r = quantizeHalfUp(tableInterpSdk(params.encode_gamma, fr_assoc));
        const uint8_t assoc_u8_g = quantizeHalfUp(tableInterpSdk(params.encode_gamma, fg_assoc));
        const uint8_t assoc_u8_b = quantizeHalfUp(tableInterpSdk(params.encode_gamma, fb_assoc));
        const uint8_t fma_u8_r = quantizeHalfUp(tableInterpSdk(params.encode_gamma, fr_fma));
        const uint8_t fma_u8_g = quantizeHalfUp(tableInterpSdk(params.encode_gamma, fg_fma));
        const uint8_t fma_u8_b = quantizeHalfUp(tableInterpSdk(params.encode_gamma, fb_fma));
        const uint8_t fma_trunc_u8_r = quantizeTrunc(tableInterpSdk(params.encode_gamma, fr_fma));
        const uint8_t fma_trunc_u8_g = quantizeTrunc(tableInterpSdk(params.encode_gamma, fg_fma));
        const uint8_t fma_trunc_u8_b = quantizeTrunc(tableInterpSdk(params.encode_gamma, fb_fma));

        encode_diff_half_vs_trunc += (fma_u8_r != fma_trunc_u8_r) ? 1u : 0u;
        encode_diff_half_vs_trunc += (fma_u8_g != fma_trunc_u8_g) ? 1u : 0u;
        encode_diff_half_vs_trunc += (fma_u8_b != fma_trunc_u8_b) ? 1u : 0u;

        int ref_r = -1, ref_g = -1, ref_b = -1;
        int test_r = -1, test_g = -1, test_b = -1;
        if (have_ref) {
            ref_r = static_cast<int>(ref_rgb[idx + 0]);
            ref_g = static_cast<int>(ref_rgb[idx + 1]);
            ref_b = static_cast<int>(ref_rgb[idx + 2]);
            ref_match_sdk_chain += (sdk_u8_r == ref_rgb[idx + 0]) ? 1u : 0u;
            ref_match_sdk_chain += (sdk_u8_g == ref_rgb[idx + 1]) ? 1u : 0u;
            ref_match_sdk_chain += (sdk_u8_b == ref_rgb[idx + 2]) ? 1u : 0u;
            ref_match_assoc_chain += (assoc_u8_r == ref_rgb[idx + 0]) ? 1u : 0u;
            ref_match_assoc_chain += (assoc_u8_g == ref_rgb[idx + 1]) ? 1u : 0u;
            ref_match_assoc_chain += (assoc_u8_b == ref_rgb[idx + 2]) ? 1u : 0u;
            ref_match_fma_chain += (fma_u8_r == ref_rgb[idx + 0]) ? 1u : 0u;
            ref_match_fma_chain += (fma_u8_g == ref_rgb[idx + 1]) ? 1u : 0u;
            ref_match_fma_chain += (fma_u8_b == ref_rgb[idx + 2]) ? 1u : 0u;
            ref_match_fma_trunc_chain += (fma_trunc_u8_r == ref_rgb[idx + 0]) ? 1u : 0u;
            ref_match_fma_trunc_chain += (fma_trunc_u8_g == ref_rgb[idx + 1]) ? 1u : 0u;
            ref_match_fma_trunc_chain += (fma_trunc_u8_b == ref_rgb[idx + 2]) ? 1u : 0u;
        }
        if (have_test) {
            test_r = static_cast<int>(test_rgb[idx + 0]);
            test_g = static_cast<int>(test_rgb[idx + 1]);
            test_b = static_cast<int>(test_rgb[idx + 2]);
            test_match_sdk_chain += (sdk_u8_r == test_rgb[idx + 0]) ? 1u : 0u;
            test_match_sdk_chain += (sdk_u8_g == test_rgb[idx + 1]) ? 1u : 0u;
            test_match_sdk_chain += (sdk_u8_b == test_rgb[idx + 2]) ? 1u : 0u;
            test_match_assoc_chain += (assoc_u8_r == test_rgb[idx + 0]) ? 1u : 0u;
            test_match_assoc_chain += (assoc_u8_g == test_rgb[idx + 1]) ? 1u : 0u;
            test_match_assoc_chain += (assoc_u8_b == test_rgb[idx + 2]) ? 1u : 0u;
            test_match_fma_chain += (fma_u8_r == test_rgb[idx + 0]) ? 1u : 0u;
            test_match_fma_chain += (fma_u8_g == test_rgb[idx + 1]) ? 1u : 0u;
            test_match_fma_chain += (fma_u8_b == test_rgb[idx + 2]) ? 1u : 0u;
            test_match_fma_trunc_chain += (fma_trunc_u8_r == test_rgb[idx + 0]) ? 1u : 0u;
            test_match_fma_trunc_chain += (fma_trunc_u8_g == test_rgb[idx + 1]) ? 1u : 0u;
            test_match_fma_trunc_chain += (fma_trunc_u8_b == test_rgb[idx + 2]) ? 1u : 0u;
        }

        compared_channels += 3;
        std::cerr << "[LSBResearch][MatrixEncodeReplay] (" << x << "," << y << ")"
                  << " ref=(" << ref_r << "," << ref_g << "," << ref_b << ")"
                  << " test=(" << test_r << "," << test_g << "," << test_b << ")"
                  << " sdk=(" << static_cast<int>(sdk_u8_r) << "," << static_cast<int>(sdk_u8_g)
                  << "," << static_cast<int>(sdk_u8_b) << ")"
                  << " assoc=(" << static_cast<int>(assoc_u8_r) << "," << static_cast<int>(assoc_u8_g)
                  << "," << static_cast<int>(assoc_u8_b) << ")"
                  << " fma=(" << static_cast<int>(fma_u8_r) << "," << static_cast<int>(fma_u8_g)
                  << "," << static_cast<int>(fma_u8_b) << ")"
                  << " fma_trunc=(" << static_cast<int>(fma_trunc_u8_r) << "," << static_cast<int>(fma_trunc_u8_g)
                  << "," << static_cast<int>(fma_trunc_u8_b) << ")\n";

        tested++;
    }

    // Synthetic boundary sweep around 1D table bin edges.
    float max_table_boundary_diff = 0.0f;
    size_t boundary_tested = 0;
    constexpr float eps = 1.0e-7f;
    for (int i = 0; i <= static_cast<int>(dng_1d_table::kTableSize); i += 257) {
        const float base = static_cast<float>(i) / static_cast<float>(dng_1d_table::kTableSize);
        const std::array<float, 5> probes = {
            std::clamp(base - eps, 0.0f, 1.0f),
            base,
            std::clamp(base + eps, 0.0f, 1.0f),
            std::nextafter(base, 0.0f),
            std::nextafter(base, 1.0f),
        };
        for (float x : probes) {
            const float sdk = tableInterpSdk(params.tone_curve, x);
            const float hal = tableInterpHalideStyle(params.tone_curve, x);
            max_table_boundary_diff = std::max(max_table_boundary_diff, std::abs(sdk - hal));
            boundary_tested++;
        }
    }

    std::cerr << "[LSBResearch][TableInterp] tested=" << tested
              << " maxAbsDiff=" << max_table_diff << "\n";
    std::cerr << "[LSBResearch][TableInterpBoundary] tested=" << boundary_tested
              << " maxAbsDiff=" << max_table_boundary_diff << "\n";
    std::cerr << "[LSBResearch][ToneReplay] tested=" << tested
              << " maxAbsDiff=" << max_tone_diff << "\n";
    std::cerr << "[LSBResearch][MatrixReplay] tested=" << tested
              << " maxAssocDiff=" << max_matrix_assoc_diff
              << " maxFmaDiff=" << max_matrix_fma_diff << "\n";
    std::cerr << "[LSBResearch][EncodeReplay] tested=" << tested
              << " maxTableInterpDiff=" << max_encode_interp_diff
              << " halfVsTruncDiffChannels=" << encode_diff_half_vs_trunc
              << "/" << compared_channels << "\n";
    if (have_ref) {
        std::cerr << "[LSBResearch][ReplayMatchRef] sdk=" << ref_match_sdk_chain
                  << " assoc=" << ref_match_assoc_chain
                  << " fma=" << ref_match_fma_chain
                  << " fma_trunc=" << ref_match_fma_trunc_chain
                  << " / " << compared_channels << "\n";
    }
    if (have_test) {
        std::cerr << "[LSBResearch][ReplayMatchTest] sdk=" << test_match_sdk_chain
                  << " assoc=" << test_match_assoc_chain
                  << " fma=" << test_match_fma_chain
                  << " fma_trunc=" << test_match_fma_trunc_chain
                  << " / " << compared_channels << "\n";
    }
}

// =============================================================================
// Step 1 — Per-pixel stage divergence dumper (Step 6.4.4 Round 2)
// =============================================================================
//
//  DNG_RENDER_STAGE_DIVERGENCE=1 enables this.
//
//  For each pixel that differs between the Halide kernel and SDK reference,
//  runs BOTH the SDK RefBaseline* chain and a scalar replica of the exact
//  Halide kernel expressions (DngRenderGenerator.cpp), stage by stage.
//
//  Prints per-stage float values for every diff pixel coordinate listed in
//  kDivergenceSampleCoords[].  First divergent stage is tagged with [FIRST_DIV].
//
//  Output format per pixel:
//    [StageDiverge] (x,y) ref=N test=N  [FIRST_DIV: stage_name]
//      STAGE_ABC      sdk_r=0.xxxxx sdk_g=0.xxxxx sdk_b=0.xxxxx  hal_r=0.xxxxx hal_g=0.xxxxx hal_b=0.xxxxx  MATCH/1ULP/ERR
//      STAGE_HSAT     ... (same)
//      STAGE_EXPOSURE ...
//      STAGE_LOOK     ...
//      STAGE_TONE     ...
//      STAGE_MATRIX   ...
//      STAGE_ENCODE   ... (float pre-quantize)
//      STAGE_FINAL    N (uint8)
//
//  This is the highest-resolution diagnostic tool for locating where the
//  ±1 LSB residual originates.
//
// =============================================================================

namespace stage_divergence {

// ---- Scalar replica of Halide kernel's 1-D table interpolation ----
static inline float halTableInterp(const float* table, int table_size, float xv) {
    xv = std::clamp(xv, 0.0f, 1.0f);
    int max_idx = table_size - 2;
    float y = xv * static_cast<float>(max_idx);
    int index = static_cast<int>(std::floor(y));
    index = std::clamp(index, 0, max_idx);
    float frac = y - static_cast<float>(index);
    return table[index] * (1.0f - frac) + table[index + 1] * frac;
}

// ---- Scalar replica of Halide rgb_to_hsv ----
static inline void halRgbToHsv(float r, float g, float b,
                               float& h, float& s, float& v) {
    v = std::max(r, std::max(g, b));
    float mn = std::min(r, std::min(g, b));
    float gap = v - mn;

    float gap_den = (gap > 0.0f) ? gap : 1.0f;
    float h_r = (g - b) / gap_den;
    h_r = (h_r < 0.0f) ? h_r + 6.0f : h_r;
    float h_g = 2.0f + (b - r) / gap_den;
    float h_b = 4.0f + (r - g) / gap_den;

    h = (gap > 0.0f)
            ? ((r == v) ? h_r
              : ((g == v) ? h_g : h_b))
            : 0.0f;
    s = (gap > 0.0f) ? gap / v : 0.0f;
}

// ---- Scalar replica of Halide hsv_to_rgb ----
static inline void halHsvToRgb(float h, float s, float v,
                                float& r, float& g, float& b) {
    bool use_sat = (s > 0.0f);
    float hh = (h < 0.0f) ? h + 6.0f : h;
    if (hh >= 6.0f) hh -= 6.0f;
    int i = static_cast<int>(hh);
    float f = hh - static_cast<float>(i);
    float p = v * (1.0f - s);
    float q = v * (1.0f - s * f);
    float t = v * (1.0f - s * (1.0f - f));
    i = std::clamp(i, 0, 5);

    float r_hsv = (i == 0) ? v : (i == 1) ? q : (i == 2) ? p : (i == 3) ? p : (i == 4) ? t : v;
    float g_hsv = (i == 0) ? t : (i == 1) ? v : (i == 2) ? v : (i == 3) ? q : (i == 4) ? p : p;
    float b_hsv = (i == 0) ? p : (i == 1) ? p : (i == 2) ? t : (i == 3) ? v : (i == 4) ? v : q;

    r = use_sat ? r_hsv : v;
    g = use_sat ? g_hsv : v;
    b = use_sat ? b_hsv : v;
}

// ---- Scalar replica of Halide sample_hsv_map (trilerp + encode/decode table) ----
static inline void halSampleHsvMap(const float* huesat_table, int huesat_table_size,
                                  const float* encode_table, int encode_size,
                                  const float* decode_table, int decode_size,
                                  int hue_div, int sat_div, int val_div,
                                  int has_table, int has_encoding,
                                  float r, float g, float b,
                                  float& out_r, float& out_g, float& out_b) {
    float h, s, v;
    halRgbToHsv(r, g, b, h, s, v);

    int hue_div_s = std::max(hue_div, 2);
    int sat_div_s = std::max(sat_div, 2);
    int val_div_s = std::max(val_div, 1);

    float hue_scale = static_cast<float>(hue_div_s) * (1.0f / 6.0f);
    float sat_scale = static_cast<float>(sat_div_s - 1);
    float val_scale = static_cast<float>(val_div_s - 1);
    int max_hue_index0 = hue_div_s - 1;
    int max_sat_index0 = sat_div_s - 2;
    int max_val_index0 = val_div_s - 2;
    int hue_step = sat_div_s;
    int val_step = hue_div_s * sat_div_s;

    bool use_encode = (has_encoding != 0) && (val_div_s >= 2);
    float v_encoded0 = v;
    float xv_v = std::clamp(v, 0.0f, 1.0f);
    int max_idx_enc = encode_size - 2;
    float y_enc = xv_v * static_cast<float>(max_idx_enc);
    int idx_enc = static_cast<int>(std::floor(y_enc));
    idx_enc = std::clamp(idx_enc, 0, max_idx_enc);
    float frac_enc = y_enc - static_cast<float>(idx_enc);
    float v_encoded = use_encode
            ? encode_table[idx_enc] * (1.0f - frac_enc) + encode_table[idx_enc + 1] * frac_enc
            : v_encoded0;

    float h_scaled = h * hue_scale;
    float s_scaled = s * sat_scale;
    float v_scaled = v_encoded * val_scale;

    int h_index0_raw = static_cast<int>(std::floor(h_scaled));
    int s_index0 = std::clamp(static_cast<int>(std::floor(s_scaled)), 0, max_sat_index0);
    int v_index0 = std::clamp(static_cast<int>(std::floor(v_scaled)), 0, max_val_index0);

    int h_index0 = std::clamp(h_index0_raw, 0, max_hue_index0);
    int h_index1 = (h_index0_raw >= max_hue_index0) ? 0 : h_index0 + 1;

    float h_fract1 = h_scaled - static_cast<float>(h_index0);
    float s_fract1 = s_scaled - static_cast<float>(s_index0);
    float v_fract1 = v_scaled - static_cast<float>(v_index0);
    float h_fract0 = 1.0f - h_fract1;
    float s_fract0 = 1.0f - s_fract1;
    float v_fract0 = 1.0f - v_fract1;

    auto tval = [&](int idx, int comp) -> float {
        idx = std::clamp(idx, 0, huesat_table_size - 1);
        return huesat_table[idx * 3 + comp];
    };

    int base2d0 = h_index0 * hue_step + s_index0;
    int base2d1 = h_index1 * hue_step + s_index0;

    float hs_hue0 = h_fract0 * tval(base2d0, 0) + h_fract1 * tval(base2d1, 0);
    float hs_sat0 = h_fract0 * tval(base2d0, 1) + h_fract1 * tval(base2d1, 1);
    float hs_val0 = h_fract0 * tval(base2d0, 2) + h_fract1 * tval(base2d1, 2);

    float hs_hue1 = h_fract0 * tval(base2d0 + 1, 0) + h_fract1 * tval(base2d1 + 1, 0);
    float hs_sat1 = h_fract0 * tval(base2d0 + 1, 1) + h_fract1 * tval(base2d1 + 1, 1);
    float hs_val1 = h_fract0 * tval(base2d0 + 1, 2) + h_fract1 * tval(base2d1 + 1, 2);

    float hue_shift_2d = s_fract0 * hs_hue0 + s_fract1 * hs_hue1;
    float sat_scale_2d = s_fract0 * hs_sat0 + s_fract1 * hs_sat1;
    float val_scale_2d = s_fract0 * hs_val0 + s_fract1 * hs_val1;

    int base3d00 = v_index0 * val_step + h_index0 * hue_step + s_index0;
    int base3d01 = v_index0 * val_step + h_index1 * hue_step + s_index0;
    int base3d10 = base3d00 + val_step;
    int base3d11 = base3d01 + val_step;

    auto lerp_hv = [&](int comp, int off) -> float {
        return v_fract0 * (h_fract0 * tval(base3d00 + off, comp) + h_fract1 * tval(base3d01 + off, comp)) +
               v_fract1 * (h_fract0 * tval(base3d10 + off, comp) + h_fract1 * tval(base3d11 + off, comp));
    };

    float hue_shift_3d = s_fract0 * lerp_hv(0, 0) + s_fract1 * lerp_hv(0, 1);
    float sat_scale_3d = s_fract0 * lerp_hv(1, 0) + s_fract1 * lerp_hv(1, 1);
    float val_scale_3d = s_fract0 * lerp_hv(2, 0) + s_fract1 * lerp_hv(2, 1);

    bool use_2d = (val_div_s < 2);
    float hue_shift = use_2d ? hue_shift_2d : hue_shift_3d;
    float sat_mult = use_2d ? sat_scale_2d : sat_scale_3d;
    float val_mult = use_2d ? val_scale_2d : val_scale_3d;

    float hh = h + hue_shift * (6.0f / 360.0f);
    float ss = std::min(s * sat_mult, 1.0f);
    float ve = std::clamp(v_encoded * val_mult, 0.0f, 1.0f);

    // decode table interpolation
    float xv_ve = std::clamp(ve, 0.0f, 1.0f);
    int max_idx_dec = decode_size - 2;
    float y_dec = xv_ve * static_cast<float>(max_idx_dec);
    int idx_dec = static_cast<int>(std::floor(y_dec));
    idx_dec = std::clamp(idx_dec, 0, max_idx_dec);
    float frac_dec = y_dec - static_cast<float>(idx_dec);
    float vv = use_encode
            ? decode_table[idx_dec] * (1.0f - frac_dec) + decode_table[idx_dec + 1] * frac_dec
            : ve;

    float rr, gg, bb;
    halHsvToRgb(hh, ss, vv, rr, gg, bb);

    out_r = (has_table != 0) ? rr : r;
    out_g = (has_table != 0) ? gg : g;
    out_b = (has_table != 0) ? bb : b;
}

// ---- Scalar replica of Halide rgb_tone (7-case RGBTone) ----
static inline void halRgbTone(float r, float g, float b,
                             const float* tone_table, int tone_size,
                             float& out_r, float& out_g, float& out_b) {
    float tr = halTableInterp(tone_table, tone_size, r);
    float tg = halTableInterp(tone_table, tone_size, g);
    float tb = halTableInterp(tone_table, tone_size, b);

    // Case 1: r >= g > b
    float den1 = ((r >= g) && (g > b)) ? (r - b) : 1.0f;
    float gg1 = tb + ((tr - tb) * (g - b) / den1);

    // Case 2: b > r >= g
    float den2 = ((r >= g) && !(g > b) && (b > r)) ? (b - g) : 1.0f;
    float rr2 = tg + ((tb - tg) * (r - g) / den2);

    // Case 3: r >= b > g
    float den3 = ((r >= g) && !(g > b) && !(b > r) && (b > g)) ? (r - g) : 1.0f;
    float bb3 = tg + ((tr - tg) * (b - g) / den3);

    // Case 5: g > r >= b
    float den5 = (!(r >= g) && (r >= b)) ? (g - b) : 1.0f;
    float rr5 = tb + ((tg - tb) * (r - b) / den5);

    // Case 6: b > g > r
    float den6 = (!(r >= g) && !(r >= b) && (b > g)) ? (b - r) : 1.0f;
    float rr6 = tr + ((tb - tr) * (g - r) / den6);

    // Case 7: g >= b > r
    float den7 = (!(r >= g) && !(r >= b) && !(b > g)) ? (g - r) : 1.0f;
    float bb7 = tr + ((tg - tr) * (b - r) / den7);

    bool c1 = (r >= g) && (g > b);
    bool c2 = (r >= g) && !(g > b) && (b > r);
    bool c3 = (r >= g) && !(g > b) && !(b > r) && (b > g);
    bool c4 = (r >= g) && !(g > b) && !(b > r) && !(b > g);
    bool c5 = !(r >= g) && (r >= b);
    bool c6 = !(r >= g) && !(r >= b) && (b > g);

    out_r = c1 ? tr : (c2 ? rr2 : (c3 ? tr : (c4 ? tr : (c5 ? rr5 : (c6 ? rr6 : tr)))));
    out_g = c1 ? gg1 : (c2 ? tg : (c3 ? tg : (c4 ? tg : (c5 ? tg : (c6 ? tg : tg)))));
    out_b = c1 ? tb : (c2 ? tb : (c3 ? bb3 : (c4 ? tg : (c5 ? tb : (c6 ? tb : bb7)))));
}

// ---- Float equality check: MATCH, 1ULP, or ERR ----
static const char* floatDiffTag(float a, float b) {
    float diff = std::abs(a - b);
    if (diff == 0.0f) return "MATCH";
    if (diff <= 1.19209290e-7f) return "1ULP";   // single-precision ULP threshold (~1e-7)
    return "ERR";
}

struct PerStageValues {
    // [sdk_r, sdk_g, sdk_b, hal_r, hal_g, hal_b]
    float sdk[3] = {0,0,0};
    float hal[3] = {0,0,0};
};

enum { STAGE_ABC = 0, STAGE_HSAT, STAGE_EXP, STAGE_LOOK, STAGE_TONE, STAGE_MATRIX, STAGE_ENCODE, STAGE_COUNT };
static const char* kStageName[STAGE_COUNT] = {
    "ABC", "HSAT", "EXPOSURE", "LOOK", "TONE", "MATRIX", "ENCODE"
};

// ---- Per-pixel per-stage comparison ----
static void comparePerStage(uint32_t px, uint32_t py,
                            float s_r, float s_g, float s_b,
                            const RenderParams& params,
                            const uint8_t* ref_full_rgb,
                            const uint8_t* test_full_rgb,
                            uint32_t width, uint32_t height) {
    size_t idx = (static_cast<size_t>(py) * width + static_cast<size_t>(px)) * 3u;
    int ref_r = static_cast<int>(ref_full_rgb[idx + 0]);
    int ref_g = static_cast<int>(ref_full_rgb[idx + 1]);
    int ref_b = static_cast<int>(ref_full_rgb[idx + 2]);
    int test_r = static_cast<int>(test_full_rgb[idx + 0]);
    int test_g = static_cast<int>(test_full_rgb[idx + 1]);
    int test_b = static_cast<int>(test_full_rgb[idx + 2]);

    std::cerr << "[StageDiverge] (" << px << "," << py << ")"
              << " ref=(" << ref_r << "," << ref_g << "," << ref_b << ")"
              << " test=(" << test_r << "," << test_g << "," << test_b << ")\n";

    // ---- SDK reference path: pre_tone_rgb from runRenderPrefixToPreTone ----
    // We re-run just this pixel through the SDK reference chain.
    // sdk_abc = RefBaselineABCtoRGB(clip(A,B,C), cameraWhite, cameraToRGB)
    float sdk_abc_r = std::min(s_r, params.camera_white[0]) * params.camera_to_rgb[0]
                    + std::min(s_g, params.camera_white[1]) * params.camera_to_rgb[1]
                    + std::min(s_b, params.camera_white[2]) * params.camera_to_rgb[2];
    float sdk_abc_g = std::min(s_r, params.camera_white[0]) * params.camera_to_rgb[3]
                    + std::min(s_g, params.camera_white[1]) * params.camera_to_rgb[4]
                    + std::min(s_b, params.camera_white[2]) * params.camera_to_rgb[5];
    float sdk_abc_b = std::min(s_r, params.camera_white[0]) * params.camera_to_rgb[6]
                    + std::min(s_g, params.camera_white[1]) * params.camera_to_rgb[7]
                    + std::min(s_b, params.camera_white[2]) * params.camera_to_rgb[8];
    sdk_abc_r = std::clamp(sdk_abc_r, 0.0f, 1.0f);
    sdk_abc_g = std::clamp(sdk_abc_g, 0.0f, 1.0f);
    sdk_abc_b = std::clamp(sdk_abc_b, 0.0f, 1.0f);

    float sdk_e_r = halTableInterp(params.exp_ramp.data(),
                                   static_cast<int>(params.exp_ramp.size()), sdk_abc_r);
    float sdk_e_g = halTableInterp(params.exp_ramp.data(),
                                   static_cast<int>(params.exp_ramp.size()), sdk_abc_g);
    float sdk_e_b = halTableInterp(params.exp_ramp.data(),
                                   static_cast<int>(params.exp_ramp.size()), sdk_abc_b);

    // ---- Halide-style path: scalar replica of DngRenderGenerator.cpp ----
    // Halide abc: wb_r = min(s_r, camera_white)
    float hal_wb_r = std::min(s_r, params.camera_white[0]);
    float hal_wb_g = std::min(s_g, params.camera_white[1]);
    float hal_wb_b = std::min(s_b, params.camera_white[2]);
    // Halide abc matrix (same formula but different accumulation order)
    float hal_abc_r = (hal_wb_r * params.camera_to_rgb[0] + hal_wb_g * params.camera_to_rgb[1])
                    + hal_wb_b * params.camera_to_rgb[2];
    float hal_abc_g = (hal_wb_r * params.camera_to_rgb[3] + hal_wb_g * params.camera_to_rgb[4])
                    + hal_wb_b * params.camera_to_rgb[5];
    float hal_abc_b = (hal_wb_r * params.camera_to_rgb[6] + hal_wb_g * params.camera_to_rgb[7])
                    + hal_wb_b * params.camera_to_rgb[8];
    hal_abc_r = std::clamp(hal_abc_r, 0.0f, 1.0f);
    hal_abc_g = std::clamp(hal_abc_g, 0.0f, 1.0f);
    hal_abc_b = std::clamp(hal_abc_b, 0.0f, 1.0f);

    float hal_e_r = halTableInterp(params.exp_ramp.data(),
                                   static_cast<int>(params.exp_ramp.size()), hal_abc_r);
    float hal_e_g = halTableInterp(params.exp_ramp.data(),
                                   static_cast<int>(params.exp_ramp.size()), hal_abc_g);
    float hal_e_b = halTableInterp(params.exp_ramp.data(),
                                   static_cast<int>(params.exp_ramp.size()), hal_abc_b);

    // ---- Stage-by-stage printing ----
    PerStageValues sv[STAGE_COUNT];

    // ABC
    sv[STAGE_ABC].sdk[0] = sdk_abc_r; sv[STAGE_ABC].sdk[1] = sdk_abc_g; sv[STAGE_ABC].sdk[2] = sdk_abc_b;
    sv[STAGE_ABC].hal[0] = hal_abc_r; sv[STAGE_ABC].hal[1] = hal_abc_g; sv[STAGE_ABC].hal[2] = hal_abc_b;

    // EXPOSURE (last common intermediate before HSAT divergence)
    sv[STAGE_EXP].sdk[0] = sdk_e_r; sv[STAGE_EXP].sdk[1] = sdk_e_g; sv[STAGE_EXP].sdk[2] = sdk_e_b;
    sv[STAGE_EXP].hal[0] = hal_e_r; sv[STAGE_EXP].hal[1] = hal_e_g; sv[STAGE_EXP].hal[2] = hal_e_b;

    const char* first_div = nullptr;
    for (int s = 0; s < STAGE_COUNT; s++) {
        bool match = (std::abs(sv[s].sdk[0] - sv[s].hal[0]) <= 1.19209290e-7f) &&
                     (std::abs(sv[s].sdk[1] - sv[s].hal[1]) <= 1.19209290e-7f) &&
                     (std::abs(sv[s].sdk[2] - sv[s].hal[2]) <= 1.19209290e-7f);
        if (!match && first_div == nullptr) {
            first_div = kStageName[s];
        }
        if (s == STAGE_HSAT || s == STAGE_LOOK || s == STAGE_TONE ||
            s == STAGE_MATRIX || s == STAGE_ENCODE) {
            // These are "placeholder" entries; only print key stages
            continue;
        }
        std::cerr << "  " << kStageName[s]
                  << "  sdk=(" << sv[s].sdk[0] << "," << sv[s].sdk[1] << "," << sv[s].sdk[2] << ")"
                  << "  hal=(" << sv[s].hal[0] << "," << sv[s].hal[1] << "," << sv[s].hal[2] << ")"
                  << "  " << (match ? "MATCH" : "DIVERGE")
                  << "\n";
    }

    if (first_div) {
        std::cerr << "  [FIRST_DIV: " << first_div << "]\n";
    } else {
        std::cerr << "  [FIRST_DIV: none in pre-ABC — diff must be in HSAT/Look/Tone/Matrix/Encode]\n";
    }

    // Print matrix + encode values separately (derived from exp stage for context)
    // Matrix (SDK order)
    float sdk_f_r = ((sdk_e_r * params.rgb_to_final[0] + sdk_e_g * params.rgb_to_final[1])
                     + sdk_e_b * params.rgb_to_final[2]);
    float sdk_f_g = ((sdk_e_r * params.rgb_to_final[3] + sdk_e_g * params.rgb_to_final[4])
                     + sdk_e_b * params.rgb_to_final[5]);
    float sdk_f_b = ((sdk_e_r * params.rgb_to_final[6] + sdk_e_g * params.rgb_to_final[7])
                     + sdk_e_b * params.rgb_to_final[8]);
    float hal_f_r = ((hal_e_r * params.rgb_to_final[0] + hal_e_g * params.rgb_to_final[1])
                     + hal_e_b * params.rgb_to_final[2]);
    float hal_f_g = ((hal_e_r * params.rgb_to_final[3] + hal_e_g * params.rgb_to_final[4])
                     + hal_e_b * params.rgb_to_final[5]);
    float hal_f_b = ((hal_e_r * params.rgb_to_final[6] + hal_e_g * params.rgb_to_final[7])
                     + hal_e_b * params.rgb_to_final[8]);

    std::cerr << "  MATRIX"
              << "  sdk=(" << std::clamp(sdk_f_r,0.0f,1.0f) << "," << std::clamp(sdk_f_g,0.0f,1.0f)
              << "," << std::clamp(sdk_f_b,0.0f,1.0f) << ")"
              << "  hal=(" << std::clamp(hal_f_r,0.0f,1.0f) << "," << std::clamp(hal_f_g,0.0f,1.0f)
              << "," << std::clamp(hal_f_b,0.0f,1.0f) << ")"
              << "  " << floatDiffTag(sdk_f_r, hal_f_r) << "/" << floatDiffTag(sdk_f_g, hal_f_g)
              << "/" << floatDiffTag(sdk_f_b, hal_f_b) << "\n";

    // Encode (pre-quantize, using hal-style table interp)
    float sdk_enc_r = halTableInterp(params.encode_gamma.data(),
                                     static_cast<int>(params.encode_gamma.size()),
                                     std::clamp(sdk_f_r, 0.0f, 1.0f));
    float sdk_enc_g = halTableInterp(params.encode_gamma.data(),
                                     static_cast<int>(params.encode_gamma.size()),
                                     std::clamp(sdk_f_g, 0.0f, 1.0f));
    float sdk_enc_b = halTableInterp(params.encode_gamma.data(),
                                     static_cast<int>(params.encode_gamma.size()),
                                     std::clamp(sdk_f_b, 0.0f, 1.0f));
    float hal_enc_r = halTableInterp(params.encode_gamma.data(),
                                     static_cast<int>(params.encode_gamma.size()),
                                     std::clamp(hal_f_r, 0.0f, 1.0f));
    float hal_enc_g = halTableInterp(params.encode_gamma.data(),
                                     static_cast<int>(params.encode_gamma.size()),
                                     std::clamp(hal_f_g, 0.0f, 1.0f));
    float hal_enc_b = halTableInterp(params.encode_gamma.data(),
                                     static_cast<int>(params.encode_gamma.size()),
                                     std::clamp(hal_f_b, 0.0f, 1.0f));

    std::cerr << "  ENCODE(float)"
              << "  sdk=(" << sdk_enc_r << "," << sdk_enc_g << "," << sdk_enc_b << ")"
              << "  hal=(" << hal_enc_r << "," << hal_enc_g << "," << hal_enc_b << ")"
              << "  " << floatDiffTag(sdk_enc_r, hal_enc_r) << "/" << floatDiffTag(sdk_enc_g, hal_enc_g)
              << "/" << floatDiffTag(sdk_enc_b, hal_enc_b) << "\n";

    // Final uint8
    std::cerr << "  FINAL(uint8)"
              << "  ref=(" << ref_r << "," << ref_g << "," << ref_b << ")"
              << "  test=(" << test_r << "," << test_g << "," << test_b << ")"
              << "  " << ((ref_r==test_r) ? "MATCH" : "DIVERGE") << "\n";

    // ---- Full pipeline reconstruction (for quantization analysis) ----
    // Trace the COMPLETE chain from the raw Bayer inputs sx/sy/sz:
    //   raw → ABC → EXPOSURE → HSAT → LOOK → TONE → MATRIX → ENCODE → QUANT
    //
    // Key insight: s_r/s_g/s_b (from pre_tone_rgb) is AFTER TONE.
    // s_r/s_g/s_b ≠ hal_e_r/g/b (EXPOSURE output).
    // The FULL scalar path from raw pixels must go through:
    //   sx/sy/sz → ABC → EXPOSURE → TONE → MATRIX → ENCODE → QUANT
    // NOT from hal_e_* → MATRIX → ENCODE → QUANT (that uses wrong input).

    // Verify: s_r is post-TONE, hal_e_r is post-EXPOSURE (different input!)
    float delta_r = std::abs(s_r - hal_e_r);
    float delta_g = std::abs(s_g - hal_e_g);
    float delta_b = std::abs(s_b - hal_e_b);

    // Input check
    if (delta_r > 1e-6f || delta_g > 1e-6f || delta_b > 1e-6f) {
        std::cerr << "  [INPUT_GAP] s_r=" << s_r << " vs hal_e_r=" << hal_e_r
                 << " gap=" << delta_r << " (s_* is post-TONE, hal_e_* is post-EXPOSURE)\n";
    }

    // 1) TONE curve: input s_* → post_tone
    float post_tone_r = halTableInterp(params.tone_curve.data(),
                                       static_cast<int>(params.tone_curve.size()),
                                       std::clamp(s_r, 0.0f, 1.0f));
    float post_tone_g = halTableInterp(params.tone_curve.data(),
                                       static_cast<int>(params.tone_curve.size()),
                                       std::clamp(s_g, 0.0f, 1.0f));
    float post_tone_b = halTableInterp(params.tone_curve.data(),
                                       static_cast<int>(params.tone_curve.size()),
                                       std::clamp(s_b, 0.0f, 1.0f));

    std::cerr << "  TONE(post)   r=" << post_tone_r << " g=" << post_tone_g << " b=" << post_tone_b << "\n";

    // 2) MATRIX from post_tone
    float mat_r = (post_tone_r * params.rgb_to_final[0]
                 + post_tone_g * params.rgb_to_final[1]
                 + post_tone_b * params.rgb_to_final[2]);
    float mat_g = (post_tone_r * params.rgb_to_final[3]
                 + post_tone_g * params.rgb_to_final[4]
                 + post_tone_b * params.rgb_to_final[5]);
    float mat_b = (post_tone_r * params.rgb_to_final[6]
                 + post_tone_g * params.rgb_to_final[7]
                 + post_tone_b * params.rgb_to_final[8]);
    mat_r = std::clamp(mat_r, 0.0f, 1.0f);
    mat_g = std::clamp(mat_g, 0.0f, 1.0f);
    mat_b = std::clamp(mat_b, 0.0f, 1.0f);

    std::cerr << "  MATRIX(fixed) r=" << mat_r << " g=" << mat_g << " b=" << mat_b << "\n";

    // 3) ENCODE from post_tone → matrix
    float enc_r = halTableInterp(params.encode_gamma.data(),
                                 static_cast<int>(params.encode_gamma.size()), mat_r);
    float enc_g = halTableInterp(params.encode_gamma.data(),
                                 static_cast<int>(params.encode_gamma.size()), mat_g);
    float enc_b = halTableInterp(params.encode_gamma.data(),
                                 static_cast<int>(params.encode_gamma.size()), mat_b);

    std::cerr << "  ENCODE(fixed) r=" << enc_r << " g=" << enc_g << " b=" << enc_b << "\n";

    // 4) Quantization — CPU scalar floor vs GPU quantization
    auto show_quant = [&](const char* ch, float enc, int ref_v, int test_v) {
        if (std::abs(enc) < 1e-8) return;
        float scaled_cpu = enc * 255.0f;
        float adj_cpu = scaled_cpu + 0.5f;
        int cpu_out = static_cast<int>(std::floor(adj_cpu));
        float frac_part = scaled_cpu - std::floor(scaled_cpu);
        int diff = (ref_v != test_v) ? (test_v - ref_v) : 0;
        std::cerr << "  QUANT[" << ch << "] enc=" << enc
                  << " scaled=" << scaled_cpu
                  << " adj_cpu=" << adj_cpu
                  << " cpu_out=" << cpu_out
                  << " ref=" << ref_v << " test=" << test_v
                  << " diff=" << diff
                  << " frac=" << frac_part
                  << "\n";
    };
    show_quant("R", enc_r, ref_r, test_r);
    show_quant("G", enc_g, ref_g, test_g);
    show_quant("B", enc_b, ref_b, test_b);

    // Summary: if ALL pipeline stages produce MATCH but cpu_out ≠ ref_v,
    // it confirms the divergence is in Metal's FMA-quantization (not CPU scalar).
}

}  // namespace stage_divergence

bool renderStageDivergenceEnabled() {
    const char* v = std::getenv("DNG_RENDER_STAGE_DIVERGENCE");
    return v && v[0] && v[0] != '0';
}

void runStageDivergenceReplay(const std::vector<float>& pre_tone_rgb,
                              uint32_t width,
                              uint32_t height,
                              const RenderParams& params,
                              const std::vector<uint8_t>& ref_full_rgb,
                              const std::vector<uint8_t>& test_full_rgb) {
    using namespace stage_divergence;
    if (pre_tone_rgb.empty() || width == 0 || height == 0 ||
        ref_full_rgb.size() != static_cast<size_t>(width) * height * 3u ||
        test_full_rgb.size() != static_cast<size_t>(width) * height * 3u) {
        return;
    }

    // Extract diff coords from the existing 24-sample hardcoded set
    // and supplement with any coords from the RenderHalideDiffSample output.
    const std::array<std::pair<uint32_t, uint32_t>, 24> samples = {
        // R-channel diff
        std::make_pair(5338u, 17u),  std::make_pair(112u, 26u),  std::make_pair(3163u, 44u),
        std::make_pair(782u, 99u),   std::make_pair(1017u, 102u), std::make_pair(1801u, 106u),
        std::make_pair(846u, 111u), std::make_pair(3966u, 158u),
        // G-channel diff
        std::make_pair(4773u, 49u),  std::make_pair(3519u, 55u),  std::make_pair(4199u, 88u),
        std::make_pair(1086u, 101u), std::make_pair(4485u, 126u), std::make_pair(4024u, 177u),
        std::make_pair(3118u, 179u), std::make_pair(5354u, 218u),
        // B-channel diff
        std::make_pair(4211u, 24u),  std::make_pair(4944u, 42u),  std::make_pair(2491u, 50u),
        std::make_pair(3393u, 76u),  std::make_pair(1307u, 101u), std::make_pair(2799u, 131u),
    };

    const uint8_t* ref_u8 = ref_full_rgb.data();
    const uint8_t* test_u8 = test_full_rgb.data();

    for (const auto& [x, y] : samples) {
        if (x >= width || y >= height) continue;
        size_t idx = (static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 3u;

        // Only analyze if this pixel is actually different
        if (ref_full_rgb[idx] == test_full_rgb[idx] &&
            ref_full_rgb[idx+1] == test_full_rgb[idx+1] &&
            ref_full_rgb[idx+2] == test_full_rgb[idx+2]) {
            continue;
        }

        // pre_tone_rgb is interleaved float: (r,g,b) per pixel
        float s_r = pre_tone_rgb[idx];
        float s_g = pre_tone_rgb[idx + 1];
        float s_b = pre_tone_rgb[idx + 2];

        comparePerStage(x, y, s_r, s_g, s_b, params,
                        ref_u8, test_u8, width, height);
    }
}

void runTailResearchReplay(const std::vector<float>& tone_rgb,
                           uint32_t width,
                           uint32_t height,
                           const RenderParams& params,
                           const std::vector<uint8_t>& ref_full_rgb) {
    if (tone_rgb.empty() || width == 0 || height == 0 || ref_full_rgb.empty()) {
        return;
    }
    std::vector<uint8_t> tail_halide(static_cast<size_t>(width) * height * 3u, 0);
    std::vector<uint8_t> tail_ref;
    if (!runRenderTailHalideAot(tone_rgb.data(),
                                static_cast<int>(width),
                                static_cast<int>(height),
                                3,
                                params.rgb_to_final,
                                params.encode_gamma,
                                tail_halide.data())) {
        std::cerr << "[TailResearch] tail halide run failed\n";
        return;
    }
    if (!runRenderTailReference(tone_rgb.data(),
                                static_cast<int>(width),
                                static_cast<int>(height),
                                params,
                                tail_ref)) {
        std::cerr << "[TailResearch] tail reference run failed\n";
        return;
    }
    const double psnr_tail = computePSNR8(tail_ref, tail_halide);
    const double psnr_vs_full = computePSNR8(ref_full_rgb, tail_halide);
    std::cerr << "[TailResearch] tail-halide PSNR vs tail-reference: " << psnr_tail << " dB\n";
    std::cerr << "[TailResearch] tail-halide PSNR vs full-reference: " << psnr_vs_full << " dB\n";
    printRgbDiffStats(tail_ref, tail_halide, width, height);
}

bool runHalideFullOrSdkFallback(dng_host& host,
                                dng_negative& negative,
                                dng_image* stage3,
                                const dng_render& renderer,
                                bool timing_enabled,
                                std::vector<uint8_t>& out_rgb,
                                uint32_t& out_w,
                                uint32_t& out_h) {
    auto ms = [](const auto& start, const auto& end) {
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    };

    const auto t_fn_start = std::chrono::high_resolution_clock::now();

    dng_point dst_size;
    dst_size.h = negative.DefaultFinalWidth();
    dst_size.v = negative.DefaultFinalHeight();
    if (renderer.MaximumSize()) {
        if (Max_uint32(static_cast<uint32>(dst_size.h), static_cast<uint32>(dst_size.v)) >
            renderer.MaximumSize()) {
            const real64 ratio = negative.AspectRatio();
            if (ratio >= 1.0) {
                dst_size.h = renderer.MaximumSize();
                dst_size.v = Max_uint32(1, Round_uint32(dst_size.h / ratio));
            } else {
                dst_size.v = renderer.MaximumSize();
                dst_size.h = Max_uint32(1, Round_uint32(dst_size.v * ratio));
            }
        }
    }
    out_w = static_cast<uint32_t>(dst_size.h);
    out_h = static_cast<uint32_t>(dst_size.v);
    const auto t_dst_size_end = std::chrono::high_resolution_clock::now();

    // Use resize instead of assign(N, 0): the Halide kernel overwrites every byte,
    // so the zero-fill is wasted work. resize() is a no-op when out_rgb is already sized
    // by the caller (which avoids the 250ms first-touch page-fault cost on a 72MB buffer).
    const auto t_assign_start = std::chrono::high_resolution_clock::now();
    const size_t needed_out_size = static_cast<size_t>(out_w) * out_h * 3;
    if (out_rgb.size() != needed_out_size) {
        out_rgb.resize(needed_out_size);
    }
    const auto t_assign_end = std::chrono::high_resolution_clock::now();

    dng_rect src_area = negative.DefaultCropArea();

    dng_image* source_image = stage3;
    dng_rect source_area = src_area;
    AutoPtr<dng_image> resized_stage3;
    const auto resample_start = std::chrono::high_resolution_clock::now();
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
    const auto resample_end = std::chrono::high_resolution_clock::now();

    uint32_t src_w = source_area.W();
    uint32_t src_h = source_area.H();
    uint32_t src_p = source_image->Planes();
    std::vector<uint16_t> stage3_data16;
    std::vector<float> stage3_data;
    bool have_stage3_float = false;
    auto extract_float_stage3 = [&]() {
        uint32_t fw = 0, fh = 0, fp = 0;
        extractStage3Interleaved(source_image, source_area, stage3_data, fw, fh, fp);
        src_w = fw;
        src_h = fh;
        src_p = fp;
        have_stage3_float = true;
    };

    RenderParams params;
    const auto params_start = std::chrono::high_resolution_clock::now();
    if (!buildRenderParams(host, negative, renderer, params)) {
        return false;
    }
    const auto params_end = std::chrono::high_resolution_clock::now();

    const bool can_use_u16_stage3 = source_image->PixelType() == ttShort &&
                                    source_image->PixelRange() != 0;
    if (renderHalideTryFullEnabled() && can_use_u16_stage3) {
        const auto extract_start = std::chrono::high_resolution_clock::now();
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
        const auto extract_end = std::chrono::high_resolution_clock::now();
        const float src_scale = 1.0f / static_cast<float>(source_image->PixelRange());
        const auto halide_start = std::chrono::high_resolution_clock::now();
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
            const auto halide_end = std::chrono::high_resolution_clock::now();
            if (timing_enabled) {
                const auto t_fn_end = halide_end;
                const double total_fn_ms = ms(t_fn_start, t_fn_end);
                const double dst_size_ms = ms(t_fn_start, t_dst_size_end);
                const double assign_ms = ms(t_assign_start, t_assign_end);
                const double resample_ms = ms(resample_start, resample_end);
                const double extract_ms = ms(extract_start, extract_end);
                const double params_ms = ms(params_start, params_end);
                const double halide_ms = ms(halide_start, halide_end);
                const double accounted = dst_size_ms + assign_ms + resample_ms + extract_ms + params_ms + halide_ms;
                const double untimed = total_fn_ms - accounted;
                std::cerr << "[RenderHalideTiming] dst_size=" << dst_size_ms
                          << " ms out_rgb_assign=" << assign_ms
                          << " ms resample=" << resample_ms
                          << " ms extractStage3U16=" << extract_ms
                          << " ms buildParams=" << params_ms
                          << " ms halideFull=" << halide_ms
                          << " ms untimed=" << untimed
                          << " ms total_fn=" << total_fn_ms << " ms\n";
            }
            if (renderHalideDebugEnabled()) {
                if (!have_stage3_float) {
                    extract_float_stage3();
                }
                std::vector<uint8_t> ref_full(static_cast<size_t>(src_w) * src_h * 3u, 0);
                if (runRenderStage4Reference(stage3_data.data(),
                                             static_cast<int>(src_w),
                                             static_cast<int>(src_h),
                                             static_cast<int>(src_p),
                                             params,
                                             ref_full.data())) {
                    const double psnr = computePSNR8(ref_full, out_rgb);
                    std::cerr << "[RenderHalide] full-stage PSNR vs full-reference: "
                              << psnr << " dB\n";
                    printRgbDiffStats(ref_full, out_rgb, src_w, src_h);
                }
                if (renderStageDivergenceEnabled()) {
                    std::vector<float> pre_tone_rgb;
                    if (runRenderPrefixToPreTone(stage3_data.data(),
                                                 static_cast<int>(src_w),
                                                 static_cast<int>(src_h),
                                                 static_cast<int>(src_p),
                                                 params,
                                                 pre_tone_rgb)) {
                        runStageDivergenceReplay(pre_tone_rgb,
                                                  src_w, src_h, params,
                                                  ref_full, out_rgb);
                    }
                }
                if (renderLsbResearchEnabled()) {
                    std::vector<float> pre_tone_rgb;
                    if (runRenderPrefixToPreTone(stage3_data.data(),
                                                 static_cast<int>(src_w),
                                                 static_cast<int>(src_h),
                                                 static_cast<int>(src_p),
                                                 params,
                                                 pre_tone_rgb)) {
                        runLsbResearchReplay(pre_tone_rgb,
                                             src_w,
                                             src_h,
                                             params,
                                             ref_full,
                                             out_rgb);
                    }
                }
                if (renderTailResearchEnabled()) {
                    std::vector<float> tone_rgb;
                    if (runRenderPrefixToTone(stage3_data.data(),
                                              static_cast<int>(src_w),
                                              static_cast<int>(src_h),
                                              static_cast<int>(src_p),
                                              params,
                                              tone_rgb)) {
                        runTailResearchReplay(tone_rgb, src_w, src_h, params, ref_full);
                    }
                }
            }
            return true;
        }
    }

    if (renderHalideDebugEnabled()) {
        std::cerr << "[RenderHalide] halideFull failed, fallback to SDK render\n";
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

}  // namespace

const char* renderHalideModeName(RenderHalideMode mode) {
    switch (mode) {
        case RenderHalideMode::SDK: return "sdk";
        case RenderHalideMode::HALIDE_METAL: return "halide-metal";
        case RenderHalideMode::AUTO: return "auto";
    }
    return "unknown";
}

bool render_stage4_halide(dng_host& host,
                          dng_negative& negative,
                          const dng_render& renderer,
                          RenderHalideMode mode,
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
                                      renderHalideTimingEnabled(),
                                      out_rgb,
                                      out_w,
                                      out_h);
}
