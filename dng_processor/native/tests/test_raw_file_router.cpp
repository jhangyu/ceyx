// Magic-byte routing: DNG vs non-DNG, from container structure only.
//
// The extension cases matter most: a .dng-named ARW must route generic and an
// .arw-named DNG must route to the DNG frontend, because the spec forbids
// deciding on the filename (section 6.1.1).
//
// Usage: no args runs the synthetic cases; one path argument probes that file
// and prints "route=<name>".
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "raw_file_router.h"

namespace {

int failures = 0;

void report(const char* name, bool ok, const char* detail) {
    std::printf("[RawRouter] %s %s -> %s\n", name, detail, ok ? "PASS" : "FAIL");
    if (!ok) ++failures;
}

void expectBytes(const char* name, const std::vector<uint8_t>& header,
                 RawErrorCode want_rc, RawRoute want_route) {
    RawRoute route = kRawRouteUnknown;
    const RawErrorCode rc =
        raw_probe_bytes(header.empty() ? nullptr : header.data(), header.size(), &route);
    char detail[160];
    std::snprintf(detail, sizeof(detail), "rc=%s route=%s want rc=%s route=%s",
                  raw_error_name(rc), raw_route_name(route),
                  raw_error_name(want_rc), raw_route_name(want_route));
    report(name, rc == want_rc && route == want_route, detail);
}

// Minimal TIFF: byte-order mark, magic 42, IFD0 offset 8, then entry count and
// entries. Each entry is 12 bytes: tag(2) type(2) count(4) value(4).
std::vector<uint8_t> makeTiff(bool little_endian, bool with_dng_version) {
    std::vector<uint8_t> b(kRawProbeHeaderBytes, 0);
    auto put16 = [&](size_t off, uint16_t v) {
        if (little_endian) { b[off] = v & 0xFF; b[off + 1] = (v >> 8) & 0xFF; }
        else               { b[off] = (v >> 8) & 0xFF; b[off + 1] = v & 0xFF; }
    };
    auto put32 = [&](size_t off, uint32_t v) {
        if (little_endian) {
            b[off] = v & 0xFF; b[off + 1] = (v >> 8) & 0xFF;
            b[off + 2] = (v >> 16) & 0xFF; b[off + 3] = (v >> 24) & 0xFF;
        } else {
            b[off] = (v >> 24) & 0xFF; b[off + 1] = (v >> 16) & 0xFF;
            b[off + 2] = (v >> 8) & 0xFF; b[off + 3] = v & 0xFF;
        }
    };

    b[0] = little_endian ? 'I' : 'M';
    b[1] = little_endian ? 'I' : 'M';
    put16(2, 42);
    put32(4, 8);              // IFD0 at offset 8
    put16(8, 2);              // two entries
    put16(10, 256); put16(12, 4); put32(14, 1); put32(18, 6048);   // ImageWidth
    if (with_dng_version) {
        put16(22, 50706); put16(24, 1); put32(26, 4); put32(30, 0x00000101);
    } else {
        put16(22, 257); put16(24, 4); put32(26, 1); put32(30, 4024);  // ImageLength
    }
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2) {
        RawRoute route = kRawRouteUnknown;
        const RawErrorCode rc = raw_probe_file(argv[1], &route);
        std::printf("route=%s rc=%s\n", raw_route_name(route), raw_error_name(rc));
        return rc == kRawSuccess ? 0 : 1;
    }

    expectBytes("tiff_le_with_dngversion", makeTiff(true, true),
                kRawSuccess, kRawRouteDng);
    expectBytes("tiff_be_with_dngversion", makeTiff(false, true),
                kRawSuccess, kRawRouteDng);
    expectBytes("tiff_le_without_dngversion", makeTiff(true, false),
                kRawSuccess, kRawRouteGeneric);

    {
        std::vector<uint8_t> raf(kRawProbeHeaderBytes, 0);
        std::memcpy(raf.data(), "FUJIFILMCCD-RAW ", 16);
        expectBytes("raf_magic", raf, kRawSuccess, kRawRouteGeneric);
    }
    {
        std::vector<uint8_t> rw2(kRawProbeHeaderBytes, 0);
        std::memcpy(rw2.data(), "IIU\x00", 4);
        expectBytes("rw2_magic", rw2, kRawSuccess, kRawRouteGeneric);
    }
    {
        std::vector<uint8_t> cr3(kRawProbeHeaderBytes, 0);
        cr3[0] = 0; cr3[1] = 0; cr3[2] = 0; cr3[3] = 0x18;
        std::memcpy(cr3.data() + 4, "ftypcrx ", 8);
        expectBytes("cr3_ftyp", cr3, kRawSuccess, kRawRouteGeneric);
    }
    {
        std::vector<uint8_t> mrm(kRawProbeHeaderBytes, 0);
        mrm[0] = 0x00; mrm[1] = 'M'; mrm[2] = 'R'; mrm[3] = 'M';
        expectBytes("mrm_magic", mrm, kRawSuccess, kRawRouteGeneric);
    }
    {
        std::vector<uint8_t> junk(kRawProbeHeaderBytes, 0xA5);
        expectBytes("garbage_header", junk, kRawErrProbeFailed, kRawRouteUnknown);
    }
    {
        std::vector<uint8_t> tiny{'I', 'I', 42, 0};
        expectBytes("header_too_short", tiny, kRawErrProbeFailed, kRawRouteUnknown);
    }
    {
        // IFD0 offset points past the probe window: must not read out of bounds,
        // and must fall back to generic rather than guessing DNG.
        std::vector<uint8_t> far = makeTiff(true, true);
        far[4] = 0x00; far[5] = 0x10; far[6] = 0x00; far[7] = 0x00;  // offset 4096
        expectBytes("truncated_ifd_offset_out_of_range", far,
                    kRawSuccess, kRawRouteGeneric);
    }
    {
        RawRoute route = kRawRouteUnknown;
        const RawErrorCode rc = raw_probe_file(nullptr, &route);
        char detail[96];
        std::snprintf(detail, sizeof(detail), "rc=%s", raw_error_name(rc));
        report("null_path", rc == kRawErrNullPath, detail);
    }
    {
        // Extension independence, both directions, via the byte-level API:
        // content decides, never the name.
        RawRoute route = kRawRouteUnknown;
        const std::vector<uint8_t> dng_bytes = makeTiff(true, true);
        raw_probe_bytes(dng_bytes.data(), dng_bytes.size(), &route);
        report("dng_content_with_arw_extension", route == kRawRouteDng,
               "content=dng");
        std::vector<uint8_t> arw_bytes = makeTiff(true, false);
        raw_probe_bytes(arw_bytes.data(), arw_bytes.size(), &route);
        report("arw_content_with_dng_extension", route == kRawRouteGeneric,
               "content=non-dng");
    }

    if (failures != 0) {
        std::printf("[RawRouter] FAIL (%d cases)\n", failures);
        return 1;
    }
    std::printf("[RawRouter] ALL PASS\n");
    return 0;
}
