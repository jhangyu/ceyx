/**
 * test_decode.cpp - DNG SDK Decoding Test Tool with Step-by-Step PSNR
 *
 * Phase 5.0 - DNG SDK Render Pipeline Testing
 *
 * This tool tests both lossless and lossy DNG files, measuring:
 * 1. Timing for each pipeline stage
 * 2. Step-by-step PSNR comparison between reference and custom implementations
 *
 * PSNR Workflow:
 * 1. First run: Generate baseline/reference outputs using dng_sdk (save to *_stage*.raw)
 * 2. Later run: Generate test outputs using dng_sdk_custom
 * 3. Compare: Compute PSNR between baseline and test at each stage
 *
 * Pipeline stages:
 * - ReadStage1Image: Read compressed tile data from DNG file
 * - BuildStage2Image: Linearization, black level, opcode list 1
 * - BuildStage3Image: Demosaicing (Bayer) or pass-through (YCbCr)
 * - Render: Tone curve + gamma + color space conversion
 */

#include "stage_contract_checks.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_ifd.h>
#include <dng_pixel_buffer.h>
#include <dng_image.h>
#include <dng_linearization_info.h>
#include <dng_camera_profile.h>
#include <dng_render.h>
#include <dng_color_space.h>
#include <dng_exceptions.h>
#include <dng_rect.h>

#include <dng_mosaic_halide.h>

using namespace std;
using namespace std::chrono;

// Global timing and PSNR data
struct StageTiming {
    double read_stage1_ms = 0;
    double build_stage2_ms = 0;
    double build_stage3_ms = 0;
    double stage3_extract_stage2_ms = 0;
    double stage3_halide_kernel_ms = 0;
    double stage3_inject_put_ms = 0;
    double stage3_apply_opcode3_ms = 0;
    double stage3_extract_stage3_ms = 0;
    double stage3_sdk_build_ms = 0;
    double render_ms = 0;
    double total_ms = 0;
};

struct StagePSNR {
    double stage1_psnr = 0;
    double stage2_psnr = 0;
    double stage3_psnr = 0;
    double render_psnr = 0;
};

void printBanner(const string& title) {
    cout << "\n" << string(70, '=') << "\n";
    cout << "  " << title << "\n";
    cout << string(70, '=') << "\n\n";
}

string compressionName(uint32 compression) {
    switch (compression) {
        case 1: return "Uncompressed";
        case 5: return "LZW";
        case 6: return "JPEG (old)";
        case 7: return "Lossless JPEG (JPEGORG)";
        case 34892: return "Lossy JPEG (DNG)";
        default: return "Unknown (" + to_string(compression) + ")";
    }
}

string photometricName(uint32 photometric) {
    switch (photometric) {
        case 0: return "MinIsWhite";
        case 1: return "MinIsBlack";
        case 2: return "RGB";
        case 6: return "YCbCr";
        case 32803: return "CFA (Bayer)";
        case 34892: return "LinearRaw";
        default: return "Unknown";
    }
}

// Save raw image data to file
bool saveRawFile(const string& filename, const void* data, size_t byteSize) {
    ofstream fout(filename, ios::binary);
    if (!fout) return false;
    fout.write(reinterpret_cast<const char*>(data), byteSize);
    return fout.good();
}

// Load raw image data from file
bool loadRawFile(const string& filename, void* data, size_t byteSize) {
    ifstream fin(filename, ios::binary);
    if (!fin) return false;
    fin.read(reinterpret_cast<char*>(data), byteSize);
    return fin.good();
}

// Compute PSNR between two buffers (16-bit data)
double computePSNR_16bit(const uint16_t* img1, const uint16_t* img2, size_t pixelCount) {
    if (!img1 || !img2 || pixelCount == 0) return 0;

    double mse = 0;
    const uint32_t maxValue = 65535;

    for (size_t i = 0; i < pixelCount; i++) {
        double diff = static_cast<double>(img1[i]) - static_cast<double>(img2[i]);
        mse += diff * diff;
    }
    mse /= pixelCount;

    if (mse < 1e-10) return 999.0;  // Nearly identical

    double psnr = 10.0 * log10((maxValue * maxValue) / mse);
    return psnr;
}

// Compute PSNR between two buffers (8-bit data)
double computePSNR_8bit(const uint8_t* img1, const uint8_t* img2, size_t pixelCount) {
    if (!img1 || !img2 || pixelCount == 0) return 0;

    double mse = 0;
    const uint32_t maxValue = 255;

    for (size_t i = 0; i < pixelCount; i++) {
        double diff = static_cast<double>(img1[i]) - static_cast<double>(img2[i]);
        mse += diff * diff;
    }
    mse /= pixelCount;

    if (mse < 1e-10) return 999.0;

    double psnr = 10.0 * log10((maxValue * maxValue) / mse);
    return psnr;
}

// Extract image data from dng_image to buffer
void extractImageData(dng_image* image, vector<uint16_t>& data, uint32_t& width, uint32_t& height, uint32_t& planes, uint32_t& pixelType, size_t& pixelSize) {
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

// Stage-by-stage test for one DNG file
void testDNG(dng_host& host, const string& dngPath, const string& prefix, bool generateBaseline) {
    printBanner(prefix + " DNG Decoding Test");

    StageTiming timing;
    StagePSNR psnr;

    auto totalStart = high_resolution_clock::now();

    try {
        dng_file_stream stream(dngPath.c_str());

        // Parse DNG info
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);

        if (!info.IsValidDNG()) {
            cerr << "ERROR: Not a valid DNG file\n";
            return;
        }

        const dng_ifd& rawIFD = *info.fIFD[info.fMainIndex];
        uint32 width = rawIFD.fImageWidth;
        uint32 height = rawIFD.fImageLength;
        StageContract::DecodePath decodePath = StageContract::detectDecodePath(rawIFD.fPhotometricInterpretation);

        cout << "Image: " << width << "x" << height << "\n";
        cout << "Photometric: " << photometricName(rawIFD.fPhotometricInterpretation)
             << " (" << rawIFD.fPhotometricInterpretation << ")"
             << " (" << StageContract::decodePathName(decodePath) << ")\n";
        cout << "Mode: " << (generateBaseline ? "BASELINE (generate)" : "TEST (compare)") << "\n\n";

        // Parse negative
        dng_negative* negativeTemplate = host.Make_dng_negative();
        AutoPtr<dng_negative> negative(negativeTemplate);
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);

        // Baseline storage for PSNR comparison
        vector<uint16_t> baseline_stage1, baseline_stage2, baseline_stage3;
        vector<uint16_t> test_stage1, test_stage2, test_stage3;
        vector<uint8_t> baseline_render, test_render;

        // === Stage 1: ReadStage1Image ===
        cout << "--- Stage 1: ReadStage1Image ---\n";
        auto stage1Start = high_resolution_clock::now();

        negative->ReadStage1Image(host, stream, info);

        auto stage1End = high_resolution_clock::now();
        timing.read_stage1_ms = duration_cast<microseconds>(stage1End - stage1Start).count() / 1000.0;
        cout << "  Time: " << fixed << setprecision(2) << timing.read_stage1_ms << " ms\n";

        // Extract Stage 1 image data
        {
            uint32_t w, h, p, pt;
            size_t ps;
            dng_image* stage1Img = const_cast<dng_image*>(negative->Stage1Image());
            vector<uint16_t> stage1Data;
            extractImageData(stage1Img, stage1Data, w, h, p, pt, ps);
            if (!StageContract::validateStageContract16("Stage1", decodePath, w, h, p, pt, ps, stage1Data.size())) {
                cerr << "ERROR: Stage1 contract check failed\n";
                return;
            }

            string filename = prefix + "_stage1_" + to_string(w) + "x" + to_string(h) + "_" + to_string(p) + "p.raw";
            if (generateBaseline) {
                saveRawFile(filename, stage1Data.data(), stage1Data.size() * sizeof(uint16_t));
                cout << "  Saved baseline: " << filename << " (" << stage1Data.size() * 2 << " bytes)\n";
            } else {
                vector<uint16_t> refData(stage1Data.size());
                string refFilename = prefix + "_stage1_" + to_string(w) + "x" + to_string(h) + "_" + to_string(p) + "p.raw";
                if (loadRawFile(refFilename, refData.data(), refData.size() * sizeof(uint16_t))) {
                    psnr.stage1_psnr = computePSNR_16bit(refData.data(), stage1Data.data(), stage1Data.size());
                    cout << "  PSNR vs baseline: " << fixed << setprecision(2) << psnr.stage1_psnr << " dB\n";
                } else {
                    cout << "  WARNING: Could not load baseline file: " << refFilename << "\n";
                }
            }
        }

        // === Stage 2: BuildStage2Image ===
        cout << "\n--- Stage 2: BuildStage2Image ---\n";
        auto stage2Start = high_resolution_clock::now();

        negative->BuildStage2Image(host);

        auto stage2End = high_resolution_clock::now();
        timing.build_stage2_ms = duration_cast<microseconds>(stage2End - stage2Start).count() / 1000.0;
        cout << "  Time: " << fixed << setprecision(2) << timing.build_stage2_ms << " ms\n";

        // Extract Stage 2 image data
        {
            uint32_t w, h, p, pt;
            size_t ps;
            dng_image* stage2Img = const_cast<dng_image*>(negative->Stage2Image());
            vector<uint16_t> stage2Data;
            extractImageData(stage2Img, stage2Data, w, h, p, pt, ps);
            if (!StageContract::validateStageContract16("Stage2", decodePath, w, h, p, pt, ps, stage2Data.size())) {
                cerr << "ERROR: Stage2 contract check failed\n";
                return;
            }

            string filename = prefix + "_stage2_" + to_string(w) + "x" + to_string(h) + "_" + to_string(p) + "p.raw";
            if (generateBaseline) {
                saveRawFile(filename, stage2Data.data(), stage2Data.size() * sizeof(uint16_t));
                cout << "  Saved baseline: " << filename << " (" << stage2Data.size() * 2 << " bytes)\n";
            } else {
                vector<uint16_t> refData(stage2Data.size());
                string refFilename = prefix + "_stage2_" + to_string(w) + "x" + to_string(h) + "_" + to_string(p) + "p.raw";
                if (loadRawFile(refFilename, refData.data(), refData.size() * sizeof(uint16_t))) {
                    psnr.stage2_psnr = computePSNR_16bit(refData.data(), stage2Data.data(), stage2Data.size());
                    cout << "  PSNR vs baseline: " << fixed << setprecision(2) << psnr.stage2_psnr << " dB\n";
                } else {
                    cout << "  WARNING: Could not load baseline file: " << refFilename << "\n";
                }
            }
        }

        // === Stage 3: BuildStage3Image (Halide demosaic for Bayer in TEST mode) ===
        cout << "\n--- Stage 3: BuildStage3Image ---\n";
        auto stage3Start = high_resolution_clock::now();

        // For Bayer CFA in TEST mode: use Halide demosaic instead of DNG SDK
        // For YCbCr (lossy) or BASELINE mode: use DNG SDK directly
        bool useHalideDemosaic = (decodePath == StageContract::DecodePath::CFA_BAYER) && !generateBaseline;

        vector<uint16_t> stage3Data;
        uint32_t s3w = 0, s3h = 0, s3p = 0, s3pt = 0;
        size_t s3ps = 0;

        if (useHalideDemosaic) {
            // Use Halide AHD demosaic on Stage2 output
            cout << "  [Halide] Using AHD demosaic on Stage2 (Bayer CFA) + inject Stage3Image...\n";

            // Extract Stage2 data for Halide input
            auto stage3ExtractStage2Start = high_resolution_clock::now();
            dng_image* stage2Img = const_cast<dng_image*>(negative->Stage2Image());
            uint32_t s2w = 0, s2h = 0, s2p = 0, s2pt = 0;
            size_t s2ps = 0;
            vector<uint16_t> stage2Data;
            extractImageData(stage2Img, stage2Data, s2w, s2h, s2p, s2pt, s2ps);
            auto stage3ExtractStage2End = high_resolution_clock::now();
            timing.stage3_extract_stage2_ms =
                duration_cast<microseconds>(stage3ExtractStage2End - stage3ExtractStage2Start).count() / 1000.0;

            // Allocate output buffer for 3-plane RGB
            size_t outputSize = static_cast<size_t>(s2w) * s2h * 3;
            stage3Data.resize(outputSize);

            // Run Halide AHD demosaic
            auto stage3KernelStart = high_resolution_clock::now();
            demosaic_ahd_halide(stage2Data.data(), s2w, s2h, stage3Data.data());
            auto stage3KernelEnd = high_resolution_clock::now();
            timing.stage3_halide_kernel_ms =
                duration_cast<microseconds>(stage3KernelEnd - stage3KernelStart).count() / 1000.0;

            // Create DNG image object from Halide output and inject back into negative
            // so subsequent Render stage uses Halide Stage3 instead of rebuilding via DNG SDK.
            dng_point stage3Size(static_cast<int32>(s2h), static_cast<int32>(s2w));
            AutoPtr<dng_image> halideStage3(host.Make_dng_image(dng_rect(stage3Size), 3, ttShort));

            dng_pixel_buffer stage3Buffer;
            stage3Buffer.fArea = halideStage3->Bounds();
            stage3Buffer.fPlane = 0;
            stage3Buffer.fPlanes = 3;
            stage3Buffer.fPixelType = ttShort;
            stage3Buffer.fPixelSize = 2;
            stage3Buffer.fData = stage3Data.data();
            stage3Buffer.fRowStep = static_cast<int32>(s2w * 3);
            stage3Buffer.fColStep = 3;
            stage3Buffer.fPlaneStep = 1;
            auto stage3InjectStart = high_resolution_clock::now();
            halideStage3->Put(stage3Buffer);
            auto stage3InjectEnd = high_resolution_clock::now();
            timing.stage3_inject_put_ms =
                duration_cast<microseconds>(stage3InjectEnd - stage3InjectStart).count() / 1000.0;

            // Match BuildStage3Image behavior by applying OpcodeList3 before render/compare.
            auto stage3OpcodeStart = high_resolution_clock::now();
            host.ApplyOpcodeList(negative->OpcodeList3(), *negative, halideStage3);
            auto stage3OpcodeEnd = high_resolution_clock::now();
            timing.stage3_apply_opcode3_ms =
                duration_cast<microseconds>(stage3OpcodeEnd - stage3OpcodeStart).count() / 1000.0;
            negative->SetStage3Image(halideStage3);

            // Re-extract Stage3 data from injected image (after opcode processing),
            // so Stage3 PSNR compares the exact data used by Render.
            auto stage3ExtractStage3Start = high_resolution_clock::now();
            dng_image* stage3Img = const_cast<dng_image*>(negative->Stage3Image());
            extractImageData(stage3Img, stage3Data, s3w, s3h, s3p, s3pt, s3ps);
            auto stage3ExtractStage3End = high_resolution_clock::now();
            timing.stage3_extract_stage3_ms =
                duration_cast<microseconds>(stage3ExtractStage3End - stage3ExtractStage3Start).count() / 1000.0;
        } else {
            // Use DNG SDK BuildStage3Image
            auto stage3SdkBuildStart = high_resolution_clock::now();
            negative->BuildStage3Image(host);
            auto stage3SdkBuildEnd = high_resolution_clock::now();
            timing.stage3_sdk_build_ms =
                duration_cast<microseconds>(stage3SdkBuildEnd - stage3SdkBuildStart).count() / 1000.0;

            dng_image* stage3Img = const_cast<dng_image*>(negative->Stage3Image());
            extractImageData(stage3Img, stage3Data, s3w, s3h, s3p, s3pt, s3ps);
        }

        auto stage3End = high_resolution_clock::now();
        timing.build_stage3_ms = duration_cast<microseconds>(stage3End - stage3Start).count() / 1000.0;
        cout << "  Time: " << fixed << setprecision(2) << timing.build_stage3_ms << " ms\n";

        // Validate contract
        if (!StageContract::validateStageContract16("Stage3", decodePath, s3w, s3h, s3p, s3pt, s3ps, stage3Data.size())) {
            cerr << "ERROR: Stage3 contract check failed\n";
            return;
        }

        string filename = prefix + "_stage3_" + to_string(s3w) + "x" + to_string(s3h) + "_" + to_string(s3p) + "p.raw";
        if (generateBaseline) {
            saveRawFile(filename, stage3Data.data(), stage3Data.size() * sizeof(uint16_t));
            cout << "  Saved baseline: " << filename << " (" << stage3Data.size() * 2 << " bytes)\n";
        } else {
            vector<uint16_t> refData(stage3Data.size());
            string refFilename = prefix + "_stage3_" + to_string(s3w) + "x" + to_string(s3h) + "_" + to_string(s3p) + "p.raw";
            if (loadRawFile(refFilename, refData.data(), refData.size() * sizeof(uint16_t))) {
                psnr.stage3_psnr = computePSNR_16bit(refData.data(), stage3Data.data(), stage3Data.size());
                cout << "  PSNR vs baseline: " << fixed << setprecision(2) << psnr.stage3_psnr << " dB\n";
            } else {
                cout << "  WARNING: Could not load baseline file: " << refFilename << "\n";
            }
        }

        // === Render ===
        cout << "\n--- Stage 4: Render (Tone Curve + Gamma + Color Space) ---\n";
        auto renderStart = high_resolution_clock::now();

        dng_render renderer(host, *negative);
        renderer.SetMaximumSize(max(width, height));
        renderer.SetFinalPixelType(ttByte);
        renderer.SetFinalSpace(dng_space_sRGB::Get());

        AutoPtr<dng_image> finalImage(renderer.Render());

        auto renderEnd = high_resolution_clock::now();
        timing.render_ms = duration_cast<microseconds>(renderEnd - renderStart).count() / 1000.0;
        cout << "  Time: " << fixed << setprecision(2) << timing.render_ms << " ms\n";

        if (finalImage.Get()) {
            cout << "  Output: " << finalImage->Width() << "x" << finalImage->Height()
                 << " (" << finalImage->PixelType() << ")\n";

            // Extract buffer (8-bit RGB)
            uint32_t outW = finalImage->Width();
            uint32_t outH = finalImage->Height();
            vector<uint8_t> rgbData(static_cast<size_t>(outW) * outH * 3);
            dng_pixel_buffer buffer;
            buffer.fArea = finalImage->Bounds();
            buffer.fPlane = 0;
            buffer.fPlanes = 3;
            buffer.fPixelType = ttByte;
            buffer.fPixelSize = 1;
            buffer.fData = rgbData.data();
            buffer.fRowStep = outW * 3;
            buffer.fColStep = 3;
            buffer.fPlaneStep = 1;
            finalImage->Get(buffer);
            if (!StageContract::validateRenderImageContract("Stage4",
                                                            outW,
                                                            outH,
                                                            finalImage->Planes(),
                                                            finalImage->PixelType(),
                                                            finalImage->PixelSize(),
                                                            rgbData.size())) {
                cerr << "ERROR: Stage4 contract check failed\n";
                return;
            }

            // Save PPM
            string ppmFilename = "output_" + prefix + "_" + to_string(outW) + "x" + to_string(outH) + ".ppm";
            ofstream fout(ppmFilename, ios::binary);
            fout << "P6\n" << outW << " " << outH << "\n255\n";
            fout.write(reinterpret_cast<const char*>(rgbData.data()), rgbData.size());
            cout << "  Saved PPM: " << ppmFilename << "\n";

            // PSNR for Render stage (8-bit)
            string filename = prefix + "_render_" + to_string(outW) + "x" + to_string(outH) + "_3p.raw";
            if (generateBaseline) {
                saveRawFile(filename, rgbData.data(), rgbData.size());
                cout << "  Saved baseline: " << filename << " (" << rgbData.size() << " bytes)\n";
            } else {
                vector<uint8_t> refData(rgbData.size());
                string refFilename = prefix + "_render_" + to_string(outW) + "x" + to_string(outH) + "_3p.raw";
                if (loadRawFile(refFilename, refData.data(), refData.size())) {
                    psnr.render_psnr = computePSNR_8bit(refData.data(), rgbData.data(), rgbData.size());
                    cout << "  PSNR vs baseline: " << fixed << setprecision(2) << psnr.render_psnr << " dB\n";
                } else {
                    cout << "  WARNING: Could not load baseline file: " << refFilename << "\n";
                }
            }
        }

        auto totalEnd = high_resolution_clock::now();
        timing.total_ms = duration_cast<microseconds>(totalEnd - totalStart).count() / 1000.0;

        // Summary
        cout << "\n--- Timing Summary ---\n";
        cout << "  ReadStage1Image:  " << fixed << setprecision(2) << timing.read_stage1_ms << " ms ("
             << (timing.read_stage1_ms / timing.total_ms * 100) << "%)\n";
        cout << "  BuildStage2Image: " << fixed << setprecision(2) << timing.build_stage2_ms << " ms ("
             << (timing.build_stage2_ms / timing.total_ms * 100) << "%)\n";
        cout << "  BuildStage3Image: " << fixed << setprecision(2) << timing.build_stage3_ms << " ms ("
             << (timing.build_stage3_ms / timing.total_ms * 100) << "%)\n";
        if (timing.stage3_halide_kernel_ms > 0 || timing.stage3_sdk_build_ms > 0) {
            cout << "    Stage3 detail:\n";
            if (timing.stage3_halide_kernel_ms > 0) {
                cout << "      Extract Stage2: " << fixed << setprecision(2) << timing.stage3_extract_stage2_ms << " ms\n";
                cout << "      Halide kernel:  " << fixed << setprecision(2) << timing.stage3_halide_kernel_ms << " ms\n";
                cout << "      Put Stage3:     " << fixed << setprecision(2) << timing.stage3_inject_put_ms << " ms\n";
                cout << "      Apply Opcode3:  " << fixed << setprecision(2) << timing.stage3_apply_opcode3_ms << " ms\n";
                cout << "      Extract Stage3: " << fixed << setprecision(2) << timing.stage3_extract_stage3_ms << " ms\n";
            } else {
                cout << "      SDK build:      " << fixed << setprecision(2) << timing.stage3_sdk_build_ms << " ms\n";
            }
        }
        cout << "  Render:           " << fixed << setprecision(2) << timing.render_ms << " ms ("
             << (timing.render_ms / timing.total_ms * 100) << "%)\n";
        cout << "  TOTAL:            " << fixed << setprecision(2) << timing.total_ms << " ms\n";

        if (!generateBaseline) {
            cout << "\n--- PSNR Summary ---\n";
            cout << "  Stage1 (ReadStage1Image):  " << fixed << setprecision(2) << psnr.stage1_psnr << " dB\n";
            cout << "  Stage2 (BuildStage2Image): " << fixed << setprecision(2) << psnr.stage2_psnr << " dB\n";
            cout << "  Stage3 (BuildStage3Image): " << fixed << setprecision(2) << psnr.stage3_psnr << " dB\n";
            cout << "  Stage4 (Render):          " << fixed << setprecision(2) << psnr.render_psnr << " dB\n";
        }

    } catch (const dng_exception& e) {
        cerr << "\nDNG Exception: " << e.ErrorCode() << "\n";
    } catch (const std::exception& e) {
        cerr << "\nException: " << e.what() << "\n";
    }
}

void printUsage(const char* programName) {
    cerr << "Usage: " << programName << " <dng_path> <mode>\n";
    cerr << "  dng_path: Path to DNG file (lossless or lossy)\n";
    cerr << "  mode: 'baseline' or 'test'\n";
    cerr << "\nExamples:\n";
    cerr << "  # Generate baseline reference outputs:\n";
    cerr << "  " << programName << " image_samples/lossless_dng_sample.dng baseline\n";
    cerr << "  " << programName << " image_samples/lossy_dng_sample.dng baseline\n";
    cerr << "\n  # Test custom implementation against baseline:\n";
    cerr << "  " << programName << " image_samples/lossless_dng_sample.dng test\n";
    cerr << "  " << programName << " image_samples/lossy_dng_sample.dng test\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    string dngPath = argv[1];
    string mode = argv[2];

    bool generateBaseline = (mode == "baseline");
    bool isLossless = dngPath.find("lossless") != string::npos;
    string prefix = isLossless ? "lossless" : "lossy";

    cout << string(70, '=') << "\n";
    cout << "  DNG SDK Decode Pipeline Test Tool\n";
    cout << "  Phase 5.0 - Step-by-Step PSNR Analysis\n";
    cout << string(70, '=') << "\n";
    cout << "\nDNG: " << dngPath << "\n";
    cout << "Mode: " << (generateBaseline ? "BASELINE (generate reference)" : "TEST (compare to baseline)") << "\n";

    try {
        dng_host host;
        testDNG(host, dngPath, prefix, generateBaseline);
    } catch (const dng_exception& e) {
        cerr << "\nDNG Exception: " << e.ErrorCode() << "\n";
        return 1;
    } catch (const std::exception& e) {
        cerr << "\nException: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
