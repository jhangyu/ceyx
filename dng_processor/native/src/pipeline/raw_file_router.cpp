#include "raw_file_router.h"

#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t kTagDngVersion = 50706;
constexpr size_t kTiffEntrySize = 12;

uint16_t read16(const uint8_t* p, bool little_endian) {
    return little_endian ? static_cast<uint16_t>(p[0] | (p[1] << 8))
                         : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read32(const uint8_t* p, bool little_endian) {
    return little_endian
        ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
        : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
}

bool startsWith(const uint8_t* h, size_t n, const char* magic, size_t magic_len,
                size_t offset = 0) {
    if (n < offset + magic_len) return false;
    return std::memcmp(h + offset, magic, magic_len) == 0;
}

// True when IFD0 carries the DNGVersion tag within the probe window.
// "Not found" deliberately means generic: a DNG whose IFD0 sits past the
// window is parsed correctly by LibRaw anyway, and the disagreement surfaces
// as a route mismatch diagnostic rather than as a wrong decode.
bool hasDngVersionTag(const uint8_t* h, size_t n, bool little_endian) {
    if (n < 8) return false;
    const uint32_t ifd0 = read32(h + 4, little_endian);
    // Do the bound check in uint64_t: ifd0 + 2 in uint32_t wraps for
    // ifd0 in {0xFFFFFFFE, 0xFFFFFFFF}, which would otherwise pass this
    // check and let read16() below dereference an out-of-bounds address.
    if (ifd0 < 8 || static_cast<uint64_t>(ifd0) + 2 > n) return false;

    const uint16_t count = read16(h + ifd0, little_endian);
    for (uint16_t i = 0; i < count; ++i) {
        // ifd0 and i are widened to size_t/uint64_t before the add, so this
        // cannot wrap the way the check above could.
        const uint64_t entry = static_cast<uint64_t>(ifd0) + 2 + static_cast<uint64_t>(i) * kTiffEntrySize;
        if (entry + kTiffEntrySize > n) return false;   // bounds check first
        if (read16(h + entry, little_endian) == kTagDngVersion) return true;
    }
    return false;
}

}  // namespace

extern "C" {

const char* raw_route_name(RawRoute route) {
    switch (route) {
        case kRawRouteDng: return "dng";
        case kRawRouteGeneric: return "generic";
        default: return "unknown";
    }
}

RawErrorCode raw_probe_bytes(const uint8_t* header, size_t header_size,
                             RawRoute* out_route) {
    if (out_route) *out_route = kRawRouteUnknown;
    if (!out_route) return kRawErrProbeFailed;
    if (!header || header_size < 8) return kRawErrProbeFailed;

    // Non-TIFF RAW containers route generic immediately.
    if (startsWith(header, header_size, "FUJIFILMCCD-RAW", 15) ||
        startsWith(header, header_size, "\x00MRM", 4) ||
        startsWith(header, header_size, "ftypcrx", 7, 4) ||
        startsWith(header, header_size, "ftypcr3", 7, 4) ||
        startsWith(header, header_size, "IIU\x00", 4) ||
        startsWith(header, header_size, "IIRO", 4) ||
        startsWith(header, header_size, "IIRS", 4) ||
        // Foveon X3F (P19 W2). Same four-byte key LibRaw dispatches parse_x3f()
        // on (third_party/libraw/src/metadata/identify.cpp:687); the container
        // is not TIFF, so without this the probe fails before the frontend.
        startsWith(header, header_size, "FOVb", 4)) {
        *out_route = kRawRouteGeneric;
        return kRawSuccess;
    }

    const bool little_endian = (header[0] == 'I' && header[1] == 'I');
    const bool big_endian = (header[0] == 'M' && header[1] == 'M');
    if (!little_endian && !big_endian) return kRawErrProbeFailed;
    if (read16(header + 2, little_endian) != 42) return kRawErrProbeFailed;

    *out_route = hasDngVersionTag(header, header_size, little_endian)
                     ? kRawRouteDng
                     : kRawRouteGeneric;
    return kRawSuccess;
}

RawErrorCode raw_probe_file(const char* file_path, RawRoute* out_route) {
    if (out_route) *out_route = kRawRouteUnknown;
    if (!file_path || file_path[0] == '\0') return kRawErrNullPath;

    std::FILE* fp = std::fopen(file_path, "rb");
    if (!fp) return kRawErrProbeFailed;

    uint8_t header[kRawProbeHeaderBytes];
    const size_t got = std::fread(header, 1, sizeof(header), fp);
    std::fclose(fp);

    return raw_probe_bytes(header, got, out_route);
}

}  // extern "C"
