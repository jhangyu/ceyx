#include <dng_host.h>
#include <dng_info.h>
#include <dng_file_stream.h>
#include <dng_negative.h>
#include <dng_exceptions.h>
#include <iostream>
#include <vector>
#include <chrono>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    try {
        auto t0 = std::chrono::steady_clock::now();
        dng_host host;
        dng_file_stream stream(argv[1]);
        dng_info info;
        info.Parse(host, stream);
        info.PostParse(host);
        
        auto t1 = std::chrono::steady_clock::now();
        std::cout << "Parse info took: " << std::chrono::duration<double, std::milli>(t1-t0).count() << " ms\n";
        
        int bestPreviewIfd = -1;
        uint32 bestPreviewWidth = 0;
        
        for (uint32 i = 0; i < info.fIFDCount; i++) {
            const dng_ifd &ifd = *info.fIFD[i];
            if (ifd.fCompression == 7 && ifd.fPhotometricInterpretation == 6 && ifd.fNewSubFileType == 1) {
                if (ifd.fImageWidth > bestPreviewWidth) {
                    bestPreviewWidth = ifd.fImageWidth;
                    bestPreviewIfd = i;
                }
            }
        }
        
        if (bestPreviewIfd != -1) {
            const dng_ifd &ifd = *info.fIFD[bestPreviewIfd];
            std::cout << "Found preview IFD: " << bestPreviewIfd << " (" << ifd.fImageWidth << "x" << ifd.fImageLength << ")\n";
            
            auto t2 = std::chrono::steady_clock::now();
            uint64 offset = ifd.fTileOffset[0];
            uint32 byteCount = ifd.fTileByteCount[0];
            
            std::vector<uint8_t> jpegData(byteCount);
            stream.SetReadPosition(offset);
            stream.Get(jpegData.data(), byteCount);
            auto t3 = std::chrono::steady_clock::now();
            
            std::cout << "Extract " << byteCount << " bytes of JPEG took: " << std::chrono::duration<double, std::milli>(t3-t2).count() << " ms\n";
            std::cout << "First bytes: " << std::hex << (int)jpegData[0] << " " << (int)jpegData[1] << std::dec << "\n";
        }
    } catch (const dng_exception& e) {
        std::cerr << "DNG Exception: " << e.ErrorCode() << "\n";
    }
    return 0;
}
