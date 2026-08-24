#include "libraw_frontend.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "dng_timing_utils.h"
#include "libraw/libraw.h"

// Policy note (spec section 6.3.6): LIBRAW_RAWSPEEDV3_IGNOREERRORS is
// deliberately NEVER set. Enabling it would let warning-flagged RawSpeed
// results through, and the spec forbids a worker turning it on just to make a
// sample pass. This comment is the single occurrence of that identifier in
// project source, and the architecture gate greps for exactly that.
//
// Pinned-revision substitution (LibRaw df226ea): the plan's reference code
// names the switch `imgdata.rawparams.use_rawspeed3`. That field does not
// exist at this revision. The real switch is the bitmask
// `imgdata.rawparams.use_rawspeed` (libraw_types.h:978), tested against
// LIBRAW_RAWSPEEDV3_USE / LIBRAW_RAWSPEEDV3_FAILONUNKNOWN
// (libraw_const.h:662-664) inside src/decoders/unpack.cpp:119-166. LibRaw's
// own default (init_close_utils.cpp:84) is 1 == LIBRAW_RAWSPEEDV1_USE, i.e.
// the V3 bit is OFF unless we set it. The timing helper is
// dng_timing::elapsed_ms() (dng_timing_utils.h), not dng_now_ms().

struct LibRawFrontendContext::Impl {
    LibRaw processor;
    LibRawRawView view;
    RawDecodeDiagnostics diag{};
    RawForcedBackend forced = RawForcedBackend::kAuto;
    bool open = false;
};

LibRawFrontendContext::LibRawFrontendContext() : impl_(new Impl()) {}

LibRawFrontendContext::~LibRawFrontendContext() {
    // Destruction order (spec section 5.1.7): the caller has already waited for
    // GPU completion; we invalidate the views, then recycle, then let the
    // processor destructor close everything LibRaw owns (including, upstream,
    // its internal RawSpeed handle).
    recycle();
}

void LibRawFrontendContext::set_forced_backend(RawForcedBackend backend) {
    impl_->forced = backend;
}

bool LibRawFrontendContext::is_open() const { return impl_->open; }

const LibRawRawView& LibRawFrontendContext::raw_view() const {
    // Contract violation to call this while closed (spec section 5.1.5):
    // assert in debug, zeroed view in release.
    assert(impl_->open && "raw_view() called on a closed LibRawFrontendContext");
    return impl_->view;
}

const RawDecodeDiagnostics& LibRawFrontendContext::diagnostics() const {
    return impl_->diag;
}

void LibRawFrontendContext::recycle() {
    if (!impl_) return;
    if (impl_->open) {
        impl_->view = LibRawRawView{};
        impl_->processor.recycle();
        impl_->open = false;
    }
}

RawErrorCode LibRawFrontendContext::open_and_unpack(const char* file_path) {
    if (!file_path || file_path[0] == '\0') return kRawErrNullPath;

    recycle();
    impl_->view = LibRawRawView{};
    impl_->diag = RawDecodeDiagnostics{};
    impl_->diag.frontend = kRawFrontendLibRaw;
    impl_->diag.unpack_backend = kRawDecoderBackendUnknown;

    // Step 1: raw decode options, recorded for observability.
    auto& params = impl_->processor.imgdata.rawparams;
    unsigned flags = 0;
    switch (impl_->forced) {
        case RawForcedBackend::kAuto:
            flags = LIBRAW_RAWSPEEDV3_USE;
            break;
        case RawForcedBackend::kRawSpeed3:
            flags = LIBRAW_RAWSPEEDV3_USE | LIBRAW_RAWSPEEDV3_FAILONUNKNOWN;
            break;
        case RawForcedBackend::kLibRawNative:
            flags = 0;
            break;
    }
    params.use_rawspeed = static_cast<int>(flags);
    impl_->diag.rawspeed_flags = flags;

    // Resource ceiling before any allocation (spec section 10.2).
    params.max_raw_memory_mb = kRawMaxRawMemoryMb;

    const auto t0 = std::chrono::high_resolution_clock::now();

    // Step 2.
    if (impl_->processor.open_file(file_path) != LIBRAW_SUCCESS) {
        impl_->processor.recycle();
        return kRawErrParseFailed;
    }

    // Step 3: ONE unpack() call. LibRaw tries RawSpeed3 when eligible and falls
    // back to the selected native load_raw inside this same call. The project
    // must never split this into tryRawSpeed()/tryLibRaw() (spec section 6.2).
    const int unpack_rc = impl_->processor.unpack();
    impl_->diag.raw_unpack_ms = dng_timing::elapsed_ms(
        t0, std::chrono::high_resolution_clock::now());

    if (unpack_rc != LIBRAW_SUCCESS) {
        impl_->diag.rawspeed_warning_bits =
            static_cast<uint32_t>(impl_->processor.imgdata.process_warnings);
        impl_->processor.recycle();
        return kRawErrUnpackFailed;
    }

    // Step 4: which decoder actually produced the pixels.
    const unsigned warnings =
        static_cast<unsigned>(impl_->processor.imgdata.process_warnings);
    impl_->diag.rawspeed_warning_bits = static_cast<uint32_t>(warnings);
    impl_->diag.unpack_backend = (warnings & LIBRAW_WARN_RAWSPEED3_PROCESSED)
                                     ? kRawDecoderBackendRawSpeed3
                                     : kRawDecoderBackendLibRawNative;

    // Step 5: pixels come only from imgdata.rawdata. P0 accepts U16 raw_image.
    const auto& rawdata = impl_->processor.imgdata.rawdata;
    const auto& sizes = impl_->processor.imgdata.sizes;
    if (rawdata.raw_image == nullptr) {
        impl_->processor.recycle();
        return kRawErrLayoutUnsupported;   // color3/color4/float variants: P1+
    }

    LibRawRawView view;
    view.plane.data = rawdata.raw_image;
    view.plane.width = sizes.raw_width;
    view.plane.height = sizes.raw_height;
    // Stride from raw_pitch, never width*2 (spec section 6.4.2).
    view.plane.row_stride_bytes = static_cast<int64_t>(sizes.raw_pitch);
    view.plane.pixel_stride_bytes = 2;
    view.plane.byte_size =
        static_cast<size_t>(sizes.raw_pitch) * static_cast<size_t>(sizes.raw_height);

    view.raw_width = sizes.raw_width;
    view.raw_height = sizes.raw_height;
    view.visible_left = sizes.left_margin;
    view.visible_top = sizes.top_margin;
    view.visible_width = sizes.width;
    view.visible_height = sizes.height;
    view.flip = sizes.flip;

    // Step 6: metadata only from LibRaw public data. Everything below aliases
    // the processor's memory; nothing is copied (spec section 6.4.3).
    const auto& idata = impl_->processor.imgdata.idata;
    const auto& color = impl_->processor.imgdata.color;
    view.colors = static_cast<uint32_t>(idata.colors);
    view.filters = idata.filters;
    view.cdesc = idata.cdesc;
    view.xtrans_pattern = (idata.filters == 9)
                              ? reinterpret_cast<const char*>(&idata.xtrans_abs[0][0])
                              : nullptr;
    view.cam_mul = color.cam_mul;
    view.pre_mul = color.pre_mul;
    view.cam_xyz = &color.cam_xyz[0][0];
    view.black_scalar = color.black;
    view.white_level = color.maximum;
    view.black_repeat_height = color.cblack[4];
    view.black_repeat_width = color.cblack[5];
    view.black_pattern = (view.black_repeat_width && view.black_repeat_height)
                             ? reinterpret_cast<const uint32_t*>(&color.cblack[6])
                             : nullptr;

    impl_->view = view;
    impl_->open = true;
    return kRawSuccess;
}
