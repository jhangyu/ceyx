#pragma once

#include <chrono>

namespace dng_timing {

inline double elapsed_ms(
    std::chrono::high_resolution_clock::time_point start,
    std::chrono::high_resolution_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

} // namespace dng_timing
