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

// F2 gate (round-3 review finding F2). LibRaw decides where a decoder's pixels
// land in third_party/libraw/src/decoders/unpack.cpp:
//   :382  else if (imgdata.idata.filters || P1.colors == 1)  -> raw_alloc is
//         allocated AND raw_image = (ushort*)raw_alloc          (our case)
//   :402  else (sRAW / legacy / Foveon)                       -> raw_alloc = 0
//         and :436 raw_image = (ushort*)imgdata.image, a 4-COMPONENT buffer
//         with S.raw_pitch = width*8. That view is internally inconsistent
//         with pixel_stride_bytes = 2 and the validator cannot see it, because
//         an over-large stride passes every stride rule.
// The legacy branch is literally the `else` of the filters/colors predicate, so
// clause 2 below is what structurally excludes it.
//
// Pinned-revision substitution (LibRaw df226ea): the review's fix text asks for
// `raw_alloc != nullptr && raw_image == raw_alloc` verbatim. That is NOT
// satisfiable at this revision for RawSpeed3-decoded files: unpack.cpp:189
// assigns raw_image = rs3ret.pixeldata and never touches raw_alloc (grep of
// src/: raw_alloc is only assigned at unpack.cpp:334/351/372/392/429/461/469
// plus the DNG-SDK / x3f / fp_dng / phaseone glues). Requiring identity would
// false-reject every RawSpeed3 Bayer decode, including the corpus case
// frontend_switch_lossless_dng. phaseone_processing.cpp:37 likewise leaves
// raw_alloc == 0 with valid raw_image pixels. So clause 3 is stated as the
// contrapositive: IF LibRaw allocated a raw store, raw_image must BE that
// store (rejects color3/color4/float allocations that leave a stale
// raw_image); if there is no raw store, the pixels are owned elsewhere and
// clause 2 decides.
bool raw_frontend_pixels_live_in_raw_image(const void* raw_alloc,
                                           const void* raw_image,
                                           uint32_t filters,
                                           uint32_t colors) {
    if (raw_image == nullptr) return false;                       // clause 1
    if (filters == 0 && colors != 1) return false;                // clause 2
    if (raw_alloc != nullptr && raw_alloc != raw_image) return false;  // clause 3
    return true;
}

struct LibRawFrontendContext::Impl {
    LibRaw processor;
    LibRawRawView view;
    RawDecodeDiagnostics diag{};
    RawForcedBackend forced = RawForcedBackend::kAuto;
    int (*cancel_poll)(void*) = nullptr;
    void* cancel_user = nullptr;
    bool open = false;

    bool cancelled() const {
        return cancel_poll && cancel_poll(cancel_user) != 0;
    }
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

void LibRawFrontendContext::set_cancel_hook(int (*poll)(void*), void* user_data) {
    impl_->cancel_poll = poll;
    impl_->cancel_user = user_data;
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

    // Cancellation poll between open_file and unpack (spec section 10.4). This
    // is the cheapest place to abort: the header is parsed but the pixel
    // allocation has not happened yet. There is no lock here by design.
    if (impl_->cancelled()) {
        impl_->processor.recycle();
        return kRawErrKernelFailed;
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

    // Second poll, after unpack: the pixels exist but nothing has borrowed them
    // yet and no GPU command has been issued, so recycling here is safe.
    if (impl_->cancelled()) {
        impl_->processor.recycle();
        return kRawErrKernelFailed;
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
    if (!raw_frontend_pixels_live_in_raw_image(
            impl_->processor.imgdata.rawdata.raw_alloc, rawdata.raw_image,
            impl_->processor.imgdata.idata.filters,
            static_cast<uint32_t>(impl_->processor.imgdata.idata.colors))) {
        // color3/color4/float variants (P1+), and the sRAW / legacy decoders
        // that alias raw_image onto the 4-component imgdata.image.
        impl_->processor.recycle();
        return kRawErrLayoutUnsupported;
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
    view.black_channel = reinterpret_cast<const uint32_t*>(&color.cblack[0]);

    impl_->view = view;
    impl_->open = true;
    return kRawSuccess;
}
