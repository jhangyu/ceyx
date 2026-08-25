// CPU-only proof that the vendored LibRaw links, opens and unpacks a RAW file.
// Links neither Halide nor dng_decoder_native: if this binary works, the
// dependency is sound before any GPU work starts. Calls open_file() + unpack()
// only; no LibRaw CPU render API (spec section 6.4.6).
#include <cstdio>

#include "libraw/libraw.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: libraw_smoke <raw-file>\n");
        return 2;
    }

    LibRaw processor;
    processor.imgdata.rawparams.use_rawspeed = LIBRAW_RAWSPEEDV3_USE;

    if (processor.open_file(argv[1]) != LIBRAW_SUCCESS) {
        std::printf("[LibRawSmoke] unpack=fail backend=unknown raw=0x0 pitch=0\n");
        return 1;
    }
    if (processor.unpack() != LIBRAW_SUCCESS) {
        std::printf("[LibRawSmoke] unpack=fail backend=unknown raw=0x0 pitch=0\n");
        return 1;
    }

    const bool rawspeed_used =
        (processor.imgdata.process_warnings & LIBRAW_WARN_RAWSPEED3_PROCESSED) != 0;

    libraw_decoder_info_t dinfo;
    const char* decoder_name = "(unknown)";
    if (processor.get_decoder_info(&dinfo) == LIBRAW_SUCCESS && dinfo.decoder_name) {
        decoder_name = dinfo.decoder_name;
    }

    std::printf("[LibRawSmoke] unpack=%s backend=%s raw=%dx%d pitch=%d "
                "fuji_width=%d filters=%u decoder=%s\n",
                "ok",
                rawspeed_used ? "rawspeed3" : "libraw_native",
                processor.imgdata.sizes.raw_width, processor.imgdata.sizes.raw_height,
                processor.imgdata.sizes.raw_pitch,
                processor.imgdata.rawdata.ioparams.fuji_width,
                processor.imgdata.idata.filters,
                decoder_name);
    return 0;
}
