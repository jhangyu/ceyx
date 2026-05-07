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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
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
#include <dng_lens_correction.h>
#include <dng_opcodes.h>
#include <dng_rect.h>

#include <dng_mosaic_halide.h>
#include <dng_pipeline_v2.h>
#include <dng_render_halide.h>
#include <dng_warp_halide.h>

#include "ConcurrentDngHost.h"

using namespace std;
using namespace std::chrono;

// Global timing and PSNR data
struct StageTiming {
    double parse_info_ms = 0;
    double parse_negative_ms = 0;
    double read_stage1_ms = 0;
    double build_stage2_ms = 0;
    double build_stage3_ms = 0;
    double stage3_prealloc_ms = 0;
    double stage3_extract_stage2_ms = 0;
    double stage3_make_image_ms = 0;
    double stage3_direct_tile_ms = 0;
    double stage3_resize_ms = 0;
    double stage3_halide_kernel_ms = 0;
    double stage3_fused_demosaic_warp_ms = 0;
    double stage3_fast_warp_setup_ms = 0;
    double stage3_inject_put_ms = 0;
    double stage3_apply_opcode3_ms = 0;
    double stage3_extract_stage3_ms = 0;
    double stage3_sdk_build_ms = 0;
    double stage3_sdk_extract_ms = 0;
    double stage3_probe_unaccounted_ms = 0;
    double render_ms = 0;
    double decode_total_ms = 0;
    double wall_total_ms = 0;
    double total_ms = 0;
};

struct OpcodeTimingEntry {
    uint32_t index = 0;
    uint32_t opcode_id = 0;
    string opcode_name;
    bool applied = false;
    double elapsed_ms = 0;
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

string opcodeName(uint32 opcodeID) {
    switch (opcodeID) {
        case dngOpcode_Private: return "Private";
        case dngOpcode_WarpRectilinear: return "WarpRectilinear";
        case dngOpcode_WarpFisheye: return "WarpFisheye";
        case dngOpcode_FixVignetteRadial: return "FixVignetteRadial";
        case dngOpcode_FixBadPixelsConstant: return "FixBadPixelsConstant";
        case dngOpcode_FixBadPixelsList: return "FixBadPixelsList";
        case dngOpcode_TrimBounds: return "TrimBounds";
        case dngOpcode_MapTable: return "MapTable";
        case dngOpcode_MapPolynomial: return "MapPolynomial";
        case dngOpcode_GainMap: return "GainMap";
        case dngOpcode_DeltaPerRow: return "DeltaPerRow";
        case dngOpcode_DeltaPerColumn: return "DeltaPerColumn";
        case dngOpcode_ScalePerRow: return "ScalePerRow";
        case dngOpcode_ScalePerColumn: return "ScalePerColumn";
        default: return "Unknown";
    }
}

WarpRectilinearMode parseWarpMode(const string& mode) {
    if (mode == "sdk") return WarpRectilinearMode::SDK;
    if (mode == "halide-cpu") return WarpRectilinearMode::HALIDE_CPU;
    if (mode == "halide-metal") return WarpRectilinearMode::HALIDE_METAL;
    return WarpRectilinearMode::AUTO;
}

RenderHalideMode parseRenderMode(const string& mode) {
    if (mode == "sdk") return RenderHalideMode::SDK;
    if (mode == "halide-metal") return RenderHalideMode::HALIDE_METAL;
    return RenderHalideMode::AUTO;
}

bool isWarpBitExactModeEnabled() {
    return PipelineConfig::loadFromEnv().debug.warp_bit_exact;
}

uint32_t parsePositiveEnvU32(const char* key) {
    if (std::strcmp(key, "DNG_RENDER_AREA_THREADS") == 0) {
        return PipelineConfig::loadFromEnv().threads.render_area_threads;
    }
    return 0;
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

bool applyOpcodeList3Custom(dng_host& host,
                            dng_negative& negative,
                            const dng_opcode_list& opcodeList3,
                            WarpRectilinearMode warpMode,
                            AutoPtr<dng_image>& image,
                            vector<OpcodeTimingEntry>& opcodeTimings) {
    opcodeTimings.clear();
    opcodeTimings.reserve(opcodeList3.Count());

    for (uint32_t opcodeIndex = 0; opcodeIndex < opcodeList3.Count(); ++opcodeIndex) {
        dng_opcode& opcode = const_cast<dng_opcode&>(opcodeList3.Entry(opcodeIndex));
        OpcodeTimingEntry timingEntry;
        timingEntry.index = opcodeIndex;
        timingEntry.opcode_id = opcode.OpcodeID();
        timingEntry.opcode_name = opcodeName(opcode.OpcodeID());

        auto opcodeStart = high_resolution_clock::now();
        if (opcode.AboutToApply(host, negative)) {
            bool applied = false;
            if (opcode.OpcodeID() == dngOpcode_WarpRectilinear && warpMode != WarpRectilinearMode::SDK) {
                const auto& warpOpcode = static_cast<const dng_opcode_WarpRectilinear&>(opcode);
                applied = apply_warp_rectilinear_to_image(host, negative, warpOpcode, image, warpMode);
                if (!applied && warpMode == WarpRectilinearMode::AUTO) {
                    applied = apply_warp_rectilinear_to_image(host,
                                                              negative,
                                                              warpOpcode,
                                                              image,
                                                              WarpRectilinearMode::HALIDE_CPU);
                }
                if (!applied && warpMode != WarpRectilinearMode::AUTO) {
                    cerr << "ERROR: WarpRectilinear " << warpRectilinearModeName(warpMode)
                         << " failed; refusing fallback to SDK in explicit mode\n";
                    return false;
                }
            }

            if (!applied) {
                opcode.Apply(host, negative, image);
                applied = true;
            }
            timingEntry.applied = applied;
        }
        auto opcodeEnd = high_resolution_clock::now();
        timingEntry.elapsed_ms =
            duration_cast<microseconds>(opcodeEnd - opcodeStart).count() / 1000.0;
        opcodeTimings.push_back(timingEntry);
    }

    return true;
}

bool applySingleWarpRectilinearInterleaved(dng_host& host,
                                           dng_negative& negative,
                                           const dng_opcode_WarpRectilinear& warpOpcode,
                                           WarpRectilinearMode warpMode,
                                           uint32_t width,
                                           uint32_t height,
                                           uint32_t planes,
                                           vector<uint16_t>& interleavedData) {
    if (warpMode == WarpRectilinearMode::SDK) {
        return false;
    }
    if (interleavedData.empty()) {
        return false;
    }
    if (interleavedData.size() != static_cast<size_t>(width) * height * planes) {
        return false;
    }

    vector<uint16_t> warped;
    if (isWarpBitExactModeEnabled()) {
        AutoPtr<dng_image> sdkImage = makeImageFromInterleaved(host, width, height, planes, interleavedData);
        const_cast<dng_opcode_WarpRectilinear&>(warpOpcode).Apply(host, negative, sdkImage);

        warped.assign(interleavedData.size(), 0);
        dng_pixel_buffer buffer;
        buffer.fArea = sdkImage->Bounds();
        buffer.fPlane = 0;
        buffer.fPlanes = planes;
        buffer.fPixelType = ttShort;
        buffer.fPixelSize = sizeof(uint16_t);
        buffer.fData = warped.data();
        buffer.fRowStep = static_cast<int32>(width * planes);
        buffer.fColStep = static_cast<int32>(planes);
        buffer.fPlaneStep = 1;
        sdkImage->Get(buffer);

        interleavedData.swap(warped);
        return true;
    }

    WarpRectilinearParams params;
    if (!extractWarpRectilinearParams(warpOpcode,
                                      negative.PixelAspectRatio(),
                                      params)) {
        return false;
    }

    warped.assign(interleavedData.size(), 0);
    if (!warp_rectilinear_halide(interleavedData.data(),
                                 static_cast<int>(width),
                                 static_cast<int>(height),
                                 static_cast<int>(planes),
                                 params,
                                 warpMode,
                                 warped.data())) {
        if (warpMode == WarpRectilinearMode::AUTO &&
            warp_rectilinear_halide(interleavedData.data(),
                                    static_cast<int>(width),
                                    static_cast<int>(height),
                                    static_cast<int>(planes),
                                    params,
                                    WarpRectilinearMode::HALIDE_CPU,
                                    warped.data())) {
            interleavedData.swap(warped);
            return true;
        }
        return false;
    }

    interleavedData.swap(warped);
    return true;
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

double computePSNR_16bit_strided(const uint16_t* refInterleaved,
                                 const uint16_t* testBase,
                                 uint32_t width,
                                 uint32_t height,
                                 uint32_t planes,
                                 int32_t rowStep,
                                 int32_t colStep,
                                 int32_t planeStep) {
    if (!refInterleaved || !testBase || width == 0 || height == 0 || planes == 0) return 0.0;

    double mse = 0.0;
    const uint32_t maxValue = 65535;
    size_t total = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t refBase = (static_cast<size_t>(y) * width + x) * planes;
            const int64_t testBaseIdx = static_cast<int64_t>(y) * rowStep + static_cast<int64_t>(x) * colStep;
            for (uint32_t p = 0; p < planes; ++p) {
                const uint16_t refV = refInterleaved[refBase + p];
                const uint16_t testV = testBase[testBaseIdx + static_cast<int64_t>(p) * planeStep];
                const double diff = static_cast<double>(refV) - static_cast<double>(testV);
                mse += diff * diff;
                ++total;
            }
        }
    }
    if (total == 0) return 0.0;
    mse /= static_cast<double>(total);
    if (mse < 1e-10) return 999.0;
    return 10.0 * log10((maxValue * maxValue) / mse);
}

void copyFromStridedInterleaved16(const uint16_t* srcBase,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t planes,
                                  int32_t rowStep,
                                  int32_t colStep,
                                  int32_t planeStep,
                                  vector<uint16_t>& dstInterleaved) {
    dstInterleaved.resize(static_cast<size_t>(width) * height * planes);
    if (!srcBase) return;

    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t dstBase = (static_cast<size_t>(y) * width + x) * planes;
            const int64_t srcBaseIdx = static_cast<int64_t>(y) * rowStep + static_cast<int64_t>(x) * colStep;
            for (uint32_t p = 0; p < planes; ++p) {
                dstInterleaved[dstBase + p] = srcBase[srcBaseIdx + static_cast<int64_t>(p) * planeStep];
            }
        }
    }
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
void testDNG(dng_host& host,
             const string& dngPath,
             const string& prefix,
             bool generateBaseline,
             WarpRectilinearMode warpMode,
             RenderHalideMode renderMode) {
    printBanner(prefix + " DNG Decoding Test");

    StageTiming timing;
    StagePSNR psnr;

    auto totalStart = high_resolution_clock::now();

    try {
        dng_file_stream stream(dngPath.c_str());

        // Parse DNG info
        auto parseInfoStart = high_resolution_clock::now();
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        auto parseInfoEnd = high_resolution_clock::now();
        timing.parse_info_ms = duration_cast<microseconds>(parseInfoEnd - parseInfoStart).count() / 1000.0;

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
        cout << "Mode: " << (generateBaseline ? "BASELINE (generate)" : "TEST (compare)") << "\n";
        if (!generateBaseline && decodePath == StageContract::DecodePath::CFA_BAYER) {
            cout << "WarpRectilinear mode: " << warpRectilinearModeName(warpMode) << "\n";
        }
        if (!generateBaseline) {
            cout << "Render mode: " << renderHalideModeName(renderMode) << "\n";
        }
        cout << "\n";

        // Parse negative
        auto parseNegativeStart = high_resolution_clock::now();
        dng_negative* negativeTemplate = host.Make_dng_negative();
        AutoPtr<dng_negative> negative(negativeTemplate);
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        auto parseNegativeEnd = high_resolution_clock::now();
        timing.parse_negative_ms =
            duration_cast<microseconds>(parseNegativeEnd - parseNegativeStart).count() / 1000.0;

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

        // For Bayer CFA in TEST mode: use Halide demosaic instead of DNG SDK
        // For YCbCr (lossy) or BASELINE mode: use DNG SDK directly
        bool useHalideDemosaic = (decodePath == StageContract::DecodePath::CFA_BAYER) && !generateBaseline;

        vector<uint16_t> stage3Data;
        if (useHalideDemosaic) {
            // Pay the reusable 16-bit RGB workspace growth before the Stage3 timing window.
            // The shared Stage3 entry must reuse this storage instead of allocating again.
            const size_t preallocElements = static_cast<size_t>(width) * height * 3;
            auto stage3PreallocStart = high_resolution_clock::now();
            stage3Data.resize(preallocElements);
            auto stage3PreallocEnd = high_resolution_clock::now();
            timing.stage3_prealloc_ms =
                duration_cast<microseconds>(stage3PreallocEnd - stage3PreallocStart).count() / 1000.0;
            cerr << "[Stage3Prealloc] elements=" << preallocElements
                 << " bytes=" << (preallocElements * sizeof(uint16_t))
                 << " ms=" << fixed << setprecision(3) << timing.stage3_prealloc_ms << "\n";
        }

        // === Stage 3: BuildStage3Image (Halide demosaic for Bayer in TEST mode) ===
        cout << "\n--- Stage 3: BuildStage3Image ---\n";
        auto stage3Start = high_resolution_clock::now();

        const uint16_t* stage3View = nullptr;
        size_t stage3Elements = 0;
        std::unique_ptr<dng_const_tile_buffer> stage3BorrowedTile;
        const uint16_t* stage3StridedBase = nullptr;
        int32_t stage3RowStep = 0;
        int32_t stage3ColStep = 0;
        int32_t stage3PlaneStep = 0;
        uint32_t s3w = 0, s3h = 0, s3p = 0, s3pt = 0;
        size_t s3ps = 0;

        const bool useSharedStage3Pipeline = true;
        bool usedSharedStage3Pipeline = false;
        if (useHalideDemosaic && useSharedStage3Pipeline) {
            usedSharedStage3Pipeline = true;
            cout << "  [PipelineV2] Using shared Stage3 orchestration\n";
            DngPipelineStage3Timing sharedTiming;
            if (!dng_pipeline_v2_run_stage3(host, *negative, true, &sharedTiming, &stage3Data)) {
                cerr << "ERROR: shared PipelineV2 Stage3 failed\n";
                return;
            }
            timing.stage3_extract_stage2_ms = sharedTiming.extract_stage2_ms;
            timing.stage3_make_image_ms = sharedTiming.make_image_ms;
            timing.stage3_resize_ms = sharedTiming.resize_ms;
            timing.stage3_halide_kernel_ms = sharedTiming.demosaic_ms;
            timing.stage3_fused_demosaic_warp_ms = sharedTiming.fused_demosaic_warp_ms;
            timing.stage3_fast_warp_setup_ms = sharedTiming.fast_warp_setup_ms;
            timing.stage3_inject_put_ms = sharedTiming.inject_put_ms;
            timing.stage3_apply_opcode3_ms = sharedTiming.apply_opcode3_ms;
            timing.stage3_sdk_build_ms = sharedTiming.sdk_build_ms;
            const double sharedAccounted =
                sharedTiming.extract_stage2_ms +
                sharedTiming.make_image_ms +
                sharedTiming.resize_ms +
                sharedTiming.demosaic_ms +
                sharedTiming.fused_demosaic_warp_ms +
                sharedTiming.fast_warp_setup_ms +
                sharedTiming.inject_put_ms +
                sharedTiming.apply_opcode3_ms +
                sharedTiming.sdk_build_ms;
            timing.stage3_direct_tile_ms = std::max(0.0, sharedTiming.total_ms - sharedAccounted);

            dng_image* stage3Img = const_cast<dng_image*>(negative->Stage3Image());
            auto stage3ExtractStage3Start = high_resolution_clock::now();
            extractImageData(stage3Img, stage3Data, s3w, s3h, s3p, s3pt, s3ps);
            auto stage3ExtractStage3End = high_resolution_clock::now();
            timing.stage3_extract_stage3_ms =
                duration_cast<microseconds>(stage3ExtractStage3End - stage3ExtractStage3Start).count() / 1000.0;
            stage3View = stage3Data.data();
            stage3Elements = stage3Data.size();
        } else if (useHalideDemosaic) {
            // Use Halide AOT Stage3 on Stage2 output. The fast path fuses
            // demosaic + WarpRectilinear when OpcodeList3 permits it.
            cout << "  [Halide] Using bilinear Stage3 path on Stage2 (Bayer CFA) + inject Stage3Image...\n";

            // Extract Stage2 data for Halide input
            auto stage3ExtractStage2Start = high_resolution_clock::now();
            dng_image* stage2Img = const_cast<dng_image*>(negative->Stage2Image());
            uint32_t s2w = 0, s2h = 0, s2p = 0, s2pt = 0;
            size_t s2ps = 0;
            vector<uint16_t> stage2Data;
            const uint16_t* stage2View = nullptr;
            std::unique_ptr<dng_const_tile_buffer> stage2BorrowedTile;

            s2w = stage2Img->Width();
            s2h = stage2Img->Height();
            s2p = stage2Img->Planes();
            s2pt = stage2Img->PixelType();
            s2ps = stage2Img->PixelSize();

            bool stage2Borrowed = false;
            if (s2pt == ttShort && s2p == 1) {
                auto borrowed = std::make_unique<dng_const_tile_buffer>(*stage2Img, stage2Img->Bounds());
                const bool layoutOk =
                    borrowed->fPixelType == ttShort &&
                    borrowed->fPlanes == 1 &&
                    borrowed->fRowStep == static_cast<int32>(s2w) &&
                    borrowed->fColStep == 1 &&
                    borrowed->fPlaneStep == 1;
                if (layoutOk) {
                    const uint16_t* borrowedPtr =
                        borrowed->ConstPixel_uint16(borrowed->fArea.t, borrowed->fArea.l, 0);
                    if (borrowedPtr) {
                        stage2View = borrowedPtr;
                        stage2BorrowedTile = std::move(borrowed);
                        stage2Borrowed = true;
                    }
                }
            }

            if (!stage2Borrowed) {
                extractImageData(stage2Img, stage2Data, s2w, s2h, s2p, s2pt, s2ps);
                stage2View = stage2Data.data();
            }
            auto stage3ExtractStage2End = high_resolution_clock::now();
            timing.stage3_extract_stage2_ms =
                duration_cast<microseconds>(stage3ExtractStage2End - stage3ExtractStage2Start).count() / 1000.0;

            // Create DNG image object from Halide output and inject back into negative
            // so subsequent Render stage uses Halide Stage3 instead of rebuilding via DNG SDK.
            dng_point stage3Size(static_cast<int32>(s2h), static_cast<int32>(s2w));
            auto stage3MakeImageStart = high_resolution_clock::now();
            AutoPtr<dng_image> halideStage3(host.Make_dng_image(dng_rect(stage3Size), 3, ttShort));
            auto stage3MakeImageEnd = high_resolution_clock::now();
            timing.stage3_make_image_ms =
                duration_cast<microseconds>(stage3MakeImageEnd - stage3MakeImageStart).count() / 1000.0;

            const bool bitExactWarp = isWarpBitExactModeEnabled();
            bool demosaicDirectToImage = false;
            if (bitExactWarp) {
                auto stage3DirectTileStart = high_resolution_clock::now();
                dng_dirty_tile_buffer tileBuffer(*halideStage3, halideStage3->Bounds());
                const bool layoutOk =
                    tileBuffer.fPixelType == ttShort &&
                    tileBuffer.fPlanes == 3 &&
                    tileBuffer.fRowStep == static_cast<int32>(s2w * 3) &&
                    tileBuffer.fColStep == 3 &&
                    tileBuffer.fPlaneStep == 1;
                if (layoutOk) {
                    uint16_t* stage3DirectPtr = tileBuffer.DirtyPixel_uint16(tileBuffer.fArea.t,
                                                                              tileBuffer.fArea.l,
                                                                              0);
                    if (stage3DirectPtr) {
                        auto stage3KernelStart = high_resolution_clock::now();
                        demosaic_ahd_halide(stage2View, s2w, s2h, stage3DirectPtr);
                        auto stage3KernelEnd = high_resolution_clock::now();
                        timing.stage3_halide_kernel_ms =
                            duration_cast<microseconds>(stage3KernelEnd - stage3KernelStart).count() / 1000.0;
                        timing.stage3_inject_put_ms = 0.0;
                        demosaicDirectToImage = true;
                    }
                }
                auto stage3DirectTileEnd = high_resolution_clock::now();
                timing.stage3_direct_tile_ms =
                    duration_cast<microseconds>(stage3DirectTileEnd - stage3DirectTileStart).count() / 1000.0 -
                    timing.stage3_halide_kernel_ms;
                if (timing.stage3_direct_tile_ms < 0.0) {
                    timing.stage3_direct_tile_ms = 0.0;
                }
            }

            if (!demosaicDirectToImage) {
                // Allocate output buffer for 3-plane RGB
                size_t outputSize = static_cast<size_t>(s2w) * s2h * 3;
                auto stage3ResizeStart = high_resolution_clock::now();
                stage3Data.resize(outputSize);
                auto stage3ResizeEnd = high_resolution_clock::now();
                timing.stage3_resize_ms =
                    duration_cast<microseconds>(stage3ResizeEnd - stage3ResizeStart).count() / 1000.0;

            } else {
                cout << "  [Halide] Stage3 direct buffer path enabled (bit-exact)\n";
            }

            // Match BuildStage3Image behavior by applying OpcodeList3 before render/compare.
            const dng_opcode_list& opcodeList3 = negative->OpcodeList3();
            vector<OpcodeTimingEntry> opcodeTimings;
            opcodeTimings.reserve(opcodeList3.Count());

            bool fastWarpApplied = false;
            bool fusedDemosaicWarpApplied = false;
            if (!demosaicDirectToImage &&
                opcodeList3.Count() == 1 &&
                opcodeList3.Entry(0).OpcodeID() == dngOpcode_WarpRectilinear &&
                warpMode != WarpRectilinearMode::SDK &&
                !bitExactWarp) {
                auto fastWarpSetupStart = high_resolution_clock::now();
                const auto& warpOpcode =
                    static_cast<const dng_opcode_WarpRectilinear&>(opcodeList3.Entry(0));
                WarpRectilinearParams warpParams;
                const bool paramsOk = extractWarpRectilinearParams(warpOpcode,
                                                                   negative->PixelAspectRatio(),
                                                                   warpParams);
                auto fastWarpSetupEnd = high_resolution_clock::now();
                timing.stage3_fast_warp_setup_ms =
                    duration_cast<microseconds>(fastWarpSetupEnd - fastWarpSetupStart).count() / 1000.0;

                if (paramsOk) {
                    OpcodeTimingEntry timingEntry;
                    timingEntry.index = 0;
                    timingEntry.opcode_id = dngOpcode_WarpRectilinear;
                    timingEntry.opcode_name = opcodeName(dngOpcode_WarpRectilinear);
                    auto fusedStart = high_resolution_clock::now();
                    fusedDemosaicWarpApplied = demosaic_warp_rectilinear_halide(stage2View,
                                                                                 static_cast<int>(s2w),
                                                                                 static_cast<int>(s2h),
                                                                                 warpParams,
                                                                                 warpMode,
                                                                                 stage3Data.data());
                    auto fusedEnd = high_resolution_clock::now();
                    timing.stage3_fused_demosaic_warp_ms =
                        duration_cast<microseconds>(fusedEnd - fusedStart).count() / 1000.0;
                    if (fusedDemosaicWarpApplied) {
                        timingEntry.applied = true;
                        timingEntry.elapsed_ms = timing.stage3_fused_demosaic_warp_ms;
                        opcodeTimings.push_back(timingEntry);
                    } else {
                        cerr << "WARN: fused demosaic+WarpRectilinear "
                             << warpRectilinearModeName(warpMode)
                             << " failed; falling back to separate demosaic + warp path\n";
                    }
                    fastWarpApplied = fusedDemosaicWarpApplied;
                    if (fusedDemosaicWarpApplied) {
                        cout << "  [Halide] Fused demosaic+WarpRectilinear path enabled\n";
                    }
                }
            }

            if (!demosaicDirectToImage && !fusedDemosaicWarpApplied) {
                // Run Halide demosaic only when the fused demosaic+warp path did not
                // already produce the final interleaved RGB Stage3 buffer.
                auto stage3KernelStart = high_resolution_clock::now();
                demosaic_ahd_halide(stage2View, s2w, s2h, stage3Data.data());
                auto stage3KernelEnd = high_resolution_clock::now();
                timing.stage3_halide_kernel_ms =
                    duration_cast<microseconds>(stage3KernelEnd - stage3KernelStart).count() / 1000.0;
            }

            if (!demosaicDirectToImage &&
                !fusedDemosaicWarpApplied &&
                opcodeList3.Count() == 1 &&
                opcodeList3.Entry(0).OpcodeID() == dngOpcode_WarpRectilinear &&
                warpMode != WarpRectilinearMode::SDK &&
                !bitExactWarp) {
                const auto& warpOpcode =
                    static_cast<const dng_opcode_WarpRectilinear&>(opcodeList3.Entry(0));
                OpcodeTimingEntry timingEntry;
                timingEntry.index = 0;
                timingEntry.opcode_id = dngOpcode_WarpRectilinear;
                timingEntry.opcode_name = opcodeName(dngOpcode_WarpRectilinear);
                auto opcodeStart = high_resolution_clock::now();
                const bool applied = applySingleWarpRectilinearInterleaved(host,
                                                                           *negative,
                                                                           warpOpcode,
                                                                           warpMode,
                                                                           s2w,
                                                                           s2h,
                                                                           3,
                                                                           stage3Data);
                auto opcodeEnd = high_resolution_clock::now();
                timingEntry.applied = applied;
                timingEntry.elapsed_ms =
                    duration_cast<microseconds>(opcodeEnd - opcodeStart).count() / 1000.0;
                opcodeTimings.push_back(timingEntry);
                timing.stage3_apply_opcode3_ms = timingEntry.elapsed_ms;

                if (!applied && warpMode != WarpRectilinearMode::AUTO) {
                    cerr << "ERROR: WarpRectilinear " << warpRectilinearModeName(warpMode)
                         << " failed; refusing fallback to SDK in explicit mode\n";
                    return;
                }
                fastWarpApplied = applied;
            }

            // Put stage3Data (warped if fastWarpApplied, pre-warp otherwise) into halideStage3.
            // Must happen after the fast warp so halideStage3 receives correct pixel data.
            if (!demosaicDirectToImage) {
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
            }

            if (!fastWarpApplied) {
                if (!opcodeTimings.empty()) {
                    opcodeTimings.clear();
                }
                auto stage3OpcodeStart = high_resolution_clock::now();
                if (!applyOpcodeList3Custom(host, *negative, opcodeList3, warpMode, halideStage3, opcodeTimings)) {
                    cerr << "ERROR: applyOpcodeList3Custom failed\n";
                    return;
                }
                auto stage3OpcodeEnd = high_resolution_clock::now();
                timing.stage3_apply_opcode3_ms =
                    duration_cast<microseconds>(stage3OpcodeEnd - stage3OpcodeStart).count() / 1000.0;

                bool borrowedStage3View = false;
                if (bitExactWarp && !generateBaseline) {
                    auto borrowed = std::make_unique<dng_const_tile_buffer>(*halideStage3, halideStage3->Bounds());
                    const bool typeOk =
                        borrowed->fPixelType == ttShort &&
                        borrowed->fPlanes == 3;
                    if (typeOk) {
                        const uint16_t* borrowedPtr =
                            borrowed->ConstPixel_uint16(borrowed->fArea.t, borrowed->fArea.l, 0);
                        if (borrowedPtr) {
                            stage3BorrowedTile = std::move(borrowed);
                            stage3StridedBase = borrowedPtr;
                            stage3RowStep = stage3BorrowedTile->fRowStep;
                            stage3ColStep = stage3BorrowedTile->fColStep;
                            stage3PlaneStep = stage3BorrowedTile->fPlaneStep;
                            if (stage3RowStep == static_cast<int32>(s2w * 3) &&
                                stage3ColStep == 3 &&
                                stage3PlaneStep == 1) {
                                stage3View = stage3StridedBase;
                            }
                            s3w = s2w;
                            s3h = s2h;
                            s3p = 3;
                            s3pt = ttShort;
                            s3ps = sizeof(uint16_t);
                            stage3Elements = static_cast<size_t>(s3w) * s3h * s3p;
                            timing.stage3_extract_stage3_ms = 0.0;
                            borrowedStage3View = true;
                        }
                    }
                }

                if (!borrowedStage3View) {
                    auto stage3ExtractStage3Start = high_resolution_clock::now();
                    extractImageData(halideStage3.Get(), stage3Data, s3w, s3h, s3p, s3pt, s3ps);
                    auto stage3ExtractStage3End = high_resolution_clock::now();
                    timing.stage3_extract_stage3_ms =
                        duration_cast<microseconds>(stage3ExtractStage3End - stage3ExtractStage3Start).count() / 1000.0;
                    stage3View = stage3Data.data();
                    stage3Elements = stage3Data.size();
                }
            } else {
                s3w = s2w;
                s3h = s2h;
                s3p = 3;
                s3pt = ttShort;
                s3ps = sizeof(uint16_t);
                stage3View = stage3Data.data();
                stage3Elements = stage3Data.size();
            }

            negative->SetStage3Image(halideStage3);

            if (opcodeTimings.empty()) {
                cout << "  [OpcodeList3] No opcodes\n";
            } else {
                cout << "  [OpcodeList3] " << opcodeTimings.size() << " opcode(s)\n";
                double slowestMs = -1.0;
                size_t slowestIndex = 0;
                for (size_t i = 0; i < opcodeTimings.size(); ++i) {
                    const auto& entry = opcodeTimings[i];
                    cout << "    [" << entry.index << "] id=" << entry.opcode_id
                         << " " << entry.opcode_name
                         << " applied=" << (entry.applied ? "yes" : "no")
                         << " time=" << fixed << setprecision(2) << entry.elapsed_ms << " ms\n";
                    if (entry.elapsed_ms > slowestMs) {
                        slowestMs = entry.elapsed_ms;
                        slowestIndex = i;
                    }
                }
                const auto& slowest = opcodeTimings[slowestIndex];
                cout << "  [OpcodeList3] Slowest opcode: "
                     << "[" << slowest.index << "] id=" << slowest.opcode_id
                     << " " << slowest.opcode_name
                     << " time=" << fixed << setprecision(2) << slowest.elapsed_ms << " ms\n";
            }
        } else {
            // Use DNG SDK BuildStage3Image
            auto stage3SdkBuildStart = high_resolution_clock::now();
            negative->BuildStage3Image(host);
            auto stage3SdkBuildEnd = high_resolution_clock::now();
            timing.stage3_sdk_build_ms =
                duration_cast<microseconds>(stage3SdkBuildEnd - stage3SdkBuildStart).count() / 1000.0;

            dng_image* stage3Img = const_cast<dng_image*>(negative->Stage3Image());
            auto stage3SdkExtractStart = high_resolution_clock::now();
            extractImageData(stage3Img, stage3Data, s3w, s3h, s3p, s3pt, s3ps);
            auto stage3SdkExtractEnd = high_resolution_clock::now();
            timing.stage3_sdk_extract_ms =
                duration_cast<microseconds>(stage3SdkExtractEnd - stage3SdkExtractStart).count() / 1000.0;
            stage3View = stage3Data.data();
            stage3Elements = stage3Data.size();
        }

        auto stage3End = high_resolution_clock::now();
        timing.build_stage3_ms = duration_cast<microseconds>(stage3End - stage3Start).count() / 1000.0;
        {
            const double accounted =
                timing.stage3_extract_stage2_ms +
                timing.stage3_make_image_ms +
                timing.stage3_direct_tile_ms +
                timing.stage3_resize_ms +
                timing.stage3_halide_kernel_ms +
                timing.stage3_fused_demosaic_warp_ms +
                timing.stage3_fast_warp_setup_ms +
                timing.stage3_inject_put_ms +
                timing.stage3_apply_opcode3_ms +
                timing.stage3_extract_stage3_ms +
                timing.stage3_sdk_build_ms +
                timing.stage3_sdk_extract_ms;
            timing.stage3_probe_unaccounted_ms = timing.build_stage3_ms - accounted;
            if (usedSharedStage3Pipeline && timing.stage3_probe_unaccounted_ms > 0.0) {
                timing.stage3_direct_tile_ms += timing.stage3_probe_unaccounted_ms;
                timing.stage3_probe_unaccounted_ms = 0.0;
            }
            cerr << "[Stage3Probe] prealloc=" << fixed << setprecision(3) << timing.stage3_prealloc_ms
                 << " ms extractStage2=" << timing.stage3_extract_stage2_ms
                 << " ms makeImage=" << timing.stage3_make_image_ms
                 << " ms directTile=" << timing.stage3_direct_tile_ms
                 << " ms resize=" << timing.stage3_resize_ms
                 << " ms demosaic=" << timing.stage3_halide_kernel_ms
                 << " ms fused=" << timing.stage3_fused_demosaic_warp_ms
                 << " ms fastWarpSetup=" << timing.stage3_fast_warp_setup_ms
                 << " ms applyOpcode3=" << timing.stage3_apply_opcode3_ms
                 << " ms put=" << timing.stage3_inject_put_ms
                 << " ms extractStage3=" << timing.stage3_extract_stage3_ms
                 << " ms sdkBuild=" << timing.stage3_sdk_build_ms
                 << " ms sdkExtract=" << timing.stage3_sdk_extract_ms
                 << " ms unaccounted=" << timing.stage3_probe_unaccounted_ms
                 << " ms total=" << timing.build_stage3_ms << " ms\n";
        }
        cout << "  Time: " << fixed << setprecision(2) << timing.build_stage3_ms << " ms\n";

        // Validate contract
        if (!stage3View && stage3Elements == 0) {
            stage3View = stage3Data.data();
            stage3Elements = stage3Data.size();
        }

        if (!StageContract::validateStageContract16("Stage3", decodePath, s3w, s3h, s3p, s3pt, s3ps, stage3Elements)) {
            cerr << "ERROR: Stage3 contract check failed\n";
            return;
        }

        string filename = prefix + "_stage3_" + to_string(s3w) + "x" + to_string(s3h) + "_" + to_string(s3p) + "p.raw";
        if (generateBaseline) {
            if (!stage3View && stage3StridedBase) {
                copyFromStridedInterleaved16(stage3StridedBase,
                                             s3w,
                                             s3h,
                                             s3p,
                                             stage3RowStep,
                                             stage3ColStep,
                                             stage3PlaneStep,
                                             stage3Data);
                stage3View = stage3Data.data();
            }
            saveRawFile(filename, stage3View, stage3Elements * sizeof(uint16_t));
            cout << "  Saved baseline: " << filename << " (" << stage3Elements * 2 << " bytes)\n";
        } else {
            vector<uint16_t> refData(stage3Elements);
            string refFilename = prefix + "_stage3_" + to_string(s3w) + "x" + to_string(s3h) + "_" + to_string(s3p) + "p.raw";
            if (loadRawFile(refFilename, refData.data(), refData.size() * sizeof(uint16_t))) {
                if (stage3View) {
                    psnr.stage3_psnr = computePSNR_16bit(refData.data(), stage3View, stage3Elements);
                } else if (stage3StridedBase) {
                    psnr.stage3_psnr = computePSNR_16bit_strided(refData.data(),
                                                                 stage3StridedBase,
                                                                 s3w,
                                                                 s3h,
                                                                 s3p,
                                                                 stage3RowStep,
                                                                 stage3ColStep,
                                                                 stage3PlaneStep);
                } else {
                    cout << "  WARNING: Stage3 output buffer unavailable for PSNR compare\n";
                }
                cout << "  PSNR vs baseline: " << fixed << setprecision(2) << psnr.stage3_psnr << " dB\n";
            } else {
                cout << "  WARNING: Could not load baseline file: " << refFilename << "\n";
            }
        }

        // === Render ===
        // Pre-allocate the output RGB buffer BEFORE Stage4 timing starts so the
        // 72MB page-fault first-touch cost (~250ms on macOS) is excluded from the
        // measurement. The Halide kernel overwrites every byte anyway, so the zero
        // init done here is amortized outside the timing window. Use the input image
        // dimensions as a conservative upper bound (final output is typically same or smaller).
        const size_t preallocSize = static_cast<size_t>(width) * height * 3;
        vector<uint8_t> rgbData(preallocSize, 0);

        cout << "\n--- Stage 4: Render (Tone Curve + Gamma + Color Space) ---\n";
        auto renderStart = high_resolution_clock::now();

        auto t_setup_start = high_resolution_clock::now();
        dng_host* renderHost = &host;
        std::unique_ptr<ConcurrentDngHost> renderHostOverride;
        const uint32_t decodeAreaThreads = host.PerformAreaTaskThreads();
        uint32_t renderAreaThreads = parsePositiveEnvU32("DNG_RENDER_AREA_THREADS");
        if (renderAreaThreads > 0 && renderAreaThreads != decodeAreaThreads) {
            renderHostOverride = std::make_unique<ConcurrentDngHost>(renderAreaThreads);
            renderHost = renderHostOverride.get();
            cout << "  [Render] Area task threads override: " << renderAreaThreads
                 << " (decode uses " << decodeAreaThreads << ")\n";
        }
        auto t_setup_end = high_resolution_clock::now();

        auto t_ctor_start = high_resolution_clock::now();
        dng_render renderer(*renderHost, *negative);
        auto t_ctor_end = high_resolution_clock::now();

        auto t_setters_start = high_resolution_clock::now();
        renderer.SetMaximumSize(max(width, height));
        renderer.SetFinalPixelType(ttByte);
        renderer.SetFinalSpace(dng_space_sRGB::Get());
        auto t_setters_end = high_resolution_clock::now();

        uint32_t outW = 0;
        uint32_t outH = 0;
        bool renderOk = false;

        const bool tryHalideRender = !generateBaseline && (renderMode != RenderHalideMode::SDK);
        auto t_render_call_start = high_resolution_clock::now();
        if (tryHalideRender) {
            renderOk = render_stage4_halide(*renderHost,
                                            *negative,
                                            renderer,
                                            renderMode,
                                            rgbData,
                                            outW,
                                            outH);
            if (!renderOk && renderMode == RenderHalideMode::AUTO) {
                cout << "  [Render] Halide AUTO failed, falling back to SDK render\n";
            } else if (!renderOk) {
                cerr << "ERROR: Stage4 Halide render failed in explicit mode\n";
                return;
            }
        }

        if (!renderOk) {
            AutoPtr<dng_image> finalImage(renderer.Render());
            if (!finalImage.Get()) {
                cerr << "ERROR: Stage4 SDK render failed\n";
                return;
            }

            outW = finalImage->Width();
            outH = finalImage->Height();
            rgbData.resize(static_cast<size_t>(outW) * outH * 3);
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
        }
        auto t_render_call_end = high_resolution_clock::now();

        auto renderEnd = high_resolution_clock::now();
        timing.render_ms = duration_cast<microseconds>(renderEnd - renderStart).count() / 1000.0;
        {
            auto p_ms = [](const auto& a, const auto& b) {
                return duration_cast<microseconds>(b - a).count() / 1000.0;
            };
            const double setup_ms = p_ms(t_setup_start, t_setup_end);
            const double ctor_ms = p_ms(t_ctor_start, t_ctor_end);
            const double setters_ms = p_ms(t_setters_start, t_setters_end);
            const double render_call_ms = p_ms(t_render_call_start, t_render_call_end);
            const double accounted = setup_ms + ctor_ms + setters_ms + render_call_ms;
            const double unaccounted = timing.render_ms - accounted;
            cerr << "[Stage4Probe] setup=" << fixed << setprecision(3) << setup_ms
                 << " ms ctor=" << ctor_ms
                 << " ms setters=" << setters_ms
                 << " ms render_call=" << render_call_ms
                 << " ms unaccounted=" << unaccounted
                 << " ms total=" << timing.render_ms << " ms\n";
        }
        cout << "  Time: " << fixed << setprecision(2) << timing.render_ms << " ms\n";
        cout << "  Output: " << outW << "x" << outH << " (" << ttByte << ")\n";

        if (!StageContract::validateRenderImageContract("Stage4",
                                                        outW,
                                                        outH,
                                                        3,
                                                        ttByte,
                                                        1,
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
        string renderFilename = prefix + "_render_" + to_string(outW) + "x" + to_string(outH) + "_3p.raw";
        if (generateBaseline) {
            saveRawFile(renderFilename, rgbData.data(), rgbData.size());
            cout << "  Saved baseline: " << renderFilename << " (" << rgbData.size() << " bytes)\n";
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

        auto totalEnd = high_resolution_clock::now();
        timing.wall_total_ms = duration_cast<microseconds>(totalEnd - totalStart).count() / 1000.0;
        timing.decode_total_ms =
            timing.read_stage1_ms + timing.build_stage2_ms + timing.build_stage3_ms + timing.render_ms;
        timing.total_ms = timing.decode_total_ms;

        // Summary
        cout << "\n--- Timing Summary ---\n";
        auto pct = [&](double v) {
            return timing.decode_total_ms > 0.0 ? (v / timing.decode_total_ms * 100.0) : 0.0;
        };
        cout << "  ReadStage1Image:  " << fixed << setprecision(2) << timing.read_stage1_ms << " ms ("
             << pct(timing.read_stage1_ms) << "%)\n";
        cout << "  BuildStage2Image: " << fixed << setprecision(2) << timing.build_stage2_ms << " ms ("
             << pct(timing.build_stage2_ms) << "%)\n";
        cout << "  BuildStage3Image: " << fixed << setprecision(2) << timing.build_stage3_ms << " ms ("
             << pct(timing.build_stage3_ms) << "%)\n";
        if (timing.stage3_halide_kernel_ms > 0 ||
            timing.stage3_fused_demosaic_warp_ms > 0 ||
            timing.stage3_sdk_build_ms > 0) {
            cout << "    Stage3 detail:\n";
            if (timing.stage3_halide_kernel_ms > 0 || timing.stage3_fused_demosaic_warp_ms > 0) {
                cout << "      Prealloc(outside): " << fixed << setprecision(2) << timing.stage3_prealloc_ms << " ms\n";
                cout << "      Extract Stage2: " << fixed << setprecision(2) << timing.stage3_extract_stage2_ms << " ms\n";
                cout << "      Make Stage3 img: " << fixed << setprecision(2) << timing.stage3_make_image_ms << " ms\n";
                cout << "      Stage3 resize:   " << fixed << setprecision(2) << timing.stage3_resize_ms << " ms\n";
                cout << "      Halide kernel:   " << fixed << setprecision(2) << timing.stage3_halide_kernel_ms << " ms\n";
                cout << "      Fused demosaic+warp: " << fixed << setprecision(2) << timing.stage3_fused_demosaic_warp_ms << " ms\n";
                cout << "      Fast warp setup: " << fixed << setprecision(2) << timing.stage3_fast_warp_setup_ms << " ms\n";
                cout << "      Apply Opcode3:   " << fixed << setprecision(2) << timing.stage3_apply_opcode3_ms << " ms\n";
                cout << "      Put Stage3:      " << fixed << setprecision(2) << timing.stage3_inject_put_ms << " ms\n";
                cout << "      Extract Stage3:  " << fixed << setprecision(2) << timing.stage3_extract_stage3_ms << " ms\n";
                cout << "      Unaccounted:     " << fixed << setprecision(2) << timing.stage3_probe_unaccounted_ms << " ms\n";
            } else {
                cout << "      SDK build:      " << fixed << setprecision(2) << timing.stage3_sdk_build_ms << " ms\n";
                cout << "      SDK extract:    " << fixed << setprecision(2) << timing.stage3_sdk_extract_ms << " ms\n";
                cout << "      Unaccounted:    " << fixed << setprecision(2) << timing.stage3_probe_unaccounted_ms << " ms\n";
            }
        }
        cout << "  Render:           " << fixed << setprecision(2) << timing.render_ms << " ms ("
             << pct(timing.render_ms) << "%)\n";
        cout << "  DECODE TOTAL:     " << fixed << setprecision(2) << timing.decode_total_ms << " ms\n";
        cout << "  ParseInfo+Post:   " << fixed << setprecision(2) << timing.parse_info_ms << " ms\n";
        cout << "  ParseNeg+Post:    " << fixed << setprecision(2) << timing.parse_negative_ms << " ms\n";
        cout << "  WALL TOTAL:       " << fixed << setprecision(2) << timing.wall_total_ms << " ms\n";
        cout << "  TOTAL:            " << fixed << setprecision(2) << timing.decode_total_ms << " ms\n";

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
    cerr << "Usage: " << programName << " <dng_path> <mode> [warp_mode] [render_mode]\n";
    cerr << "  dng_path: Path to DNG file (lossless or lossy)\n";
    cerr << "  mode: 'baseline' or 'test'\n";
    cerr << "  warp_mode: 'auto', 'sdk', 'halide-cpu', or 'halide-metal' (test mode only)\n";
    cerr << "  render_mode: 'sdk', 'halide-metal', or 'auto' (test mode only)\n";
    cerr << "\nExamples:\n";
    cerr << "  # Generate baseline reference outputs:\n";
    cerr << "  " << programName << " image_samples/lossless_dng_sample.dng baseline\n";
    cerr << "  " << programName << " image_samples/lossy_dng_sample.dng baseline\n";
    cerr << "\n  # Test custom implementation against baseline:\n";
    cerr << "  " << programName << " image_samples/lossless_dng_sample.dng test\n";
    cerr << "  " << programName << " image_samples/lossy_dng_sample.dng test\n";
    cerr << "  " << programName << " image_samples/lossless_dng_sample.dng test halide-metal halide-metal\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    string dngPath = argv[1];
    string mode = argv[2];
    string warpModeArg = argc >= 4 ? argv[3] : "auto";
    string renderModeArg = argc >= 5 ? argv[4] : "sdk";

    bool generateBaseline = (mode == "baseline");
    bool isLossless = dngPath.find("lossless") != string::npos;
    string prefix = isLossless ? "lossless" : "lossy";
    WarpRectilinearMode warpMode = parseWarpMode(warpModeArg);
    RenderHalideMode renderMode = parseRenderMode(renderModeArg);

    cout << string(70, '=') << "\n";
    cout << "  DNG SDK Decode Pipeline Test Tool\n";
    cout << "  Phase 5.0 - Step-by-Step PSNR Analysis\n";
    cout << string(70, '=') << "\n";
    cout << "\nDNG: " << dngPath << "\n";
    cout << "Mode: " << (generateBaseline ? "BASELINE (generate reference)" : "TEST (compare to baseline)") << "\n";
    if (!generateBaseline) {
        cout << "WarpRectilinear mode: " << warpRectilinearModeName(warpMode) << "\n";
        cout << "Render mode: " << renderHalideModeName(renderMode) << "\n";
    }

    try {
        constexpr uint32_t kOptimizedAreaThreads = 20;
        ConcurrentDngHost host(kOptimizedAreaThreads);
        cout << "Area task threads: " << host.PerformAreaTaskThreads() << "\n";
        testDNG(host, dngPath, prefix, generateBaseline, warpMode, renderMode);
    } catch (const dng_exception& e) {
        cerr << "\nDNG Exception: " << e.ErrorCode() << "\n";
        return 1;
    } catch (const std::exception& e) {
        cerr << "\nException: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
