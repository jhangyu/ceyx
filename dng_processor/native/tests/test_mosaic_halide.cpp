/**
 * test_mosaic_halide.cpp - Test Halide demosaic against DNG SDK baseline
 *
 * Phase 5.3 - Demosaic PSNR Test
 *
 * This tool:
 * 1. Runs DNG SDK to BuildStage2Image (linearized CFA image)
 * 2. Clears OpcodeList3 and runs BuildStage3Image for demosaic-only reference
 * 3. Runs custom Halide demosaic on Stage2
 * 4. Compares with DNG SDK Stage3 (pre-Opcode3)
 * 4. Computes PSNR
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cmath>
#include <chrono>
#include <cstring>
#include <limits>

// DNG SDK includes
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_ifd.h>
#include <dng_pixel_buffer.h>
#include <dng_image.h>
#include <dng_exceptions.h>

#include "stage_contract_checks.h"

extern "C" {
#include "dng_mosaic_halide.h"
}

// Compute PSNR between two 16-bit buffers
double computePSNR_16bit(const uint16_t* img1, const uint16_t* img2, size_t pixelCount) {
    if (!img1 || !img2 || pixelCount == 0) return 0;

    double mse = 0;
    const uint32_t maxValue = 65535;

    for (size_t i = 0; i < pixelCount; i++) {
        double diff = static_cast<double>(img1[i]) - static_cast<double>(img2[i]);
        mse += diff * diff;
    }
    mse /= pixelCount;

    if (mse < 1e-10) return 999.0;
    double psnr = 10.0 * log10((maxValue * maxValue) / mse);
    return psnr;
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

bool saveRawFile(const std::string& filename, const void* data, size_t byteSize) {
    std::ofstream fout(filename, std::ios::binary);
    if (!fout) return false;
    fout.write(reinterpret_cast<const char*>(data), byteSize);
    return fout.good();
}

void extractImageData(dng_image* image,
                      std::vector<uint16_t>& data,
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

    size_t totalPixels = static_cast<size_t>(width) * height * planes;
    data.resize(totalPixels);

    dng_pixel_buffer buffer;
    buffer.fArea = image->Bounds();
    buffer.fPlane = 0;
    buffer.fPlanes = planes;
    buffer.fPixelType = pixelType;
    buffer.fPixelSize = pixelSize;
    buffer.fData = data.data();
    buffer.fRowStep = width * planes;
    buffer.fColStep = planes;
    buffer.fPlaneStep = 1;

    image->Get(buffer);
}

int main(int argc, char** argv) {
    std::cout << "======================================================================\n";
    std::cout << "  Halide Demosaic PSNR Test\n";
    std::cout << "  Phase 5.3 - BuildStage3Image Demosaic Comparison (pre-Opcode3)\n";
    std::cout << "======================================================================\n\n";

    const char* dngPath = (argc > 1) ? argv[1] : "image_samples/lossless_dng_sample.dng";

    dng_host host;
    std::vector<uint16_t> stage2Data;
    std::vector<uint16_t> stage3Ref;
    uint32_t width = 0;
    uint32_t height = 0;

    try {
        dng_file_stream stream(dngPath);
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        if (!info.IsValidDNG()) {
            std::cerr << "ERROR: Not a valid DNG: " << dngPath << "\n";
            return 1;
        }

        dng_negative* negativeTemplate = host.Make_dng_negative();
        AutoPtr<dng_negative> negative(negativeTemplate);
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);

        std::cout << "Input DNG: " << dngPath << "\n";
        std::cout << "Running DNG SDK Stage1 -> Stage2...\n";
        negative->ReadStage1Image(host, stream, info);
        negative->BuildStage2Image(host);

        uint32_t s2w = 0;
        uint32_t s2h = 0;
        uint32_t s2p = 0, s2pt = 0;
        size_t s2ps = 0;
        dng_image* stage2Img = const_cast<dng_image*>(negative->Stage2Image());
        extractImageData(stage2Img, stage2Data, s2w, s2h, s2p, s2pt, s2ps);
        if (!StageContract::validateStageContract16("Stage2",
                                                    StageContract::DecodePath::CFA_BAYER,
                                                    s2w,
                                                    s2h,
                                                    s2p,
                                                    s2pt,
                                                    s2ps,
                                                    stage2Data.size())) {
            std::cerr << "ERROR: Stage2 contract failed\n";
            return 1;
        }

        // Ensure Stage3 reference is demosaic-only (exclude geometric opcode effects).
        negative->OpcodeList3().Clear();
        std::cout << "Running DNG SDK Stage3 (demosaic-only, OpcodeList3 cleared)...\n";
        negative->BuildStage3Image(host);

        uint32_t s3w = 0;
        uint32_t s3h = 0;
        uint32_t s3p = 0, s3pt = 0;
        size_t s3ps = 0;
        dng_image* stage3Img = const_cast<dng_image*>(negative->Stage3Image());
        extractImageData(stage3Img, stage3Ref, s3w, s3h, s3p, s3pt, s3ps);
        if (!StageContract::validateStageContract16("Stage3",
                                                    StageContract::DecodePath::CFA_BAYER,
                                                    s3w,
                                                    s3h,
                                                    s3p,
                                                    s3pt,
                                                    s3ps,
                                                    stage3Ref.size())) {
            std::cerr << "ERROR: Stage3 contract failed\n";
            return 1;
        }
        if (s2w != s3w || s2h != s3h) {
            std::cerr << "ERROR: Stage2/Stage3 size mismatch: "
                      << s2w << "x" << s2h << " vs " << s3w << "x" << s3h << "\n";
            return 1;
        }

        width = s2w;
        height = s2h;
        std::cout << "Resolved size: " << width << "x" << height << "\n";
        auto s2At = [&](int x, int y) -> uint16_t {
            return stage2Data[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
        };
        std::cout << "Stage2 samples: "
                  << "[0,0]=" << s2At(0, 0) << " "
                  << "[0,1]=" << s2At(0, 1) << " "
                  << "[1,0]=" << s2At(1, 0) << " "
                  << "[1,1]=" << s2At(1, 1) << "\n";
    } catch (const dng_exception& e) {
        std::cerr << "DNG SDK exception: " << e.ErrorCode() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception while preparing DNG SDK reference\n";
        return 1;
    }

    // Run custom Halide demosaic
    std::cout << "\nRunning Halide AHD demosaic...\n";
    std::vector<uint16_t> stage3Test(width * height * 3);

    auto start = std::chrono::high_resolution_clock::now();
    demosaic_ahd_halide(stage2Data.data(), width, height, stage3Test.data());
    auto end = std::chrono::high_resolution_clock::now();

    double demosaicMs = std::chrono::duration<double, std::milli>(end - start).count();
    std::cout << "  Demosaic time: " << std::fixed << std::setprecision(2) << demosaicMs << " ms\n";
    if (!StageContract::validateStageContract16("Stage3",
                                                StageContract::DecodePath::CFA_BAYER,
                                                width,
                                                height,
                                                3,
                                                StageContract::kPixelTypeShort,
                                                sizeof(uint16_t),
                                                stage3Test.size())) {
        std::cerr << "ERROR: Halide Stage3 contract failed\n";
        return 1;
    }

    // DNG SDK extractImageData stores Stage3 as interleaved RGB.
    auto printSample = [&](int x, int y) {
        const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 3;
        std::cout << "  [" << x << "," << y << "] "
                  << "ref=(" << stage3Ref[idx] << "," << stage3Ref[idx + 1] << "," << stage3Ref[idx + 2] << ") "
                  << "test=(" << stage3Test[idx] << "," << stage3Test[idx + 1] << "," << stage3Test[idx + 2] << ")\n";
    };
    std::cout << "  Sample pixels (interleaved interpretation):\n";
    printSample(0, 0);
    printSample(0, 1);
    printSample(1, 0);
    printSample(1, 1);

    // Compute PSNR
    std::cout << "\nComputing PSNR...\n";
    double psnr = computePSNR_16bit(stage3Ref.data(), stage3Test.data(), stage3Ref.size());

    std::cout << "\n======================================================================\n";
    std::cout << "  PSNR Result: " << std::fixed << std::setprecision(2) << psnr << " dB\n";
    std::cout << "======================================================================\n";

    ChannelDiffStats stats[3];
    computeInterleavedRgbDiffStats(stage3Ref, stage3Test, width, height, stats);
    std::cout << "  Diff stats (test - ref):\n";
    std::cout << "    R: MAE=" << stats[0].mae << " maxAbs=" << stats[0].maxAbs
              << " @(" << stats[0].maxX << "," << stats[0].maxY << ")\n";
    std::cout << "    G: MAE=" << stats[1].mae << " maxAbs=" << stats[1].maxAbs
              << " @(" << stats[1].maxX << "," << stats[1].maxY << ")\n";
    std::cout << "    B: MAE=" << stats[2].mae << " maxAbs=" << stats[2].maxAbs
              << " @(" << stats[2].maxX << "," << stats[2].maxY << ")\n";

    if (psnr > 36.0) {
        std::cout << "PASS: PSNR > 36 dB threshold met!\n";
    } else {
        std::cout << "FAIL: PSNR < 36 dB threshold\n";
        std::cout << "      Demosaic quality needs improvement.\n";
    }

    // Save test output for inspection
    std::string testOutput = "halide_demosaic_output.raw";
    saveRawFile(testOutput, stage3Test.data(), stage3Test.size() * sizeof(uint16_t));
    std::cout << "  Test output saved: " << testOutput << "\n";

    return (psnr > 36.0) ? 0 : 1;
}
