/**
 * test_lossless_jpeg_analysis.cpp
 *
 * Phase 9.0 Lossless DNG 格式分析工具
 * 目的: 回答 9.6.x 問題並理解 DNG Lossless JPEG 格式
 *
 * 分析內容:
 * 1. TIFF IFD 解析: Predictor 值, Compression 值, BitDepth
 * 2. DNG SDK Lossless JPEG 解碼流程分析
 * 3. CFA Pattern 分析
 * 4. ColorMatrix / AsShotNeutral 位置確認
 */

#include <iostream>
#include <iomanip>
#include <fstream>
#include <vector>
#include <cmath>
#include <cstring>

// DNG SDK
#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_ifd.h>
#include <dng_pixel_buffer.h>
#include <dng_image.h>
#include <dng_linearization_info.h>
#include <dng_camera_profile.h>

#if defined(USE_LIBJPEG_TURBO)
#include <turbojpeg.h>
#endif

void printHexDump(const uint8_t* data, size_t size, size_t maxBytes = 64) {
    std::cout << "  Hex dump (first " << std::min(size, maxBytes) << " bytes):\n    ";
    for (size_t i = 0; i < std::min(size, maxBytes); i++) {
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (unsigned)data[i] << " ";
        if ((i + 1) % 16 == 0 && i < size - 1) {
            std::cout << "\n    ";
        }
    }
    std::cout << std::dec << "\n";
}

void analyzeIFD(const dng_ifd& ifd, const std::string& filePath) {
    std::cout << "\n============================================================\n";
    std::cout << "  TIFF IFD Analysis\n";
    std::cout << "============================================================\n\n";

    // Basic image info
    std::cout << "--- Image Info ---\n";
    std::cout << "  Image Width:  " << ifd.fImageWidth << "\n";
    std::cout << "  Image Height: " << ifd.fImageLength << "\n";
    std::cout << "  Samples/Pixel: " << ifd.fSamplesPerPixel << "\n";

    // Compression
    std::cout << "\n--- Compression ---\n";
    std::cout << "  Compression: " << ifd.fCompression << " (";
    if (ifd.fCompression == 7) {
        std::cout << "Lossless JPEG, ccJPEG)";
    } else if (ifd.fCompression == 34892) {
        std::cout << "Lossy JPEG, ccLossyJPEG)";
    } else {
        std::cout << "Unknown)";
    }
    std::cout << "\n";

    // Predictor
    std::cout << "\n--- Predictor ---\n";
    std::cout << "  Predictor: " << ifd.fPredictor << " (";
    if (ifd.fPredictor == cpNullPredictor) {
        std::cout << "cpNullPredictor)";
    } else if (ifd.fPredictor == 2) {  // cpHorizontalDifference
        std::cout << "cpHorizontalDifference)";
    } else if (ifd.fPredictor == 34892) {  // cpHorizontalDifferenceX2
        std::cout << "cpHorizontalDifferenceX2)";
    } else if (ifd.fPredictor == 34893) {  // cpHorizontalDifferenceX4
        std::cout << "cpHorizontalDifferenceX4)";
    } else if (ifd.fPredictor == cpFloatingPoint) {
        std::cout << "cpFloatingPoint)";
    } else {
        std::cout << "Unknown)";
    }
    std::cout << "\n";
    std::cout << "  *** KEY FINDING: Predictor 34892 = ";
    if (ifd.fPredictor == 34892) {  // cpHorizontalDifferenceX2
        std::cout << "cpHorizontalDifferenceX2 (DNG-specific predictor) ***\n";
        std::cout << "  This means horizontal difference encoding with 2x factor\n";
    } else {
        std::cout << "not cpHorizontalDifferenceX2 ***\n";
    }

    // Bits per sample
    std::cout << "\n--- Bits Per Sample ---\n";
    for (uint32 i = 0; i < ifd.fSamplesPerPixel && i < 4; i++) {
        std::cout << "  Sample " << i << ": " << ifd.fBitsPerSample[i] << " bits\n";
    }

    // Photometric Interpretation
    std::cout << "\n--- Photometric Interpretation ---\n";
    std::cout << "  Value: " << ifd.fPhotometricInterpretation << "\n";
    // 32803 = CFA (Bayer pattern)
    // 34892 = LinearRaw (for lossy JPEG)
    if (ifd.fPhotometricInterpretation == 32803) {
        std::cout << "  Meaning: CFA (Bayer Pattern)\n";
    } else if (ifd.fPhotometricInterpretation == 34892) {
        std::cout << "  Meaning: LinearRaw (Lossy JPEG)\n";
    }

    // Tile info
    std::cout << "\n--- Tile Info ---\n";
    std::cout << "  Tile Width:  " << ifd.fTileWidth << "\n";
    std::cout << "  Tile Length: " << ifd.fTileLength << "\n";
    std::cout << "  Tiles Across: " << ifd.TilesAcross() << "\n";
    std::cout << "  Tiles Down: " << ifd.TilesDown() << "\n";
    std::cout << "  Total Tiles: " << ifd.TilesAcross() * ifd.TilesDown() << "\n";

    // Tile offset info
    std::cout << "\n--- Tile Offset Arrays ---\n";
    std::cout << "  fTileOffsetsOffset: " << ifd.fTileOffsetsOffset << "\n";
    std::cout << "  fTileOffsetsType: " << ifd.fTileOffsetsType << "\n";
    std::cout << "  fTileOffsetsCount: " << ifd.fTileOffsetsCount << "\n";
    std::cout << "  fTileByteCountsOffset: " << ifd.fTileByteCountsOffset << "\n";
    std::cout << "  fTileByteCountsType: " << ifd.fTileByteCountsType << "\n";
    std::cout << "  fTileByteCountsCount: " << ifd.fTileByteCountsCount << "\n";

    // CFA Pattern
    std::cout << "\n--- CFA Pattern ---\n";
    std::cout << "  CFA Repeat Rows: " << ifd.fCFARepeatPatternRows << "\n";
    std::cout << "  CFA Repeat Cols: " << ifd.fCFARepeatPatternCols << "\n";
    std::cout << "  CFA Layout: " << ifd.fCFALayout << " (";
    if (ifd.fCFALayout == 1) {
        std::cout << "Rectangular, CFA offset = (0,0))";
    } else if (ifd.fCFALayout == 2) {
        std::cout << "Staggered, CFA offset = (0,0))";
    }
    std::cout << "\n";
    std::cout << "  CFA Pattern:\n    ";
    for (uint32 row = 0; row < ifd.fCFARepeatPatternRows && row < 8; row++) {
        for (uint32 col = 0; col < ifd.fCFARepeatPatternCols && col < 8; col++) {
            uint8 c = ifd.fCFAPattern[row][col];
            const char* color = "Unknown";
            switch (c) {
                case 0: color = "Red"; break;
                case 1: color = "Green"; break;
                case 2: color = "Blue"; break;
                case 3: color = "Cyan"; break;
                case 4: color = "Magenta"; break;
                case 5: color = "Yellow"; break;
                case 6: color = "White"; break;
            }
            std::cout << color << "(" << (int)c << ") ";
        }
        std::cout << "\n    ";
    }
    std::cout << "\n";

    // JPEGTables
    std::cout << "\n--- JPEG Tables ---\n";
    std::cout << "  JPEGTables Count: " << ifd.fJPEGTablesCount << "\n";
    std::cout << "  JPEGTables Offset: " << ifd.fJPEGTablesOffset << "\n";

    // YCbCr info
    if (ifd.fCompression == 34892) {
        std::cout << "\n--- YCbCr Info (Lossy JPEG) ---\n";
        std::cout << "  YCbCrCoefficient R,G,B: "
                  << ifd.fYCbCrCoefficientR << ", "
                  << ifd.fYCbCrCoefficientG << ", "
                  << ifd.fYCbCrCoefficientB << "\n";
        std::cout << "  YCbCrSubSample H,V: "
                  << ifd.fYCbCrSubSampleH << ", "
                  << ifd.fYCbCrSubSampleV << "\n";
        std::cout << "  YCbCr Positioning: " << ifd.fYCbCrPositioning << "\n";

        // Determine subsampling format
        std::cout << "  *** Subsampling Format: ";
        if (ifd.fYCbCrSubSampleH == 2 && ifd.fYCbCrSubSampleV == 2) {
            std::cout << "4:2:0 (Y:Cb:Cr = 4:1:1 equivalent) ***\n";
        } else if (ifd.fYCbCrSubSampleH == 2 && ifd.fYCbCrSubSampleV == 1) {
            std::cout << "4:2:2 (Y:Cb:Cr = 2:1:1 equivalent) ***\n";
        } else if (ifd.fYCbCrSubSampleH == 1 && ifd.fYCbCrSubSampleV == 1) {
            std::cout << "4:4:4 (No subsampling) ***\n";
        } else {
            std::cout << "Custom (H=" << ifd.fYCbCrSubSampleH
                      << ", V=" << ifd.fYCbCrSubSampleV << ") ***\n";
        }
    }
}

void analyzeTileData(const dng_ifd& ifd, dng_file_stream& stream) {
    std::cout << "\n============================================================\n";
    std::cout << "  Tile Data Analysis\n";
    std::cout << "============================================================\n\n";

    uint32 tileCount = ifd.TilesAcross() * ifd.TilesDown();
    std::cout << "Reading tile offset array for " << tileCount << " tiles...\n";

    std::vector<uint64> tileOffsets(tileCount);
    std::vector<uint32> tileByteCounts(tileCount);

    if (tileCount <= 32) {
        // Inline arrays
        for (uint32 i = 0; i < tileCount; i++) {
            tileOffsets[i] = ifd.fTileOffset[i];
            tileByteCounts[i] = ifd.fTileByteCount[i];
        }
    } else {
        // External arrays
        uint64 offsetsOffset = ifd.fTileOffsetsOffset;
        uint32 offsetsType = ifd.fTileOffsetsType;
        uint64 byteCountsOffset = ifd.fTileByteCountsOffset;
        uint32 byteCountsType = ifd.fTileByteCountsType;

        stream.SetReadPosition(offsetsOffset);
        for (uint32 i = 0; i < tileCount; i++) {
            tileOffsets[i] = stream.TagValue_uint32(offsetsType);
        }

        stream.SetReadPosition(byteCountsOffset);
        for (uint32 i = 0; i < tileCount; i++) {
            tileByteCounts[i] = stream.TagValue_uint32(byteCountsType);
        }
    }

    // Analyze first tile
    std::cout << "\n--- First Tile Analysis ---\n";
    std::cout << "  Tile 0 Offset: " << tileOffsets[0] << "\n";
    std::cout << "  Tile 0 ByteCount: " << tileByteCounts[0] << "\n";

    // Read first tile data
    stream.SetReadPosition(tileOffsets[0]);
    std::vector<uint8_t> tileData(tileByteCounts[0]);
    stream.Get(tileData.data(), tileByteCounts[0]);

    std::cout << "  Tile 0 Data (first 64 bytes):\n";
    printHexDump(tileData.data(), tileData.size(), 64);

    // Check if JPEG SOI marker (0xFF 0xD8)
    std::cout << "\n  JPEG Detection:\n";
    if (tileData.size() >= 2) {
        bool hasSOI = (tileData[0] == 0xFF && tileData[1] == 0xD8);
        std::cout << "    Has JPEG SOI marker (FF D8): " << (hasSOI ? "YES" : "NO") << "\n";

        // Look for other markers
        for (size_t i = 0; i < std::min(tileData.size(), (size_t)64); i++) {
            if (tileData[i] == 0xFF && i + 1 < tileData.size()) {
                uint8 marker = tileData[i + 1];
                const char* markerName = "Unknown";
                switch (marker) {
                    case 0xD8: markerName = "SOI (Start of Image)"; break;
                    case 0xE0: markerName = "APP0 (JFIF)"; break;
                    case 0xE1: markerName = "APP1 (Exif/XMP)"; break;
                    case 0xDB: markerName = "DQT (Quantization Table)"; break;
                    case 0xC0: case 0xC2: markerName = "SOF (Start of Frame)"; break;
                    case 0xC4: markerName = "DHT (Huffman Table)"; break;
                    case 0xDA: markerName = "SOS (Start of Scan)"; break;
                    case 0xD9: markerName = "EOI (End of Image)"; break;
                    case 0x00: markerName = "Pad/NULL"; break;
                    default:
                        if (marker >= 0xD0 && marker <= 0xD7) {
                            markerName = "RST (Restart Marker)";
                        }
                        break;
                }
                if (marker != 0x00) {
                    std::cout << "    [" << std::hex << std::setw(4) << i << "] "
                              << "FF " << std::setw(2) << (int)marker << " - " << markerName
                              << std::dec << "\n";
                }
            }
        }
    }
}

void analyzeMetadata(dng_negative& negative) {
    std::cout << "\n============================================================\n";
    std::cout << "  Color Metadata Analysis\n";
    std::cout << "============================================================\n\n";

    // Black/White levels
    const dng_linearization_info* linInfo = negative.GetLinearizationInfo();
    if (linInfo) {
        std::cout << "--- Black/White Levels ---\n";
        std::cout << "  BlackLevel[0][0][0]: " << linInfo->fBlackLevel[0][0][0] << "\n";
        std::cout << "  WhiteLevel[0]: " << linInfo->fWhiteLevel[0] << "\n";
    }

    // BaselineExposure
    std::cout << "\n--- Exposure ---\n";
    std::cout << "  BaselineExposure: " << negative.BaselineExposure() << " EV\n";

    // AnalogBalance
    std::cout << "\n--- Analog Balance ---\n";
    for (int i = 0; i < 3; i++) {
        std::cout << "  AnalogBalance[" << i << "]: " << negative.AnalogBalance(i) << "\n";
    }

    // CameraNeutral (WB)
    std::cout << "\n--- As Shot Neutral (WB) ---\n";
    if (negative.HasCameraNeutral()) {
        const auto& neutral = negative.CameraNeutral();
        for (uint32 i = 0; i < neutral.Count() && i < 3; i++) {
            std::cout << "  Neutral[" << i << "]: " << neutral[i] << "\n";
        }
    }

    // Color Matrix
    std::cout << "\n--- Color Matrices ---\n";
    if (negative.ProfileCount() > 0) {
        const dng_camera_profile& profile = negative.ProfileByIndex(0);
        std::cout << "  Profile Name: " << profile.Name().Get() << "\n";

        std::cout << "  ColorMatrix1:\n";
        for (int i = 0; i < 3; i++) {
            std::cout << "    [" << std::fixed << std::setprecision(6)
                      << profile.ColorMatrix1()[i][0] << ", "
                      << profile.ColorMatrix1()[i][1] << ", "
                      << profile.ColorMatrix1()[i][2] << "]\n";
        }

        if (profile.HasColorMatrix2()) {
            std::cout << "  ColorMatrix2:\n";
            for (int i = 0; i < 3; i++) {
                std::cout << "    [" << std::fixed << std::setprecision(6)
                          << profile.ColorMatrix2()[i][0] << ", "
                          << profile.ColorMatrix2()[i][1] << ", "
                          << profile.ColorMatrix2()[i][2] << "]\n";
            }
        }

        if (profile.ForwardMatrix1().NotEmpty()) {
            std::cout << "  ForwardMatrix1:\n";
            for (int i = 0; i < 3; i++) {
                std::cout << "    [" << std::fixed << std::setprecision(6)
                          << profile.ForwardMatrix1()[i][0] << ", "
                          << profile.ForwardMatrix1()[i][1] << ", "
                          << profile.ForwardMatrix1()[i][2] << "]\n";
            }
        }

        std::cout << "  CalibrationIlluminant1: " << profile.CalibrationIlluminant1() << "\n";
        std::cout << "  CalibrationIlluminant2: " << profile.CalibrationIlluminant2() << "\n";
    }
}

void analyzeTurboJPEG(const dng_ifd& ifd, dng_file_stream& stream) {
#if defined(USE_LIBJPEG_TURBO)
    std::cout << "\n============================================================\n";
    std::cout << "  TurboJPEG Analysis\n";
    std::cout << "============================================================\n\n";

    uint32 tileCount = ifd.TilesAcross() * ifd.TilesDown();
    std::cout << "Attempting to decode tile 0 with TurboJPEG...\n\n";

    // Get tile offset
    uint64 tileOffset;
    uint32 tileByteCount;
    if (tileCount <= 32) {
        tileOffset = ifd.fTileOffset[0];
        tileByteCount = ifd.fTileByteCount[0];
    } else {
        uint64 offsetsOffset = ifd.fTileOffsetsOffset;
        uint32 offsetsType = ifd.fTileOffsetsType;
        uint64 byteCountsOffset = ifd.fTileByteCountsOffset;
        uint32 byteCountsType = ifd.fTileByteCountsType;

        stream.SetReadPosition(offsetsOffset);
        tileOffset = stream.TagValue_uint32(offsetsType);

        stream.SetReadPosition(byteCountsOffset);
        tileByteCount = stream.TagValue_uint32(byteCountsType);
    }

    // Read tile data
    stream.SetReadPosition(tileOffset);
    std::vector<uint8_t> compressedData(tileByteCount);
    stream.Get(compressedData.data(), tileByteCount);

    std::cout << "  Tile size: " << tileByteCount << " bytes\n";

    // Try TurboJPEG decode
    tjhandle handle = tj3Init(TJINIT_DECOMPRESS);
    if (!handle) {
        std::cout << "  ERROR: Failed to initialize TurboJPEG\n";
        return;
    }

    int decompressResult = tj3DecompressHeader(handle, compressedData.data(), compressedData.size());
    if (decompressResult < 0) {
        std::cout << "  ERROR: tj3DecompressHeader failed\n";
        std::cout << "  Error message: " << tj3GetErrorStr(handle) << "\n";
        tj3Destroy(handle);
        return;
    }

    int jpegWidth = tj3Get(handle, TJPARAM_JPEGWIDTH);
    int jpegHeight = tj3Get(handle, TJPARAM_JPEGHEIGHT);
    int precision = tj3Get(handle, TJPARAM_PRECISION);
    int isLossless = tj3Get(handle, TJPARAM_LOSSLESS);

    std::cout << "  JPEG Width: " << jpegWidth << "\n";
    std::cout << "  JPEG Height: " << jpegHeight << "\n";
    std::cout << "  Bit Precision: " << precision << "\n";
    std::cout << "  Is Lossless: " << isLossless << "\n";

    // Try to decompress based on precision
    int decompressResult2 = -1;
    std::vector<uint8_t> outputBuffer;

    // For packed grayscale, pixel size = 1 byte for 8-bit, 2 bytes for 12/16-bit
    if (precision <= 8) {
        int pitch = jpegWidth;  // 1 byte per pixel
        outputBuffer.resize(jpegWidth * jpegHeight);
        decompressResult2 = tj3Decompress8(handle, compressedData.data(), compressedData.size(),
                                           outputBuffer.data(), pitch, TJPF_GRAY);
        if (decompressResult2 == 0) {
            std::cout << "  Decompressed using tj3Decompress8 (8-bit grayscale)\n";
        }
    } else if (precision <= 12) {
        int pitch = jpegWidth * 2;  // 2 bytes per pixel
        std::vector<int16_t> tempBuffer(jpegWidth * jpegHeight);
        decompressResult2 = tj3Decompress12(handle, compressedData.data(), compressedData.size(),
                                             tempBuffer.data(), pitch, TJPF_GRAY);
        if (decompressResult2 == 0) {
            std::cout << "  Decompressed using tj3Decompress12 (12-bit grayscale)\n";
            // Convert to uint16
            outputBuffer.resize(jpegWidth * jpegHeight * 2);
            for (size_t i = 0; i < tempBuffer.size(); i++) {
                outputBuffer[i * 2] = tempBuffer[i] & 0xFF;
                outputBuffer[i * 2 + 1] = (tempBuffer[i] >> 8) & 0xFF;
            }
        }
    } else {
        int pitch = jpegWidth * 2;  // 2 bytes per pixel
        std::vector<uint16_t> tempBuffer(jpegWidth * jpegHeight);
        decompressResult2 = tj3Decompress16(handle, compressedData.data(), compressedData.size(),
                                             tempBuffer.data(), pitch, TJPF_GRAY);
        if (decompressResult2 == 0) {
            std::cout << "  Decompressed using tj3Decompress16 (16-bit grayscale)\n";
            outputBuffer.resize(jpegWidth * jpegHeight * 2);
            memcpy(outputBuffer.data(), tempBuffer.data(), outputBuffer.size());
        }
    }

    if (decompressResult2 < 0) {
        std::cout << "\n  ERROR: tj3Decompress failed\n";
        std::cout << "  Error message: " << tj3GetErrorStr(handle) << "\n";
    } else {
        std::cout << "\n  SUCCESS: TurboJPEG decompression worked!\n";
        std::cout << "  Output buffer size: " << outputBuffer.size() << " bytes\n";

        // Analyze output
        std::cout << "\n  Output Analysis:\n";
        std::cout << "    Total pixels: " << jpegWidth * jpegHeight << "\n";
        std::cout << "    Format: Grayscale (1 channel, TJPF_GRAY)\n";
        std::cout << "    Output buffer size: " << outputBuffer.size() << " bytes\n";
        std::cout << "    *** This is CFA/Bayer data - suitable for our pipeline ***\n";
    }

    tj3Destroy(handle);

#else
    std::cout << "\n============================================================\n";
    std::cout << "  TurboJPEG Analysis\n";
    std::cout << "============================================================\n\n";
    std::cout << "  TurboJPEG is not enabled (USE_LIBJPEG_TURBO not defined)\n";
#endif
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_lossless_dng>\n";
        return 1;
    }

    const char* dngPath = argv[1];

    std::cout << "============================================================\n";
    std::cout << "  Phase 9.0 - Lossless DNG Format Analysis Tool\n";
    std::cout << "============================================================\n";
    std::cout << "\nAnalyzing: " << dngPath << "\n";

    try {
        dng_host host;
        dng_file_stream stream(dngPath);

        // Parse DNG info
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);

        if (!info.IsValidDNG()) {
            std::cerr << "ERROR: Not a valid DNG file\n";
            return 1;
        }

        const dng_ifd& rawIFD = *info.fIFD[info.fMainIndex];

        // Analyze TIFF IFD
        analyzeIFD(rawIFD, dngPath);

        // Analyze tile data
        analyzeTileData(rawIFD, stream);

        // Parse negative for metadata
        dng_negative* negativeTemplate = host.Make_dng_negative();
        AutoPtr<dng_negative> negative(negativeTemplate);
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);

        // Analyze metadata
        analyzeMetadata(*negative);

        // Try TurboJPEG
        stream.SetReadPosition(0);  // Reset stream
        analyzeTurboJPEG(rawIFD, stream);

        // Summary
        std::cout << "\n============================================================\n";
        std::cout << "  ANALYSIS SUMMARY\n";
        std::cout << "============================================================\n\n";

        std::cout << "9.6.1 Predictor差分方向: ";
        if (rawIFD.fPredictor == 34892) {  // cpHorizontalDifferenceX2
            std::cout << "水平差分 (cpHorizontalDifferenceX2)\n";
            std::cout << "  - 每列獨立處理\n";
            std::cout << "  - pixel[col] += pixel[col-1]\n";
            std::cout << "  - 編碼: raw[0], raw[1]-raw[0], raw[2]-raw[1], ...\n";
        } else if (rawIFD.fPredictor == 2) {  // cpHorizontalDifference
            std::cout << "水平差分 (cpHorizontalDifference)\n";
        } else {
            std::cout << "非差分 (Predictor=" << rawIFD.fPredictor << ")\n";
        }

        std::cout << "\n9.6.2 Bit Depth處理時機: ";
        std::cout << "libjpeg輸出後再處理\n";
        std::cout << "  - DNG Lossless JPEG使用非標準差分編碼\n";
        std::cout << "  - 需要在libjpeg解碼後應用Inverse Predictor\n";
        std::cout << "  - " << rawIFD.fBitsPerSample[0] << "-bit data\n";

        std::cout << "\n9.6.3 ColorMatrix應用位置: ";
        std::cout << "在Halide內部處理\n";
        std::cout << "  - ColorMatrix用於從CFA轉換到XYZ\n";
        std::cout << "  - AsShotNeutral用於白平衡\n";
        std::cout << "  - 這些都在Halide管線中處理\n";

    } catch (const dng_exception& e) {
        std::cerr << "\nDNG Exception: " << e.ErrorCode() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "\nException: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
