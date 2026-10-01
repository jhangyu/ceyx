// Shared RAW-suite manifest helpers (test_libraw_adapter, test_libraw_frontend,
// test_raw_end_to_end, test_raw_hardening). Deliberately a minimal hand-rolled
// reader: the tests must not gain a JSON dependency, and the manifest fields
// they need are flat strings. Behaviour is the verbatim union of the four
// per-file copies it replaced.
#ifndef RAW_TEST_SUPPORT_H_
#define RAW_TEST_SUPPORT_H_

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace raw_test_support {

struct RawManifestSample {
    std::string id, path, expect_route, expect_backend, expect_error, expect_layout;
};

inline std::string manifestField(const std::string& object, const char* key) {
    const std::string needle = std::string("\"") + key + "\": \"";
    const size_t at = object.find(needle);
    if (at == std::string::npos) return "";
    const size_t start = at + needle.size();
    const size_t end = object.find('"', start);
    return end == std::string::npos ? "" : object.substr(start, end - start);
}

inline std::vector<RawManifestSample> loadRawManifest(const char* path) {
    std::ifstream in(path);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    std::vector<RawManifestSample> out;
    size_t position = 0;
    while ((position = text.find('{', position)) != std::string::npos) {
        const size_t end = text.find('}', position);
        if (end == std::string::npos) break;
        const std::string object = text.substr(position, end - position);
        RawManifestSample sample{manifestField(object, "id"),
                                 manifestField(object, "path"),
                                 manifestField(object, "expect_route"),
                                 manifestField(object, "expect_backend"),
                                 manifestField(object, "expect_error"),
                                 manifestField(object, "expect_layout")};
        if (!sample.id.empty() && !sample.path.empty()) out.push_back(sample);
        position = end + 1;
    }
    return out;
}

inline bool fileExists(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return file.good();
}

}  // namespace raw_test_support

#endif  // RAW_TEST_SUPPORT_H_
