// Pins the ABI layouts the Dart side mirrors. A field-count mismatch shipped
// once before (heif_api.h:14-15, Gotcha #58); this is the mechanical guard.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "ceyx_encode_api.h"
#include "ceyx_still_api.h"
#include "heif_api.h"

static int g_failures = 0;

static void expect_eq(const char *what, long long got, long long want) {
  if (got != want) {
    std::printf("FAIL %s: got %lld, want %lld\n", what, got, want);
    ++g_failures;
  } else {
    std::printf("ok   %s == %lld\n", what, got);
  }
}

int main() {
  // --- struct sizes -------------------------------------------------------
  expect_eq("sizeof(CeyxEncodeOptions)", (long long)sizeof(CeyxEncodeOptions), 20);
  expect_eq("sizeof(CeyxStillResult)", (long long)sizeof(CeyxStillResult), 32);
  expect_eq("sizeof(CeyxStillResult)==sizeof(HeifResult)",
            (long long)sizeof(CeyxStillResult), (long long)sizeof(HeifResult));

  // --- CeyxStillResult offsets must equal HeifResult's, field for field ----
  expect_eq("offsetof(CeyxStillResult, error_code)",
            (long long)offsetof(CeyxStillResult, error_code),
            (long long)offsetof(HeifResult, error_code));
  expect_eq("offsetof(CeyxStillResult, width)",
            (long long)offsetof(CeyxStillResult, width),
            (long long)offsetof(HeifResult, width));
  expect_eq("offsetof(CeyxStillResult, height)",
            (long long)offsetof(CeyxStillResult, height),
            (long long)offsetof(HeifResult, height));
  expect_eq("offsetof(CeyxStillResult, orientation)",
            (long long)offsetof(CeyxStillResult, orientation),
            (long long)offsetof(HeifResult, orientation));
  expect_eq("offsetof(CeyxStillResult, rgba)",
            (long long)offsetof(CeyxStillResult, rgba), 16);
  expect_eq("offsetof(CeyxStillResult, rgba_len)",
            (long long)offsetof(CeyxStillResult, rgba_len), 24);

  // --- CeyxEncodeOptions offsets ------------------------------------------
  expect_eq("offsetof(CeyxEncodeOptions, struct_size)",
            (long long)offsetof(CeyxEncodeOptions, struct_size), 0);
  expect_eq("offsetof(CeyxEncodeOptions, quality)",
            (long long)offsetof(CeyxEncodeOptions, quality), 4);
  expect_eq("offsetof(CeyxEncodeOptions, lossless)",
            (long long)offsetof(CeyxEncodeOptions, lossless), 8);
  expect_eq("offsetof(CeyxEncodeOptions, effort)",
            (long long)offsetof(CeyxEncodeOptions, effort), 12);
  expect_eq("offsetof(CeyxEncodeOptions, reserved0)",
            (long long)offsetof(CeyxEncodeOptions, reserved0), 16);

  // --- CeyxEncodeMetadata layout (LP64) -----------------------------------
  expect_eq("sizeof(CeyxEncodeMetadata)", (long long)sizeof(CeyxEncodeMetadata), 56);
  expect_eq("offsetof(CeyxEncodeMetadata, exif)",
            (long long)offsetof(CeyxEncodeMetadata, exif), 8);
  expect_eq("offsetof(CeyxEncodeMetadata, icc_len)",
            (long long)offsetof(CeyxEncodeMetadata, icc_len), 48);

  // --- error-code values: disjointness is the whole contract --------------
  expect_eq("kCeyxEncodeErrBadOptions", kCeyxEncodeErrBadOptions, -408);
  expect_eq("kCeyxEncodeErrMetadataRejected", kCeyxEncodeErrMetadataRejected, -409);
  expect_eq("kCeyxEncodeErrBadFormat", kCeyxEncodeErrBadFormat, -410);
  expect_eq("kCeyxEncodeErrLosslessUnsupported", kCeyxEncodeErrLosslessUnsupported, -411);

  expect_eq("kCeyxStillSuccess", kCeyxStillSuccess, 0);
  expect_eq("kCeyxStillErrNullPath", kCeyxStillErrNullPath, -501);
  expect_eq("kCeyxStillErrOpenFailed", kCeyxStillErrOpenFailed, -502);
  expect_eq("kCeyxStillErrBadFormat", kCeyxStillErrBadFormat, -503);
  expect_eq("kCeyxStillErrUnsupported", kCeyxStillErrUnsupported, -504);
  expect_eq("kCeyxStillErrNoPrimaryItem", kCeyxStillErrNoPrimaryItem, -505);
  expect_eq("kCeyxStillErrDecodeFailed", kCeyxStillErrDecodeFailed, -506);
  expect_eq("kCeyxStillErrColorConversion", kCeyxStillErrColorConversion, -507);
  expect_eq("kCeyxStillErrAllocationFailed", kCeyxStillErrAllocationFailed, -508);
  expect_eq("kCeyxStillErrSizeOverflow", kCeyxStillErrSizeOverflow, -509);
  expect_eq("kCeyxStillErrMetadataInvalid", kCeyxStillErrMetadataInvalid, -510);
  expect_eq("kCeyxStillErrUnknownException", kCeyxStillErrUnknownException, -511);

  // --- format enum values -------------------------------------------------
  expect_eq("kCeyxFormatUnknown", kCeyxFormatUnknown, 0);
  expect_eq("kCeyxFormatJpeg", kCeyxFormatJpeg, 1);
  expect_eq("kCeyxFormatWebp", kCeyxFormatWebp, 2);
  expect_eq("kCeyxFormatHeic", kCeyxFormatHeic, 3);
  expect_eq("kCeyxFormatAvif", kCeyxFormatAvif, 4);
  expect_eq("kCeyxFormatJxl", kCeyxFormatJxl, 5);

  std::printf(g_failures == 0 ? "ABI_LAYOUT_OK\n" : "ABI_LAYOUT_FAILED\n");
  return g_failures == 0 ? 0 : 1;
}
