// test_gpu_alloc_failure_is_recoverable — an impossible GPU allocation must
// return an error code (not abort the process), and the next real decode must
// still succeed. Design: Halcyon docs/logs/2026-10-04/halide-error-handler-
// recommendation.md section 3.4. Without the Halide error handler installed by
// dng_halide_gpu_device_interface(), halide_device_malloc below calls
// halide_default_error -> abort() (RC=134).
// Usage: test_gpu_alloc_failure_is_recoverable <decodable RAW/DNG>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "ceyx_decode_into.h"
#include "dng_ffi_api.h"
#include "dng_halide_device.h"
#include "test_report.h"

namespace {
constexpr const char kReportPrefix[] = "GpuAllocFailure";

bool decode_one(const char* path) {
  int32_t w = 0, h = 0;
  if (ceyx_probe_output_size(path, 0, &w, &h) != 0 || w <= 0 || h <= 0) return false;
  std::vector<uint8_t> dst(static_cast<size_t>(w) * h * 4);
  DngResult* r = ceyx_decode_into_buffer(path, 0, dst.data(), dst.size());
  const bool ok = r != nullptr && r->error_code == 0 && r->rgba_data == dst.data();
  if (r != nullptr) dng_free_result(r);
  return ok;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("[%s] usage: %s <raw>\n", kReportPrefix, argv[0]);
    return 2;
  }
  // Warm decode first so the GPU runtime exists, as in production.
  if (!decode_one(argv[1])) {
    std::printf("[%s] warm decode of %s failed\n", kReportPrefix, argv[1]);
    return 2;
  }
  const halide_device_interface_t* iface = dng_halide_gpu_device_interface();
  if (iface == nullptr) {
    std::printf("[%s] no GPU interface\n", kReportPrefix);
    return 2;
  }
  // 2^30 x 2^12 x 4 B = 16 TiB: larger than any device heap.
  halide_dimension_t dim[2] = {{0, 1 << 30, 1, 0}, {0, 1 << 12, 1 << 30, 0}};
  halide_buffer_t buf = {};
  buf.type = halide_type_t(halide_type_uint, 32);
  buf.dimensions = 2;
  buf.dim = dim;
  const int rc = halide_device_malloc(nullptr, &buf, iface);
  std::printf("[%s] oversized halide_device_malloc rc=%d\n", kReportPrefix, rc);
  test_report::report(kReportPrefix, "oversized_malloc_returns_error", rc != 0, "");
  test_report::report(kReportPrefix, "no_device_handle_left", buf.device == 0, "");
  test_report::report(kReportPrefix, "decode_after_failure_succeeds", decode_one(argv[1]), "");
  dng_halide_release_device();
  return test_report::finish(kReportPrefix);
}
