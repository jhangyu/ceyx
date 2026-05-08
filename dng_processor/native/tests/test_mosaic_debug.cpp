/**
 * test_mosaic_debug.cpp - Debug demosaic by comparing pixel values
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cmath>

#include "stage_contract_checks.h"

extern "C" {
#include "dng_mosaic_halide.h"
}

bool loadRawFile(const std::string& filename, void* data, size_t byteSize) {
    std::ifstream fin(filename, std::ios::binary);
    if (!fin) return false;
    fin.read(reinterpret_cast<char*>(data), byteSize);
    return fin.good();
}

int main(int argc, char** argv) {
    std::cout << "=== Debug: DNG Stage Data Analysis ===\n\n";

    const char* stage2File = "lossless_stage2_6048x4024_1p.raw";
    const char* stage3File = "lossless_stage3_6048x4024_3p.raw";

    size_t width = 6048;
    size_t height = 4024;
    size_t stage2Bytes = width * height * 2;
    size_t stage3Bytes = width * height * 3 * 2;

    // Load Stage2 (CFA input)
    std::vector<uint16_t> stage2(width * height);
    if (!loadRawFile(stage2File, stage2.data(), stage2Bytes)) {
        std::cerr << "ERROR: Could not load " << stage2File << "\n";
        return 1;
    }
    if (!StageContract::validateRawBufferContract("Stage2(raw)",
                                                  width,
                                                  height,
                                                  1,
                                                  sizeof(uint16_t),
                                                  stage2Bytes)) {
        return 1;
    }

    // Load Stage3 (demosaiced reference)
    std::vector<uint16_t> stage3(width * height * 3);
    if (!loadRawFile(stage3File, stage3.data(), stage3Bytes)) {
        std::cerr << "ERROR: Could not load " << stage3File << "\n";
        return 1;
    }
    if (!StageContract::validateRawBufferContract("Stage3(raw)",
                                                  width,
                                                  height,
                                                  3,
                                                  sizeof(uint16_t),
                                                  stage3Bytes)) {
        return 1;
    }

    // Analyze Stage2 (CFA)
    std::cout << "=== Stage2 (CFA Input) Analysis ===\n";
    uint64_t sumS2 = 0;
    uint16_t minS2 = 65535, maxS2 = 0;
    for (size_t i = 0; i < stage2.size(); i++) {
        sumS2 += stage2[i];
        minS2 = std::min(minS2, stage2[i]);
        maxS2 = std::max(maxS2, stage2[i]);
    }
    double avgS2 = (double)sumS2 / stage2.size();
    std::cout << "  Range: [" << minS2 << ", " << maxS2 << "]\n";
    std::cout << "  Average: " << avgS2 << "\n";

    // Sample CFA pattern at specific locations
    std::cout << "\n=== CFA Pattern Samples (Stage2 at specific positions) ===\n";
    int samplePos[5][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {100, 100}};
    for (auto& pos : samplePos) {
        size_t idx = pos[1] * width + pos[0];
        std::cout << "  [" << pos[0] << "," << pos[1] << "]: "
                  << stage2[idx] << "\n";
    }

    // Analyze Stage3 (RGB output)
    std::cout << "\n=== Stage3 (Demosaiced RGB Reference) Analysis ===\n";
    uint64_t sumR = 0, sumG = 0, sumB = 0;
    uint16_t minR = 65535, maxR = 0;
    uint16_t minG = 65535, maxG = 0;
    uint16_t minB = 65535, maxB = 0;

    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            size_t idx = (y * width + x) * 3;
            uint16_t r = stage3[idx + 0];
            uint16_t g = stage3[idx + 1];
            uint16_t b = stage3[idx + 2];

            sumR += r; sumG += g; sumB += b;
            minR = std::min(minR, r); maxR = std::max(maxR, r);
            minG = std::min(minG, g); maxG = std::max(maxG, g);
            minB = std::min(minB, b); maxB = std::max(maxB, b);
        }
    }

    std::cout << "  R: Range [" << minR << ", " << maxR << "], Avg " << (double)sumR / (width * height) << "\n";
    std::cout << "  G: Range [" << minG << ", " << maxG << "], Avg " << (double)sumG / (width * height) << "\n";
    std::cout << "  B: Range [" << minB << ", " << maxB << "], Avg " << (double)sumB / (width * height) << "\n";

    // Sample Stage3 RGB values
    std::cout << "\n=== Stage3 RGB Samples at specific positions ===\n";
    for (auto& pos : samplePos) {
        size_t idx = (pos[1] * width + pos[0]) * 3;
        uint16_t r = stage3[idx + 0];
        uint16_t g = stage3[idx + 1];
        uint16_t b = stage3[idx + 2];
        std::cout << "  [" << pos[0] << "," << pos[1] << "]: R=" << r
                  << " G=" << g << " B=" << b << "\n";
    }

    // Run demosaic and compare at same positions
    std::cout << "\n=== Running Halide Demosaic ===\n";
    std::vector<uint16_t> halideOutput(width * height * 3);
    demosaic_bilinear_compat(stage2.data(), width, height, halideOutput.data());
    if (!StageContract::validateRawBufferContract("Stage3(Halide)",
                                                  width,
                                                  height,
                                                  3,
                                                  sizeof(uint16_t),
                                                  halideOutput.size() * sizeof(uint16_t))) {
        return 1;
    }

    std::cout << "\n=== Halide Output RGB Samples ===\n";
    for (auto& pos : samplePos) {
        size_t idx = (pos[1] * width + pos[0]) * 3;
        uint16_t r = halideOutput[idx + 0];
        uint16_t g = halideOutput[idx + 1];
        uint16_t b = halideOutput[idx + 2];
        std::cout << "  [" << pos[0] << "," << pos[1] << "]: R=" << r
                  << " G=" << g << " B=" << b << "\n";
    }

    return 0;
}
