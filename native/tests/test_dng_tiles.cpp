#include <dng_file_stream.h>
#include <dng_host.h>
#include <dng_info.h>
#include <dng_ifd.h>
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <dng_file>\n";
        return 1;
    }

    try {
        dng_host host;
        dng_file_stream stream(argv[1]);
        dng_info info;
        
        info.Parse(host, stream);
        info.PostParse(host);
        
        std::cout << "=== Main IFDs ===\n";
        std::cout << "IFD Count: " << info.fIFDCount << std::endl;
        for (uint32_t i = 0; i < info.fIFDCount; i++) {
            dng_ifd* ifd = info.fIFD[i].Get();
            if (ifd) {
                std::cout << "IFD " << i << ":\n";
                std::cout << "  New SubFileType: " << ifd->fNewSubFileType << " (0=Main, 1=Preview)\n";
                std::cout << "  Image Size: " << ifd->fImageWidth << " x " << ifd->fImageLength << "\n";
                std::cout << "  Uses Tiles: " << ifd->fUsesTiles << "\n";
                std::cout << "  Uses Strips: " << ifd->fUsesStrips << "\n";
                std::cout << "  Tile Size: " << ifd->fTileWidth << " x " << ifd->fTileLength << "\n";
                std::cout << "  Compression: " << ifd->fCompression << " (7=JPEG, 34892=Lossless JPEG, 1=Uncompressed)\n";
                if (i == info.fMainIndex) {
                    std::cout << "  *** THIS IS THE MAIN RAW IMAGE ***\n";
                }
                if (ifd->fUsesTiles || ifd->fUsesStrips) {
                    uint32_t tilesAcross = (ifd->fImageWidth + ifd->fTileWidth - 1) / ifd->fTileWidth;
                    uint32_t tilesDown = (ifd->fImageLength + ifd->fTileLength - 1) / ifd->fTileLength;
                    std::cout << "  Calculated Tiles/Strips: " << (tilesAcross * tilesDown) << "\n";
                }
            }
        }
        
        std::cout << "\n=== Chained IFDs ===\n";
        std::cout << "Chained IFD Count: " << info.fChainedIFDCount << std::endl;
        for (uint32_t i = 0; i < info.fChainedIFDCount; i++) {
            dng_ifd* ifd = info.fChainedIFD[i].Get();
            if (ifd) {
                std::cout << "Chained IFD " << i << ":\n";
                std::cout << "  New SubFileType: " << ifd->fNewSubFileType << "\n";
                std::cout << "  Image Size: " << ifd->fImageWidth << " x " << ifd->fImageLength << "\n";
                std::cout << "  Compression: " << ifd->fCompression << "\n";
            }
        }
    } catch (const dng_exception& e) {
        std::cerr << "DNG Exception: " << e.ErrorCode() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception.\n";
        return 1;
    }
    
    return 0;
}
