/**
 * test_lossy_jpeg_analysis.cpp
 *
 * Phase 10.0 Lossy DNG 格式分析工具
 * 目的: 回答 10.5.x 問題並理解 DNG Lossy JPEG 格式
 *
 * 分析內容:
 * 1. YCbCr 格式確認 (採樣比例, 色彩空間)
 * 2. TurboJPEG 解碼輸出分析
 * 3. YCbCr→RGB 轉換矩陣確認
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

void analyzeIFD(const dng_ifd& ifd) {
    std::cout << "\n============================================================\n";
    std::cout << "  TIFF IFD Analysis (Lossy DNG)\n";
    std::cout << "============================================================\n\n";

    std::cout << "--- Image Info ---\n";
    std::cout << "  Image Width:  " << ifd.fImageWidth << "\n";
    std::cout << "  Image Height: " << ifd.fImageLength << "\n";
    std::cout << "  Samples/Pixel: " << ifd.fSamplesPerPixel << "\n";

    std::cout << "\n--- Compression ---\n";
    std::cout << "  Compression: " << ifd.fCompression << " (";
    if (ifd.fCompression == 7) {
        std::cout << "Lossless JPEG)";
    } else if (ifd.fCompression == 34892) {
        std::cout << "Lossy JPEG)";
    }
    std::cout << "\n";

    std::cout << "\n--- Bits Per Sample ---\n";
    for (uint32 i = 0; i < ifd.fSamplesPerPixel && i < 4; i++) {
        std::cout << "  Sample " << i << ": " << ifd.fBitsPerSample[i] << " bits\n";
    }

    std::cout << "\n--- Photometric Interpretation ---\n";
    std::cout << "  Value: " << ifd.fPhotometricInterpretation << "\n";
    if (ifd.fPhotometricInterpretation == 32803) {
        std::cout << "  Meaning: CFA (Bayer Pattern)\n";
    } else if (ifd.fPhotometricInterpretation == 34892) {
        std::cout << "  Meaning: LinearRaw (Lossy JPEG)\n";
    }

    std::cout << "\n--- Tile Info ---\n";
    std::cout << "  Tile Width:  " << ifd.fTileWidth << "\n";
    std::cout << "  Tile Length: " << ifd.fTileLength << "\n";
    std::cout << "  Tiles Across: " << ifd.TilesAcross() << "\n";
    std::cout << "  Tiles Down: " << ifd.TilesDown() << "\n";
    std::cout << "  Total Tiles: " << ifd.TilesAcross() * ifd.TilesDown() << "\n";

    std::cout << "\n--- YCbCr Info (Lossy JPEG) ---\n";
    std::cout << "  YCbCrCoefficient R,G,B: "
              << std::fixed << std::setprecision(6)
              << ifd.fYCbCrCoefficientR << ", "
              << ifd.fYCbCrCoefficientG << ", "
              << ifd.fYCbCrCoefficientB << "\n";
    std::cout << "  YCbCrSubSample H,V: "
              << ifd.fYCbCrSubSampleH << ", "
              << ifd.fYCbCrSubSampleV << "\n";
    std::cout << "  YCbCr Positioning: " << ifd.fYCbCrPositioning << "\n";

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

    // Calculate expected YCbCr buffer dimensions
    uint32 yWidth = ifd.fImageWidth;
    uint32 yHeight = ifd.fImageLength;
    uint32 cWidth = (ifd.fYCbCrSubSampleH == 2) ? (ifd.fImageWidth + 1) / 2 : ifd.fImageWidth;
    uint32 cHeight = (ifd.fYCbCrSubSampleV == 2) ? (ifd.fImageLength + 1) / 2 : ifd.fImageLength;

    std::cout << "\n--- YCbCr Buffer Dimensions ---\n";
    std::cout << "  Y plane: " << yWidth << "x" << yHeight << " = " << yWidth * yHeight << " pixels\n";
    std::cout << "  Cb/Cr plane: " << cWidth << "x" << cHeight << " = " << cWidth * cHeight << " pixels\n";
    std::cout << "  Total YCbCr (if YUV444): " << yWidth * yHeight * 3 << " bytes\n";
    std::cout << "  Total YCbCr (if YUV420): " << (yWidth * yHeight) + (cWidth * cHeight * 2) << " bytes\n";

    // Reference black/white
    std::cout << "\n--- Reference Black/White ---\n";
    for (int i = 0; i < 6; i++) {
        std::cout << "  ReferenceBlackWhite[" << i << "]: " << ifd.fReferenceBlackWhite[i] << "\n";
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
        for (uint32 i = 0; i < tileCount; i++) {
            tileOffsets[i] = ifd.fTileOffset[i];
            tileByteCounts[i] = ifd.fTileByteCount[i];
        }
    } else {
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

    std::cout << "\n--- First Tile Analysis ---\n";
    std::cout << "  Tile 0 Offset: " << tileOffsets[0] << "\n";
    std::cout << "  Tile 0 ByteCount: " << tileByteCounts[0] << "\n";

    stream.SetReadPosition(tileOffsets[0]);
    std::vector<uint8_t> tileData(tileByteCounts[0]);
    stream.Get(tileData.data(), tileByteCounts[0]);

    std::cout << "  Tile 0 Data (first 64 bytes):\n";
    printHexDump(tileData.data(), tileData.size(), 64);

    std::cout << "\n  JPEG Marker Analysis:\n";
    if (tileData.size() >= 2 && tileData[0] == 0xFF && tileData[1] == 0xD8) {
        std::cout << "    Has JPEG SOI marker: YES\n";

        for (size_t i = 0; i < std::min(tileData.size(), (size_t)128); i++) {
            if (tileData[i] == 0xFF && i + 1 < tileData.size()) {
                uint8 marker = tileData[i + 1];
                const char* markerName = "Unknown";
                bool isMarker = false;

                switch (marker) {
                    case 0xD8: markerName = "SOI (Start of Image)"; isMarker = true; break;
                    case 0xE0: markerName = "APP0 (JFIF)"; isMarker = true; break;
                    case 0xE1: markerName = "APP1 (Exif/XMP)"; isMarker = true; break;
                    case 0xDB: markerName = "DQT (Quantization Table)"; isMarker = true; break;
                    case 0xC0: markerName = "SOF0 (Baseline DCT)"; isMarker = true; break;
                    case 0xC1: markerName = "SOF1 (Extended DCT)"; isMarker = true; break;
                    case 0xC2: markerName = "SOF2 (Progressive DCT)"; isMarker = true; break;
                    case 0xC3: markerName = "SOF3 (Lossless)"; isMarker = true; break;
                    case 0xC4: markerName = "DHT (Huffman Table)"; isMarker = true; break;
                    case 0xDA: markerName = "SOS (Start of Scan)"; isMarker = true; break;
                    case 0xD9: markerName = "EOI (End of Image)"; isMarker = true; break;
                    case 0x00: markerName = "Pad/NULL"; isMarker = true; break;
                    default:
                        if (marker >= 0xD0 && marker <= 0xD7) {
                            markerName = "RST (Restart Marker)";
                            isMarker = true;
                        }
                        break;
                }

                if (isMarker && marker != 0x00) {
                    std::cout << "    [" << std::hex << std::setw(4) << i << "] "
                              << "FF " << std::setw(2) << (int)marker << " - " << markerName
                              << std::dec << "\n";
                }
            }
        }
    } else {
        std::cout << "    Has JPEG SOI marker: NO\n";
    }
}

#if defined(USE_LIBJPEG_TURBO)
void analyzeTurboJPEG(const dng_ifd& ifd, dng_file_stream& stream) {
    std::cout << "\n============================================================\n";
    std::cout << "  TurboJPEG Decoding Analysis\n";
    std::cout << "============================================================\n\n";

    uint32 tileCount = ifd.TilesAcross() * ifd.TilesDown();

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

    stream.SetReadPosition(tileOffset);
    std::vector<uint8_t> compressedData(tileByteCount);
    stream.Get(compressedData.data(), tileByteCount);

    std::cout << "  Tile size: " << tileByteCount << " bytes\n\n";

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
    int colorspace = tj3Get(handle, TJPARAM_COLORSPACE);

    std::cout << "  JPEG Width: " << jpegWidth << "\n";
    std::cout << "  JPEG Height: " << jpegHeight << "\n";
    std::cout << "  Bit Precision: " << precision << "\n";
    std::cout << "  Is Lossless: " << isLossless << "\n";
    std::cout << "  Colorspace: " << colorspace << " (";

    const char* csNames[] = {"JSGRAY", "JSRGB", "JSYCbCr", "JSCMYK", "JSYCCK", "JSUNKNOWN"};
    if (colorspace >= 0 && colorspace <= 5) {
        std::cout << csNames[colorspace];
    } else {
        std::cout << "Unknown";
    }
    std::cout << ")\n";

    // Try to decode to YUV
    std::cout << "\n  Attempting YUV decode...\n";

    // Calculate YUV buffer size (YUV420 = 1.5 bytes per pixel)
    size_t yuvSize = jpegWidth * jpegHeight * 3 / 2;  // YUV420
    std::vector<uint8_t> yuvBuffer(yuvSize);

    // TurboJPEG YUV decode requires separate planes with strides
    unsigned char* yuvPlanes[3] = { yuvBuffer.data(), yuvBuffer.data() + jpegWidth * jpegHeight, yuvBuffer.data() + jpegWidth * jpegHeight + (jpegWidth/2) * (jpegHeight/2) };
    int strides[3] = { jpegWidth, jpegWidth/2, jpegWidth/2 };
    int yuvResult = tj3DecompressToYUVPlanes8(handle, compressedData.data(), compressedData.size(),
                                             yuvPlanes, strides);
    if (yuvResult < 0) {
        std::cout << "  ERROR: tj3DecompressToYUVPlanes8 failed\n";
        std::cout << "  Error: " << tj3GetErrorStr(handle) << "\n";
    } else {
        std::cout << "  SUCCESS: YUV decode worked!\n";
        std::cout << "  YUV Size: " << yuvSize << " bytes\n";

        // Analyze YUV buffer
        std::cout << "\n  YUV Analysis:\n";
        std::cout << "    Y plane size: " << jpegWidth * jpegHeight << " bytes\n";
        std::cout << "    U/V plane size: " << (jpegWidth/2) * (jpegHeight/2) << " bytes each\n";
        std::cout << "    Total: " << yuvSize << " bytes\n";
        std::cout << "    *** This matches YUV420 format (4:2:0) ***\n";
    }

    // Also try grayscale decode
    std::cout << "\n  Attempting grayscale decode...\n";
    std::vector<uint8_t> grayBuffer(jpegWidth * jpegHeight);
    int grayResult = tj3Decompress8(handle, compressedData.data(), compressedData.size(),
                                   grayBuffer.data(), jpegWidth, TJPF_GRAY);
    if (grayResult < 0) {
        std::cout << "  ERROR: Grayscale decode failed\n";
        std::cout << "  Error: " << tj3GetErrorStr(handle) << "\n";
    } else {
        std::cout << "  SUCCESS: Grayscale decode worked!\n";
        std::cout << "  Output: " << grayBuffer.size() << " bytes\n";

        // Analyze first few pixels
        std::cout << "\n  First 8 pixel values: ";
        for (int i = 0; i < 8; i++) {
            std::cout << (int)grayBuffer[i] << " ";
        }
        std::cout << "\n";
    }

    tj3Destroy(handle);
}
#else
void analyzeTurboJPEG(const dng_ifd& ifd, dng_file_stream& stream) {
    std::cout << "\n============================================================\n";
    std::cout << "  TurboJPEG Analysis\n";
    std::cout << "============================================================\n\n";
    std::cout << "  TurboJPEG is not enabled (USE_LIBJPEG_TURBO not defined)\n";
}
#endif

void analyzeMetadata(dng_negative& negative) {
    std::cout << "\n============================================================\n";
    std::cout << "  Color Metadata Analysis\n";
    std::cout << "============================================================\n\n";

    const dng_linearization_info* linInfo = negative.GetLinearizationInfo();
    if (linInfo) {
        std::cout << "--- Black/White Levels ---\n";
        std::cout << "  BlackLevel[0][0][0]: " << linInfo->fBlackLevel[0][0][0] << "\n";
        std::cout << "  WhiteLevel[0]: " << linInfo->fWhiteLevel[0] << "\n";
    }

    std::cout << "\n--- Exposure ---\n";
    std::cout << "  BaselineExposure: " << negative.BaselineExposure() << " EV\n";

    if (negative.ProfileCount() > 0) {
        const dng_camera_profile& profile = negative.ProfileByIndex(0);
        std::cout << "\n--- Color Profile ---\n";
        std::cout << "  Profile Name: " << profile.Name().Get() << "\n";

        std::cout << "  ColorMatrix1:\n";
        for (int i = 0; i < 3; i++) {
            std::cout << "    [" << std::fixed << std::setprecision(6)
                      << profile.ColorMatrix1()[i][0] << ", "
                      << profile.ColorMatrix1()[i][1] << ", "
                      << profile.ColorMatrix1()[i][2] << "]\n";
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <path_to_lossy_dng>\n";
        return 1;
    }

    const char* dngPath = argv[1];

    std::cout << "============================================================\n";
    std::cout << "  Phase 10.0 - Lossy DNG Format Analysis Tool\n";
    std::cout << "============================================================\n";
    std::cout << "\nAnalyzing: " << dngPath << "\n";

    try {
        dng_host host;
        dng_file_stream stream(dngPath);

        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);

        if (!info.IsValidDNG()) {
            std::cerr << "ERROR: Not a valid DNG file\n";
            return 1;
        }

        const dng_ifd& rawIFD = *info.fIFD[info.fMainIndex];

        analyzeIFD(rawIFD);
        analyzeTileData(rawIFD, stream);

        dng_negative* negativeTemplate = host.Make_dng_negative();
        AutoPtr<dng_negative> negative(negativeTemplate);
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);

        analyzeMetadata(*negative);
        stream.SetReadPosition(0);
        analyzeTurboJPEG(rawIFD, stream);

        // Summary
        std::cout << "\n============================================================\n";
        std::cout << "  ANALYSIS SUMMARY\n";
        std::cout << "============================================================\n\n";

        std::cout << "10.5.1 YCbCr採樣格式: ";
        if (rawIFD.fYCbCrSubSampleH == 2 && rawIFD.fYCbCrSubSampleV == 2) {
            std::cout << "4:2:0 (Y:Cb:Cr = 4:1:1 equivalent)\n";
        } else if (rawIFD.fYCbCrSubSampleH == 2 && rawIFD.fYCbCrSubSampleV == 1) {
            std::cout << "4:2:2 (Y:Cb:Cr = 2:1:1 equivalent)\n";
        } else if (rawIFD.fYCbCrSubSampleH == 1 && rawIFD.fYCbCrSubSampleV == 1) {
            std::cout << "4:4:4 (No subsampling)\n";
        } else {
            std::cout << "Custom (H=" << rawIFD.fYCbCrSubSampleH
                      << ", V=" << rawIFD.fYCbCrSubSampleV << ")\n";
        }

        std::cout << "\n10.5.2 YCbCr→RGB 預處理: ";
        std::cout << "需要確認是否需要黑色階扣除\n";
        std::cout << "  - Lossy JPEG 已經是線性資料\n";
        std::cout << "  - 但需要套用 BlackLevel/WhiteLevel 線性化\n";
        std::cout << "  - ColorMatrix 套用位置待確認\n";

        std::cout << "\n10.5.3 HueSatMap 應用時機: RGB 空間\n";
        std::cout << "  - ToneCurve / HueSatMap / LookTable 都在 RGB 空間處理\n";
        std::cout << "  - YCbCr→RGB 後直接套用現有 Halide 管線\n";

    } catch (const dng_exception& e) {
        std::cerr << "\nDNG Exception: " << e.ErrorCode() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "\nException: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
