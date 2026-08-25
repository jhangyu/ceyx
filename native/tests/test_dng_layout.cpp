#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <fstream>

#include <dng_host.h>
#include <dng_info.h>
#include <dng_negative.h>
#include <dng_file_stream.h>
#include <dng_exceptions.h>
#include <dng_image.h>
#include <dng_ifd.h>

bool extractPreviewJPEG(const std::string& path, std::vector<uint8_t>& outJpegBytes) {
    auto start = std::chrono::high_resolution_clock::now();
    try {
        dng_host host;
        dng_file_stream stream(path.c_str());
        dng_info info;
        
        info.Parse(host, stream);
        info.PostParse(host);
        
        if (!info.IsValidDNG()) {
            std::cerr << "Invalid DNG file\n";
            return false;
        }

        for (uint32_t i = 0; i < info.fIFDCount; ++i) {
            auto ifd = info.fIFD[i].Get();
            if (!ifd) continue;

            if (ifd->fNewSubFileType == 1 && ifd->fCompression == 7) {
                if (ifd->fTileOffsetsCount > 0 && ifd->fTileByteCountsCount > 0) {
                    uint64_t offset = ifd->fTileOffset[0];
                    uint32_t byteCount = ifd->fTileByteCount[0];
                    
                    std::cout << "Found Preview JPEG in IFD " << i << ". Offset: " << offset << ", Size: " << byteCount << " bytes\n";
                    
                    outJpegBytes.resize(byteCount);
                    stream.SetReadPosition(offset);
                    stream.Get(outJpegBytes.data(), byteCount);
                    
                    auto end = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
                    std::cout << "Successfully extracted JPEG preview in " << duration.count() << " ms.\n";
                    return true;
                }
            }
        }
    } catch (const dng_exception& e) {
        std::cerr << "DNG Exception: " << e.ErrorCode() << std::endl;
        return false;
    } catch (...) {
        std::cerr << "Unknown exception" << std::endl;
        return false;
    }
    std::cout << "No Preview JPEG found in IFDs.\n";
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <sample.dng>\n";
        return 1;
    }
    
    std::vector<uint8_t> jpegBytes;
    if (extractPreviewJPEG(argv[1], jpegBytes)) {
        std::cout << "Successfully read " << jpegBytes.size() << " bytes of JPEG preview.\n";
        
        std::ofstream outfile("preview.jpg", std::ios::binary);
        if (outfile) {
            outfile.write(reinterpret_cast<const char*>(jpegBytes.data()), jpegBytes.size());
            std::cout << "Saved preview to preview.jpg\n";
        }
    }

    try {
        dng_host host;
        dng_file_stream stream(argv[1]);
        dng_info info;
        
        info.Parse(host, stream);
        info.PostParse(host);
        
        if (!info.IsValidDNG()) {
            std::cerr << "Invalid DNG file\n";
            return 1;
        }

        std::cout << "--- DNG Layout Test ---" << std::endl;
        std::cout << "IFD Count: " << info.fIFDCount << std::endl;
        
        for (uint32_t i = 0; i < info.fIFDCount; ++i) {
            std::cout << "\nIFD " << i << ":" << std::endl;
            if (info.fIFD[i].Get() == nullptr) {
                std::cout << "  (null IFD)" << std::endl;
                continue;
            }
            std::cout << "  NewSubFileType: " << info.fIFD[i]->fNewSubFileType << std::endl;
            std::cout << "  Image Width: " << info.fIFD[i]->fImageWidth << std::endl;
            std::cout << "  Image Length: " << info.fIFD[i]->fImageLength << std::endl;
            std::cout << "  Compression: " << info.fIFD[i]->fCompression;
            if (info.fIFD[i]->fCompression == 7) {
                std::cout << " (Lossless JPEG)";
            } else if (info.fIFD[i]->fCompression == 1) {
                std::cout << " (Uncompressed)";
            }
            std::cout << std::endl;
            std::cout << "  Uses Tiles: " << (info.fIFD[i]->fUsesTiles ? "Yes" : "No") << std::endl;
            std::cout << "  Tile Width: " << info.fIFD[i]->fTileWidth << std::endl;
            std::cout << "  Tile Length: " << info.fIFD[i]->fTileLength << std::endl;
            std::cout << "  Tiles (Across x Down): " << info.fIFD[i]->TilesAcross() << " x " << info.fIFD[i]->TilesDown() << " (" << info.fIFD[i]->TilesPerImage() << " total units)" << std::endl;
        }
        
        AutoPtr<dng_negative> negative(host.Make_dng_negative());
        negative->Parse(host, stream, info);
        negative->PostParse(host, stream, info);
        negative->SynchronizeMetadata();

        std::cout << "\nNegative Summary:" << std::endl;
        std::cout << "  Negative Raw Image Width: " << negative->RawImage().Bounds().W() << std::endl;
        std::cout << "  Negative Raw Image Height: " << negative->RawImage().Bounds().H() << std::endl;

    } catch (const dng_exception& e) {
        std::cerr << "DNG Exception: " << e.ErrorCode() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception" << std::endl;
        return 1;
    }

    return 0;
}
