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

bool runRenderPrefixToTone(const float* src,
                           int src_w,
                           int src_h,
                           int src_p,
                           const RenderParams& params,
                           std::vector<float>& tone_rgb) {
    return runRenderPrefix(src, src_w, src_h, src_p, params, true, tone_rgb);
}

bool runRenderPrefixToPreTone(const float* src,
                              int src_w,
                              int src_h,
                              int src_p,
                              const RenderParams& params,
                              std::vector<float>& pre_tone_rgb) {
    return runRenderPrefix(src, src_w, src_h, src_p, params, false, pre_tone_rgb);
}

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

    const bool timing_enabled = renderHalideTimingEnabled();
    auto ms = [](const auto& start, const auto& end) {
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0;
    };

    if (renderHalideBitExactModeEnabled()) {
        const uint32_t decode_threads = host.PerformAreaTaskThreads();
        const uint32_t render_threads_override = parsePositiveEnvU32("DNG_RENDER_AREA_THREADS");
        dng_host* quality_lock_host = &host;
        std::unique_ptr<dng_render> quality_lock_renderer;
        if (render_threads_override > 0 && render_threads_override != decode_threads) {
            if (dng_host* override_host = qualityLockRenderHostCache().acquire(render_threads_override)) {
                quality_lock_host = override_host;
                quality_lock_renderer = std::make_unique<dng_render>(*quality_lock_host, negative);
                copyRenderSettings(renderer, *quality_lock_renderer);
            }
        }
        dng_render& quality_renderer =
            quality_lock_renderer ? *quality_lock_renderer : const_cast<dng_render&>(renderer);
        const uint32_t render_threads = quality_lock_host->PerformAreaTaskThreads();

        const auto sdk_render_start = std::chrono::high_resolution_clock::now();
        AutoPtr<dng_image> final_image(quality_renderer.Render());
        const auto sdk_render_end = std::chrono::high_resolution_clock::now();
        if (!final_image.Get()) {
            return false;
        }
        out_w = final_image->Width();
        out_h = final_image->Height();
        out_rgb.resize(static_cast<size_t>(out_w) * out_h * 3);

        const auto extract_start = std::chrono::high_resolution_clock::now();
        bool extracted = false;
        std::unique_ptr<dng_const_tile_buffer> borrowed_tile;
        const uint8_t* borrowed_ptr = nullptr;
        uint32_t bw = 0, bh = 0, bp = 0;
        int32_t brow = 0, bcol = 0, bplane = 0;
        const dng_rect bounds = final_image->Bounds();
        if (borrowImageInterleaved8(final_image.Get(),
                                    bounds,
                                    borrowed_tile,
                                    borrowed_ptr,
                                    bw,
                                    bh,
                                    bp,
                                    brow,
                                    bcol,
                                    bplane)) {
            if (bw == out_w && bh == out_h && bp == 3) {
                packBorrowedInterleaved8(borrowed_ptr, bw, bh, bp, brow, bcol, bplane, out_rgb);
                extracted = true;
            }
        }

        if (!extracted) {
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
        }
        const auto extract_end = std::chrono::high_resolution_clock::now();
        if (timing_enabled) {
            std::cerr << "[RenderHalideTiming] qualityLockSDKRender="
                      << ms(sdk_render_start, sdk_render_end)
                      << " ms qualityLockExtract="
                      << ms(extract_start, extract_end)
                      << " ms [RenderHalideInfo] qualityLockRenderThreads="
                      << render_threads << "\n";
        }
        return true;
    }

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
    out_rgb.assign(static_cast<size_t>(out_w) * out_h * 3, 0);

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
                std::cerr << "[RenderHalideTiming] resample=" << ms(resample_start, resample_end)
                          << " ms extractStage3U16=" << ms(extract_start, extract_end)
                          << " ms buildParams=" << ms(params_start, params_end)
                          << " ms halideFull=" << ms(halide_start, halide_end)
                          << " ms\n";
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
