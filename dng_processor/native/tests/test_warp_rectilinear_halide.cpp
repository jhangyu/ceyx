#include "stage_contract_checks.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <vector>

#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_ifd.h>
#include <dng_image.h>
#include <dng_info.h>
#include <dng_lens_correction.h>
#include <dng_mosaic_halide.h>
#include <dng_negative.h>
#include <dng_pixel_buffer.h>
#include <dng_rect.h>
#include <dng_warp_halide.h>

using namespace std;
using namespace std::chrono;

namespace {

double computePSNR16(const uint16_t* a, const uint16_t* b, size_t count) {
    if (!a || !b || count == 0) return 0.0;
    double mse = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        mse += d * d;
    }
    mse /= static_cast<double>(count);
    if (mse <= 0.0) return 999.0;
    return 20.0 * log10(65535.0 / sqrt(mse));
}

struct ChannelDiffStats {
    double mae = 0.0;
    uint16_t maxAbs = 0;
    int maxX = 0;
    int maxY = 0;
};

void computeInterleavedRgbDiffStats(const std::vector<uint16_t>& ref,
                                    const std::vector<uint16_t>& test,
                                    uint32_t width,
                                    uint32_t height,
                                    ChannelDiffStats out[3]) {
    const size_t pixelCount = static_cast<size_t>(width) * height;
    if (pixelCount == 0 || ref.size() != test.size() || ref.size() != pixelCount * 3) {
        return;
    }
    double sumAbs[3] = {0.0, 0.0, 0.0};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t base = (static_cast<size_t>(y) * width + x) * 3;
            for (int c = 0; c < 3; ++c) {
                const int diff = static_cast<int>(test[base + c]) - static_cast<int>(ref[base + c]);
                const uint16_t absDiff = static_cast<uint16_t>(std::abs(diff));
                sumAbs[c] += static_cast<double>(absDiff);
                if (absDiff > out[c].maxAbs) {
                    out[c].maxAbs = absDiff;
                    out[c].maxX = static_cast<int>(x);
                    out[c].maxY = static_cast<int>(y);
                }
            }
        }
    }
    for (int c = 0; c < 3; ++c) {
        out[c].mae = sumAbs[c] / static_cast<double>(pixelCount);
    }
}

bool saveRawFile(const string& filename, const void* data, size_t bytes) {
    ofstream out(filename, ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data), bytes);
    return out.good();
}

void extractImageData(dng_image* image,
                      vector<uint16_t>& data,
                      uint32_t& width,
                      uint32_t& height,
                      uint32_t& planes,
                      uint32_t& pixelType,
                      size_t& pixelSize) {
    width = image->Width();
    height = image->Height();
    planes = image->Planes();
    pixelType = image->PixelType();
    pixelSize = image->PixelSize();

    data.resize(static_cast<size_t>(width) * height * planes);

    dng_pixel_buffer buffer;
    buffer.fArea = image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = planes;
    buffer.fPixelType = ttShort;
    buffer.fPixelSize = sizeof(uint16_t);
    buffer.fData = data.data();
    buffer.fRowStep = static_cast<int32>(width * planes);
    buffer.fColStep = static_cast<int32>(planes);
    buffer.fPlaneStep = 1;
    image->Get(buffer);
}

AutoPtr<dng_image> makeImageFromInterleaved(dng_host& host,
                                            uint32_t width,
                                            uint32_t height,
                                            uint32_t planes,
                                            const vector<uint16_t>& data) {
    dng_point size(static_cast<int32>(height), static_cast<int32>(width));
    AutoPtr<dng_image> image(host.Make_dng_image(dng_rect(size), planes, ttShort));

    dng_pixel_buffer buffer;
    buffer.fArea = image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = planes;
    buffer.fPixelType = ttShort;
    buffer.fPixelSize = sizeof(uint16_t);
    buffer.fData = const_cast<uint16_t*>(data.data());
    buffer.fRowStep = static_cast<int32>(width * planes);
    buffer.fColStep = static_cast<int32>(planes);
    buffer.fPlaneStep = 1;
    image->Put(buffer);
    return AutoPtr<dng_image>(image.Release());
}

const dng_opcode_WarpRectilinear* findWarpRectilinear(const dng_negative& negative) {
    const dng_opcode_list& list = negative.OpcodeList3();
    for (uint32_t i = 0; i < list.Count(); ++i) {
        const dng_opcode& opcode = list.Entry(i);
        if (opcode.OpcodeID() == dngOpcode_WarpRectilinear) {
            return &static_cast<const dng_opcode_WarpRectilinear&>(opcode);
        }
    }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "Usage: " << argv[0] << " <dng_path>\n";
        return 1;
    }

    const string dngPath = argv[1];

    try {
        dng_host host;
        dng_file_stream stream(dngPath.c_str());

        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        if (!info.IsValidDNG()) {
            cerr << "ERROR: Not a valid DNG file\n";
            return 1;
        }

        const dng_ifd& rawIFD = *info.fIFD[info.fMainIndex];
        const auto decodePath = StageContract::detectDecodePath(rawIFD.fPhotometricInterpretation);
        if (decodePath != StageContract::DecodePath::CFA_BAYER) {
            cerr << "ERROR: test_warp_rectilinear_halide currently expects CFA Bayer input\n";
            return 1;
        }

        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        negative->ReadStage1Image(host, stream, info);
        negative->BuildStage2Image(host);

        dng_image* stage2Image = const_cast<dng_image*>(negative->Stage2Image());
        uint32_t s2w = 0, s2h = 0, s2p = 0, s2pt = 0;
        size_t s2ps = 0;
        vector<uint16_t> stage2Data;
        extractImageData(stage2Image, stage2Data, s2w, s2h, s2p, s2pt, s2ps);

        if (!StageContract::validateStageContract16("Stage2",
                                                    decodePath,
                                                    s2w,
                                                    s2h,
                                                    s2p,
                                                    s2pt,
                                                    s2ps,
                                                    stage2Data.size())) {
            cerr << "ERROR: Stage2 contract check failed\n";
            return 1;
        }

        vector<uint16_t> preOpcode3(static_cast<size_t>(s2w) * s2h * 3);
        demosaic_ahd_halide(stage2Data.data(),
                            static_cast<int>(s2w),
                            static_cast<int>(s2h),
                            preOpcode3.data());

        if (!StageContract::validateStageContract16("WarpRectilinear",
                                                    decodePath,
                                                    s2w,
                                                    s2h,
                                                    3,
                                                    ttShort,
                                                    sizeof(uint16_t),
                                                    preOpcode3.size())) {
            cerr << "ERROR: pre-Opcode3 contract check failed\n";
            return 1;
        }

        const dng_opcode_WarpRectilinear* warpOpcode = findWarpRectilinear(*negative);
        if (!warpOpcode) {
            cerr << "ERROR: OpcodeList3 does not contain WarpRectilinear\n";
            return 1;
        }

        AutoPtr<dng_image> sdkImage = makeImageFromInterleaved(host, s2w, s2h, 3, preOpcode3);
        const auto sdkStart = high_resolution_clock::now();
        const_cast<dng_opcode_WarpRectilinear*>(warpOpcode)->Apply(host, *negative, sdkImage);
        const auto sdkEnd = high_resolution_clock::now();

        uint32_t refW = 0, refH = 0, refP = 0, refPT = 0;
        size_t refPS = 0;
        vector<uint16_t> sdkData;
        extractImageData(sdkImage.Get(), sdkData, refW, refH, refP, refPT, refPS);

        if (!StageContract::validateStageContract16("WarpRectilinear",
                                                    decodePath,
                                                    refW,
                                                    refH,
                                                    refP,
                                                    refPT,
                                                    refPS,
                                                    sdkData.size())) {
            cerr << "ERROR: SDK warp contract check failed\n";
            return 1;
        }

        WarpRectilinearParams params;
        if (!extractWarpRectilinearParams(*warpOpcode,
                                          negative->PixelAspectRatio(),
                                          params)) {
            cerr << "ERROR: Failed to extract warp params\n";
            return 1;
        }

        vector<uint16_t> cpuData(preOpcode3.size());
        const auto cpuStart = high_resolution_clock::now();
        if (!warp_rectilinear_halide(preOpcode3.data(),
                                     static_cast<int>(s2w),
                                     static_cast<int>(s2h),
                                     3,
                                     params,
                                     WarpRectilinearMode::HALIDE_CPU,
                                     cpuData.data())) {
            cerr << "ERROR: CPU warp_rectilinear_halide failed\n";
            return 1;
        }
        const auto cpuEnd = high_resolution_clock::now();

        if (!StageContract::validateStageContract16("WarpRectilinear",
                                                    decodePath,
                                                    s2w,
                                                    s2h,
                                                    3,
                                                    ttShort,
                                                    sizeof(uint16_t),
                                                    cpuData.size())) {
            cerr << "ERROR: CPU warp contract check failed\n";
            return 1;
        }

        cout << fixed << setprecision(2);
        cout << "SDK WarpRectilinear: "
             << duration_cast<microseconds>(sdkEnd - sdkStart).count() / 1000.0 << " ms\n";
        cout << "Halide CPU WarpRectilinear: "
             << duration_cast<microseconds>(cpuEnd - cpuStart).count() / 1000.0 << " ms\n";
        cout << "Halide CPU PSNR vs SDK: " << computePSNR16(sdkData.data(), cpuData.data(), sdkData.size())
             << " dB\n";
        {
            ChannelDiffStats stats[3];
            computeInterleavedRgbDiffStats(sdkData, cpuData, s2w, s2h, stats);
            cout << "Halide CPU diff stats (vs SDK):\n";
            cout << "  R MAE=" << stats[0].mae << " maxAbs=" << stats[0].maxAbs
                 << " @(" << stats[0].maxX << "," << stats[0].maxY << ")\n";
            cout << "  G MAE=" << stats[1].mae << " maxAbs=" << stats[1].maxAbs
                 << " @(" << stats[1].maxX << "," << stats[1].maxY << ")\n";
            cout << "  B MAE=" << stats[2].mae << " maxAbs=" << stats[2].maxAbs
                 << " @(" << stats[2].maxX << "," << stats[2].maxY << ")\n";
        }

        saveRawFile("lossless_stage3_pre_opcode3.raw", preOpcode3.data(), preOpcode3.size() * sizeof(uint16_t));
        saveRawFile("lossless_stage3_post_warprectilinear_sdk.raw", sdkData.data(), sdkData.size() * sizeof(uint16_t));
        saveRawFile("lossless_stage3_post_warprectilinear_halide_cpu.raw",
                    cpuData.data(),
                    cpuData.size() * sizeof(uint16_t));

#if defined(__APPLE__)
        vector<uint16_t> metalData(preOpcode3.size());
        const auto metalStart = high_resolution_clock::now();
        const bool metalOk = warp_rectilinear_halide(preOpcode3.data(),
                                                     static_cast<int>(s2w),
                                                     static_cast<int>(s2h),
                                                     3,
                                                     params,
                                                     WarpRectilinearMode::HALIDE_METAL,
                                                     metalData.data());
        const auto metalEnd = high_resolution_clock::now();
        if (metalOk) {
            cout << "Halide Metal WarpRectilinear: "
                 << duration_cast<microseconds>(metalEnd - metalStart).count() / 1000.0 << " ms\n";
            cout << "Halide Metal PSNR vs SDK: "
                 << computePSNR16(sdkData.data(), metalData.data(), sdkData.size()) << " dB\n";
            ChannelDiffStats stats[3];
            computeInterleavedRgbDiffStats(sdkData, metalData, s2w, s2h, stats);
            cout << "Halide Metal diff stats (vs SDK):\n";
            cout << "  R MAE=" << stats[0].mae << " maxAbs=" << stats[0].maxAbs
                 << " @(" << stats[0].maxX << "," << stats[0].maxY << ")\n";
            cout << "  G MAE=" << stats[1].mae << " maxAbs=" << stats[1].maxAbs
                 << " @(" << stats[1].maxX << "," << stats[1].maxY << ")\n";
            cout << "  B MAE=" << stats[2].mae << " maxAbs=" << stats[2].maxAbs
                 << " @(" << stats[2].maxX << "," << stats[2].maxY << ")\n";
            saveRawFile("lossless_stage3_post_warprectilinear_halide_metal.raw",
                        metalData.data(),
                        metalData.size() * sizeof(uint16_t));
        } else {
            cout << "Halide Metal WarpRectilinear: failed\n";
        }
#endif

        return 0;
    } catch (const dng_exception& e) {
        cerr << "DNG Exception: " << e.ErrorCode() << "\n";
    } catch (const exception& e) {
        cerr << "Exception: " << e.what() << "\n";
    }

    return 1;
}
