#ifndef STAGE_CONTRACT_CHECKS_H_
#define STAGE_CONTRACT_CHECKS_H_

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace StageContract {

constexpr uint32_t kPixelTypeByte = 1;   // DNG ttByte
constexpr uint32_t kPixelTypeShort = 3;  // DNG ttShort

enum class DecodePath {
    CFA_BAYER,
    LINEAR_RAW_LOSSY,
    YCBCR,
    OTHER
};

inline DecodePath detectDecodePath(uint32_t photometric) {
    if (photometric == 32803) return DecodePath::CFA_BAYER;
    if (photometric == 34892) return DecodePath::LINEAR_RAW_LOSSY;
    if (photometric == 6) return DecodePath::YCBCR;
    return DecodePath::OTHER;
}

inline std::string decodePathName(DecodePath path) {
    switch (path) {
        case DecodePath::CFA_BAYER: return "CFA_BAYER";
        case DecodePath::LINEAR_RAW_LOSSY: return "LINEAR_RAW_LOSSY";
        case DecodePath::YCBCR: return "YCBCR";
        default: return "OTHER";
    }
}

inline bool validateStageContract16(const std::string& stageName,
                                    DecodePath path,
                                    uint32_t width,
                                    uint32_t height,
                                    uint32_t planes,
                                    uint32_t pixelType,
                                    size_t pixelSize,
                                    size_t elementCount,
                                    std::ostream& os = std::cout) {
    bool ok = true;
    std::vector<std::string> errors;

    if (width == 0 || height == 0) {
        ok = false;
        errors.emplace_back("width/height must be > 0");
    }

    if (path == DecodePath::CFA_BAYER) {
        if (pixelType != kPixelTypeShort) {
            ok = false;
            errors.emplace_back("CFA path requires pixelType ttShort");
        }
        if (pixelSize != sizeof(uint16_t)) {
            ok = false;
            errors.emplace_back("CFA path requires pixelSize = 2 bytes");
        }
    } else if (path == DecodePath::LINEAR_RAW_LOSSY || path == DecodePath::YCBCR) {
        const bool typeOk = (pixelType == kPixelTypeByte || pixelType == kPixelTypeShort);
        const bool sizeOk = (pixelSize == sizeof(uint8_t) || pixelSize == sizeof(uint16_t));
        if (!typeOk) {
            ok = false;
            errors.emplace_back("lossy/YCbCr path requires pixelType ttByte or ttShort");
        }
        if (!sizeOk) {
            ok = false;
            errors.emplace_back("lossy/YCbCr path requires pixelSize 1 or 2 bytes");
        }
    }

    const size_t expectedElements = static_cast<size_t>(width) * height * planes;
    if (expectedElements != elementCount) {
        ok = false;
        errors.emplace_back("buffer element count mismatch");
    }

    uint32_t expectedPlanes = 0;
    if (stageName == "Stage1" || stageName == "Stage2") {
        if (path == DecodePath::CFA_BAYER) expectedPlanes = 1;
        else if (path == DecodePath::LINEAR_RAW_LOSSY || path == DecodePath::YCBCR) expectedPlanes = 3;
    } else if (stageName == "Stage3") {
        if (path == DecodePath::CFA_BAYER ||
            path == DecodePath::LINEAR_RAW_LOSSY ||
            path == DecodePath::YCBCR) expectedPlanes = 3;
    }

    if (expectedPlanes != 0 && planes != expectedPlanes) {
        ok = false;
        errors.emplace_back("unexpected planes count, expected " + std::to_string(expectedPlanes) +
                            ", got " + std::to_string(planes));
    }

    os << "  [Contract] " << stageName
       << " path=" << decodePathName(path)
       << " size=" << width << "x" << height
       << " planes=" << planes
       << " pixelType=" << pixelType
       << " pixelSize=" << pixelSize
       << " layout=interleaved(rowStep=width*planes,colStep=planes,planeStep=1)"
       << " -> " << (ok ? "PASS" : "FAIL") << "\n";

    if (!ok) {
        for (const auto& e : errors) {
            os << "    - " << e << "\n";
        }
    }

    return ok;
}

inline bool validateRenderImageContract(const std::string& stageName,
                                        uint32_t outW,
                                        uint32_t outH,
                                        uint32_t planes,
                                        uint32_t pixelType,
                                        size_t pixelSize,
                                        size_t rgbBytes,
                                        std::ostream& os = std::cout) {
    bool ok = true;
    std::vector<std::string> errors;

    if (pixelType != kPixelTypeByte) {
        ok = false;
        errors.emplace_back("render output pixelType must be ttByte");
    }

    if (planes != 3) {
        ok = false;
        errors.emplace_back("render output planes must be 3 (RGB)");
    }

    const size_t expectedBytes = static_cast<size_t>(outW) * outH * 3;
    if (rgbBytes != expectedBytes) {
        ok = false;
        errors.emplace_back("render buffer size mismatch");
    }

    os << "  [Contract] " << stageName << " path=RENDER"
       << " size=" << outW << "x" << outH
       << " planes=" << planes
       << " pixelType=" << pixelType
       << " pixelSize=" << pixelSize
       << " layout=interleaved(rowStep=width*3,colStep=3,planeStep=1)"
       << " -> " << (ok ? "PASS" : "FAIL") << "\n";

    if (!ok) {
        for (const auto& e : errors) {
            os << "    - " << e << "\n";
        }
    }

    return ok;
}

inline bool validateRawBufferContract(const std::string& stageName,
                                      size_t width,
                                      size_t height,
                                      size_t planes,
                                      size_t bytesPerSample,
                                      size_t actualBytes,
                                      std::ostream& os = std::cout) {
    const size_t expected = width * height * planes * bytesPerSample;
    const bool ok = (actualBytes == expected && width > 0 && height > 0);
    os << "[Contract] " << stageName
       << " size=" << width << "x" << height
       << " planes=" << planes
       << " bytesPerSample=" << bytesPerSample
       << " layout=interleaved"
       << " bytes=" << actualBytes
       << " -> " << (ok ? "PASS" : "FAIL") << "\n";
    if (!ok) {
        os << "  - expected bytes: " << expected << "\n";
    }
    return ok;
}

}  // namespace StageContract

#endif  // STAGE_CONTRACT_CHECKS_H_
